#include "state_storage.h"

#include <ydb/core/base/statestorage_impl.h>
#include <ydb/core/tx/scheme_board/replica.h>

#include <cstring>

namespace NKikimr {

    template<size_t N>
    static TIntrusivePtr<TStateStorageInfo> GenerateStateStorageInfo(const TActorId (&replicas)[N])
    {
        auto info = MakeIntrusive<TStateStorageInfo>();
        info->RingGroups.resize(1);
        auto& group = info->RingGroups.back();
        group.NToSelect = N;
        group.Rings.resize(N);
        for (size_t i = 0; i < N; ++i) {
            group.Rings[i].Replicas.push_back(replicas[i]);
        }

        return info;
    }

    static TIntrusivePtr<TStateStorageInfo> GenerateStateStorageInfo(const TVector<TActorId> &replicas, ui32 NToSelect, ui32 nrings, ui32 ringSize, ui32 ringGroups = 1)
    {
        Y_ABORT_UNLESS(replicas.size() >= ringGroups * nrings * ringSize);
        Y_ABORT_UNLESS(NToSelect <= nrings);

        auto info = MakeIntrusive<TStateStorageInfo>();
        info->RingGroups.resize(ringGroups);
        ui32 inode = 0;
        for (ui32 rg : xrange(ringGroups)) {
            auto& group = info->RingGroups[rg];
            group.NToSelect = NToSelect;
            group.Rings.resize(nrings);

            for (size_t i = 0; i < nrings; ++i) {
                for (size_t j = 0; j < ringSize; ++j) {
                    group.Rings[i].Replicas.push_back(replicas[inode++]);
                }
            }
        }

        return info;
    }

    TActorId MakeBoardReplicaID(ui32 node, ui32 replicaIndex) {
        char x[12] = { 's', 's', 'b' };
        x[3] = (char)1;
        memcpy(x + 5, &replicaIndex, sizeof(ui32));
        return TActorId(node, TStringBuf(x, 12));
    }

    void SetupCustomStateStorage(
        TTestActorRuntime &runtime,
        ui32 NToSelect,
        ui32 nrings,
        ui32 ringSize,
        ui32 ringGroups)
    {
        TVector<TActorId> ssreplicas;
        for (size_t i = 0; i < ringGroups * nrings * ringSize; ++i) {
            ssreplicas.push_back(MakeStateStorageReplicaID(runtime.GetNodeId(i), i));
        }

        TVector<TActorId> breplicas;
        for (size_t i = 0; i < ringGroups * nrings * ringSize; ++i) {
            breplicas.push_back(MakeBoardReplicaID(runtime.GetNodeId(i), i));
        }

        TVector<TActorId> sbreplicas;
        for (size_t i = 0; i < ringGroups * nrings * ringSize; ++i) {
            sbreplicas.push_back(MakeSchemeBoardReplicaID(runtime.GetNodeId(i), i));
        }

        const TActorId ssproxy = MakeStateStorageProxyID();

        auto ssInfo = GenerateStateStorageInfo(ssreplicas, NToSelect, nrings, ringSize, ringGroups);
        auto sbInfo = GenerateStateStorageInfo(sbreplicas, NToSelect, nrings, ringSize, ringGroups);
        auto bInfo = GenerateStateStorageInfo(breplicas, NToSelect, nrings, ringSize, ringGroups);


        for (ui32 ssIndex = 0; ssIndex < ringGroups * nrings * ringSize; ++ssIndex) {
            runtime.AddLocalService(ssreplicas[ssIndex],
                TActorSetupCmd(CreateStateStorageReplica(ssInfo.Get(), ssIndex), TMailboxType::Revolving, 0), ssIndex);
            runtime.AddLocalService(sbreplicas[ssIndex],
                TActorSetupCmd(CreateSchemeBoardReplica(sbInfo.Get(), ssIndex), TMailboxType::Revolving, 0), ssIndex);
            runtime.AddLocalService(breplicas[ssIndex],
                TActorSetupCmd(CreateStateStorageBoardReplica(bInfo.Get(), ssIndex), TMailboxType::Revolving, 0), ssIndex);
        }

        for (ui32 nodeIndex = 0; nodeIndex < runtime.GetNodeCount(); ++nodeIndex) {
            runtime.AddLocalService(ssproxy,
                    TActorSetupCmd(CreateStateStorageProxy(ssInfo.Get(), bInfo.Get(), sbInfo.Get()), TMailboxType::Revolving, 0), nodeIndex);
        }
    }


    void SetupStateStorage(TTestActorRuntime& runtime, ui32 nodeIndex, bool firstNode)
    {
        const TActorId ssreplicas[3] = {
            MakeStateStorageReplicaID(runtime.GetNodeId(0), 0),
            MakeStateStorageReplicaID(runtime.GetNodeId(0), 1),
            MakeStateStorageReplicaID(runtime.GetNodeId(0), 2),
        };

        const TActorId breplicas[3] = {
            MakeBoardReplicaID(runtime.GetNodeId(0), 0),
            MakeBoardReplicaID(runtime.GetNodeId(0), 1),
            MakeBoardReplicaID(runtime.GetNodeId(0), 2),
        };

        const TActorId sbreplicas[3] = {
            MakeSchemeBoardReplicaID(runtime.GetNodeId(0), 0),
            MakeSchemeBoardReplicaID(runtime.GetNodeId(0), 1),
            MakeSchemeBoardReplicaID(runtime.GetNodeId(0), 2),
        };

        const TActorId ssproxy = MakeStateStorageProxyID();

        auto ssInfo = GenerateStateStorageInfo(ssreplicas);
        auto sbInfo = GenerateStateStorageInfo(sbreplicas);
        auto bInfo = GenerateStateStorageInfo(breplicas);

        if (!firstNode || nodeIndex == 0) {
            for (ui32 i = 0; i < 3; ++i) {
                runtime.AddLocalService(ssreplicas[i],
                    TActorSetupCmd(CreateStateStorageReplica(ssInfo.Get(), i), TMailboxType::Revolving, 0), nodeIndex);
                runtime.AddLocalService(sbreplicas[i],
                    TActorSetupCmd(CreateSchemeBoardReplica(sbInfo.Get(), i), TMailboxType::Revolving, 0), nodeIndex);
                runtime.AddLocalService(breplicas[i],
                    TActorSetupCmd(CreateStateStorageBoardReplica(bInfo.Get(), i), TMailboxType::Revolving, 0), nodeIndex);
            }
        }

        runtime.AddLocalService(ssproxy,
            TActorSetupCmd(CreateStateStorageProxy(ssInfo.Get(), bInfo.Get(), sbInfo.Get()), TMailboxType::Revolving, 0), nodeIndex);
    }

    namespace {

        void AddReplicas(TStateStorageInfo::TRingGroup& group, const TVector<TActorId>& replicas) {
            group.NToSelect = group.NToSelect ? group.NToSelect : replicas.size();
            group.Rings.resize(replicas.size());
            for (size_t i = 0; i < replicas.size(); ++i) {
                // one replica per ring
                group.Rings[i].Replicas.push_back(replicas[i]);
            }
        }

        TIntrusivePtr<TStateStorageInfo> GenerateStateStorageInfo(const TVector<TStateStorageInfo::TRingGroup>& ringGroups) {
            auto info = MakeIntrusive<TStateStorageInfo>();
            info->RingGroups = ringGroups;
            return info;
        }

    }

    TStateStorageSetupper CreateCustomStateStorageSetupper(const TVector<TStateStorageInfo::TRingGroup>& ringGroups, int replicasInRingGroup) {
        THashMap<ui32, TVector<ui32>> ringGroupsIdToNodeIds;
        for (ui32 i = 0; i < ringGroups.size(); ++i) {
            ringGroupsIdToNodeIds[i] = TVector<ui32>(replicasInRingGroup, 0);
        }
        return CreateCustomStateStorageSetupper(ringGroups, ringGroupsIdToNodeIds);
    }

    TStateStorageSetupper CreateCustomStateStorageSetupper(const TVector<TStateStorageInfo::TRingGroup>& ringGroups,
                                                           const THashMap<ui32, TVector<ui32>>& ringGroupIdToNodeIds) {
        return [=](TTestActorRuntime& runtime, ui32 nodeIndex) {
            TSet<ui32> nodes;
            for (const auto& [_, nodeIds] : ringGroupIdToNodeIds) {
                nodes.insert(nodeIds.begin(), nodeIds.end());
            }
            auto ssInfo = GenerateStateStorageInfo(ringGroups);
            auto sbInfo = GenerateStateStorageInfo(ringGroups);
            auto bInfo = GenerateStateStorageInfo(ringGroups);
            for (const auto& [pileId, nodeIds] : ringGroupIdToNodeIds) {
                auto addReplicas = [&](auto& group, auto makeId) {
                    TVector<TActorId> replicas;
                    for (ui32 i = 0; i < nodeIds.size(); ++i) {
                        replicas.emplace_back(makeId(runtime.GetNodeId(nodeIds[i]), nodeIds.size() * pileId + i));
                    }
                    AddReplicas(group, replicas);
                    return replicas;
                };

                auto ssreplicas = addReplicas(ssInfo->RingGroups[pileId], MakeStateStorageReplicaID);
                auto sbreplicas = addReplicas(sbInfo->RingGroups[pileId], MakeSchemeBoardReplicaID);
                auto breplicas = addReplicas(bInfo->RingGroups[pileId], MakeBoardReplicaID);

                auto addLocalServices = [&](const TVector<TActorId>& replicas, auto createCmd, auto* info) {
                    for (ui32 i = 0; i < nodeIds.size(); ++i) {
                        runtime.AddLocalService(
                            replicas[i],
                            TActorSetupCmd(createCmd(info, nodeIds.size() * pileId + i), TMailboxType::Revolving, 0),
                            nodeIndex
                        );
                    }
                };

                if (nodes.contains(nodeIndex)) {
                    addLocalServices(ssreplicas, CreateStateStorageReplica, ssInfo.Get());
                    addLocalServices(sbreplicas, CreateSchemeBoardReplica, sbInfo.Get());
                    addLocalServices(breplicas, CreateStateStorageBoardReplica, bInfo.Get());
                }
            }

            const TActorId ssproxy = MakeStateStorageProxyID();
            runtime.AddLocalService(ssproxy,
                TActorSetupCmd(CreateStateStorageProxy(ssInfo.Get(), bInfo.Get(), sbInfo.Get()), TMailboxType::Revolving, 0),
                nodeIndex
            );
        };
    }

    constexpr int ReplicasInRingGroup = 3;

    TStateStorageSetupper CreateDefaultStateStorageSetupper() {
        return CreateCustomStateStorageSetupper({ TStateStorageInfo::TRingGroup{} }, ReplicasInRingGroup);
    }

} // namespace NKikimr
