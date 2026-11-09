#pragma once

#include "ut_helpers.h"

#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/core/testlib/basics/helpers.h>
#include <ydb/core/testlib/fake_coordinator.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_allocator/txallocator.h>

namespace NKikimr {
namespace NSchemeBoard {

class TTestWithSchemeshard: public NUnitTest::TTestBase {
    static void AddDomain(
        TTestActorRuntime& runtime,
        TAppPrepare& app,
        const TString& name,
        ui32 domainUid,
        ui64 hiveTabletId,
        ui64 schemeshardTabletId
    ) {
        app.ClearDomainsAndHive();
        ui32 planResolution = 50;
        auto domain = TDomainsInfo::TDomain::ConstructDomainWithExplicitTabletIds(
            name, domainUid, schemeshardTabletId,
            planResolution,
            TVector<ui64>{TDomainsInfo::MakeTxCoordinatorIDFixed(1)},
            TVector<ui64>{},
            TVector<ui64>{TDomainsInfo::MakeTxAllocatorIDFixed(1)},
            DefaultPoolKinds(2)
        );

        TVector<ui64> ids = runtime.GetTxAllocatorTabletIds();
        ids.insert(ids.end(), domain->TxAllocators.begin(), domain->TxAllocators.end());
        runtime.SetTxAllocatorTabletIds(ids);

        app.AddDomain(domain.Release());
        app.AddHive(hiveTabletId);
    }

    void SetupRuntime(TTestActorRuntime& runtime) {
        for (ui32 i : xrange(runtime.GetNodeCount())) {
            SetupStateStorage(runtime, i, 0);
        }

        TAppPrepare app;
        AddDomain(runtime, app, "Root", 0, TTestTxConfig::Hive, RootSchemeshardTabletId);
        SetupChannelProfiles(app, 1);
        SetupTabletServices(runtime, &app, true);
    }

    static void BootSchemeShard(TTestActorRuntime& runtime, ui64 tabletId) {
        using namespace NSchemeShard;

        CreateTestBootstrapper(runtime, CreateTestTabletInfo(tabletId, TTabletTypes::SchemeShard), &CreateFlatTxSchemeShard);

        const TActorId edge = runtime.AllocateEdgeActor();

        auto init = new TEvSchemeShard::TEvInitRootShard(edge, 32, "Root");
        runtime.SendToPipe(tabletId, edge, init, 0, GetPipeConfigWithRetries());
        auto ev = runtime.GrabEdgeEvent<TEvSchemeShard::TEvInitRootShardResult>(edge);

        UNIT_ASSERT(ev->Get());
        UNIT_ASSERT_VALUES_EQUAL(ev->Get()->Record.GetOrigin(), tabletId);
        UNIT_ASSERT_VALUES_EQUAL(ev->Get()->Record.GetStatus(), (ui32)TEvSchemeShard::TEvInitRootShardResult::StatusAlreadyInitialized);
    }

    static void BootTxAllocator(TTestActorRuntime& runtime, ui64 tabletId) {
        CreateTestBootstrapper(runtime, CreateTestTabletInfo(tabletId, TTabletTypes::TxAllocator), &CreateTxAllocator);
    }

    void BootCoordinator(TTestActorRuntime& runtime, ui64 tabletId) {
        CoordinatorState = new TFakeCoordinator::TState();
        BootFakeCoordinator(runtime, tabletId, CoordinatorState);
    }

    void BootHive(TTestActorRuntime& runtime, ui64 tabletId) {
        HiveState = new TFakeHiveState();
        BootFakeHive(runtime, tabletId, HiveState);
    }

    virtual TTestContext::TEventObserver ObserverFunc() {
        return TTestContext::DefaultObserverFunc;
    }

    void AddSysViewsRosterUpdateObserver() {
        if (!Context->IsRealThreads()) {
            SysViewsRosterUpdateFinished = false;
            SysViewsRosterUpdateObserver = Context->AddObserver<NSysView::TEvSysView::TEvRosterUpdateFinished>([this](auto&) {
                SysViewsRosterUpdateFinished = true;
            });
        }
    }

    void WaitForSysViewsRosterUpdate() {
        if (!Context->IsRealThreads()) {
            Context->WaitFor("SysViewsRoster update finished", [this] {
                return SysViewsRosterUpdateFinished;
            });
            SysViewsRosterUpdateObserver.Remove();
        }
    }

public:
    void PrepareContext() {
        Context = MakeHolder<TTestContext>();
        Context->SetObserverFunc(ObserverFunc());

        SetupRuntime(*Context);
    }

    void BootActors() {
        // Create Observer to catch an event of system views update finished.
        // For more info, see comments in ydb/core/testlib/test_client.cpp
        if (Context->GetAppData().FeatureFlags.GetEnableRealSystemViewPaths()) {
            AddSysViewsRosterUpdateObserver();
        }

        BootSchemeShard(*Context, RootSchemeshardTabletId);
        BootTxAllocator(*Context, TTestTxConfig::TxAllocator);
        BootCoordinator(*Context, TTestTxConfig::Coordinator);
        BootHive(*Context, TTestTxConfig::Hive);

        // Wait until schemeshard completes creating system view paths
        if (Context->GetAppData().FeatureFlags.GetEnableRealSystemViewPaths()) {
            WaitForSysViewsRosterUpdate();
        }
    }

    void SetUp() override {
        PrepareContext();
        BootActors();
    }

    void TurnOnTabletsScheduling() {
        if (SchedulingGuard) {
            return;
        }

        TActorId sender = Context->AllocateEdgeActor();
        TVector<ui64> tabletIds;
        tabletIds.push_back(RootSchemeshardTabletId);
        for (auto x: xrange(TTestTxConfig::FakeHiveTablets,  TTestTxConfig::FakeHiveTablets + 10)) {
            tabletIds.push_back(x);
        }

        SchedulingGuard = CreateTabletScheduledEventsGuard(tabletIds, *Context, sender);

        // make schemeShard visible for ScheduledEventsGuard
        // trigger actor resolving for existed tablet
        RebootTablet(*Context, RootSchemeshardTabletId, sender);
    }

    void TearDown() override {
        SchedulingGuard.Reset();
        CoordinatorState.Drop();
        HiveState.Drop();
        SysViewsRosterUpdateObserver.Remove();
        Context.Reset();
    }

public:
    ui64 RootSchemeshardTabletId = TTestTxConfig::SchemeShard;

protected:
    THolder<TTestContext> Context;

    TTestContext::TEventObserverHolder SysViewsRosterUpdateObserver;
    bool SysViewsRosterUpdateFinished;

private:
    TFakeCoordinator::TState::TPtr CoordinatorState;
    TFakeHiveState::TPtr HiveState;
    THolder<ITabletScheduledEventsGuard> SchedulingGuard;

}; // TTestWithSchemeshard

} // NSchemeBoard
} // NKikimr
