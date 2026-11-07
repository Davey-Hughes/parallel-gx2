#pragma once

#include <atomic>
#include <cstdint>

#include "util/helpers/SeqlockValue.h"

#if !defined(__SIZEOF_INT128__) && defined(_MSC_VER)
#include <intrin.h>
#endif

// The guest's time base, from the host's TSC, without a lock.
//
// Guest time is the TSC elapsed since an epoch, at the console's clock over the
// TSC's rate, times 8 >> the timer shift (the speed setting), plus the guest
// time at the epoch. The epoch, the rate and the shift it was taken with are
// one SeqlockValue; a read takes them and does the arithmetic, and nothing is
// written on the way. Only when the rate has been refined (the calibration
// thread publishes it) or the shift changed does a read start a new epoch at
// its own TSC, with the guest time the old one gives there, so no time already
// counted moves - and only one read does: the others go on with the old epoch
// rather than wait for it. A last-returned value, raised with a compare and
// exchange only when a read is ahead of it, keeps the time from ever going
// backwards across cores, whose TSCs can be read a little out of order.
//
// It replaces an accumulator every read added to under a spinlock - every
// guest time read on every core.
class PPCTimerClock
{
public:
	static constexpr uint64_t kConsoleClock = 1243125000ull; // Espresso::CORE_CLOCK

	// Guest time 0 at this TSC; nobody may be reading (the title starts)
	void Restart(uint64_t tsc)
	{
		m_epoch.Store(Epoch{tsc, 0, 0, 0});
		m_last.store(0, std::memory_order_relaxed);
	}

	uint64_t Read(uint64_t tsc, uint64_t freq, uint8_t shift)
	{
		Epoch e = m_epoch.Load();
		if (freq != 0 && (freq != e.freq || shift != e.shift) && !m_rebasing.exchange(true, std::memory_order_acquire))
		{
			e = m_epoch.Load(); // as it is now, with the flag held
			if (freq != e.freq || shift != e.shift)
			{
				uint64_t at = At(e, tsc);
				const uint64_t last = m_last.load(std::memory_order_relaxed);
				if (at < last)
					at = last;
				e = Epoch{tsc, at, freq, shift};
				m_epoch.Store(e);
			}
			m_rebasing.store(false, std::memory_order_release);
		}
		const uint64_t t = At(e, tsc);
		uint64_t last = m_last.load(std::memory_order_relaxed);
		while (t > last)
		{
			if (m_last.compare_exchange_weak(last, t, std::memory_order_relaxed))
				return t;
		}
		return last;
	}

private:
	struct Epoch
	{
		uint64_t tsc;
		uint64_t guest;
		uint64_t freq; // 0: no rate yet, the time stands still
		uint64_t shift;
	};

	static uint64_t At(const Epoch& e, uint64_t tsc)
	{
		if (e.freq == 0 || tsc <= e.tsc)
			return e.guest;
		const uint64_t elapsed = MulDiv(tsc - e.tsc, kConsoleClock, e.freq);
		return e.guest + ((elapsed << 3) >> e.shift);
	}

	// a * b / d with a 128-bit product; the quotient fits for centuries
	static uint64_t MulDiv(uint64_t a, uint64_t b, uint64_t d)
	{
#if defined(__SIZEOF_INT128__)
		return (uint64_t)(((unsigned __int128)a * b) / d);
#else
		uint64_t high;
		const uint64_t low = _umul128(a, b, &high);
		uint64_t remainder;
		return _udiv128(high, low, d, &remainder);
#endif
	}

	SeqlockValue<Epoch> m_epoch;
	std::atomic<uint64_t> m_last{0};
	std::atomic_bool m_rebasing{false};
};
