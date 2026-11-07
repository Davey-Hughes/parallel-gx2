#pragma once

#include <chrono>
#include <cstdlib>
#include <new>
#include <rthreads/retro_eventcount.h>

// Something to sleep on until a condition comes true, instead of checking it
// by the clock: whoever makes the condition true calls Notify, and a waiter
// sleeps until then with its own predicate. No lock - an eventcount, so Notify
// never blocks and costs an atomic increment when nobody is asleep.
//
// For waits whose notifying side is a detached thread that can outlive static
// destruction, use ImmortalEventWake below.
class EventWake
{
public:
	EventWake()
	{
		if (!retro_eventcount_init(&m_ec))
			std::abort();
	}
	~EventWake() { retro_eventcount_free(&m_ec); }
	EventWake(const EventWake&) = delete;
	EventWake& operator=(const EventWake&) = delete;

	void Notify() { retro_eventcount_notify(&m_ec); }

	template<typename TPred>
	void WaitUntil(TPred pred)
	{
		for (;;)
		{
			if (pred())
				return;
			const int key = retro_eventcount_prepare_wait(&m_ec);
			if (pred())
			{
				retro_eventcount_cancel_wait(&m_ec);
				return;
			}
			retro_eventcount_commit_wait(&m_ec, key);
		}
	}

	// By the deadline at the latest; returns whether the predicate holds
	template<typename TPred>
	bool WaitUntil(TPred pred, std::chrono::steady_clock::time_point deadline)
	{
		for (;;)
		{
			if (pred())
				return true;
			const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
				deadline - std::chrono::steady_clock::now()).count();
			if (left <= 0)
				return pred();
			const int key = retro_eventcount_prepare_wait(&m_ec);
			if (pred())
			{
				retro_eventcount_cancel_wait(&m_ec);
				return true;
			}
			retro_eventcount_commit_wait_timeout(&m_ec, key, left);
		}
	}

private:
	retro_eventcount_t m_ec{};
};

// An EventWake that static destruction leaves alone: a detached thread still
// notifying it as the process exits finds it as it was. Built in place in its
// own storage rather than on the heap, so it is not a leak either.
class ImmortalEventWake
{
public:
	ImmortalEventWake() { new (m_storage) EventWake(); }
	ImmortalEventWake(const ImmortalEventWake&) = delete;
	ImmortalEventWake& operator=(const ImmortalEventWake&) = delete;

	EventWake& Get() { return *std::launder(reinterpret_cast<EventWake*>(m_storage)); }
	EventWake* operator->() { return &Get(); }

private:
	alignas(EventWake) unsigned char m_storage[sizeof(EventWake)];
};
