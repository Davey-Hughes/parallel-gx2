#pragma once

#include <rthreads/retro_eventcount.h>

// What the GPU thread sleeps on while it waits for its own helpers - the
// pipeline cache's compile threads during a load - instead of sleeping by the
// clock. Notified by those helpers as they make progress, and by anything that
// asks the GPU thread to stop doing what it is doing (a pause request, a stop),
// so a wait here never outlasts the reason for it.
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
