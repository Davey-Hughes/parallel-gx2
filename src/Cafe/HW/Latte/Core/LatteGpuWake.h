#pragma once

#include <chrono>
#include <rthreads/retro_eventcount.h>

// What the GPU thread and the threads handing it work sleep on, instead of
// sleeping by the clock: the pipeline cache's compile threads during a load,
// the GPU thread's start, the title starting, GX2Init, the graphic packs, an
// OSScreen flip, the pause gate's release, a new renderer. Notified by each of
// those as it happens, and by anything that asks the GPU thread to stop doing
// what it is doing (a pause request, a stop), so a wait here never outlasts the
// reason for it. One for all of them: every waiter has its own predicate, so a
// wake meant for another one costs a check and nothing else.
//
// Usage, on the waiting thread, with a predicate of its own:
//   LatteGpuWake_WaitUntil([&] { return done() || Latte_GetStopSignal() ||
//                                       Latte_IsGpuPauseRequested(); });
retro_eventcount_t* LatteGpuWake_Get();
void LatteGpuWake_Notify();

template<typename TPred>
void LatteGpuWake_WaitUntil(TPred pred)
{
	retro_eventcount_t* ec = LatteGpuWake_Get();
	for (;;)
	{
		if (pred())
			return;
		const int key = retro_eventcount_prepare_wait(ec);
		if (pred())
		{
			retro_eventcount_cancel_wait(ec);
			return;
		}
		retro_eventcount_commit_wait(ec, key);
	}
}

// As above, but back by the deadline whether or not the predicate came true -
// for a waiter that has something to say when a wait runs long. The deadline
// is only ever a bound, never the thing that ends the wait: returns whether
// the predicate holds.
template<typename TPred>
bool LatteGpuWake_WaitUntil(TPred pred, std::chrono::steady_clock::time_point deadline)
{
	retro_eventcount_t* ec = LatteGpuWake_Get();
	for (;;)
	{
		if (pred())
			return true;
		const int key = retro_eventcount_prepare_wait(ec);
		if (pred())
		{
			retro_eventcount_cancel_wait(ec);
			return true;
		}
		const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0)
		{
			retro_eventcount_cancel_wait(ec);
			return false;
		}
		retro_eventcount_commit_wait_timeout(ec, key, left);
	}
}
