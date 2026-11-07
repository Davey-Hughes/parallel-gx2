#include "Cafe/HW/Espresso/Const.h"
#include "config/ActiveSettings.h"
#include "util/helpers/fspinlock.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Common/cpu_features.h"

#include <atomic>
#include <thread>
#include <rthreads/retro_eventcount.h>

#if defined(ARCH_X86_64)
#include <immintrin.h>
#pragma intrinsic(__rdtsc)
#endif

uint64 _rdtscLastMeasure = 0;
// Published by the calibration thread, first after 25 ms and then twice more
// as it gets better (PPCTimer_initThread); read under sTimerSpinlock
static std::atomic<uint64> _rdtscFrequency{0};
// The title start waits on this for the first estimate (PPCTimer_waitForInit)
static retro_eventcount_t sTimerReady{};
static bool sTimerReadyOk = false;

struct uint128_t
{
	uint64 low;
	uint64 high;
};

static_assert(sizeof(uint128_t) == 16);

uint128_t _rdtscAcc{};

uint64 muldiv64(uint64 a, uint64 b, uint64 d)
{
	uint64 diva = a / d;
	uint64 moda = a % d;
	uint64 divb = b / d;
	uint64 modb = b % d;
	return diva * b + moda * divb + moda * modb / d;
}

// The TSC's rate, measured against the high resolution timer. Upstream
// measured once, for three seconds, and the title start waited for it - three
// seconds off every boot that came sooner than that after the core was loaded,
// which in RetroArch is every boot. The two clocks are read back to back, so
// the error is the gap between those reads (well under a microsecond) over the
// time measured: 25 ms is already good to about one part in 10^5. So the first
// estimate is published after 25 ms, and the same measurement, still running,
// replaces it at 200 ms and at 3 s with the one upstream would have had. The
// rate is only ever a divisor of elapsed TSC ticks (PPCTimer_getFromRDTSC), so
// a refined one changes how fast guest time runs by that one part in 10^5 from
// then on, and moves no time already counted.
int PPCTimer_initThread()
{
    #if defined(ARCH_X86_64)
	if (!g_CPUFeatures.x86.invariant_tsc)
		cemuLog_log(LogType::Force, "Invariant TSC not supported");
    #endif

	_mm_mfence();
	const uint64 tscStart = __rdtsc();
	const HRTick startTick = HighResolutionTimer::now().getTick();
	const auto start = std::chrono::steady_clock::now();
	for (const int ms : { 25, 200, 3000 })
	{
		std::this_thread::sleep_until(start + std::chrono::milliseconds(ms));
		_mm_mfence();
		const HRTick stopTick = HighResolutionTimer::now().getTick();
		const uint64 tscEnd = __rdtsc();
		uint64 hrtFreq = 0;
		const uint64 hrtDiff = HighResolutionTimer::getTimeDiffEx(startTick, stopTick, hrtFreq);
		if (hrtDiff == 0)
			continue;
		const uint64 tscFreq = muldiv64(tscEnd - tscStart, hrtFreq, hrtDiff);
		if (tscFreq == 0)
			continue;
		_rdtscFrequency.store(tscFreq, std::memory_order_release);
		if (sTimerReadyOk)
			retro_eventcount_notify(&sTimerReady);
	}
	return 0;
}

void PPCTimer_init()
{
	if (!sTimerReadyOk)
		sTimerReadyOk = retro_eventcount_init(&sTimerReady);
	std::thread t(PPCTimer_initThread);
	t.detach();
	_rdtscLastMeasure = __rdtsc();
}

uint64 _tickSummary = 0;

void PPCTimer_start()
{
	_rdtscLastMeasure = __rdtsc();
	_tickSummary = 0;
}

uint64 PPCTimer_getRawTsc()
{
	return __rdtsc();
}

uint64 PPCTimer_microsecondsToTsc(uint64 us)
{
	return (us * _rdtscFrequency.load(std::memory_order_relaxed)) / 1000000ULL;
}

uint64 PPCTimer_tscToMicroseconds(uint64 us)
{
	uint128_t r{};
	r.low = _umul128(us, 1000000ULL, &r.high);

	uint64 remainder;
	const uint64 microseconds = _udiv128(r.high, r.low, _rdtscFrequency.load(std::memory_order_relaxed), &remainder);

	return microseconds;
}

bool PPCTimer_isReady()
{
	return _rdtscFrequency.load(std::memory_order_acquire) != 0;
}

void PPCTimer_waitForInit()
{
	// Asleep until the first estimate, 25 ms after PPCTimer_init at most
	while (!PPCTimer_isReady())
	{
		if (!sTimerReadyOk)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		const int key = retro_eventcount_prepare_wait(&sTimerReady);
		if (PPCTimer_isReady())
		{
			retro_eventcount_cancel_wait(&sTimerReady);
			break;
		}
		retro_eventcount_commit_wait(&sTimerReady, key);
	}
}

FSpinlock sTimerSpinlock;

// thread safe
uint64 PPCTimer_getFromRDTSC()
{
	sTimerSpinlock.lock();
	_mm_mfence();
	uint64 rdtscCurrentMeasure = __rdtsc();
	uint64 rdtscDif = rdtscCurrentMeasure - _rdtscLastMeasure;
	// optimized max(rdtscDif, 0) without conditionals
	rdtscDif = rdtscDif & ~(uint64)((sint64)rdtscDif >> 63);

	uint128_t diff{};
	diff.low = _umul128(rdtscDif, Espresso::CORE_CLOCK, &diff.high);

	if(rdtscCurrentMeasure > _rdtscLastMeasure)
		_rdtscLastMeasure = rdtscCurrentMeasure; // only travel forward in time

	uint8 c = 0;
	#if BOOST_OS_WINDOWS
	c = _addcarry_u64(c, _rdtscAcc.low, diff.low, &_rdtscAcc.low);
	_addcarry_u64(c, _rdtscAcc.high, diff.high, &_rdtscAcc.high);
	#else
	// requires casting because of long / long long nonesense
	c = _addcarry_u64(c, _rdtscAcc.low, diff.low, (unsigned long long*)&_rdtscAcc.low);
	_addcarry_u64(c, _rdtscAcc.high, diff.high, (unsigned long long*)&_rdtscAcc.high);
	#endif

	uint64 remainder;
	uint64 elapsedTick = _udiv128(_rdtscAcc.high, _rdtscAcc.low, _rdtscFrequency.load(std::memory_order_relaxed), &remainder);

	_rdtscAcc.low = remainder;
	_rdtscAcc.high = 0;

	// timer scaling
	elapsedTick <<= 3ull; // *8
	uint8 timerShiftFactor = ActiveSettings::GetTimerShiftFactor();
	elapsedTick >>= timerShiftFactor;

	_tickSummary += elapsedTick;

	sTimerSpinlock.unlock();
	return _tickSummary;
}
