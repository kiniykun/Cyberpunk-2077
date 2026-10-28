/*
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "jobDispatcherCounterValue.h"
#include "D:/2077/src/engine-wgtrace-web.h"

namespace job { namespace prv
{

DispatcherCounterValue::ExchangeAddResult DispatcherCounterValue::ExchangeAdd( Uint32 value )
{
	RED_FATAL_ASSERT( value > 0 );

	ExchangeAddResult result;
    const bool cpTraceActive=cpEngineTrace::Enabled();
    if(cpTraceActive)cpEngineTrace::Register();
    const auto cpAddToken=cpTraceActive?wgtrace::ctr_add_begin():wgtrace::CtrAddToken{};
	const Uint32 oldValue = m_counterValue.ExchangeAdd( value );
    // No enqueue IDs are invented: delta accounts for all real and deferred contributions.
    if(cpTraceActive)wgtrace::ctr_add_end(cpAddToken,this,0,Int32(value),oldValue);

	// Only care about the lower half of oldValue, the upper half is for locking the waiting list.
	// Also if oldValue was zero, then some thread will try to release the counter entry. We can't stop
	// other threads from trying to lock the upper half anyway.
	if ( oldValue == 0 )
	{
		result.wasZero = true;
	}
	return result;
}

DispatcherCounterValue::DecrementResult DispatcherCounterValue::Decrement()
{
	DecrementResult result;

    const bool cpTraceActive=cpEngineTrace::Enabled();
    if(cpTraceActive)cpEngineTrace::Register();
    const auto cpDecToken=cpTraceActive?wgtrace::ctr_dec_begin(this):wgtrace::CtrDecToken{};
	const Uint32 newCounterValue = m_counterValue.Decrement();
    if(cpTraceActive)wgtrace::ctr_dec_end(cpDecToken,wgtrace::tl_job,newCounterValue);
	RED_FATAL_ASSERT( newCounterValue != std::numeric_limits<Uint32>::max(), "Counter value underflow!" );

	if ( newCounterValue == 0 )
	{
		result.isZero = true;
	}
	return result;
}

} } // job/prv
