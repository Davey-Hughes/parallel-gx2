#pragma once

#include <atomic>
#include <cstdlib>
#include <rthreads/retro_eventcount.h>

// "The thread I just started is up": set once by that thread, waited for by
// the one that started it. The starter sleeps until the flag is set rather
// than checking it by the clock - a 10 ms sleep per service was most of what
// starting the IOSU services cost, since the first check always comes before
// the new thread has run at all.
//
// No lock: an atomic flag and an eventcount, so Set never blocks.
class ThreadStartedFlag
{
public:
	ThreadStartedFlag()
	{
		if (!retro_eventcount_init(&m_ec))
			std::abort();
	}
	~ThreadStartedFlag() { retro_eventcount_free(&m_ec); }
	ThreadStartedFlag(const ThreadStartedFlag&) = delete;
	ThreadStartedFlag& operator=(const ThreadStartedFlag&) = delete;

	// Before the thread is started, by the thread that will wait.
	void Reset() { m_set.store(false, std::memory_order_release); }

	// By the started thread, once it is ready.
	void Set()
	{
		m_set.store(true, std::memory_order_release);
		retro_eventcount_notify(&m_ec);
	}

	// By the started thread, on its way out, so the flag reads as it did
	// before the start.
	void Clear() { m_set.store(false, std::memory_order_release); }

	bool IsSet() const { return m_set.load(std::memory_order_acquire); }

	void Wait()
	{
		for (;;)
		{
			if (IsSet())
				return;
			const int key = retro_eventcount_prepare_wait(&m_ec);
			if (IsSet())
			{
				retro_eventcount_cancel_wait(&m_ec);
				return;
			}
			retro_eventcount_commit_wait(&m_ec, key);
		}
	}

private:
	std::atomic_bool m_set{false};
	retro_eventcount_t m_ec{};
};
