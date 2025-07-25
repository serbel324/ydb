#pragma once

#include "events.h"
#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/tablet_flat/tablet_flat_executed.h>
#include <ydb/core/tablet_flat/util_fmt_abort.h>
#include <ydb/library/actors/core/actor.h>
#include <util/system/type_name.h>

namespace NKikimr {
namespace NFake {
    using TExecuted = NTabletFlatExecutor::TTabletExecutedFlat;

    class TDummySnapshotContext : public NTabletFlatExecutor::TTableSnapshotContext {
    public:
        virtual NFake::TEvExecute* OnFinished() = 0;
    };

    class TDummy : public TActor<TDummy>, public TExecuted {
        enum EState {
            Boot    = 1,
            Work    = 2,
            Stop    = 3,
        };

    public:
        using TEventHandlePtr = TAutoPtr<::NActors::IEventHandle>;
        using ELnLev = NUtil::ELnLev;
        using TInfo = TTabletStorageInfo;
        using TEvDead = TEvTablet::TEvTabletDead;

        enum class EFlg : ui32 {
            Comp    = 0x01,
            Clean   = 0x02,
        };

        TDummy(const TActorId &tablet, TInfo *info, const TActorId& owner,
                ui32 flags = 0 /* ORed EFlg enum */)
            : TActor(&TDummy::Inbox, NKikimrServices::TActivity::FAKE_ENV_A)
            , TTabletExecutedFlat(info, tablet, nullptr)
            , Owner(owner)
            , Flags(flags)
        {
        }

    private:
        void Inbox(TEventHandlePtr &eh)
        {
            if (auto *ev = eh->CastAsLocal<NFake::TEvExecute>()) {
                Y_ENSURE(State == EState::Work, "Cannot handle TX now");

                for (auto& tx : ev->Txs) {
                    Execute(tx.Release(), this->ActorContext());
                }
                for (auto& lambda : ev->Lambdas) {
                    std::move(lambda)(Executor(), this->ActorContext());
                }
            } else if (auto *ev = eh->CastAsLocal<NFake::TEvCompact>()) {
                Y_ENSURE(State == EState::Work, "Cannot handle compaction now");

                if (ev->MemOnly) {
                    Executor()->CompactMemTable(ev->Table);
                } else {
                    Executor()->CompactTable(ev->Table);
                }
                Send(Owner, new TEvents::TEvWakeup);
            } else if (eh->CastAsLocal<NFake::TEvReturn>()) {
                Send(Owner, new TEvents::TEvWakeup);
            } else if (auto *ev = eh->CastAsLocal<NFake::TEvCall>()) {
                ev->Callback(Executor(), this->ActorContext());
            } else if (eh->CastAsLocal<TEvents::TEvPoison>()) {
                if (std::exchange(State, EState::Stop) != EState::Stop) {
                    /* This hack stops TExecutor before TOwner death. TOwner
                        unbale to wait for tablet death and may yield false
                        TEvGone to leader actor on handling its own TEvPoison.
                     */

                    auto ctx(this->ActorContext());
                    Executor()->DetachTablet(), Detach(ctx);
                }
            } else if (State == EState::Boot) {
                TTabletExecutedFlat::StateInitImpl(eh, SelfId());

            } else if (eh->CastAsLocal<TEvTabletPipe::TEvServerConnected>()) {

            } else if (eh->CastAsLocal<TEvTabletPipe::TEvServerDisconnected>()){

            } else if (!TTabletExecutedFlat::HandleDefaultEvents(eh, SelfId())) {
                Y_TABLET_ERROR("Unexpected event " << eh->GetTypeName());
            }
        }

        void Enqueue(TEventHandlePtr &eh) override
        {
            const auto &name = eh->GetTypeName();

            Y_TABLET_ERROR("Got unexpected event " << name << " on tablet booting");
        }

        void DefaultSignalTabletActive(const TActorContext&) override
        {
            // must be empty
        }

        void OnActivateExecutor(const TActorContext&) override
        {
            if (std::exchange(State, EState::Work) != EState::Work) {
                SignalTabletActive(SelfId());
                Send(Owner, new NFake::TEvReady(TabletID(), SelfId()));
            } else {
                Y_TABLET_ERROR("Received unexpected TExecutor activation");
            }
        }

        void OnTabletDead(TEvDead::TPtr&, const TActorContext &ctx) override
        {
            OnDetach(ctx);
        }

        void OnDetach(const TActorContext&) override
        {
            PassAway();
        }

        void CompactionComplete(ui32 table, const TActorContext&) override
        {
            if (Flags & ui32(EFlg::Comp))
                Send(Owner, new NFake::TEvCompacted(table));
        }

        void VacuumComplete(ui64 vacuumGeneration, const TActorContext&) override
        {
            if (Flags & ui32(EFlg::Clean))
                Send(Owner, new NFake::TEvDataCleaned(vacuumGeneration));
        }

        void SnapshotComplete(
                TIntrusivePtr<NTabletFlatExecutor::TTableSnapshotContext> rawSnapContext,
                const TActorContext&) override
        {
            if (auto* snapContext = dynamic_cast<TDummySnapshotContext*>(rawSnapContext.Get())) {
                Send(SelfId(), snapContext->OnFinished());
            } else {
                Y_TABLET_ERROR("Unsupported snapshot context");
            }
        }

    private:
        TActorId Owner;
        const ui32 Flags = 0;
        EState State = EState::Boot;
    };

}
}
