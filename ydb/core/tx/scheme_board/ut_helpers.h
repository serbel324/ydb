#pragma once

#include "defs.h"
#include "events.h"
#include "events_internal.h"
#include "subscriber.h"

#include <ydb/core/base/statestorage_impl.h>
#include <ydb/core/base/tablet_types.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/testlib/basics/runtime/runtime.h>
#include <ydb/core/testlib/basics/state_storage/state_storage.h>

#include <ydb/library/actors/interconnect/interconnect_impl.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/algorithm.h>
#include <util/generic/string.h>

namespace NKikimr {
namespace NSchemeBoard {

void SetupMinimalRuntime(TTestActorRuntime& runtime, const TStateStorageSetupper& setupStateStorage = CreateDefaultStateStorageSetupper());

TIntrusiveConstPtr<TStateStorageInfo> GetStateStorageInfo(TTestActorRuntime& runtime);
TEvStateStorage::TEvResolveReplicasList::TPtr ResolveReplicas(TTestActorRuntime& runtime, const TString& path);

template <typename TEvent>
size_t CountEvents(TTestBasicRuntime& runtime, bool dispatch = true, TActorId recipient = {}) {
    if (dispatch) {
        if (!runtime.DispatchEvents()) {
            return 0;
        }
    } else {
        // is necessary to accumulate sent events
        runtime.SimulateSleep(TDuration::Seconds(1));
    }

    return CountIf(runtime.CaptureEvents(), [=](const TAutoPtr<IEventHandle> ev) {
        return ev->GetTypeRewrite() == TEvent::EventType && (recipient ? ev->Recipient == recipient : true);
    });
}

class TTestContext: public TTestBasicRuntime {
public:
    using TTestBasicRuntime::TTestBasicRuntime;

    void Send(
        const TActorId& recipient,
        const TActorId& sender,
        IEventBase* ev,
        ui32 flags = 0,
        ui64 cookie = 0,
        ui32 senderNodeIndex = 0,
        bool viaActorSystem = false
    ) {
        TTestBasicRuntime::Send(new IEventHandle(recipient, sender, ev, flags, cookie), senderNodeIndex, viaActorSystem);
    }

    void WaitForEvent(ui32 eventType) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(eventType);
        DispatchEvents(options);
    }

    void Connect(ui32 nodeIndexFrom, ui32 nodeIndexTo) {
        const TActorId proxy = GetInterconnectProxy(nodeIndexFrom, nodeIndexTo);

        Send(proxy, TActorId(), new TEvInterconnect::TEvConnectNode(), 0, 0, nodeIndexFrom, true);
        WaitForEvent(TEvInterconnect::EvNodeConnected);
    }

    void Disconnect(ui32 nodeIndexFrom, ui32 nodeIndexTo) {
        const TActorId proxy = GetInterconnectProxy(nodeIndexFrom, nodeIndexTo);

        Send(proxy, TActorId(), new TEvInterconnect::TEvDisconnect(), 0, 0, nodeIndexFrom, true);
        WaitForEvent(TEvInterconnect::EvNodeDisconnected);
    }

    template <typename TEvent>
    size_t CountEdgeEvents() {
        return CountEvents<TEvent>(*this, false);
    }

    NInternalEvents::TEvHandshakeResponse::TPtr HandshakeReplica(
        const TActorId& replica,
        const TActorId& sender,
        ui64 owner = 1,
        ui64 generation = 1,
        bool grabResponse = true
    ) {
        Send(replica, sender, new NInternalEvents::TEvHandshakeRequest(owner, generation));

        if (grabResponse) {
            return GrabEdgeEvent<NInternalEvents::TEvHandshakeResponse>(sender);
        }

        return nullptr;
    }

    void CommitReplica(
        const TActorId& replica,
        const TActorId& sender,
        ui64 owner = 1,
        ui64 generation = 1
    ) {
        Send(replica, sender, new NInternalEvents::TEvCommitRequest(owner, generation));
    }

    template <typename TPath>
    NInternalEvents::TEvNotify::TPtr SubscribeReplica(
        const TActorId& replica,
        const TActorId& sender,
        const TPath& path,
        bool grabResponse = true,
        const ui64 domainOwnerId = 0,
        const NKikimrSchemeBoard::TEvSubscribe::TCapabilities& capabilities = NKikimrSchemeBoard::TEvSubscribe::TCapabilities()
    ) {
        auto subscribe = MakeHolder<NInternalEvents::TEvSubscribe>(path, domainOwnerId);
        subscribe->Record.MutableCapabilities()->CopyFrom(capabilities);

        Send(replica, sender, subscribe.Release());

        if (grabResponse) {
            return GrabEdgeEvent<NInternalEvents::TEvNotify>(sender);
        }

        return nullptr;
    }

    template <typename TPath>
    void UnsubscribeReplica(const TActorId& replica, const TActorId& sender, const TPath& path) {
        Send(replica, sender, new NInternalEvents::TEvUnsubscribe(path));
    }

    template <typename TEvent, typename TPath>
    TActorId CreateSubscriber(
        const TActorId& owner,
        const TPath& path,
        ui64 domainOwnerId = 1,
        bool grabResponse = true,
        ui32 nodeIndex = 0
    ) {
        const TActorId subscriber = Register(
            CreateSchemeBoardSubscriber(owner, path, domainOwnerId), nodeIndex
        );
        EnableScheduleForActor(subscriber, true);

        if (grabResponse) {
            GrabEdgeEvent<TEvent>(owner);
        }

        return subscriber;
    }

    template <typename TPath>
    TActorId CreateSubscriber(
        const TActorId& owner,
        const TPath& path,
        ui64 domainOwnerId = 1,
        ui32 nodeIndex = 0
    ) {
        return CreateSubscriber<NInternalEvents::TEvNotify>(
            owner, path, domainOwnerId, false, nodeIndex
        );
    }

}; // TTestContext

NKikimrScheme::TEvDescribeSchemeResult GenerateDescribe(
    const TString& path,
    TPathId pathId,
    ui64 version = 1,
    TDomainId domainId = TDomainId()
);

NInternalEvents::TEvUpdate* GenerateUpdate(
    const NKikimrScheme::TEvDescribeSchemeResult& describe,
    ui64 owner = 1,
    ui64 generation = 1,
    bool isDeletion = false
);

struct TCombinationsArgs {
    TString Path;
    TPathId PathId;
    ui64 Version;
    TDomainId DomainId;

    ui64 OwnerId;
    ui64 Generation;
    bool IsDeletion;

    NInternalEvents::TEvUpdate* GenerateUpdate() const {
        return ::NKikimr::NSchemeBoard::GenerateUpdate(GenerateDescribe(), OwnerId, Generation, IsDeletion);
    }

    NKikimrScheme::TEvDescribeSchemeResult GenerateDescribe() const {
        return ::NKikimr::NSchemeBoard::GenerateDescribe(Path, PathId, Version, DomainId);
    }

    using TSuperId = std::tuple<TDomainId, bool, TPathId, ui64>;

    TSuperId GetSuperId() const {
        return {DomainId, IsDeletion, PathId, Version};
    }
};

TVector<TCombinationsArgs> GenerateCombinationsDomainRoot(TString path = TString("/Root/Tenant"), ui64 gssOwnerID = 800, TVector<ui64> tenantsOwners = TVector<ui64>{900, 910});
TVector<TCombinationsArgs> GenerateCombinationsMigratedPath(TString path,
                                                            ui64 gssID, TVector<ui64> tssIDs,
                                                            ui64 gssLocalPathId, ui64 tssLocalPathId);

} // NSchemeBoard
} // NKikimr
