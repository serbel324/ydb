#pragma once

#include "public.h"
#include "invoker.h"

#include <yt/yt/core/actions/public.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <bool VirtualizeBase>
struct TMaybeVirtualInvokerBase
    : public IInvoker
{ };

////////////////////////////////////////////////////////////////////////////////

template <>
struct TMaybeVirtualInvokerBase<true>
    : public virtual IInvoker
{ };

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

template <bool VirtualizeBase>
class TInvokerWrapper
    : public NDetail::TMaybeVirtualInvokerBase<VirtualizeBase>
{
public:
    void Invoke(TMutableRange<TClosure> callbacks) override;

    NThreading::TThreadId GetThreadId() const override;
    bool CheckAffinity(const IInvokerPtr& invoker) const override;
    bool IsSerialized() const override;
    DECLARE_SIGNAL_OVERRIDE(IInvoker::TWaitTimeObserver::TSignature, WaitTimeObserved);

protected:
    const IInvokerPtr UnderlyingInvoker_;

    explicit TInvokerWrapper(IInvokerPtr underlyingInvoker);
};

////////////////////////////////////////////////////////////////////////////////

//! A helper base which makes callbacks track their invocation time and profile their wait time.
class TInvokerProfilingWrapper
{
public:
    //! Constructs a wrapper with profiling disabled.
    TInvokerProfilingWrapper() = default;

    //! Constructs a wrapper with profiling enabled.
    /*!
    *  #registry defines a profile registry where sensors data is stored.
    *  #invokerFamily defines a family of invokers, e.g. "serialized" or "prioritized" and appears in sensor's name.
    *  #tagSet defines a particular instance of the invoker and appears in sensor's tags.
    */
    TInvokerProfilingWrapper(
        NProfiling::IRegistryPtr registry,
        const std::string& invokerFamily,
        const NProfiling::TTagSet& tagSet);

protected:
    TClosure WrapCallback(TClosure callback);

private:
    NProfiling::TEventTimer WaitTimer_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
