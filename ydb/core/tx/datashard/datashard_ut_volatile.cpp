#include <ydb/core/tx/datashard/ut_common/datashard_ut_common.h>
#include "datashard_ut_common_kqp.h"
#include "datashard_ut_common_pq.h"
#include "datashard_active_transaction.h"

#include <ydb/core/base/blobstorage.h>
#include <ydb/core/kqp/executer_actor/kqp_executer.h>
#include <ydb/core/testlib/actors/block_events.h>

namespace NKikimr {

using namespace NKikimr::NDataShard;
using namespace NKikimr::NDataShard::NKqpHelpers;
using namespace NSchemeShard;
using namespace Tests;

Y_UNIT_TEST_SUITE(DataShardVolatile) {

    Y_UNIT_TEST(DistributedWrite) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        Cerr << "!!! distributed write begin" << Endl;
        ExecSQL(server, sender, R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )");
        Cerr << "!!! distributed write end" << Endl;

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } }");

        const auto shards1 = GetTableShards(server, sender, "/Root/table-1");
        RebootTablet(runtime, shards1.at(0), sender);

        // We should see same results after restart
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } }");
    }

    Y_UNIT_TEST(DistributedWriteBrokenLock) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        // Start transaction that reads from table-1 and table-2
        TString sessionId, txId;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleBegin(runtime, sessionId, txId, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
            )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }");

        // Break lock using a blind write to table-1
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);");

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleCommit(runtime, sessionId, txId, R"(
                UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 3);
                UPSERT INTO `/Root/table-2` (key, value) VALUES (30, 30);
            )"),
            "ERROR: ABORTED");

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Verify transaction was not committed
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }");
    }

    Y_UNIT_TEST_TWIN(DistributedWriteShardRestartBeforePlan, UseSink) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetAppConfig(appConfig);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        TVector<THolder<IEventHandle>> capturedPlans;
        auto capturePlans = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvPlanStep::EventType: {
                    Cerr << "... captured TEvPlanStep" << Endl;
                    capturedPlans.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(capturePlans);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        Cerr << "!!! distributed write begin" << Endl;
        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedPlans.size() >= 2; }, "both captured plans");

        runtime.SetObserverFunc(prevObserverFunc);

        // Restart the first table shard
        auto shards1 = GetTableShards(server, sender, "/Root/table-1");
        RebootTablet(runtime, shards1.at(0), sender);

        // Unblock captured plan messages
        for (auto& ev : capturedPlans) {
            runtime.Send(ev.Release(), 0, true);
        }
        capturedPlans.clear();

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(future))),
            "ERROR: UNDETERMINED");
        Cerr << "!!! distributed write end" << Endl;

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Verify transaction was not committed
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }");
    }

    Y_UNIT_TEST_TWIN(DistributedWriteShardRestartAfterExpectation, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        auto shard1 = GetTableShards(server, sender, "/Root/table-1").at(0);

        TVector<THolder<IEventHandle>> capturedPlans;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvPlanStep::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvPlanStep>();
                    if (msg->Record.GetTabletID() == shard1) {
                        Cerr << "... captured TEvPlanStep for " << msg->Record.GetTabletID() << Endl;
                        capturedPlans.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    if (msg->Record.GetTabletDest() == shard1) {
                        Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                        capturedReadSets.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        Cerr << "!!! distributed write begin" << Endl;
        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedPlans.size() >= 1 && capturedReadSets.size() >= 2; }, "captured plan and readsets");

        runtime.SetObserverFunc(prevObserverFunc);

        // Restart the first table shard
        RebootTablet(runtime, shard1, sender);

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(future))),
            "ERROR: UNDETERMINED");
        Cerr << "!!! distributed write end" << Endl;

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Verify transaction was not committed
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }");
    }

    Y_UNIT_TEST(DistributedWriteEarlierSnapshotNotBlocked) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        TString sessionIdSnapshot, txIdSnapshot;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleBegin(runtime, sessionIdSnapshot, txIdSnapshot, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
            )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }");

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest()
                        << " with flags " << msg->Record.GetFlags() << Endl;
                    capturedReadSets.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        Cerr << "!!! distributed write begin" << Endl;
        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");

        runtime.SetObserverFunc(prevObserverFunc);
        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Make sure snapshot transaction cannot see uncommitted changes and doesn't block on them
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleContinue(runtime, sessionIdSnapshot, txIdSnapshot, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
            )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }");
    }

    Y_UNIT_TEST_TWIN(DistributedWriteLaterSnapshotBlockedThenCommit, UseSink) {
        TPortManager pm;
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest()
                        << " with flags " << msg->Record.GetFlags() << Endl;
                    capturedReadSets.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        Cerr << "!!! distributed write begin" << Endl;
        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");

        runtime.SetObserverFunc(prevObserverFunc);
        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        TString sessionIdSnapshot = CreateSessionRPC(runtime, "/Root");
        auto snapshotReadFuture = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            SELECT key, value FROM `/Root/table-1`
            UNION ALL
            SELECT key, value FROM `/Root/table-2`
            ORDER BY key
        )", sessionIdSnapshot, "", false /* commitTx */), "/Root");

        // Let some virtual time pass
        SimulateSleep(runtime, TDuration::Seconds(1));

        // Read should be blocked, so we don't expect a reply
        UNIT_ASSERT(!snapshotReadFuture.HasValue());

        // Unblock readsets and sleep some more
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }
        SimulateSleep(runtime, TDuration::Seconds(1));

        // We expect successful commit and read including that data
        UNIT_ASSERT(snapshotReadFuture.HasValue());
        UNIT_ASSERT(future.HasValue());
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(future.ExtractValue()),
            "<empty>");
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(snapshotReadFuture.ExtractValue()),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } }");
    }

    Y_UNIT_TEST(DistributedWriteLaterSnapshotBlockedThenAbort) {
        TPortManager pm;
        NKikimrConfig::TAppConfig app;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        const auto shard1 = GetTableShards(server, sender, "/Root/table-1").at(0);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        size_t observedPlans = 0;
        TVector<THolder<IEventHandle>> capturedPlans;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvPlanStep::EventType: {
                    ++observedPlans;
                    const auto* msg = ev->Get<TEvTxProcessing::TEvPlanStep>();
                    if (msg->Record.GetTabletID() == shard1) {
                        Cerr << "... captured TEvPlanStep for " << msg->Record.GetTabletID() << Endl;
                        capturedPlans.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        Cerr << "!!! distributed write begin" << Endl;
        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return observedPlans >= 2; }, "observed both plans");
        UNIT_ASSERT_VALUES_EQUAL(capturedPlans.size(), 1u);

        runtime.SetObserverFunc(prevObserverFunc);
        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Start reading from table-2
        TString sessionIdSnapshot = CreateSessionRPC(runtime, "/Root");
        auto snapshotReadFuture = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            SELECT key, value FROM `/Root/table-2`
            ORDER BY key
        )", sessionIdSnapshot, "", false /* commitTx */), "/Root");

        // Let some virtual time pass
        SimulateSleep(runtime, TDuration::Seconds(1));

        // Read should be blocked, so we don't expect a reply
        UNIT_ASSERT(!snapshotReadFuture.HasValue());
        UNIT_ASSERT(!future.HasValue());

        // Reboot table-1 tablet and sleep a little, this will abort the write
        RebootTablet(runtime, shard1, sender);
        SimulateSleep(runtime, TDuration::Seconds(1));

        // We expect aborted commit and read without that data
        UNIT_ASSERT(snapshotReadFuture.HasValue());
        UNIT_ASSERT(future.HasValue());
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(future.ExtractValue()),
            "ERROR: UNDETERMINED");
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(snapshotReadFuture.ExtractValue()),
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }");
    }

    Y_UNIT_TEST(DistributedWriteAsymmetricExecute) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        const auto shard1 = GetTableShards(server, sender, "/Root/table-1").at(0);

        size_t observedPlans = 0;
        TVector<THolder<IEventHandle>> capturedPlans;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvPlanStep::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvPlanStep>();
                    ++observedPlans;
                    if (msg->Record.GetTabletID() == shard1) {
                        Cerr << "... captured TEvPlanStep for " << msg->Record.GetTabletID() << Endl;
                        capturedPlans.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return observedPlans >= 2; }, "observed plans");
        UNIT_ASSERT_VALUES_EQUAL(capturedPlans.size(), 1u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Wait until it completes at shard2
        SimulateSleep(runtime, TDuration::Seconds(1));

        // Unblock plan at shard1
        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : capturedPlans) {
            runtime.Send(ev.Release(), 0, true);
        }

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(future))),
            "<empty>");
        Cerr << "!!! distributed write end" << Endl;
    }

    Y_UNIT_TEST(DistributedWriteThenDropTable) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        size_t observedPropose = 0;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvDataShard::TEvProposeTransaction::EventType: {
                    ++observedPropose;
                    Cerr << "... observed TEvProposeTransaction" << Endl;
                    break;
                }
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                    capturedReadSets.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        observedPropose = 0;
        ui64 txId = AsyncDropTable(server, sender, "/Root", "table-1");
        WaitFor(runtime, [&]{ return observedPropose > 0; }, "observed propose");

        SimulateSleep(runtime, TDuration::Seconds(1));

        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        WaitTxNotification(server, sender, txId);
    }

    Y_UNIT_TEST(DistributedWriteThenImmediateUpsert) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        auto opts = TShardedTableOptions()
                        .Columns({
                            {"key", "Uint32", true, false},
                            {"value", "Uint32", false, false},
                            {"value2", "Uint32", false, false}});
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        const auto shard1 = GetTableShards(server, sender, "/Root/table-1").at(0);
        const auto tableId1 = ResolveTableId(server, sender, "/Root/table-1");

        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                    capturedReadSets.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value, value2) VALUES (2, 2, 42);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Note: this upsert happens over the upsert into the value column
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value2) VALUES (2, 51);");

        // This compaction verifies there's no commit race with the waiting
        // distributed transaction. If commits happen in incorrect order we
        // would observe unexpected results.
        CompactTable(runtime, shard1, tableId1, false);

        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(future))),
            "<empty>");

        // Verify the result
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value, value2 FROM `/Root/table-1`
                UNION ALL
                SELECT key, value, value2 FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } items { null_flag_value: NULL_VALUE } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } items { uint32_value: 51 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } items { null_flag_value: NULL_VALUE } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } items { null_flag_value: NULL_VALUE } }");
    }

    Y_UNIT_TEST(DistributedWriteThenCopyTable) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        size_t observedPropose = 0;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvDataShard::TEvProposeTransaction::EventType: {
                    ++observedPropose;
                    Cerr << "... observed TEvProposeTransaction" << Endl;
                    break;
                }
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                    capturedReadSets.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        observedPropose = 0;
        ui64 txId = AsyncCreateCopyTable(server, sender, "/Root", "table-1-copy", "/Root/table-1");
        WaitFor(runtime, [&]{ return observedPropose > 0; }, "observed propose");

        SimulateSleep(runtime, TDuration::Seconds(1));

        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        // Wait for copy table to finish
        WaitTxNotification(server, sender, txId);

        // Verify table copy has above changes committed
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1-copy`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }");
    }

    Y_UNIT_TEST(DistributedWriteThenSplit) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        size_t observedSplit = 0;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvDataShard::TEvSplit::EventType: {
                    ++observedSplit;
                    Cerr << "... observed TEvSplit" << Endl;
                    break;
                }
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                    capturedReadSets.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        SetSplitMergePartCountLimit(server->GetRuntime(), -1);
        auto shards1before = GetTableShards(server, sender, "/Root/table-1");
        ui64 txId = AsyncSplitTable(server, sender, "/Root/table-1", shards1before.at(0), 2);
        WaitFor(runtime, [&]{ return observedSplit > 0; }, "observed split");

        SimulateSleep(runtime, TDuration::Seconds(1));

        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        // Wait for split to finish
        WaitTxNotification(server, sender, txId);

        // Verify table has changes committed
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }");
    }

    Y_UNIT_TEST(DistributedWriteThenReadIterator) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        auto opts = TShardedTableOptions()
                        .Columns({
                            {"key", "Uint32", true, false},
                            {"value", "Uint32", false, false},
                            {"value2", "Uint32", false, false}});
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        ui64 maxReadSetStep = 0;
        bool captureReadSets = true;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    maxReadSetStep = Max(maxReadSetStep, msg->Record.GetStep());
                    if (captureReadSets) {
                        Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                        capturedReadSets.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value, value2) VALUES (2, 2, 42);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        const auto shard1 = GetTableShards(server, sender, "/Root/table-1").at(0);
        const auto tableId1 = ResolveTableId(server, sender, "/Root/table-1");

        // Note: observer works strangely with edge actor results, so we use a normal actor here
        TVector<THolder<IEventHandle>> readResults;
        auto readSender = runtime.Register(new TLambdaActor([&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvDataShard::TEvReadResult::EventType: {
                    Cerr << "... observed TEvReadResult:" << Endl;
                    Cerr << ev->Get<TEvDataShard::TEvReadResult>()->Record.DebugString() << Endl;
                    readResults.emplace_back(ev.Release());
                    break;
                }
                default: {
                    Cerr << "... ignore event " << ev->GetTypeRewrite() << Endl;
                }
            }
        }));

        {
            auto msg = std::make_unique<TEvDataShard::TEvRead>();
            msg->Record.SetReadId(1);
            msg->Record.MutableTableId()->SetOwnerId(tableId1.PathId.OwnerId);
            msg->Record.MutableTableId()->SetTableId(tableId1.PathId.LocalPathId);
            msg->Record.MutableTableId()->SetSchemaVersion(tableId1.SchemaVersion);
            msg->Record.MutableSnapshot()->SetStep(maxReadSetStep);
            msg->Record.MutableSnapshot()->SetTxId(Max<ui64>());
            msg->Record.AddColumns(1);
            msg->Record.AddColumns(2);
            msg->Record.SetResultFormat(NKikimrDataEvents::FORMAT_ARROW);

            TVector<TCell> fromKeyCells = { TCell::Make(ui32(0)) };
            TVector<TCell> toKeyCells = { TCell::Make(ui32(10)) };
            auto fromBuf = TSerializedCellVec::Serialize(fromKeyCells);
            auto toBuf = TSerializedCellVec::Serialize(toKeyCells);
            msg->Ranges.emplace_back(fromBuf, toBuf, true, true);

            ForwardToTablet(runtime, shard1, readSender, msg.release());
        }

        // Since key=2 is not committed we must not observe results yet
        SimulateSleep(runtime, TDuration::Seconds(2));
        UNIT_ASSERT_VALUES_EQUAL(readResults.size(), 0u);

        captureReadSets = false;
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        WaitFor(runtime, [&]{ return readResults.size() > 0; }, "read result");
        UNIT_ASSERT_VALUES_EQUAL(readResults.size(), 1u);

        {
            auto* msg = readResults[0]->Get<TEvDataShard::TEvReadResult>();
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetStatus().GetCode(), Ydb::StatusIds::SUCCESS);
            UNIT_ASSERT_VALUES_EQUAL(msg->GetArrowBatch()->ToString(),
                "key:   [\n"
                "    1,\n"
                "    2\n"
                "  ]\n"
                "value:   [\n"
                "    1,\n"
                "    2\n"
                "  ]\n");
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetFinished(), true);
        }
    }

    Y_UNIT_TEST(DistributedWriteThenReadIteratorStream) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        auto opts = TShardedTableOptions()
                        .Columns({
                            {"key", "Uint32", true, false},
                            {"value", "Uint32", false, false},
                            {"value2", "Uint32", false, false}});
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        ui64 maxReadSetStep = 0;
        bool captureReadSets = true;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    maxReadSetStep = Max(maxReadSetStep, msg->Record.GetStep());
                    if (captureReadSets) {
                        Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                        capturedReadSets.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value, value2) VALUES (2, 2, 42);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        const auto shard1 = GetTableShards(server, sender, "/Root/table-1").at(0);
        const auto tableId1 = ResolveTableId(server, sender, "/Root/table-1");

        // Note: observer works strangely with edge actor results, so we use a normal actor here
        TVector<THolder<IEventHandle>> readResults;
        auto readSender = runtime.Register(new TLambdaActor([&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvDataShard::TEvReadResult::EventType: {
                    Cerr << "... observed TEvReadResult:" << Endl;
                    Cerr << ev->Get<TEvDataShard::TEvReadResult>()->Record.DebugString() << Endl;
                    readResults.emplace_back(ev.Release());
                    break;
                }
                default: {
                    Cerr << "... ignore event " << ev->GetTypeRewrite() << Endl;
                }
            }
        }));

        {
            auto msg = std::make_unique<TEvDataShard::TEvRead>();
            msg->Record.SetReadId(1);
            msg->Record.MutableTableId()->SetOwnerId(tableId1.PathId.OwnerId);
            msg->Record.MutableTableId()->SetTableId(tableId1.PathId.LocalPathId);
            msg->Record.MutableTableId()->SetSchemaVersion(tableId1.SchemaVersion);
            msg->Record.MutableSnapshot()->SetStep(maxReadSetStep);
            msg->Record.MutableSnapshot()->SetTxId(Max<ui64>());
            msg->Record.AddColumns(1);
            msg->Record.AddColumns(2);
            msg->Record.SetResultFormat(NKikimrDataEvents::FORMAT_ARROW);
            msg->Record.SetMaxRowsInResult(1);

            TVector<TCell> fromKeyCells = { TCell::Make(ui32(0)) };
            TVector<TCell> toKeyCells = { TCell::Make(ui32(10)) };
            auto fromBuf = TSerializedCellVec::Serialize(fromKeyCells);
            auto toBuf = TSerializedCellVec::Serialize(toKeyCells);
            msg->Ranges.emplace_back(fromBuf, toBuf, true, true);

            ForwardToTablet(runtime, shard1, readSender, msg.release());
        }

        // We expect to receive key=1 as soon as possible since it's committed
        // However further data should not be available so soon
        SimulateSleep(runtime, TDuration::Seconds(2));
        UNIT_ASSERT_VALUES_EQUAL(readResults.size(), 1u);

        // Verify we actually receive key=1
        {
            auto* msg = readResults[0]->Get<TEvDataShard::TEvReadResult>();
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetStatus().GetCode(), Ydb::StatusIds::SUCCESS);
            UNIT_ASSERT_VALUES_EQUAL(msg->GetArrowBatch()->ToString(),
                "key:   [\n"
                "    1\n"
                "  ]\n"
                "value:   [\n"
                "    1\n"
                "  ]\n");
            readResults.clear();
        }

        // Unblock readsets and let key=2 to commit
        captureReadSets = false;
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        WaitFor(runtime, [&]{ return readResults.size() > 0; }, "read result");
        UNIT_ASSERT_GE(readResults.size(), 1u);

        {
            auto* msg = readResults[0]->Get<TEvDataShard::TEvReadResult>();
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetStatus().GetCode(), Ydb::StatusIds::SUCCESS);
            UNIT_ASSERT_VALUES_EQUAL(msg->GetArrowBatch()->ToString(),
                "key:   [\n"
                "    2\n"
                "  ]\n"
                "value:   [\n"
                "    2\n"
                "  ]\n");
        }

        SimulateSleep(runtime, TDuration::MilliSeconds(0));

        {
            auto* msg = readResults.back()->Get<TEvDataShard::TEvReadResult>();
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetStatus().GetCode(), Ydb::StatusIds::SUCCESS);
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetFinished(), true);
        }
    }

    Y_UNIT_TEST(DistributedWriteThenScanQuery) {
        TPortManager pm;
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableKqpScanQuerySourceRead(false);
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::KQP_COMPUTE, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        TShardedTableOptions opts;
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        ui64 maxReadSetStep = 0;
        bool captureReadSets = true;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    maxReadSetStep = Max(maxReadSetStep, msg->Record.GetStep());
                    if (captureReadSets) {
                        Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                        capturedReadSets.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        std::vector<TString> observedResults;
        TMaybe<Ydb::StatusIds::StatusCode> observedStatus;
        auto scanSender = runtime.Register(new TLambdaActor([&](TAutoPtr<IEventHandle>& ev, const TActorContext& ctx) {
            switch (ev->GetTypeRewrite()) {
                case NKqp::TEvKqpExecuter::TEvStreamData::EventType: {
                    auto* msg = ev->Get<NKqp::TEvKqpExecuter::TEvStreamData>();
                    Cerr << "... observed stream data" << Endl;
                    if (msg->Record.GetResultSet().rows().size()) {
                        observedResults.emplace_back(FormatResult(msg->Record.GetResultSet()));
                    }
                    auto resp = MakeHolder<NKqp::TEvKqpExecuter::TEvStreamDataAck>(msg->Record.GetSeqNo(), msg->Record.GetChannelId());
                    resp->Record.SetFreeSpace(1);
                    ctx.Send(ev->Sender, resp.Release());
                    break;
                }
                case NKqp::TEvKqp::TEvQueryResponse::EventType: {
                    auto* msg = ev->Get<NKqp::TEvKqp::TEvQueryResponse>();
                    Cerr << "... observed query result" << Endl;
                    observedStatus = msg->Record.GetYdbStatus();
                    break;
                }
                default: {
                    Cerr << "... ignored event " << ev->GetTypeRewrite();
                    if (ev->HasEvent()) {
                        Cerr << " " << ev->GetTypeName();
                    }
                    Cerr << Endl;
                }
            }
        }));

        SendRequest(runtime, scanSender, MakeStreamRequest(scanSender, R"(
            SELECT key, value FROM `/Root/table-1`
            ORDER BY key;
        )"));

        SimulateSleep(runtime, TDuration::Seconds(2));

        UNIT_ASSERT_VALUES_EQUAL(observedResults.size(), 0u);

        captureReadSets = false;
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        SimulateSleep(runtime, TDuration::Seconds(2));

        UNIT_ASSERT_VALUES_EQUAL(observedResults.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(
            observedResults[0],
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }");
        UNIT_ASSERT_VALUES_EQUAL(observedStatus, Ydb::StatusIds::SUCCESS);
    }

    Y_UNIT_TEST(DistributedWriteWithAsyncIndex) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        auto opts = TShardedTableOptions()
            .Indexes({
                {"by_value", {"value"}, {}, NKikimrSchemeOp::EIndexTypeGlobalAsync},
            });
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);
        ExecSQL(server, sender, R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 3);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 30);
        )");
        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Make sure changes are actually delivered
        SimulateSleep(runtime, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleStaleRoExec(runtime, R"(
                SELECT key, value
                FROM `/Root/table-1` VIEW by_value
                ORDER BY key
            )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 3 } }");

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleStaleRoExec(runtime, R"(
                SELECT key, value
                FROM `/Root/table-2` VIEW by_value
                ORDER BY key
            )"),
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 30 } }");
    }

    Y_UNIT_TEST(DistributedWriteThenBulkUpsert) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        auto opts = TShardedTableOptions()
                        .Columns({
                            {"key", "Uint32", true, false},
                            {"value", "Uint32", false, false},
                            {"value2", "Uint32", false, false}});
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        ui64 maxReadSetStep = 0;
        bool captureReadSets = true;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    maxReadSetStep = Max(maxReadSetStep, msg->Record.GetStep());
                    if (captureReadSets) {
                        Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                        capturedReadSets.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value, value2) VALUES (2, 2, 42);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Write to key 2 using bulk upsert
        NThreading::TFuture<Ydb::Table::BulkUpsertResponse> bulkUpsertFuture;
        {
            Ydb::Table::BulkUpsertRequest request;
            request.set_table("/Root/table-1");
            auto* r = request.mutable_rows();

            auto* reqRowType = r->mutable_type()->mutable_list_type()->mutable_item()->mutable_struct_type();
            auto* reqKeyType = reqRowType->add_members();
            reqKeyType->set_name("key");
            reqKeyType->mutable_type()->set_type_id(Ydb::Type::UINT32);
            auto* reqValueType = reqRowType->add_members();
            reqValueType->set_name("value");
            reqValueType->mutable_type()->set_type_id(Ydb::Type::UINT32);

            auto* reqRows = r->mutable_value();
            auto* row1 = reqRows->add_items();
            row1->add_items()->set_uint32_value(2);
            row1->add_items()->set_uint32_value(22);

            using TEvBulkUpsertRequest = NKikimr::NGRpcService::TGrpcRequestOperationCall<
                Ydb::Table::BulkUpsertRequest, Ydb::Table::BulkUpsertResponse>;
            bulkUpsertFuture = NRpcService::DoLocalRpc<TEvBulkUpsertRequest>(
                std::move(request), "/Root", "", runtime.GetActorSystem(0));

            auto response = AwaitResponse(runtime, std::move(bulkUpsertFuture));
            UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        }

        // This compaction verifies there's no commit race with the waiting
        // distributed transaction. If commits happen in incorrect order we
        // would observe unexpected results.
        const auto shard1 = GetTableShards(server, sender, "/Root/table-1").at(0);
        const auto tableId1 = ResolveTableId(server, sender, "/Root/table-1");
        CompactTable(runtime, shard1, tableId1, false);

        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(future))),
            "<empty>");

        // Verify the result
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value, value2 FROM `/Root/table-1`
                UNION ALL
                SELECT key, value, value2 FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } items { null_flag_value: NULL_VALUE } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 22 } items { uint32_value: 42 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } items { null_flag_value: NULL_VALUE } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } items { null_flag_value: NULL_VALUE } }");
    }

    namespace {
        using TCdcStream = TShardedTableOptions::TCdcStream;

        TCdcStream NewAndOldImages(NKikimrSchemeOp::ECdcStreamFormat format, const TString& name = "Stream") {
            return TCdcStream{
                .Name = name,
                .Mode = NKikimrSchemeOp::ECdcStreamModeNewAndOldImages,
                .Format = format,
            };
        }

        TCdcStream WithInitialScan(TCdcStream streamDesc) {
            streamDesc.InitialState = NKikimrSchemeOp::ECdcStreamStateScan;
            return streamDesc;
        }

        void WaitForContent(TServer::TPtr server, const TActorId& sender, const TString& path, const TVector<TString>& expected) {
            while (true) {
                const auto records = GetPqRecords(*server->GetRuntime(), sender, path, 0);
                if (records.size() >= expected.size()) {
                    for (ui32 i = 0; i < expected.size(); ++i) {
                        UNIT_ASSERT_VALUES_EQUAL(records.at(i).second, expected.at(i));
                    }

                    UNIT_ASSERT_VALUES_EQUAL(records.size(), expected.size());
                    break;
                }

                SimulateSleep(server, TDuration::Seconds(1));
            }
        }
    } // namespace

    Y_UNIT_TEST(DistributedWriteThenBulkUpsertWithCdc) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetEnableChangefeedInitialScan(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        auto opts = TShardedTableOptions()
                        .Columns({
                            {"key", "Uint32", true, false},
                            {"value", "Uint32", false, false},
                            {"value2", "Uint32", false, false}});
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        WaitTxNotification(server, sender, AsyncAlterAddStream(server, "/Root", "table-1",
            WithInitialScan(NewAndOldImages(NKikimrSchemeOp::ECdcStreamFormatJson))));

        WaitForContent(server, sender, "/Root/table-1/Stream", {
            R"({"update":{},"newImage":{"value2":null,"value":1},"key":[1]})",
        });

        ui64 maxReadSetStep = 0;
        bool captureReadSets = true;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    maxReadSetStep = Max(maxReadSetStep, msg->Record.GetStep());
                    if (captureReadSets) {
                        Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                        capturedReadSets.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        runtime.GetAppData(0).FeatureFlags.SetEnableDataShardVolatileTransactions(true);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value, value2) VALUES (2, 2, 42);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", true /* commitTx */), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        runtime.GetAppData(0).FeatureFlags.ClearEnableDataShardVolatileTransactions();

        // Write to key 2 using bulk upsert
        NThreading::TFuture<Ydb::Table::BulkUpsertResponse> bulkUpsertFuture;
        {
            Ydb::Table::BulkUpsertRequest request;
            request.set_table("/Root/table-1");
            auto* r = request.mutable_rows();

            auto* reqRowType = r->mutable_type()->mutable_list_type()->mutable_item()->mutable_struct_type();
            auto* reqKeyType = reqRowType->add_members();
            reqKeyType->set_name("key");
            reqKeyType->mutable_type()->set_type_id(Ydb::Type::UINT32);
            auto* reqValueType = reqRowType->add_members();
            reqValueType->set_name("value");
            reqValueType->mutable_type()->set_type_id(Ydb::Type::UINT32);

            auto* reqRows = r->mutable_value();
            auto* row1 = reqRows->add_items();
            row1->add_items()->set_uint32_value(2);
            row1->add_items()->set_uint32_value(22);

            using TEvBulkUpsertRequest = NKikimr::NGRpcService::TGrpcRequestOperationCall<
                Ydb::Table::BulkUpsertRequest, Ydb::Table::BulkUpsertResponse>;
            bulkUpsertFuture = NRpcService::DoLocalRpc<TEvBulkUpsertRequest>(
                std::move(request), "/Root", "", runtime.GetActorSystem(0));

            // Note: we expect bulk upsert to block until key 2 outcome is decided
            SimulateSleep(runtime, TDuration::Seconds(1));
            UNIT_ASSERT(!bulkUpsertFuture.HasValue());
        }

        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : capturedReadSets) {
            runtime.Send(ev.Release(), 0, true);
        }

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(future))),
            "<empty>");

        SimulateSleep(runtime, TDuration::Seconds(1));

        // Verify the result
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value, value2 FROM `/Root/table-1`
                UNION ALL
                SELECT key, value, value2 FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } items { null_flag_value: NULL_VALUE } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 22 } items { uint32_value: 42 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } items { null_flag_value: NULL_VALUE } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } items { null_flag_value: NULL_VALUE } }");

        WaitForContent(server, sender, "/Root/table-1/Stream", {
            R"({"update":{},"newImage":{"value2":null,"value":1},"key":[1]})",
            R"({"update":{},"newImage":{"value2":42,"value":2},"key":[2]})",
            R"({"update":{},"newImage":{"value2":42,"value":22},"key":[2],"oldImage":{"value2":42,"value":2}})",
        });

        UNIT_ASSERT(bulkUpsertFuture.HasValue());
    }

    Y_UNIT_TEST(DistributedWriteThenLateWriteReadCommit) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig appCfg;
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetAppConfig(appCfg)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        TShardedTableOptions opts;
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (10, 10);");

        ui64 maxReadSetStep = 0;
        bool captureReadSets = true;
        TVector<THolder<IEventHandle>> capturedReadSets;
        auto captureEvents = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvReadSet::EventType: {
                    const auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    maxReadSetStep = Max(maxReadSetStep, msg->Record.GetStep());
                    if (captureReadSets) {
                        Cerr << "... captured TEvReadSet for " << msg->Record.GetTabletDest() << Endl;
                        capturedReadSets.emplace_back(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", /* commitTx */ true), "/Root");

        WaitFor(runtime, [&]{ return capturedReadSets.size() >= 4; }, "captured readsets");
        UNIT_ASSERT_VALUES_EQUAL(capturedReadSets.size(), 4u);

        // Make an uncommitted write and read it to make sure it's flushed to datashard
        TString sessionId2 = CreateSessionRPC(runtime, "/Root");
        TString txId2;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleBegin(runtime, sessionId2, txId2, R"(
                UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 22);
            )"),
            "<empty>");
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleContinue(runtime, sessionId2, txId2, R"(
                SELECT key, value FROM `/Root/table-1` WHERE key = 2;
            )"),
            "{ items { uint32_value: 2 } items { uint32_value: 22 } }");

        // Unblock readsets and let it commit
        runtime.SetObserverFunc(prevObserverFunc);
        for (auto& ev : std::exchange(capturedReadSets, {})) {
            runtime.Send(ev.Release(), 0, true);
        }

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(future))),
            "<empty>");

        // Commit the transaction, it should succeed without crashing
        UNIT_ASSERT_VALUES_EQUAL(
            CommitTransactionRPC(runtime, sessionId2, txId2),
            "");

        // Verify the result
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 22 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } }");
    }

    Y_UNIT_TEST(DistributedWriteLostPlanThenDrop) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        const auto shards1 = GetTableShards(server, sender, "/Root/table-1");
        UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);

        bool removeTransactions = true;
        size_t removedTransactions = 0;
        size_t receivedReadSets = 0;
        auto observer = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvPlanStep::EventType: {
                    auto* msg = ev->Get<TEvTxProcessing::TEvPlanStep>();
                    auto step = msg->Record.GetStep();
                    auto tabletId = msg->Record.GetTabletID();
                    auto recipient = ev->GetRecipientRewrite();
                    Cerr << "... observed step " << step << " at tablet " << tabletId << Endl;
                    if (removeTransactions && tabletId == shards1.at(0)) {
                        THashMap<TActorId, TVector<ui64>> acks;
                        for (auto& tx : msg->Record.GetTransactions()) {
                            // Acknowledge transaction to coordinator
                            auto ackTo = ActorIdFromProto(tx.GetAckTo());
                            acks[ackTo].push_back(tx.GetTxId());
                            ++removedTransactions;
                        }
                        // Acknowledge transactions to coordinator and remove them
                        // It would be as if shard missed them for some reason
                        for (auto& pr : acks) {
                            auto* ack = new TEvTxProcessing::TEvPlanStepAck(tabletId, step, pr.second.begin(), pr.second.end());
                            runtime.Send(new IEventHandle(pr.first, recipient, ack), 0, true);
                        }
                        auto* accept = new TEvTxProcessing::TEvPlanStepAccepted(tabletId, step);
                        runtime.Send(new IEventHandle(ev->Sender, recipient, accept), 0, true);
                        msg->Record.ClearTransactions();
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
                case TEvTxProcessing::TEvReadSet::EventType: {
                    auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    auto tabletId = msg->Record.GetTabletDest();
                    Cerr << "... observed readset at " << tabletId << Endl;
                    if (tabletId == shards1.at(0)) {
                        ++receivedReadSets;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(observer);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", /* commitTx */ true), "/Root");

        WaitFor(runtime, [&]{ return removedTransactions > 0 && receivedReadSets >= 2; }, "readset exchange start");
        UNIT_ASSERT_VALUES_EQUAL(removedTransactions, 1u);
        UNIT_ASSERT_VALUES_EQUAL(receivedReadSets, 2u);

        removeTransactions = false;

        auto dropStartTs = runtime.GetCurrentTime();
        Cerr << "... dropping table" << Endl;
        ui64 txId = AsyncDropTable(server, sender, "/Root", "table-2");
        Cerr << "... drop table txId# " << txId << " started" << Endl;
        WaitTxNotification(server, sender, txId);
        auto dropLatency = runtime.GetCurrentTime() - dropStartTs;
        Cerr << "... drop finished in " << dropLatency << Endl;
        UNIT_ASSERT(dropLatency < TDuration::Seconds(5));
    }

    Y_UNIT_TEST(DistributedWriteLostPlanThenSplit) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        SetSplitMergePartCountLimit(server->GetRuntime(), -1);
        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        const auto shards1 = GetTableShards(server, sender, "/Root/table-1");
        UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);

        bool removeTransactions = true;
        size_t removedTransactions = 0;
        size_t receivedReadSets = 0;
        auto observer = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvTxProcessing::TEvPlanStep::EventType: {
                    auto* msg = ev->Get<TEvTxProcessing::TEvPlanStep>();
                    auto step = msg->Record.GetStep();
                    auto tabletId = msg->Record.GetTabletID();
                    auto recipient = ev->GetRecipientRewrite();
                    Cerr << "... observed step " << step << " at tablet " << tabletId << Endl;
                    if (removeTransactions && tabletId == shards1.at(0)) {
                        THashMap<TActorId, TVector<ui64>> acks;
                        for (auto& tx : msg->Record.GetTransactions()) {
                            // Acknowledge transaction to coordinator
                            auto ackTo = ActorIdFromProto(tx.GetAckTo());
                            acks[ackTo].push_back(tx.GetTxId());
                            ++removedTransactions;
                        }
                        // Acknowledge transactions to coordinator and remove them
                        // It would be as if shard missed them for some reason
                        for (auto& pr : acks) {
                            auto* ack = new TEvTxProcessing::TEvPlanStepAck(tabletId, step, pr.second.begin(), pr.second.end());
                            runtime.Send(new IEventHandle(pr.first, recipient, ack), 0, true);
                        }
                        auto* accept = new TEvTxProcessing::TEvPlanStepAccepted(tabletId, step);
                        runtime.Send(new IEventHandle(ev->Sender, recipient, accept), 0, true);
                        msg->Record.ClearTransactions();
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
                case TEvTxProcessing::TEvReadSet::EventType: {
                    auto* msg = ev->Get<TEvTxProcessing::TEvReadSet>();
                    auto tabletId = msg->Record.GetTabletDest();
                    Cerr << "... observed readset at " << tabletId << Endl;
                    if (tabletId == shards1.at(0)) {
                        ++receivedReadSets;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        auto prevObserverFunc = runtime.SetObserverFunc(observer);

        TString sessionId = CreateSessionRPC(runtime, "/Root");

        auto future = SendRequest(runtime, MakeSimpleRequestRPC(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1), (2, 2);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (20, 20);
        )", sessionId, "", /* commitTx */ true), "/Root");

        WaitFor(runtime, [&]{ return removedTransactions > 0 && receivedReadSets >= 2; }, "readset exchange start");
        UNIT_ASSERT_VALUES_EQUAL(removedTransactions, 1u);
        UNIT_ASSERT_VALUES_EQUAL(receivedReadSets, 2u);

        removeTransactions = false;

        auto splitStartTs = runtime.GetCurrentTime();
        Cerr << "... splitting table" << Endl;
        ui64 txId = AsyncSplitTable(server, sender, "/Root/table-1", shards1.at(0), 2);
        Cerr << "... split txId# " << txId << " started" << Endl;
        WaitTxNotification(server, sender, txId);
        auto splitLatency = runtime.GetCurrentTime() - splitStartTs;
        Cerr << "... split finished in " << splitLatency << Endl;
        UNIT_ASSERT(splitLatency < TDuration::Seconds(5));
    }

    Y_UNIT_TEST(DistributedOutOfOrderFollowerConsistency) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetNodeCount(1)
            .SetUseRealThreads(false)
            .SetEnableForceFollowers(true)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::TABLET_RESOLVER, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::STATESTORAGE, NLog::PRI_TRACE);

        InitRoot(server, sender);

        auto opts = TShardedTableOptions().Followers(1);
        CreateShardedTable(server, sender, "/Root", "table-1", opts);
        CreateShardedTable(server, sender, "/Root", "table-2", opts);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 2);");

        // Let followers catch up
        runtime.SimulateSleep(TDuration::Seconds(1));

        // Block readset exchange
        std::vector<std::unique_ptr<IEventHandle>> readSets;
        auto blockReadSets = runtime.AddObserver<TEvTxProcessing::TEvReadSet>([&](TEvTxProcessing::TEvReadSet::TPtr& ev) {
            readSets.emplace_back(ev.Release());
        });

        // Start a distributed write to both tables
        TString sessionId = CreateSessionRPC(runtime, "/Root");
        auto upsertResult = SendRequest(
            runtime,
            MakeSimpleRequestRPC(R"(
                UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 3);
                UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 4);
                )", sessionId, /* txId */ "", /* commitTx */ true),
            "/Root");
        WaitFor(runtime, [&]{ return readSets.size() >= 4; }, "readsets");

        // Stop blocking further readsets
        blockReadSets.Remove();

        // Start another distributed write to both tables, it should succeed
        ExecSQL(server, sender, R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 5);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (6, 6);
        )");

        // Let followers catch up
        runtime.SimulateSleep(TDuration::Seconds(1));
        for (ui64 shard : GetTableShards(server, sender, "/Root/table-1")) {
            InvalidateTabletResolverCache(runtime, shard);
        }
        for (ui64 shard : GetTableShards(server, sender, "/Root/table-2")) {
            InvalidateTabletResolverCache(runtime, shard);
        }

        // Check tables, they shouldn't see inconsistent results with the latest write
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleStaleRoExec(runtime, Q_(R"(
                SELECT key, value
                FROM `/Root/table-1`
                ORDER BY key
                )"), "/Root"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }");
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleStaleRoExec(runtime, Q_(R"(
                SELECT key, value
                FROM `/Root/table-2`
                ORDER BY key
                )"), "/Root"),
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }");

        // Unblock readsets
        for (auto& ev : readSets) {
            ui32 nodeIndex = ev->GetRecipientRewrite().NodeId() - runtime.GetNodeId(0);
            runtime.Send(ev.release(), nodeIndex, true);
        }
        readSets.clear();

        // Let followers catch up
        runtime.SimulateSleep(TDuration::Seconds(1));

        // Check tables again, they should have all rows visible now
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleStaleRoExec(runtime, Q_(R"(
                SELECT key, value
                FROM `/Root/table-1`
                ORDER BY key
                )")),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 3 } items { uint32_value: 3 } }, "
            "{ items { uint32_value: 5 } items { uint32_value: 5 } }");
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleStaleRoExec(runtime, Q_(R"(
                SELECT key, value
                FROM `/Root/table-2`
                ORDER BY key
                )")),
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
            "{ items { uint32_value: 4 } items { uint32_value: 4 } }, "
            "{ items { uint32_value: 6 } items { uint32_value: 6 } }");
    }

    // Regression test for KIKIMR-21060
    Y_UNIT_TEST(DistributedWriteRSNotAckedBeforeCommit) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 10);");
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 20);");

        // Block readset exchange
        std::vector<std::unique_ptr<IEventHandle>> readSets;
        auto blockReadSets = runtime.AddObserver<TEvTxProcessing::TEvReadSet>([&](TEvTxProcessing::TEvReadSet::TPtr& ev) {
            Cerr << "... blocking readset" << Endl;
            readSets.emplace_back(ev.Release());
        });

        // Start a distributed write to both tables
        TString sessionId = CreateSessionRPC(runtime, "/Root");
        auto upsertResult = SendRequest(
            runtime,
            MakeSimpleRequestRPC(R"(
                UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 30);
                UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 40);
                )", sessionId, /* txId */ "", /* commitTx */ true),
            "/Root");
        WaitFor(runtime, [&]{ return readSets.size() >= 4; }, "readsets");

        // Stop blocking further readsets
        blockReadSets.Remove();

        // Sleep a little to make sure everything so far is fully committed
        runtime.SimulateSleep(TDuration::Seconds(1));

        // Start blocking commits for table-1
        const auto shards1 = GetTableShards(server, sender, "/Root/table-1");
        UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);
        std::vector<std::unique_ptr<IEventHandle>> putResponses;
        auto blockCommits = runtime.AddObserver<TEvBlobStorage::TEvPut>([&](TEvBlobStorage::TEvPut::TPtr& ev) {
            auto* msg = ev->Get();
            // Drop all put requests for table-1
            if (msg->Id.TabletID() == shards1.at(0)) {
                // We can't just drop requests, we must reply to it later
                putResponses.emplace_back(new IEventHandle(
                    ev->Sender,
                    ev->GetRecipientRewrite(),
                    msg->MakeErrorResponse(NKikimrProto::BLOCKED, "Fake blocked response", TGroupId::Zero()).release(),
                    0,
                    ev->Cookie));
                Cerr << "... dropping put " << msg->Id << Endl;
                ev.Reset();
            }
        });

        // Unblock readsets
        for (auto& ev : readSets) {
            runtime.Send(ev.release(), 0, true);
        }
        readSets.clear();

        // Sleep to make sure those readsets are fully processed
        // Bug was acknowledging readsets before tx state is fully persisted
        runtime.SimulateSleep(TDuration::Seconds(1));

        // Transaction will return success even when commits are blocked at this point
        Cerr << "... awaiting upsert result" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(upsertResult))),
            "<empty>");

        // Now we stop blocking commits and gracefully restart the tablet, all pending commits will be lost
        blockCommits.Remove();
        for (auto& ev : putResponses) {
            runtime.Send(ev.release(), 0, true);
        }
        Cerr << "... restarting tablet " << shards1.at(0) << Endl;
        GracefulRestartTablet(runtime, shards1.at(0), sender);

        // We must see all rows as committed, i.e. nothing should be lost
        Cerr << "... reading final result" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 20 } }, "
            "{ items { uint32_value: 3 } items { uint32_value: 30 } }, "
            "{ items { uint32_value: 4 } items { uint32_value: 40 } }");
    }

    Y_UNIT_TEST_TWIN(TwoAppendsMustBeVolatile, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetNodeCount(2)
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(100)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        // Insert initial values
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 10);"));
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 20);"));

        size_t volatileTxs = 0;
        auto proposeObserver = runtime.AddObserver<TEvDataShard::TEvProposeTransaction>([&](TEvDataShard::TEvProposeTransaction::TPtr& ev) {
            auto* msg = ev->Get();
            if (msg->Record.GetFlags() & TTxFlags::VolatilePrepare) {
                ++volatileTxs;
            }
        });
        auto proposeObserverEvWrite = runtime.AddObserver<NKikimr::NEvents::TDataEvents::TEvWrite>([&](NKikimr::NEvents::TDataEvents::TEvWrite::TPtr& ev) {
            auto* msg = ev->Get();
            if (msg->Record.GetTxMode() == NKikimrDataEvents::TEvWrite::MODE_VOLATILE_PREPARE) {
                ++volatileTxs;
            }
        });

        // This simulates a jepsen transaction that appends two values at different shards
        TString sessionId, txId;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleBegin(runtime, sessionId, txId, Q_(R"(
                $next_index = (
                    SELECT COALESCE(MAX(key) + 1u, 0u)
                    FROM (
                        SELECT key FROM `/Root/table-1`
                        UNION ALL
                        SELECT key FROM `/Root/table-2`
                    )
                );
                UPSERT INTO `/Root/table-1` (key, value) VALUES ($next_index, 30u);
                )")),
            "<empty>");

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleCommit(runtime, sessionId, txId, Q_(R"(
                $next_index = (
                    SELECT COALESCE(MAX(key) + 1u, 0u)
                    FROM (
                        SELECT key FROM `/Root/table-1`
                        UNION ALL
                        SELECT key FROM `/Root/table-2`
                    )
                );
                UPSERT INTO `/Root/table-2` (key, value) VALUES ($next_index, 40u);
                )")),
            "<empty>");

        // There should have been volatile transactions at both shards
        UNIT_ASSERT_VALUES_EQUAL(volatileTxs, 2u);
    }

    // Regression test for KIKIMR-21156
    Y_UNIT_TEST_TWIN(VolatileCommitOnBlobStorageFailure, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TDisableDataShardLogBatching disableDataShardLogBatching;
        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        // Make sure read flags are persisted by performing a snapshot read
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table-1`
                UNION ALL
                SELECT key, value FROM `/Root/table-2`
                ORDER BY key
                )"),
            "");

        // Insert initial values
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 10);"));
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 20);"));

        // Start blocking commits for table-1
        const auto shards1 = GetTableShards(server, sender, "/Root/table-1");
        UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);
        std::deque<TEvBlobStorage::TEvPut::TPtr> blockedPuts;
        auto blockCommits = runtime.AddObserver<TEvBlobStorage::TEvPut>([&](TEvBlobStorage::TEvPut::TPtr& ev) {
            auto* msg = ev->Get();
            // Drop all put requests for table-1
            if (msg->Id.TabletID() == shards1.at(0)) {
                Cerr << "... blocking put " << msg->Id << Endl;
                blockedPuts.push_back(std::move(ev));
            }
        });

        // Start an upsert to table-1, this will block further readonly localdb tx completions
        Cerr << "... starting an upsert to table-1" << Endl;
        auto firstUpsertFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 30);
            )");

        // Wait until puts are blocked
        WaitFor(runtime, [&]{ return blockedPuts.size() > 0; }, "blocked puts");
        auto firstUpsertPuts = std::move(blockedPuts);
        UNIT_ASSERT(blockedPuts.empty());

        // Read from table-2 and write to table-1 based on the result
        // This will result in a two-shard volatile tx writing to table-1
        Cerr << "... starting distributed tx between table-1 and table-2" << Endl;
        auto volatileFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table-1`
                SELECT key + 2u AS key, value + 2u AS value
                FROM `/Root/table-2`;
            )");

        // Wait until it also tries to commit
        WaitFor(runtime, [&]{ return blockedPuts.size() > 0; }, "blocked puts");

        // Now unblock the first upsert puts
        blockCommits.Remove();
        for (auto& ev : firstUpsertPuts) {
            runtime.Send(ev.Release(), 0, true);
        }
        firstUpsertPuts.clear();

        // And wait for it to finish successfully
        Cerr << "... waiting for first upsert result" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(firstUpsertFuture))),
            "<empty>");

        // Reply to everything previously blocked with an error, the shard will restart
        for (auto& ev : blockedPuts) {
            auto proxy = ev->Recipient;
            ui32 groupId = GroupIDFromBlobStorageProxyID(proxy);
            auto res = ev->Get()->MakeErrorResponse(NKikimrProto::ERROR, "Something went wrong", TGroupId::FromValue(groupId));
            runtime.Send(new IEventHandle(ev->Sender, proxy, res.release()), 0, true);
        }

        // Wait for the volatile tx result
        Cerr << "... waiting for volatile tx result" << Endl;
        auto result = FormatResult(AwaitResponse(runtime, std::move(volatileFuture)));
        if (result == "<empty>") {
            // A success result is not ok now, but in the future we might migrate state
            // Check that the supposedly committed row actually exists
            UNIT_ASSERT_VALUES_EQUAL(
                KqpSimpleExec(runtime, R"(
                    SELECT key, value FROM `/Root/table-1` ORDER BY key;
                    )"),
                "{ items { uint32_value: 1 } items { uint32_value: 10 } }, "
                "{ items { uint32_value: 3 } items { uint32_value: 30 } }, "
                "{ items { uint32_value: 4 } items { uint32_value: 22 } }");
        } else {
            // Otherwise the result must be undetermined
            UNIT_ASSERT_VALUES_EQUAL(result, "ERROR: UNDETERMINED");
        }
    }

    Y_UNIT_TEST(VolatileTxAbortedOnSplit) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TDisableDataShardLogBatching disableDataShardLogBatching;
        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        // Insert initial values
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 10);"));
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 20);"));

        // Block propose to coordinator
        std::vector<TEvTxProxy::TEvProposeTransaction::TPtr> coordinatorProposes;
        auto blockCoordinatorProposes = runtime.AddObserver<TEvTxProxy::TEvProposeTransaction>(
            [&](TEvTxProxy::TEvProposeTransaction::TPtr& ev) {
                Cerr << "... blocked propose to coordinator" << Endl;
                coordinatorProposes.emplace_back(ev.Release());
            });

        auto upsertStartTs = runtime.GetCurrentTime();
        auto upsertFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 30);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 40);
        )");

        WaitFor(runtime, [&]{ return coordinatorProposes.size() > 0; }, "coordinator propose");
        UNIT_ASSERT_VALUES_EQUAL(coordinatorProposes.size(), 1u);
        blockCoordinatorProposes.Remove();
        coordinatorProposes.clear();

        auto splitStartTs = runtime.GetCurrentTime();
        Cerr << "... splitting table" << Endl;
        SetSplitMergePartCountLimit(server->GetRuntime(), -1);
        auto shards1before = GetTableShards(server, sender, "/Root/table-1");
        ui64 txId = AsyncSplitTable(server, sender, "/Root/table-1", shards1before.at(0), 3);
        Cerr << "... split txId# " << txId << " started" << Endl;
        WaitTxNotification(server, sender, txId);
        auto splitLatency = runtime.GetCurrentTime() - splitStartTs;
        Cerr << "... split finished in " << splitLatency << Endl;
        UNIT_ASSERT_C(splitLatency < TDuration::Seconds(5), "split latency: " << splitLatency);

        Cerr << "... waiting for upsert result" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(upsertFuture))),
            "ERROR: ABORTED");
        auto upsertLatency = runtime.GetCurrentTime() - upsertStartTs;
        Cerr << "... upsert finished in " << upsertLatency << Endl;
        UNIT_ASSERT_C(upsertLatency < TDuration::Seconds(5), "upsert latency: " << upsertLatency);
    }

    Y_UNIT_TEST(VolatileTxAbortedOnDrop) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetDomainPlanResolution(1000)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TDisableDataShardLogBatching disableDataShardLogBatching;
        CreateShardedTable(server, sender, "/Root", "table-1", 1);
        CreateShardedTable(server, sender, "/Root", "table-2", 1);

        // Insert initial values
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 10);"));
        ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 20);"));

        // Block propose to coordinator
        std::vector<TEvTxProxy::TEvProposeTransaction::TPtr> coordinatorProposes;
        auto blockCoordinatorProposes = runtime.AddObserver<TEvTxProxy::TEvProposeTransaction>(
            [&](TEvTxProxy::TEvProposeTransaction::TPtr& ev) {
                Cerr << "... blocked propose to coordinator" << Endl;
                coordinatorProposes.emplace_back(ev.Release());
            });

        auto upsertStartTs = runtime.GetCurrentTime();
        auto upsertFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 30);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 40);
        )");

        WaitFor(runtime, [&]{ return coordinatorProposes.size() > 0; }, "coordinator propose");
        UNIT_ASSERT_VALUES_EQUAL(coordinatorProposes.size(), 1u);
        blockCoordinatorProposes.Remove();
        coordinatorProposes.clear();

        auto dropStartTs = runtime.GetCurrentTime();
        Cerr << "... dropping table" << Endl;
        ui64 txId = AsyncDropTable(server, sender, "/Root", "table-1");
        WaitTxNotification(server, sender, txId);
        auto dropLatency = runtime.GetCurrentTime() - dropStartTs;
        Cerr << "... drop finished in " << dropLatency << Endl;
        UNIT_ASSERT_C(dropLatency < TDuration::Seconds(5), "drop latency: " << dropLatency);

        Cerr << "... waiting for upsert result" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(upsertFuture))),
            "ERROR: ABORTED");
        auto upsertLatency = runtime.GetCurrentTime() - upsertStartTs;
        Cerr << "... upsert finished in " << upsertLatency << Endl;
        UNIT_ASSERT_C(upsertLatency < TDuration::Seconds(5), "upsert latency: " << upsertLatency);
    }

    class TForceVolatileProposeArbiter {
    public:
        TForceVolatileProposeArbiter(TTestActorRuntime& runtime, ui64 arbiterShard)
            : ArbiterShard(arbiterShard)
            , Observer(runtime.AddObserver<TEvDataShard::TEvProposeTransaction>(
                [this](auto& ev) {
                    this->OnEvent(ev);
                }))
            , ObserverEvWrite(runtime.AddObserver<NKikimr::NEvents::TDataEvents::TEvWrite>(
                [this](auto& ev) {
                    this->OnEvent(ev);
                }))
        {}

        void Remove() {
            Observer.Remove();
            ObserverEvWrite.Remove();
        }

    private:
        void OnEvent(TEvDataShard::TEvProposeTransaction::TPtr& ev) {
            auto* msg = ev->Get();
            int kind = msg->Record.GetTxKind();
            if (kind != NKikimrTxDataShard::TX_KIND_DATA) {
                Cerr << "... skipping TEvProposeTransaction with kind " << kind
                    << " (expected " << int(NKikimrTxDataShard::TX_KIND_DATA) << ")"
                    << Endl;
                return;
            }

            ui32 flags = msg->Record.GetFlags();
            if (!(flags & TTxFlags::VolatilePrepare)) {
                Cerr << "... skipping TEvProposeTransaction with flags " << flags
                    << " (missing VolatilePrepare flag)"
                    << Endl;
                return;
            }

            NKikimrTxDataShard::TDataTransaction tx;
            bool ok = tx.ParseFromString(msg->Record.GetTxBody());
            Y_ENSURE(ok, "Failed to parse data transaction");
            if (!tx.HasKqpTransaction()) {
                Cerr << "... skipping TEvProposeTransaction without kqp transaction" << Endl;
                return;
            }

            auto* kqpTx = tx.MutableKqpTransaction();

            int kqpType = kqpTx->GetType();
            if (kqpType != NKikimrTxDataShard::KQP_TX_TYPE_DATA) {
                Cerr << "... skipping TEvProposeTransaction with kqp type " << kqpType
                    << " (expected " << int(NKikimrTxDataShard::KQP_TX_TYPE_DATA) << ")"
                    << Endl;
                return;
            }

            if (!kqpTx->HasLocks()) {
                Cerr << "... skipping TEvProposeTransaction without locks" << Endl;
                return;
            }

            auto* kqpLocks = kqpTx->MutableLocks();
            const auto& sendingShards = kqpLocks->GetSendingShards();
            const auto& receivingShards = kqpLocks->GetReceivingShards();

            if (std::find(sendingShards.begin(), sendingShards.end(), ArbiterShard) == sendingShards.end()) {
                Cerr << "... skipping TEvProposeTransaction without " << ArbiterShard << " in sending shards" << Endl;
                return;
            }

            if (std::find(receivingShards.begin(), receivingShards.end(), ArbiterShard) == receivingShards.end()) {
                Cerr << "... skipping TEvProposeTransaction without " << ArbiterShard << " in receiving shards" << Endl;
                return;
            }

            kqpLocks->SetArbiterShard(ArbiterShard);
            ok = tx.SerializeToString(msg->Record.MutableTxBody());
            Y_ENSURE(ok, "Failed to serialize data transaction");
            ++Modified;
        }

        void OnEvent(NKikimr::NEvents::TDataEvents::TEvWrite::TPtr& ev) {
            auto* msg = ev->Get();

            if (msg->Record.GetTxMode() != NKikimrDataEvents::TEvWrite::MODE_VOLATILE_PREPARE) {
                Cerr << "... skipping TEvWrite with TxMode=" << NKikimrDataEvents::TEvWrite_ETxMode_Name(msg->Record.GetTxMode())
                    << "(expected " << NKikimrDataEvents::TEvWrite_ETxMode_Name(NKikimrDataEvents::TEvWrite::MODE_VOLATILE_PREPARE) << ")"<< Endl;
                return;
            }

            if (!msg->Record.HasLocks()) {
                Cerr << "... skipping TEvWrite without locks" << Endl;
                return;
            }

            auto* kqpLocks = msg->Record.MutableLocks();
            const auto& sendingShards = kqpLocks->GetSendingShards();
            const auto& receivingShards = kqpLocks->GetReceivingShards();

            if (std::find(sendingShards.begin(), sendingShards.end(), ArbiterShard) == sendingShards.end()) {
                Cerr << "... skipping TEvWrite without " << ArbiterShard << " in sending shards" << Endl;
                return;
            }

            if (std::find(receivingShards.begin(), receivingShards.end(), ArbiterShard) == receivingShards.end()) {
                Cerr << "... skipping TEvWrite without " << ArbiterShard << " in receiving shards" << Endl;
                return;
            }

            kqpLocks->SetArbiterShard(ArbiterShard);
            ++Modified;
        }

    public:
        size_t Modified = 0;

    private:
        const ui64 ArbiterShard;
        TTestActorRuntime::TEventObserverHolder Observer;
        TTestActorRuntime::TEventObserverHolder ObserverEvWrite;
    };

    class TForceBrokenLock {
    public:
        TForceBrokenLock(TTestActorRuntime& runtime, const TTableId& tableId, ui64 shard)
            : TableId(tableId)
            , Shard(shard)
            , ShardActor(ResolveTablet(runtime, shard))
            , Observer(runtime.AddObserver<TEvDataShard::TEvProposeTransaction>(
                [this](auto& ev) {
                    this->OnEvent(ev);
                }))
            , ObserverEvWrite(runtime.AddObserver<NKikimr::NEvents::TDataEvents::TEvWrite>(
                [this](auto& ev) {
                    this->OnEvent(ev);
                }))
        {}

    private:
        void OnEvent(TEvDataShard::TEvProposeTransaction::TPtr& ev) {
            if (ev->GetRecipientRewrite() != ShardActor) {
                return;
            }

            auto* msg = ev->Get();
            int kind = msg->Record.GetTxKind();
            if (kind != NKikimrTxDataShard::TX_KIND_DATA) {
                Cerr << "... skipping TEvProposeTransaction with kind " << kind
                    << " (expected " << int(NKikimrTxDataShard::TX_KIND_DATA) << ")"
                    << Endl;
                return;
            }

            ui32 flags = msg->Record.GetFlags();
            if (!(flags & TTxFlags::VolatilePrepare)) {
                Cerr << "... skipping TEvProposeTransaction with flags " << flags
                    << " (missing VolatilePrepare flag)"
                    << Endl;
                return;
            }

            NKikimrTxDataShard::TDataTransaction tx;
            bool ok = tx.ParseFromString(msg->Record.GetTxBody());
            Y_ENSURE(ok, "Failed to parse data transaction");
            if (!tx.HasKqpTransaction()) {
                Cerr << "... skipping TEvProposeTransaction without kqp transaction" << Endl;
                return;
            }

            auto* kqpTx = tx.MutableKqpTransaction();

            int kqpType = kqpTx->GetType();
            if (kqpType != NKikimrTxDataShard::KQP_TX_TYPE_DATA) {
                Cerr << "... skipping TEvProposeTransaction with kqp type " << kqpType
                    << " (expected " << int(NKikimrTxDataShard::KQP_TX_TYPE_DATA) << ")"
                    << Endl;
                return;
            }

            if (!kqpTx->HasLocks()) {
                Cerr << "... skipping TEvProposeTransaction without locks" << Endl;
                return;
            }

            auto* kqpLocks = kqpTx->MutableLocks();

            // We use a lock that should have never existed to simulate a broken lock
            auto* kqpLock = kqpLocks->AddLocks();
            kqpLock->SetLockId(msg->Record.GetTxId());
            kqpLock->SetDataShard(Shard);
            kqpLock->SetGeneration(1);
            kqpLock->SetCounter(1);
            kqpLock->SetSchemeShard(TableId.PathId.OwnerId);
            kqpLock->SetPathId(TableId.PathId.LocalPathId);

            ok = tx.SerializeToString(msg->Record.MutableTxBody());
            Y_ENSURE(ok, "Failed to serialize data transaction");
            ++Modified;
        }

        void OnEvent(NKikimr::NEvents::TDataEvents::TEvWrite::TPtr& ev) {
            if (ev->GetRecipientRewrite() != ShardActor) {
                return;
            }

            auto* msg = ev->Get();

            if (msg->Record.GetTxMode() != NKikimrDataEvents::TEvWrite::MODE_VOLATILE_PREPARE) {
                Cerr << "... skipping TEvWrite with TxMode=" << NKikimrDataEvents::TEvWrite_ETxMode_Name(msg->Record.GetTxMode())
                    << "(expected " << NKikimrDataEvents::TEvWrite_ETxMode_Name(NKikimrDataEvents::TEvWrite::MODE_VOLATILE_PREPARE) << ")"<< Endl;
                return;
            }

            if (!msg->Record.HasLocks()) {
                Cerr << "... skipping TEvWrite without locks" << Endl;
                return;
            }

            auto* kqpLocks = msg->Record.MutableLocks();

            // We use a lock that should have never existed to simulate a broken lock
            auto* kqpLock = kqpLocks->AddLocks();
            kqpLock->SetLockId(msg->Record.GetTxId());
            kqpLock->SetDataShard(Shard);
            kqpLock->SetGeneration(1);
            kqpLock->SetCounter(1);
            kqpLock->SetSchemeShard(TableId.PathId.OwnerId);
            kqpLock->SetPathId(TableId.PathId.LocalPathId);

            ++Modified;
        }

    public:
        size_t Modified = 0;

    private:
        const TTableId TableId;
        const ui64 Shard;
        const TActorId ShardActor;
        TTestActorRuntime::TEventObserverHolder Observer;
        TTestActorRuntime::TEventObserverHolder ObserverEvWrite;
    };

    class TBlockReadSets : public std::vector<TEvTxProcessing::TEvReadSet::TPtr> {
    public:
        TBlockReadSets(TTestActorRuntime& runtime)
            : Runtime(runtime)
            , Observer(runtime.AddObserver<TEvTxProcessing::TEvReadSet>(
                [this](auto& ev) {
                    this->OnEvent(ev);
                }))
        {}

        void Remove() {
            Observer.Remove();
            clear();
        }

        void Unblock() {
            Observer.Remove();
            for (auto& ev : *this) {
                Runtime.Send(ev.Release(), 0, true);
            }
            clear();
        }

    private:
        void OnEvent(TEvTxProcessing::TEvReadSet::TPtr& ev) {
            push_back(std::move(ev));
        }

    private:
        TTestActorRuntime& Runtime;
        TTestActorRuntime::TEventObserverHolder Observer;
    };

    class TCountReadSets {
    public:
        TCountReadSets(TTestActorRuntime& runtime)
            : Observer(runtime.AddObserver<TEvTxProcessing::TEvReadSet>(
                [this](auto& ev) {
                    this->OnEvent(ev);
                }))
        {}

    private:
        void OnEvent(TEvTxProcessing::TEvReadSet::TPtr&) {
            ++Count;
        }

    public:
        size_t Count = 0;

    private:
        TTestActorRuntime::TEventObserverHolder Observer;
    };

    Y_UNIT_TEST_TWIN(UpsertNoLocksArbiter, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);

        InitRoot(server, sender);

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key int, value int, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10, 20, 30));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 4u);

        TForceVolatileProposeArbiter forceArbiter(runtime, shards.at(0));
        TCountReadSets countReadSets(runtime);

        Cerr << "========= Starting upsert =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value)
                VALUES (1, 11), (11, 111), (21, 211), (31, 311);
            )"),
            "<empty>");

        // arbiter will send 6 readsets (3 decisions + 3 expectations)
        // shards will send 2 readsets each (decision + expectation)
        UNIT_ASSERT_VALUES_EQUAL(countReadSets.Count, 6 + 3 * 2);

        UNIT_ASSERT_VALUES_EQUAL(forceArbiter.Modified, 4u);
        forceArbiter.Remove();

        Cerr << "========= Checking table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table`
                ORDER BY key;
            )"),
            "{ items { int32_value: 1 } items { int32_value: 11 } }, "
            "{ items { int32_value: 11 } items { int32_value: 111 } }, "
            "{ items { int32_value: 21 } items { int32_value: 211 } }, "
            "{ items { int32_value: 31 } items { int32_value: 311 } }");
    }

    Y_UNIT_TEST_TWIN(UpsertBrokenLockArbiter, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);

        InitRoot(server, sender);

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key int, value int, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10, 20, 30));
            )"),
            "SUCCESS");

        const auto tableId = ResolveTableId(server, sender, "/Root/table");
        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 4u);

        for (size_t i = 0; i < shards.size(); ++i) {
            TForceVolatileProposeArbiter forceArbiter(runtime, shards.at(0));
            TForceBrokenLock forceBrokenLock(runtime, tableId, shards.at(i));
            TCountReadSets countReadSets(runtime);

            int key = 1 + i;
            int value = key * 10 + key;
            TString query = Sprintf(
                R"(UPSERT INTO `/Root/table` (key, value) VALUES (%d, %d), (%d, %d), (%d, %d), (%d, %d);)",
                key, value,
                key + 10, value + 100,
                key + 20, value + 200,
                key + 30, value + 300);

            Cerr << "========= Starting query " << query << " =========" << Endl;
            UNIT_ASSERT_VALUES_EQUAL(
                KqpSimpleExec(runtime, query),
                "ERROR: ABORTED");

            // Note: kqp stops gathering responses early on abort, allow everyone to settle
            runtime.SimulateSleep(TDuration::MilliSeconds(1));

            // arbiter will send 3 or 6 readsets (3 aborts or 3 decisions + 3 expectations)
            // shards will send 1 or 2 readsets each (abort or decision + expectation)
            size_t expectedReadSets = (i == 0 ? 3 + 3 * 2 : 6 + 2 * 2 + 1);
            UNIT_ASSERT_VALUES_EQUAL(countReadSets.Count, expectedReadSets);

            UNIT_ASSERT_VALUES_EQUAL(forceBrokenLock.Modified, 1u);
            UNIT_ASSERT_VALUES_EQUAL(forceArbiter.Modified, 4u);
        }

        Cerr << "========= Checking table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table`
                ORDER BY key;
            )"),
            "");
    }

    Y_UNIT_TEST_TWIN(UpsertNoLocksArbiterRestart, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key int, value int, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10, 20, 30));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 4u);

        TForceVolatileProposeArbiter forceArbiter(runtime, shards.at(0));
        TBlockReadSets blockedReadSets(runtime);

        Cerr << "========= Starting upsert =========" << Endl;
        auto upsertFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value)
            VALUES (1, 11), (11, 111), (21, 211), (31, 311);
            )");

        // arbiter will send 3 expectations
        // shards will send 1 commit decision + 1 expectation
        size_t expectedReadSets = 3 + 3 * 2;
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(blockedReadSets.size(), expectedReadSets);

        // Reboot arbiter
        Cerr << "========= Rebooting arbiter =========" << Endl;
        blockedReadSets.clear();
        RebootTablet(runtime, shards.at(0), sender);
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(blockedReadSets.size(), expectedReadSets);

        Cerr << "========= Unblocking readsets =========" << Endl;
        blockedReadSets.Unblock();
        TCountReadSets countReadSets(runtime);
        runtime.SimulateSleep(TDuration::Seconds(1));

        // arbiter will send 3 additional commit decisions
        UNIT_ASSERT_VALUES_EQUAL(countReadSets.Count, expectedReadSets + 3);

        Cerr << "========= Checking table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table`
                ORDER BY key;
            )"),
            "{ items { int32_value: 1 } items { int32_value: 11 } }, "
            "{ items { int32_value: 11 } items { int32_value: 111 } }, "
            "{ items { int32_value: 21 } items { int32_value: 211 } }, "
            "{ items { int32_value: 31 } items { int32_value: 311 } }");

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(upsertFuture))),
            "ERROR: UNDETERMINED");
    }

    Y_UNIT_TEST_TWIN(UpsertBrokenLockArbiterRestart, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key int, value int, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10, 20, 30));
            )"),
            "SUCCESS");

        const auto tableId = ResolveTableId(server, sender, "/Root/table");
        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 4u);

        TForceVolatileProposeArbiter forceArbiter(runtime, shards.at(0));
        TForceBrokenLock forceBrokenLock(runtime, tableId, shards.at(3));

        TBlockReadSets blockedReadSets(runtime);

        Cerr << "========= Starting upsert =========" << Endl;
        auto upsertFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value)
            VALUES (1, 11), (11, 111), (21, 211), (31, 311);
            )");

        // arbiter will send 3 expectations
        // two shards will send 1 commit decision + 1 expectation
        size_t expectedReadSets = 3 + 2 * 2;
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(blockedReadSets.size(), expectedReadSets);

        // Reboot arbiter
        Cerr << "========= Rebooting arbiter =========" << Endl;
        blockedReadSets.clear();
        RebootTablet(runtime, shards.at(0), sender);
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(blockedReadSets.size(), expectedReadSets);

        Cerr << "========= Unblocking readsets =========" << Endl;
        blockedReadSets.Unblock();
        TCountReadSets countReadSets(runtime);
        runtime.SimulateSleep(TDuration::Seconds(1));

        // arbiter will send 3 additional commit decisions
        // the last shard sends a nodata readset to answer expectation
        UNIT_ASSERT_VALUES_EQUAL(countReadSets.Count, expectedReadSets + 4);

        Cerr << "========= Checking table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table`
                ORDER BY key;
            )"),
            "");

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(AwaitResponse(runtime, std::move(upsertFuture))),
            "ERROR: ABORTED");
    }

    Y_UNIT_TEST_TWIN(UpsertDependenciesShardsRestart, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 2u);

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value)
                VALUES (1, 1), (11, 11)
                )"),
            "<empty>");

        TForceVolatileProposeArbiter forceArbiter(runtime, shards.at(0));
        TBlockEvents<TEvTxProcessing::TEvPlanStep> blockedPlan(runtime,
            [actor = ResolveTablet(runtime, shards.at(0))](const auto& ev) {
                return ev->GetRecipientRewrite() == actor;
            });

        Cerr << "========= Starting upsert 1 =========" << Endl;
        auto upsertFuture1 = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value)
            VALUES (2, 2), (12, 12);
            )");
        runtime.SimulateSleep(TDuration::Seconds(1));

        Cerr << "========= Starting upsert 2 =========" << Endl;
        auto upsertFuture2 = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value)
            VALUES (2, 1002), (12, 1012);
            )");
        runtime.SimulateSleep(TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(blockedPlan.size(), 2u);

        // We expect transaction to execute at shards[1]
        // However at shards[0] it didn't even start due to blocked plans
        // Now we need to restart both shards, without giving them a chance to communicate
        std::vector<TActorId> shardActors{
            ResolveTablet(runtime, shards.at(0)),
            ResolveTablet(runtime, shards.at(1)),
        };
        for (auto& shardActor : shardActors) {
            Cerr << "... killing actor " << shardActor << Endl;
            // Perform a synchronous send, this makes sure both shards handle TEvPoison before anything else
            runtime.Send(new IEventHandle(shardActor, TActorId(), new TEvents::TEvPoison), 0, /* viaActorSystem */ false);
        }

        blockedPlan.Stop().clear();

        // Both queries should abort with UNDETERMINED
        Cerr << "... waiting for query results" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(upsertFuture1))),
            "ERROR: UNDETERMINED");
        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(upsertFuture2))),
            "ERROR: UNDETERMINED");

        // Split the second shard, which makes sure it's not stuck
        Cerr << "========= Splitting shard 2 =========" << Endl;
        SetSplitMergePartCountLimit(server->GetRuntime(), -1);
        ui64 txId = AsyncSplitTable(server, sender, "/Root/table", shards.at(1), 15);
        Cerr << "... split txId# " << txId << " started" << Endl;
        WaitTxNotification(server, sender, txId);
        Cerr << "... split finished" << Endl;
    }

    Y_UNIT_TEST_TWIN(DistributedUpsertRestartBeforePrepare, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        Cerr << "========= Creating the table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 2u);

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value)
                VALUES (1, 1), (11, 11)
                )"),
            "<empty>");

        TBlockEvents<TEvDataShard::TEvProposeTransaction> blockedPrepare(runtime);
        TBlockEvents<NKikimr::NEvents::TDataEvents::TEvWrite> blockedEvWrite(runtime);

        Cerr << "========= Starting upsert 1 =========" << Endl;
        auto upsertFuture1 = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value)
            VALUES (2, 2), (12, 12);
            )");

        runtime.WaitFor("prepare requests", [&]{ return blockedPrepare.size() + blockedEvWrite.size() >= 2; });
        UNIT_ASSERT_VALUES_EQUAL(blockedPrepare.size() + blockedEvWrite.size(), 2u);

        blockedPrepare.Stop();
        blockedEvWrite.Stop();

        Cerr << "========= Restarting shard 1 =========" << Endl;
        GracefulRestartTablet(runtime, shards.at(0), sender);

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(upsertFuture1))),
            "ERROR: UNAVAILABLE");
    }

    Y_UNIT_TEST_TWIN(DistributedUpsertRestartAfterPrepare, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        Cerr << "========= Creating the table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 2u);

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value)
                VALUES (1, 1), (11, 11)
                )"),
            "<empty>");

        TBlockEvents<TEvDataShard::TEvProposeTransactionResult> blockedPrepare(runtime);
        TBlockEvents<NKikimr::NEvents::TDataEvents::TEvWriteResult> blockedEvWrite(runtime);

        Cerr << "========= Starting upsert 1 =========" << Endl;
        auto upsertFuture1 = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value)
            VALUES (2, 2), (12, 12);
            )");

        runtime.WaitFor("prepare results", [&]{ return blockedPrepare.size() + blockedEvWrite.size() >= 2; });
        UNIT_ASSERT_VALUES_EQUAL(blockedPrepare.size() + blockedEvWrite.size(), 2u);

        for (auto& ev : blockedPrepare) {
            auto* msg = ev->Get();
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetStatus(), NKikimrTxDataShard::TEvProposeTransactionResult::PREPARED);
        }

        for (auto& ev : blockedEvWrite) {
            auto* msg = ev->Get();
            UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetStatus(), NKikimrDataEvents::TEvWriteResult::STATUS_PREPARED);
        }

        // Unblock prepare results and restart the first shard
        blockedPrepare.Stop().Unblock();
        blockedEvWrite.Stop().Unblock();

        Cerr << "========= Restarting shard 1 =========" << Endl;
        GracefulRestartTablet(runtime, shards.at(0), sender);

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(upsertFuture1))),
            "ERROR: ABORTED");
    }

    Y_UNIT_TEST(DistributedUpsertRestartAfterPlan) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        Cerr << "========= Creating the table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 2u);

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value)
                VALUES (1, 1), (11, 11)
                )"),
            "<empty>");

        TBlockEvents<TEvTxProcessing::TEvPlanStep> blockedPlan(runtime);

        Cerr << "========= Starting upsert 1 =========" << Endl;
        auto upsertFuture1 = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value)
            VALUES (2, 2), (12, 12);
            )");

        runtime.WaitFor("shard plans", [&]{ return blockedPlan.size() >= 2; });
        UNIT_ASSERT_VALUES_EQUAL(blockedPlan.size(), 2u);

        // Block TEvPrivate::TEvProgressTransaction for shard1
        auto shard1actor = ResolveTablet(runtime, shards.at(0));
        TBlockEvents<IEventHandle> blockedProgress(runtime,
            [&](const TAutoPtr<IEventHandle>& ev) {
                return ev->GetRecipientRewrite() == shard1actor &&
                    ev->GetTypeRewrite() == EventSpaceBegin(TKikimrEvents::ES_PRIVATE) + 0;
            });

        // Unblock prepare results and restart the first shard
        blockedPlan.Stop().Unblock();
        runtime.WaitFor("blocked progress", [&]{ return blockedProgress.size() >= 1; });
        UNIT_ASSERT_VALUES_EQUAL(blockedProgress.size(), 1u);

        Cerr << "... sleeping for 1 second" << Endl;
        runtime.SimulateSleep(TDuration::Seconds(1));

        Cerr << "========= Restarting shard 1 =========" << Endl;
        GracefulRestartTablet(runtime, shards.at(0), sender);

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(upsertFuture1))),
            "ERROR: ABORTED");

        Cerr << "========= Checking table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table`
                ORDER BY key;
            )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 11 } items { uint32_value: 11 } }");
    }

    // Regression test for KIKIMR-22506
    Y_UNIT_TEST_TWIN(NotCachingAbortingDeletes, UseSink) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true)
            .SetAppConfig(app);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TDisableDataShardLogBatching disableDataShardLogBatching;

        Cerr << "========= Creating table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (100));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 2u);

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value)
                VALUES
                    (1, 1), (2, 2), (3, 3), (4, 4), (5, 5),
                    (6, 6), (7, 7), (8, 8), (9, 9), (10, 10),
                    (11, 11), (12, 12), (13, 13), (14, 14), (15, 15),
                    (16, 16), (17, 17), (18, 18), (19, 19), (20, 20);
                )"),
            "<empty>");

        // We need to delete the first key (will be the trigger)
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                DELETE FROM `/Root/table` WHERE key = 1;
                )"),
            "<empty>");

        // Start transaction that deletes many rows and reads the result
        // It is not committed yet, so should not be cached
        Cerr << "========= Deleting rows (uncommitted) =========" << Endl;
        TString sessionId, txId;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleBegin(runtime, sessionId, txId, R"(
                DELETE FROM `/Root/table` WHERE key < 20;
                SELECT key, value FROM `/Root/table` ORDER BY key;
            )"),
            "{ items { uint32_value: 20 } items { uint32_value: 20 } }");

        // Make sure the lock is broken at the second shard
        Cerr << "========= Upserting key 200 (breaking lock) =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value)
                VALUES (200, 200);
                )"),
            "<empty>");

        Cerr << "========= Validating table contents =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table` ORDER BY key;
                )"),
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
            "{ items { uint32_value: 3 } items { uint32_value: 3 } }, "
            "{ items { uint32_value: 4 } items { uint32_value: 4 } }, "
            "{ items { uint32_value: 5 } items { uint32_value: 5 } }, "
            "{ items { uint32_value: 6 } items { uint32_value: 6 } }, "
            "{ items { uint32_value: 7 } items { uint32_value: 7 } }, "
            "{ items { uint32_value: 8 } items { uint32_value: 8 } }, "
            "{ items { uint32_value: 9 } items { uint32_value: 9 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 11 } items { uint32_value: 11 } }, "
            "{ items { uint32_value: 12 } items { uint32_value: 12 } }, "
            "{ items { uint32_value: 13 } items { uint32_value: 13 } }, "
            "{ items { uint32_value: 14 } items { uint32_value: 14 } }, "
            "{ items { uint32_value: 15 } items { uint32_value: 15 } }, "
            "{ items { uint32_value: 16 } items { uint32_value: 16 } }, "
            "{ items { uint32_value: 17 } items { uint32_value: 17 } }, "
            "{ items { uint32_value: 18 } items { uint32_value: 18 } }, "
            "{ items { uint32_value: 19 } items { uint32_value: 19 } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } }, "
            "{ items { uint32_value: 200 } items { uint32_value: 200 } }");

        TBlockEvents<TEvDataShard::TEvProposeTransactionResult> blockedResults(runtime,
            [&](const auto& ev) {
                auto* msg = ev->Get();
                if (msg->Record.GetStatus() == NKikimrTxDataShard::TEvProposeTransactionResult::PREPARED) {
                    return false;
                }
                return true;
            });
        TBlockEvents<NKikimr::NEvents::TDataEvents::TEvWriteResult> blockedEvWriteResults(runtime,
            [&](const auto& ev) {
                auto* msg = ev->Get();
                if (msg->Record.GetStatus() == NKikimrDataEvents::TEvWriteResult::STATUS_PREPARED) {
                    return false;
                }
                return true;
            });

        size_t otherReadSets = 0;
        TBlockEvents<TEvTxProcessing::TEvReadSet> blockedReadSets(runtime,
            [&otherReadSets, actor = ResolveTablet(runtime, shards.at(0))](const auto& ev) {
                if (ev->GetRecipientRewrite() == actor) {
                    return true;
                }
                ++otherReadSets;
                return false;
            });

        Cerr << "========= Starting commit =========" << Endl;
        auto commitFuture = KqpSimpleSendCommit(runtime, sessionId, txId, "SELECT 1");

        runtime.WaitFor("blocked readsets", [&]{ return blockedReadSets.size() >= 1 && otherReadSets >= 1; });
        UNIT_ASSERT_VALUES_EQUAL(blockedReadSets.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(otherReadSets, 1u);
        runtime.SimulateSleep(TDuration::MilliSeconds(1));

        TBlockEvents<TEvBlobStorage::TEvPut> blockedCommits(runtime,
            [&](const auto& ev) {
                auto* msg = ev->Get();
                if (msg->Id.TabletID() == shards.at(0)) {
                    Cerr << "... blocking put " << msg->Id << Endl;
                    return true;
                }
                return false;
            });

        // Unblock readsets, but block commits, so abort can't commit
        blockedReadSets.Stop().Unblock();
        runtime.WaitFor("blocked commit", [&]{ return blockedCommits.size() >= 1; });

        Cerr << "========= Starting a concurrent read =========" << Endl;
        auto readFuture = KqpSimpleSend(runtime, R"(
            SELECT key, value FROM `/Root/table` WHERE key <= 30 ORDER BY key;
        )");
        runtime.SimulateSleep(TDuration::Seconds(1));

        Cerr << "========= Unblocking commits and checking results =========" << Endl;
        blockedCommits.Stop().Unblock();

        runtime.WaitFor("both results", [&]{ return blockedResults.size() + blockedEvWriteResults.size() >= 2; });
        blockedResults.Stop().Unblock();
        blockedEvWriteResults.Stop().Unblock();

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(commitFuture))),
            "ERROR: ABORTED");

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(readFuture))),
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
            "{ items { uint32_value: 3 } items { uint32_value: 3 } }, "
            "{ items { uint32_value: 4 } items { uint32_value: 4 } }, "
            "{ items { uint32_value: 5 } items { uint32_value: 5 } }, "
            "{ items { uint32_value: 6 } items { uint32_value: 6 } }, "
            "{ items { uint32_value: 7 } items { uint32_value: 7 } }, "
            "{ items { uint32_value: 8 } items { uint32_value: 8 } }, "
            "{ items { uint32_value: 9 } items { uint32_value: 9 } }, "
            "{ items { uint32_value: 10 } items { uint32_value: 10 } }, "
            "{ items { uint32_value: 11 } items { uint32_value: 11 } }, "
            "{ items { uint32_value: 12 } items { uint32_value: 12 } }, "
            "{ items { uint32_value: 13 } items { uint32_value: 13 } }, "
            "{ items { uint32_value: 14 } items { uint32_value: 14 } }, "
            "{ items { uint32_value: 15 } items { uint32_value: 15 } }, "
            "{ items { uint32_value: 16 } items { uint32_value: 16 } }, "
            "{ items { uint32_value: 17 } items { uint32_value: 17 } }, "
            "{ items { uint32_value: 18 } items { uint32_value: 18 } }, "
            "{ items { uint32_value: 19 } items { uint32_value: 19 } }, "
            "{ items { uint32_value: 20 } items { uint32_value: 20 } }");
    }

    Y_UNIT_TEST(CompactedVolatileChangesCommit) {
        // Test explained:
        // 1. distributed tx that writes some data (commits to channel 0 are blocked)
        // 2. shard receives a successful readset and marks commits in local db
        // 3. table is compacted (commit to channel 0 are blocked, but SST switches)
        // 4. perform a read, it shouldn't reply with success (tx not persistent yet and may rollback)
        // Note: this is already the case due to committing ops tracking
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TDisableDataShardLogBatching disableDataShardLogBatching;

        Cerr << "========= Creating table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 2u);
        const auto tableId = ResolveTableId(server, sender, "/Root/table");

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value) VALUES (1, 1), (11, 11);
                )"),
            "<empty>");
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table` WHERE key = 2;
            )"),
            "");

        // We need to start a distributed write
        TBlockEvents<TEvTxProcessing::TEvPlanStep> blockedPlan(runtime);
        Cerr << "========= Starting a distributed write =========" << Endl;
        auto upsertFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value) VALUES (2, 2), (12, 12);
        )");
        runtime.WaitFor("blocked plans", [&]{ return blockedPlan.size() >= 2; });
        UNIT_ASSERT_VALUES_EQUAL(blockedPlan.size(), 2u);

        // Make sure the first blocked message is for the first shard
        auto shard1actor = ResolveTablet(runtime, shards.at(0));
        if (blockedPlan[0]->GetRecipientRewrite() != shard1actor) {
            std::swap(blockedPlan[0], blockedPlan[1]);
        }
        UNIT_ASSERT_VALUES_EQUAL(blockedPlan[0]->GetRecipientRewrite(), shard1actor);

        // Block channel 0 commits at the first shard and wait for the commit attempt
        TBlockEvents<TEvBlobStorage::TEvPut> blockedCommits(runtime,
            [shard1 = shards.at(0)](const auto& ev) {
                auto* msg = ev->Get();
                if (msg->Id.TabletID() == shard1 && msg->Id.Channel() == 0) {
                    return true;
                }
                return false;
            });
        Cerr << "========= Unblocking plan at shard1 =========" << Endl;
        blockedPlan.Unblock(1);
        runtime.WaitFor("two commits (plan + exec)", [&]{ return blockedCommits.size() >= 2; });

        Cerr << "========= Unblocking plan at shard2 =========" << Endl;
        blockedPlan.Unblock();
        runtime.WaitFor("two more commits (readset + volatile commit)", [&]{ return blockedCommits.size() >= 4; });

        Cerr << "========= Compacting table =========" << Endl;
        CompactTable(runtime, shards.at(0), tableId, false);

        Cerr << "========= Starting an immediate read =========" << Endl;
        auto readFuture = KqpSimpleSend(runtime, R"(
            SELECT key, value FROM `/Root/table` WHERE key = 2;
        )");
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT(!readFuture.HasValue());

        blockedCommits.Stop().Unblock();

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(upsertFuture))),
            "<empty>");

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(readFuture))),
            "{ items { uint32_value: 2 } items { uint32_value: 2 } }");
    }

    Y_UNIT_TEST(CompactedVolatileChangesAbort) {
        // Test explained:
        // 1. distributed tx that writes some data, volatile transactions are persisted
        // 2. commits to channel 0 are blocked
        // 3. shard receives a nodata readset and starts to rollback the tx
        // 4. tx is marked as removed in the local db, but it's not persistent yet
        // 5. table is compacted, the commit is blocked, but SST switches
        // 6. since there are no traces of the volatile tx in the table the
        //    read may erroneously reply that these changes do not exist
        // 7. a nodata readset is allowed to be spurious in tablets that cannot
        //    commit, technically a newer generation may have committed already
        // Note: in practice this shouldn't happen, since new generation commit
        // implies older generations no longer having a read lease, and read
        // results wouldn't be sent anyway, but there's no guarantee that it's
        // true for all reads and decisions, we must wait for abort to commit.
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TDisableDataShardLogBatching disableDataShardLogBatching;

        Cerr << "========= Creating table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 2u);
        const auto tableId = ResolveTableId(server, sender, "/Root/table");

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value) VALUES (1, 1), (11, 11);
                )"),
            "<empty>");
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/table` WHERE key = 2;
            )"),
            "");

        // We need to start a distributed write
        TBlockEvents<TEvTxProcessing::TEvReadSet> blockedReadSets(runtime);
        Cerr << "========= Starting a distributed write =========" << Endl;
        auto upsertFuture = KqpSimpleSend(runtime, R"(
            UPSERT INTO `/Root/table` (key, value) VALUES (2, 2), (12, 12);
        )");
        runtime.WaitFor("blocked readsets", [&]{ return blockedReadSets.size() >= 4; });
        UNIT_ASSERT_VALUES_EQUAL(blockedReadSets.size(), 4u);
        blockedReadSets.Stop();

        // Block channel 0 commits at the first shard
        TBlockEvents<TEvBlobStorage::TEvPut> blockedCommits(runtime,
            [shard1 = shards.at(0)](const auto& ev) {
                auto* msg = ev->Get();
                if (msg->Id.TabletID() == shard1 && msg->Id.Channel() == 0) {
                    return true;
                }
                return false;
            });

        // Send a nodata reply to the shard1 expectation
        Cerr << "========= Sending a spurious nodata reply =========" << Endl;
        auto shard1actor = ResolveTablet(runtime, shards.at(0));
        bool replied = false;
        for (auto& ev : blockedReadSets) {
            if (ev->Sender == shard1actor) {
                const auto& record = ev->Get()->Record;
                if (record.GetFlags() & NKikimrTx::TEvReadSet::FLAG_EXPECT_READSET) {
                    auto event = MakeHolder<TEvTxProcessing::TEvReadSet>(
                        record.GetStep(),
                        record.GetTxId(),
                        record.GetTabletDest(),
                        record.GetTabletSource(),
                        shards.at(1));
                    event->Record.SetFlags(NKikimrTx::TEvReadSet::FLAG_NO_DATA | NKikimrTx::TEvReadSet::FLAG_NO_ACK);
                    runtime.Send(new IEventHandle(ev->Sender, ev->GetRecipientRewrite(), event.Release()));
                    replied = true;
                    break;
                }
            }
        }
        UNIT_ASSERT_C(replied, "Could not find an expected expectation readset");
        runtime.WaitFor("one commit (volatile abort)", [&]{ return blockedCommits.size() >= 1; });

        Cerr << "========= Compacting table =========" << Endl;
        CompactTable(runtime, shards.at(0), tableId, false);

        Cerr << "========= Starting an immediate read =========" << Endl;
        auto readFuture = KqpSimpleSend(runtime, R"(
            SELECT key, value FROM `/Root/table` WHERE key = 2;
        )");
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT(!readFuture.HasValue());
    }

    Y_UNIT_TEST(GracefulShardRestartNoEarlyReadSetAck) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false)
            .SetEnableDataShardVolatileTransactions(true);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PIPE_CLIENT, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TDisableDataShardLogBatching disableDataShardLogBatching;

        struct TBootInfo {
            ui32 Generation;
            TActorId Launcher;
        };
        THashMap<ui64, TBootInfo> bootInfo;
        auto observeBootInfo = runtime.AddObserver<TEvTablet::TEvBoot>([&](auto& ev) {
            auto* msg = ev->Get();
            auto& info = bootInfo[msg->TabletID];
            info.Generation = msg->Generation;
            info.Launcher = msg->Launcher;
        });

        Cerr << "========= Creating table =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSchemeExec(runtime, R"(
                CREATE TABLE `/Root/table` (key uint32, value uint32, PRIMARY KEY (key))
                WITH (PARTITION_AT_KEYS = (10, 20));
            )"),
            "SUCCESS");

        const auto shards = GetTableShards(server, sender, "/Root/table");
        UNIT_ASSERT_VALUES_EQUAL(shards.size(), 3u);
        // const auto tableId = ResolveTableId(server, sender, "/Root/table");

        // We need to fill table with some data
        Cerr << "========= Upserting initial values =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value) VALUES (1, 1), (11, 11), (21, 21);
                )"),
            "<empty>");

        Cerr << "========= Starting a transaction =========" << Endl;
        TString sessionId, txId;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleBegin(runtime, sessionId, txId, R"(
                SELECT key, value FROM `/Root/table` ORDER BY key;
            )"),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 11 } items { uint32_value: 11 } }, "
            "{ items { uint32_value: 21 } items { uint32_value: 21 } }");

        Cerr << "========= Upserting a row to shard 2 to break the lock =========" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                UPSERT INTO `/Root/table` (key, value) VALUES (11, 111);
                )"),
            "<empty>");

        runtime.SimulateSleep(TDuration::MilliSeconds(1));

        // Block channel 0 commits at shard 3
        TBlockEvents<TEvBlobStorage::TEvPut> blockedCommits(runtime,
            [shard3 = shards.at(2)](const auto& ev) {
                auto* msg = ev->Get();
                if (msg->Id.TabletID() == shard3 && msg->Id.Channel() == 0) {
                    return true;
                }
                return false;
            });

        // Block readsets at shard 3
        TBlockEvents<TEvTxProcessing::TEvReadSet> blockedReadSets(runtime,
            [shard3actor = ResolveTablet(runtime, shards.at(2))](const auto& ev) {
                return ev->GetRecipientRewrite() == shard3actor;
            });

        // Force shard 1 to be the arbiter
        TForceVolatileProposeArbiter forceArbiter(runtime, shards.at(0));

        Cerr << "========= Starting to commit =========" << Endl;
        auto commitFuture = KqpSimpleSendCommit(runtime, sessionId, txId, R"(
            UPSERT INTO `/Root/table` (key, value) VALUES (2, 2), (12, 12), (22, 22);
        )");

        // After a short while shard 3 must have 2 blocked commits and 2 blocked readsets (expectation + abort):
        // - Sys update on the new PlanStep
        // - Execute and persist volatile tx
        // - Processed abort tx persistence
        runtime.SimulateSleep(TDuration::MilliSeconds(1));
        UNIT_ASSERT_VALUES_EQUAL(blockedCommits.size(), 2u);
        UNIT_ASSERT_VALUES_EQUAL(blockedReadSets.size(), 2u);

        // Unblock readsets and wait for one more commit attempt (abort persistence)
        blockedReadSets.Stop().Unblock();
        runtime.WaitFor("1 more commit", [&]{ return blockedCommits.size() >= 3; });

        // Block channel 0 commit responses at shard 3
        TBlockEvents<TEvBlobStorage::TEvPutResult> blockedCommitResults(runtime,
            [shard3 = shards.at(2)](const auto& ev) {
                auto* msg = ev->Get();
                if (msg->Id.TabletID() == shard3 && msg->Id.Channel() == 0) {
                    return true;
                }
                return false;
            });

        // Unblock the first two commits and wait for their blocked responses
        blockedCommits.Unblock(2);
        runtime.WaitFor("2 commit results", [&]{ return blockedCommitResults.size() >= 2; });
        blockedCommits.Stop();
        blockedCommitResults.Stop();

        Cerr << "========= Starting new shard3 generation =========" << Endl;
        UNIT_ASSERT(bootInfo.contains(shards.at(2)));
        auto shard3sys = ResolveTablet(runtime, shards.at(2), 0, true);
        auto shard3info = bootInfo.at(shards.at(2));
        runtime.Send(
            new IEventHandle(shard3info.Launcher, shard3sys,
                new TEvTablet::TEvTabletDead(shards.at(2), TEvTablet::TEvTabletDead::ReasonPill, shard3info.Generation)),
            0, true);
        runtime.SimulateSleep(TDuration::MilliSeconds(1));
        InvalidateTabletResolverCache(runtime, shards.at(2), 0);

        Cerr << "========= Unblocking old shard3 generation =========" << Endl;
        blockedCommitResults.Unblock();
        blockedCommits.Unblock();

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(runtime.WaitFuture(std::move(commitFuture))),
            "ERROR: ABORTED");

        Cerr << "========= Final read (must not hang) =========" << Endl;
        auto readFuture = KqpSimpleSend(runtime, R"(
            SELECT key, value FROM `/Root/table` ORDER BY key;
        )");
        runtime.SimulateSleep(TDuration::MilliSeconds(100));
        UNIT_ASSERT_C(readFuture.HasValue(), "Read didn't finish in 100ms of simulated time");

        UNIT_ASSERT_VALUES_EQUAL(
            FormatResult(readFuture.ExtractValueSync()),
            "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
            "{ items { uint32_value: 11 } items { uint32_value: 111 } }, "
            "{ items { uint32_value: 21 } items { uint32_value: 21 } }");
    }

} // Y_UNIT_TEST_SUITE(DataShardVolatile)

} // namespace NKikimr
