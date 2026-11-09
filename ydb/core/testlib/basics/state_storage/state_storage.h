#pragma once

#include <ydb/core/base/statestorage.h>
#include <ydb/core/testlib/actors/test_runtime.h>

#include <functional>

namespace NKikimr {

    using TStateStorageSetupper = std::function<void(TTestActorRuntime&, ui32)>;

    void SetupStateStorage(TTestActorRuntime& runtime, ui32 nodeIndex,
                           bool replicasOnFirstNode = false);
    void SetupCustomStateStorage(TTestActorRuntime &runtime, ui32 NToSelect, ui32 nrings, ui32 ringSize, ui32 ringGroups = 1);
    TStateStorageSetupper CreateCustomStateStorageSetupper(const TVector<TStateStorageInfo::TRingGroup>& ringGroups, int replicasInRingGroup);
    TStateStorageSetupper CreateCustomStateStorageSetupper(const TVector<TStateStorageInfo::TRingGroup>& ringGroups,
                                                           const THashMap<ui32, TVector<ui32>>& pileIdToNodeIds);
    TStateStorageSetupper CreateDefaultStateStorageSetupper();
    TActorId MakeBoardReplicaID(ui32 node, ui32 replicaIndex);

} // namespace NKikimr
