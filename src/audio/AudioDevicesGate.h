#pragma once

#include <atomic>
#include <cstdint>

#include "util/helpers/EventWake.h"

// What used to be a reader-writer lock around the audio devices (IAudioAPI.h),
// without the lock. Whatever plays through them - AX, every 3 ms - holds an
// AudioDevicesUse for the time it touches them: an atomic count and nothing
// else, never a wait. Whatever creates or destroys them - a title starting or
// stopping, a Skylander portal coming up - holds an AudioDevicesChange, which
// turns new users away (they leave the devices alone for that one call) and
// waits for the ones already in to leave. Changes take turns with each other.
namespace AudioDevicesGate
{
	inline std::atomic<uint32_t> users{0};
	inline std::atomic_bool closed{false};
	inline std::atomic_bool changing{false};
	inline ImmortalEventWake wake;
}

class AudioDevicesUse
{
public:
	AudioDevicesUse()
	{
		AudioDevicesGate::users.fetch_add(1, std::memory_order_seq_cst);
		m_ok = !AudioDevicesGate::closed.load(std::memory_order_seq_cst);
	}
	~AudioDevicesUse()
	{
		if (AudioDevicesGate::users.fetch_sub(1, std::memory_order_seq_cst) == 1)
			AudioDevicesGate::wake->Notify(); // a change waiting for the last one out
	}
	AudioDevicesUse(const AudioDevicesUse&) = delete;
	AudioDevicesUse& operator=(const AudioDevicesUse&) = delete;
	// False while the devices are being changed: leave them alone
	explicit operator bool() const { return m_ok; }

private:
	bool m_ok;
};

class AudioDevicesChange
{
public:
	AudioDevicesChange()
	{
		using namespace AudioDevicesGate;
		for (;;)
		{
			if (!changing.exchange(true, std::memory_order_acquire))
				break;
			wake->WaitUntil([] { return !changing.load(std::memory_order_acquire); });
		}
		closed.store(true, std::memory_order_seq_cst);
		wake->WaitUntil([] { return users.load(std::memory_order_seq_cst) == 0; });
	}
	~AudioDevicesChange()
	{
		using namespace AudioDevicesGate;
		closed.store(false, std::memory_order_seq_cst);
		changing.store(false, std::memory_order_release);
		wake->Notify(); // another change waiting its turn
	}
	AudioDevicesChange(const AudioDevicesChange&) = delete;
	AudioDevicesChange& operator=(const AudioDevicesChange&) = delete;
};
