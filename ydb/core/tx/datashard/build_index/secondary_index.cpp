#include "common_helper.h"
#include "../datashard_impl.h"
#include "../range_ops.h"
#include "../scan_common.h"
#include "../upload_stats.h"
#include "../buffer_data.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/tablet_flat/flat_row_state.h>
#include <ydb/core/kqp/common/kqp_types.h>

#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/tx/tx_proxy/upload_rows.h>

#include <yql/essentials/public/issue/yql_issue_message.h>

#include <ydb/core/ydb_convert/ydb_convert.h>
#include <ydb/core/ydb_convert/table_description.h>

#include <util/generic/algorithm.h>
#include <util/string/builder.h>

namespace NKikimr::NDataShard {

static std::shared_ptr<NTxProxy::TUploadTypes> BuildTypes(const TUserTable& tableInfo, const NKikimrIndexBuilder::TColumnBuildSettings& buildSettings) {
    auto types = GetAllTypes(tableInfo);

    Y_ENSURE(buildSettings.columnSize() > 0);
    auto result = std::make_shared<NTxProxy::TUploadTypes>();
    result->reserve(tableInfo.KeyColumnIds.size() + buildSettings.columnSize());

    for (const auto& keyColId : tableInfo.KeyColumnIds) {
        auto it = tableInfo.Columns.at(keyColId);
        Ydb::Type type;
        NScheme::ProtoFromTypeInfo(it.Type, type);
        result->emplace_back(it.Name, type);
    }
    for (size_t i = 0; i < buildSettings.columnSize(); i++) {
        const auto& column = buildSettings.column(i);
        result->emplace_back(column.GetColumnName(), column.default_from_literal().type());
    }
    return result;
}

static std::shared_ptr<NTxProxy::TUploadTypes> BuildTypes(const TUserTable& tableInfo, TProtoColumnsCRef indexColumns, TProtoColumnsCRef dataColumns) {
    auto types = GetAllTypes(tableInfo);

    auto result = std::make_shared<NTxProxy::TUploadTypes>();
    result->reserve(indexColumns.size() + dataColumns.size());

    for (const auto& colName : indexColumns) {
        Ydb::Type type;
        NScheme::ProtoFromTypeInfo(types.at(colName), type);
        result->emplace_back(colName, type);
    }
    for (const auto& colName : dataColumns) {
        Ydb::Type type;
        NScheme::ProtoFromTypeInfo(types.at(colName), type);
        result->emplace_back(colName, type);
    }
    return result;
}

bool BuildExtraColumns(TVector<TCell>& cells, const NKikimrIndexBuilder::TColumnBuildSettings& buildSettings, TString& err, TMemoryPool& valueDataPool) {
    cells.clear();
    cells.reserve(buildSettings.columnSize());
    for (size_t i = 0; i < buildSettings.columnSize(); i++) {
        const auto& column = buildSettings.column(i);

        NScheme::TTypeInfo typeInfo;
        i32 typeMod = -1;
        Ydb::StatusIds::StatusCode status;

        if (column.default_from_literal().type().has_pg_type()) {
            typeMod = column.default_from_literal().type().pg_type().typmod();
        }

        TString unusedtm;
        if (!ExtractColumnTypeInfo(typeInfo, unusedtm, column.default_from_literal().type(), status, err)) {
            return false;
        }

        auto& back = cells.emplace_back();
        if (!CellFromProtoVal(typeInfo, typeMod, &column.default_from_literal().value(), false, back, err, valueDataPool)) {
            return false;
        }
    }

    return true;
}

template <NKikimrServices::TActivity::EType Activity>
class TBuildScanUpload: public TActor<TBuildScanUpload<Activity>>, public IActorExceptionHandler, public NTable::IScan {
    using TThis = TBuildScanUpload<Activity>;
    using TBase = TActor<TThis>;

protected:
    const TIndexBuildScanSettings ScanSettings;

    const ui64 BuildIndexId;
    const TString TargetTable;
    const TScanRecord::TSeqNo SeqNo;

    const ui64 DataShardId;
    const TActorId ProgressActorId;

    TTags ScanTags;                                             // first: columns we scan, order as in IndexTable
    std::shared_ptr<NTxProxy::TUploadTypes> UploadColumnsTypes; // columns types we upload to indexTable
    NTxProxy::EUploadRowsMode UploadMode;

    const TTags KeyColumnIds;
    const TVector<NScheme::TTypeInfo> KeyTypes;

    const TSerializedTableRange TableRange;
    const TSerializedTableRange RequestedRange;

    IDriver* Driver = nullptr;

    TBufferData ReadBuf;
    TBufferData WriteBuf;
    TSerializedCellVec LastUploadedKey;

    TActorId Uploader;
    ui32 RetryCount = 0;

    TUploadMonStats Stats = TUploadMonStats("tablets", "build_index_upload");
    TUploadStatus UploadStatus;

    TBuildScanUpload(ui64 buildIndexId,
                     const TString& target,
                     const TScanRecord::TSeqNo& seqNo,
                     ui64 dataShardId,
                     const TActorId& progressActorId,
                     const TSerializedTableRange& range,
                     const TUserTable& tableInfo,
                     const TIndexBuildScanSettings& scanSettings)
        : TBase(&TThis::StateWork)
        , ScanSettings(scanSettings)
        , BuildIndexId(buildIndexId)
        , TargetTable(target)
        , SeqNo(seqNo)
        , DataShardId(dataShardId)
        , ProgressActorId(progressActorId)
        , KeyColumnIds(tableInfo.KeyColumnIds)
        , KeyTypes(tableInfo.KeyColumnTypes)
        , TableRange(tableInfo.Range)
        , RequestedRange(range) {
    }

    template <typename TAddRow>
    EScan FeedImpl([[maybe_unused]] TArrayRef<const TCell> key, const TRow& /*row*/, TAddRow&& addRow) {
        // LOG_T("Feed key " << DebugPrintPoint(KeyTypes, key, *AppData()->TypeRegistry) << " " << Debug());

        addRow();

        if (!ReadBuf.HasReachedLimits(ScanSettings)) {
            return EScan::Feed;
        }

        if (!WriteBuf.IsEmpty()) {
            return EScan::Sleep;
        }

        ReadBuf.FlushTo(WriteBuf);

        Upload();

        return EScan::Feed;
    }

public:
    static constexpr auto ActorActivityType() {
        return Activity;
    }

    ~TBuildScanUpload() override = default;

    TInitialState Prepare(IDriver* driver, TIntrusiveConstPtr<TScheme>) override {
        TActivationContext::AsActorContext().RegisterWithSameMailbox(this);

        LOG_I("Prepare " << Debug());

        Driver = driver;

        return {EScan::Feed, {}};
    }

    EScan Seek(TLead& lead, ui64 seq) override {
        LOG_T("Seek no " << seq << " " << Debug());
        if (seq) {
            if (!WriteBuf.IsEmpty()) {
                return EScan::Sleep;
            }

            if (!ReadBuf.IsEmpty()) {
                ReadBuf.FlushTo(WriteBuf);
                Upload();
                return EScan::Sleep;
            }

            if (UploadStatus.IsNone()) {
                UploadStatus.StatusCode = Ydb::StatusIds::SUCCESS;
                UploadStatus.Issues.AddIssue(NYql::TIssue("Shard or requested range is empty"));
            }

            return EScan::Final;
        }

        auto scanRange = Intersect(KeyTypes, RequestedRange.ToTableRange(), TableRange.ToTableRange());

        if (scanRange.From) {
            auto seek = scanRange.InclusiveFrom ? NTable::ESeek::Lower : NTable::ESeek::Upper;
            lead.To(ScanTags, scanRange.From, seek);
        } else {
            lead.To(ScanTags, {}, NTable::ESeek::Lower);
        }

        if (scanRange.To) {
            lead.Until(scanRange.To, scanRange.InclusiveTo);
        }

        return EScan::Feed;
    }

    TAutoPtr<IDestructable> Finish(const std::exception& exc) final
    {
        UploadStatus.Issues.AddIssue(NYql::TIssue(TStringBuilder()
            << "Scan failed " << exc.what()));
        return Finish(EStatus::Exception);
    }

    TAutoPtr<IDestructable> Finish(EStatus status) override {
        if (Uploader) {
            this->Send(Uploader, new TEvents::TEvPoisonPill);
            Uploader = {};
        }

        TAutoPtr<TEvDataShard::TEvBuildIndexProgressResponse> progress = new TEvDataShard::TEvBuildIndexProgressResponse;
        FillScanResponseCommonFields(*progress, BuildIndexId, DataShardId, SeqNo);

        if (status == EStatus::Exception) {
            progress->Record.SetStatus(NKikimrIndexBuilder::EBuildStatus::BUILD_ERROR);
        } else if (status != EStatus::Done) {
            progress->Record.SetStatus(NKikimrIndexBuilder::EBuildStatus::ABORTED);
        } else if (!UploadStatus.IsSuccess()) {
            progress->Record.SetStatus(NKikimrIndexBuilder::EBuildStatus::BUILD_ERROR);
        } else {
            progress->Record.SetStatus(NKikimrIndexBuilder::EBuildStatus::DONE);
        }

        UploadStatusToMessage(progress->Record);

        if (progress->Record.GetStatus() == NKikimrIndexBuilder::DONE) {
            LOG_N("Done " << Debug() << " " << progress->Record.ShortDebugString());
        } else {
            LOG_E("Failed " << Debug() << " " << progress->Record.ShortDebugString());
        }
        this->Send(ProgressActorId, progress.Release());

        Driver = nullptr;
        this->PassAway();
        return nullptr;
    }

    bool OnUnhandledException(const std::exception& exc) override {
        if (!Driver) {
            return false;
        }
        Driver->Throw(exc);
        return true;
    }

    void UploadStatusToMessage(NKikimrTxDataShard::TEvBuildIndexProgressResponse& msg) {
        msg.SetUploadStatus(UploadStatus.StatusCode);
        NYql::IssuesToMessage(UploadStatus.Issues, msg.MutableIssues());
    }

    void Describe(IOutputStream& out) const override {
        out << Debug();
    }

    TString Debug() const {
        return TStringBuilder() << "TBuildIndexScan TabletId: " << DataShardId << " Id: " << BuildIndexId
            << ", requested range: " << DebugPrintRange(KeyTypes, RequestedRange.ToTableRange(), *AppData()->TypeRegistry)
            << ", last acked point: " << DebugPrintPoint(KeyTypes, LastUploadedKey.GetCells(), *AppData()->TypeRegistry)
            << " " << Stats.ToString()
            << " " << UploadStatus.ToString();
    }

    EScan PageFault() override {
        LOG_T("Page fault"
              << " ReadBuf empty: " << ReadBuf.IsEmpty()
              << " WriteBuf empty: " << WriteBuf.IsEmpty()
              << " " << Debug());

        if (!ReadBuf.IsEmpty() && WriteBuf.IsEmpty()) {
            ReadBuf.FlushTo(WriteBuf);
            Upload();
        }

        return EScan::Feed;
    }

private:
    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvTxUserProxy::TEvUploadRowsResponse, Handle);
            CFunc(TEvents::TSystem::Wakeup, HandleWakeup);
            default:
                LOG_E("TBuildIndexScan: StateWork unexpected event type: " << ev->GetTypeRewrite() << " event: " << ev->ToString());
        }
    }

    void HandleWakeup(const NActors::TActorContext& /*ctx*/) {
        LOG_D("Retry upload " << Debug());

        if (!WriteBuf.IsEmpty()) {
            RetryUpload();
        }
    }

    void Handle(TEvTxUserProxy::TEvUploadRowsResponse::TPtr& ev, const TActorContext& ctx) {
        LOG_D("Handle TEvUploadRowsResponse "
              << Debug()
              << " Uploader: " << Uploader.ToString()
              << " ev->Sender: " << ev->Sender.ToString());

        if (Uploader) {
            Y_ENSURE(Uploader == ev->Sender,
                       "Mismatch"
                           << " Uploader: " << Uploader.ToString()
                           << " ev->Sender: " << ev->Sender.ToString());
        } else {
            Y_ENSURE(Driver == nullptr);
            return;
        }

        UploadStatus.StatusCode = ev->Get()->Status;
        UploadStatus.Issues.AddIssues(ev->Get()->Issues);

        if (UploadStatus.IsSuccess()) {
            Stats.Aggr(WriteBuf.GetRows(), WriteBuf.GetRowCellBytes());
            LastUploadedKey = WriteBuf.ExtractLastKey();

            //send progress
            TAutoPtr<TEvDataShard::TEvBuildIndexProgressResponse> progress = new TEvDataShard::TEvBuildIndexProgressResponse;
            FillScanResponseCommonFields(*progress, BuildIndexId, DataShardId, SeqNo);

            // TODO(mbkkt) ReleaseBuffer isn't possible, we use LastUploadedKey for logging
            progress->Record.SetLastKeyAck(LastUploadedKey.GetBuffer());
            progress->Record.SetRowsDelta(WriteBuf.GetRows());
            // TODO: use GetRowCellBytes method?
            progress->Record.SetBytesDelta(WriteBuf.GetBufferBytes());
            WriteBuf.Clear();

            progress->Record.SetStatus(NKikimrIndexBuilder::EBuildStatus::IN_PROGRESS);
            UploadStatusToMessage(progress->Record);

            this->Send(ProgressActorId, progress.Release());

            if (ReadBuf.HasReachedLimits(ScanSettings)) {
                ReadBuf.FlushTo(WriteBuf);
                Upload();
            }

            Driver->Touch(EScan::Feed);
            return;
        }

        if (RetryCount < ScanSettings.GetMaxBatchRetries() && UploadStatus.IsRetriable()) {
            LOG_N("Got retriable error, " << Debug());

            ctx.Schedule(GetRetryWakeupTimeoutBackoff(RetryCount), new TEvents::TEvWakeup());
            return;
        }

        LOG_N("Got error, abort scan, " << Debug());

        Driver->Touch(EScan::Final);
    }

    void RetryUpload() {
        Upload(true);
    }

    void Upload(bool isRetry = false) {
        if (isRetry) {
            ++RetryCount;
        } else {
            RetryCount = 0;
        }

        LOG_D("Upload, last key " << DebugPrintPoint(KeyTypes, WriteBuf.GetLastKey().GetCells(), *AppData()->TypeRegistry) << " " << Debug());

        auto actor = NTxProxy::CreateUploadRowsInternal(
            this->SelfId(), TargetTable,
            UploadColumnsTypes,
            WriteBuf.GetRowsData(),
            UploadMode,
            true /*writeToPrivateTable*/,
            true /*writeToIndexImplTable*/);

        Uploader = this->Register(actor);
    }
};

class TBuildIndexScan final: public TBuildScanUpload<NKikimrServices::TActivity::BUILD_INDEX_SCAN_ACTOR> {
    const ui32 TargetDataColumnPos; // positon of first data column in target table

public:
    TBuildIndexScan(ui64 buildIndexId,
                    const TString& target,
                    const TScanRecord::TSeqNo& seqNo,
                    ui64 dataShardId,
                    const TActorId& progressActorId,
                    const TSerializedTableRange& range,
                    TProtoColumnsCRef targetIndexColumns,
                    TProtoColumnsCRef targetDataColumns,
                    const TUserTable& tableInfo,
                    const TIndexBuildScanSettings& scanSettings)
        : TBuildScanUpload(buildIndexId, target, seqNo, dataShardId, progressActorId, range, tableInfo, scanSettings)
        , TargetDataColumnPos(targetIndexColumns.size()) {
        ScanTags = BuildTags(tableInfo, targetIndexColumns, targetDataColumns);
        UploadColumnsTypes = BuildTypes(tableInfo, targetIndexColumns, targetDataColumns);
        UploadMode = NTxProxy::EUploadRowsMode::WriteToTableShadow;
    }

    EScan Feed(TArrayRef<const TCell> key, const TRow& row) final {
        return FeedImpl(key, row, [&] {
            const auto rowCells = *row;

            ReadBuf.AddRow(
                rowCells.Slice(0, TargetDataColumnPos),
                rowCells.Slice(TargetDataColumnPos),
                key);
        });
    }
};

class TBuildColumnsScan final: public TBuildScanUpload<NKikimrServices::TActivity::BUILD_COLUMNS_SCAN_ACTOR> {
    TVector<TCell> Value;
    TString ValueSerialized;

public:
    TBuildColumnsScan(ui64 buildIndexId,
                      const TString& target,
                      const TScanRecord::TSeqNo& seqNo,
                      ui64 dataShardId,
                      const TActorId& progressActorId,
                      const TSerializedTableRange& range,
                      const NKikimrIndexBuilder::TColumnBuildSettings& columnBuildSettings,
                      const TUserTable& tableInfo,
                      const TIndexBuildScanSettings& scanSettings)
        : TBuildScanUpload(buildIndexId, target, seqNo, dataShardId, progressActorId, range, tableInfo, scanSettings) {
        Y_ENSURE(columnBuildSettings.columnSize() > 0);
        UploadColumnsTypes = BuildTypes(tableInfo, columnBuildSettings);
        UploadMode = NTxProxy::EUploadRowsMode::UpsertIfExists;

        TMemoryPool valueDataPool(256);
        TString err;
        Y_ENSURE(BuildExtraColumns(Value, columnBuildSettings, err, valueDataPool));
        ValueSerialized = TSerializedCellVec::Serialize(Value);
    }

    EScan Feed(TArrayRef<const TCell> key, const TRow& row) final {
        return FeedImpl(key, row, [&] {
            auto valueSerializedCopy = ValueSerialized;
            ReadBuf.AddRow(
                key,
                Value,
                std::move(valueSerializedCopy),
                key);
        });
    }
};

TAutoPtr<NTable::IScan> CreateBuildIndexScan(
    ui64 buildIndexId,
    TString target,
    const TScanRecord::TSeqNo& seqNo,
    ui64 dataShardId,
    const TActorId& progressActorId,
    const TSerializedTableRange& range,
    TProtoColumnsCRef targetIndexColumns,
    TProtoColumnsCRef targetDataColumns,
    const NKikimrIndexBuilder::TColumnBuildSettings& columnsToBuild,
    const TUserTable& tableInfo,
    const TIndexBuildScanSettings& scanSettings) {
    if (columnsToBuild.columnSize() > 0) {
        return new TBuildColumnsScan(
            buildIndexId, target, seqNo, dataShardId, progressActorId, range, columnsToBuild, tableInfo, scanSettings);
    }
    return new TBuildIndexScan(
        buildIndexId, target, seqNo, dataShardId, progressActorId, range, targetIndexColumns, targetDataColumns, tableInfo, scanSettings);
}

class TDataShard::TTxHandleSafeBuildIndexScan: public NTabletFlatExecutor::TTransactionBase<TDataShard> {
public:
    TTxHandleSafeBuildIndexScan(TDataShard* self, TEvDataShard::TEvBuildIndexCreateRequest::TPtr&& ev)
        : TTransactionBase(self)
        , Ev(std::move(ev)) {
    }

    bool Execute(TTransactionContext&, const TActorContext& ctx) {
        Self->HandleSafe(Ev, ctx);
        return true;
    }

    void Complete(const TActorContext&) {
        // nothing
    }

private:
    TEvDataShard::TEvBuildIndexCreateRequest::TPtr Ev;
};

void TDataShard::Handle(TEvDataShard::TEvBuildIndexCreateRequest::TPtr& ev, const TActorContext&) {
    Execute(new TTxHandleSafeBuildIndexScan(this, std::move(ev)));
}

void TDataShard::HandleSafe(TEvDataShard::TEvBuildIndexCreateRequest::TPtr& ev, const TActorContext& ctx) {
    auto& request = ev->Get()->Record;
    const ui64 id = request.GetId();
    TRowVersion rowVersion(request.GetSnapshotStep(), request.GetSnapshotTxId());
    TScanRecord::TSeqNo seqNo = {request.GetSeqNoGeneration(), request.GetSeqNoRound()};

    try {
        auto response = MakeHolder<TEvDataShard::TEvBuildIndexProgressResponse>();
        FillScanResponseCommonFields(*response, request.GetId(), TabletID(), seqNo);

        LOG_N("Starting TBuildIndexScan TabletId: " << TabletID()
            << " " << request.ShortDebugString()
            << " row version " << rowVersion);

        // Note: it's very unlikely that we have volatile txs before this snapshot
        if (VolatileTxManager.HasVolatileTxsAtSnapshot(rowVersion)) {
            VolatileTxManager.AttachWaitingSnapshotEvent(rowVersion, std::unique_ptr<IEventHandle>(ev.Release()));
            return;
        }

        auto badRequest = [&](const TString& error) {
            response->Record.SetStatus(NKikimrIndexBuilder::EBuildStatus::BAD_REQUEST);
            auto issue = response->Record.AddIssues();
            issue->set_severity(NYql::TSeverityIds::S_ERROR);
            issue->set_message(error);
        };
        auto trySendBadRequest = [&] {
            if (response->Record.GetStatus() == NKikimrIndexBuilder::EBuildStatus::BAD_REQUEST) {
                LOG_E("Rejecting TBuildIndexScan bad request TabletId: " << TabletID()
                    << " " << request.ShortDebugString()
                    << " with response " << response->Record.ShortDebugString());
                ctx.Send(ev->Sender, std::move(response));
                return true;
            } else {
                return false;
            }
        };

        // 1. Validating table and path existence
        const auto tableId = TTableId(request.GetOwnerId(), request.GetPathId());
        if (request.GetTabletId() != TabletID()) {
            badRequest(TStringBuilder() << "Wrong shard " << request.GetTabletId() << " this is " << TabletID());
        }
        if (!IsStateActive()) {
            badRequest(TStringBuilder() << "Shard " << TabletID() << " is " << State << " and not ready for requests");
        }
        if (!GetUserTables().contains(tableId.PathId.LocalPathId)) {
            badRequest(TStringBuilder() << "Unknown table id: " << tableId.PathId.LocalPathId);
        }
        if (trySendBadRequest()) {
            return;
        }
        const auto& userTable = *GetUserTables().at(tableId.PathId.LocalPathId);

        // 2. Validating request fields
        if (!request.HasSnapshotStep() || !request.HasSnapshotTxId()) {
            badRequest(TStringBuilder() << "Empty snapshot");
        }
        const TSnapshotKey snapshotKey(tableId.PathId, rowVersion.Step, rowVersion.TxId);
        if (!SnapshotManager.FindAvailable(snapshotKey)) {
            badRequest(TStringBuilder() << "Unknown snapshot for path id " << tableId.PathId.OwnerId << ":" << tableId.PathId.LocalPathId
                << ", snapshot step is " << snapshotKey.Step << ", snapshot tx is " << snapshotKey.TxId);
        }

        TSerializedTableRange requestedRange;
        requestedRange.Load(request.GetKeyRange());
        auto scanRange = Intersect(userTable.KeyColumnTypes, requestedRange.ToTableRange(), userTable.Range.ToTableRange());
        if (scanRange.IsEmptyRange(userTable.KeyColumnTypes)) {
            badRequest(TStringBuilder() << " requested range doesn't intersect with table range"
                << " requestedRange: " << DebugPrintRange(userTable.KeyColumnTypes, requestedRange.ToTableRange(), *AppData()->TypeRegistry)
                << " tableRange: " << DebugPrintRange(userTable.KeyColumnTypes, userTable.Range.ToTableRange(), *AppData()->TypeRegistry)
                << " scanRange: " << DebugPrintRange(userTable.KeyColumnTypes, scanRange, *AppData()->TypeRegistry));
        }

        if (!request.GetTargetName()) {
            badRequest(TStringBuilder() << "Empty target table name");
        }

        auto tags = GetAllTags(userTable);
        for (auto column : request.GetIndexColumns()) {
            if (!tags.contains(column)) {
                badRequest(TStringBuilder() << "Unknown index column: " << column);
            }
        }
        for (auto column : request.GetDataColumns()) {
            if (!tags.contains(column)) {
                badRequest(TStringBuilder() << "Unknown data column: " << column);
            }
        }

        if (trySendBadRequest()) {
            return;
        }

        // 3. Creating scan
        TAutoPtr<NTable::IScan> scan = CreateBuildIndexScan(id,
            request.GetTargetName(),
            seqNo,
            request.GetTabletId(),
            ev->Sender,
            requestedRange,
            request.GetIndexColumns(),
            request.GetDataColumns(),
            request.GetColumnBuildSettings(),
            userTable,
            request.GetScanSettings());

        StartScan(this, std::move(scan), id, seqNo, rowVersion, userTable.LocalTid);
    } catch (const std::exception& exc) {
        FailScan<TEvDataShard::TEvBuildIndexProgressResponse>(id, TabletID(), ev->Sender, seqNo, exc, "TBuildIndexScan");
    }
}

}
