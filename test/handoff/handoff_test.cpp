// The lock-free handoffs between the core's threads, each tested for what its
// users rely on:
//  - the recompiler's work queue delivers every address exactly once and in
//    each pusher's order, wakes its consumer on a push (no poll - a lost wake
//    hangs this test), refuses rather than grows when full, and lets a waiting
//    consumer out on a stop;
//  - the GPU thread's wake ends a wait as soon as the predicate is made true,
//    a bounded wait comes back by its deadline when it is not, and the boot's
//    back-and-forth between the GPU thread and the CPU thread - several
//    waiters, each with its own predicate, on the one wake - never loses one;
//  - the GPU's command ring as LatteCP_readU32Deprc now waits on it: every word
//    arrives, in order, and a consumer with nothing to read sleeps until a
//    submission or its vsync deadline instead of spinning;
//  - the thread-started flag the IOSU services start their threads with lets
//    the starter go as soon as the thread says so, not on a clock;
//  - the audio ring hands out whole frames in place, across the wrap, for 2 and
//    6 channels, and streams intact between a producer and a consumer thread.
#include "Cafe/HW/Espresso/Recompiler/PPCRecompilerWorkQueue.h"
#include "Cafe/HW/Latte/Core/LatteGpuWake.h"
#include "util/helpers/ThreadStartedFlag.h"
#include "libretro/LibretroAudioRing.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- recompiler work queue ----------------------------------------------

// The core's pushers are serialised by the recompiler's spinlock; this is that
static std::atomic_flag s_pushLock = ATOMIC_FLAG_INIT;

static void TestWorkQueueStream()
{
	auto q = std::make_unique<PPCRecompilerWorkQueue>();
	CHECK(q->Init());
	constexpr uint32_t kProducers = 3, kPerProducer = 60000;
	std::atomic_bool stop{false};
	std::vector<uint32_t> lastSeen(kProducers, 0);
	std::vector<uint8_t> seen(kProducers * kPerProducer + 1, 0);
	std::atomic<uint32_t> received{0};
	bool ordered = true, unique = true;

	std::thread consumer([&] {
		for (;;)
		{
			q->WaitForWork(stop);
			uint32_t a;
			while (q->Pop(a))
			{
				const uint32_t producer = a / (kPerProducer + 1);
				const uint32_t seq = a % (kPerProducer + 1);
				if (seq <= lastSeen[producer])
					ordered = false;
				lastSeen[producer] = seq;
				if (seen[producer * kPerProducer + seq - 1]++)
					unique = false;
				received.fetch_add(1, std::memory_order_relaxed);
			}
			if (stop.load())
				return;
		}
	});
	std::vector<std::thread> producers;
	for (uint32_t p = 0; p < kProducers; p++)
	{
		producers.emplace_back([&, p] {
			for (uint32_t seq = 1; seq <= kPerProducer;)
			{
				while (s_pushLock.test_and_set(std::memory_order_acquire))
					std::this_thread::yield();
				const bool pushed = q->Push(p * (kPerProducer + 1) + seq);
				s_pushLock.clear(std::memory_order_release);
				if (pushed)
					seq++;
				else
					std::this_thread::yield(); // full: the interpreter would offer it again later
			}
		});
	}
	for (auto& t : producers)
		t.join();
	while (received.load() < kProducers * kPerProducer)
		std::this_thread::yield();
	stop = true;
	q->Wake();
	consumer.join();
	CHECK(received.load() == kProducers * kPerProducer);
	CHECK(ordered);
	CHECK(unique);
	q->Free();
}

static void TestWorkQueueFullAndStop()
{
	auto q = std::make_unique<PPCRecompilerWorkQueue>();
	CHECK(!q->Push(1)); // not initialised: refused, not a crash
	CHECK(q->Init());
	size_t pushed = 0;
	while (q->Push((uint32_t)pushed + 1))
		pushed++;
	CHECK(pushed == PPCRecompilerWorkQueue::kCapacity); // bounded, and that bound
	uint32_t a = 0;
	CHECK(q->Pop(a) && a == 1);
	CHECK(q->Push(0xABCD)); // room again
	q->Clear();
	CHECK(!q->Pop(a));

	// A consumer asleep on an empty queue is woken by a push: it has been
	// asleep long enough to be past any spin when the push comes, and a lost
	// wake shows as the push never being taken
	for (int round = 0; round < 5; round++)
	{
		std::atomic_bool never{false};
		std::atomic_bool took{false};
		std::thread sleeper([&] {
			q->WaitForWork(never);
			uint32_t v;
			took = q->Pop(v) && v == 77;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		CHECK(!took.load());
		CHECK(q->Push(77));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!took.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(took.load());
		if (!took.load())
		{
			// Let it out so the test can end and say so
			never = true;
			q->Wake();
		}
		sleeper.join();
	}

	// A consumer asleep on an empty queue is let out by a stop
	std::atomic_bool stop{false};
	std::atomic_bool out{false};
	std::thread consumer([&] {
		q->WaitForWork(stop);
		out = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(!out.load());
	stop = true;
	q->Wake();
	consumer.join();
	CHECK(out.load());
	q->Free();
}

// --- GPU thread wake ----------------------------------------------------

static void TestGpuWake()
{
	for (int round = 0; round < 2000; round++)
	{
		std::atomic_bool ready{false};
		std::thread helper([&] {
			ready.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
		});
		LatteGpuWake_WaitUntil([&] { return ready.load(std::memory_order_acquire); });
		helper.join();
		CHECK(ready.load());
	}
	// Asleep past any spin when the predicate turns true
	{
		std::atomic_bool ready{false};
		std::atomic_bool done{false};
		std::thread waiter([&] {
			LatteGpuWake_WaitUntil([&] { return ready.load(std::memory_order_acquire); });
			done = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		ready.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
		waiter.join(); // a lost wake hangs here, and the runner's timeout says so
		CHECK(done.load());
	}
	// Already true: returns without waiting
	LatteGpuWake_WaitUntil([] { return true; });
}

static void TestGpuWakeDeadline()
{
	using clock = std::chrono::steady_clock;
	// Never true: back by the deadline, saying so
	{
		const auto start = clock::now();
		const bool r = LatteGpuWake_WaitUntil([] { return false; }, start + std::chrono::milliseconds(50));
		CHECK(!r);
		CHECK(clock::now() - start >= std::chrono::milliseconds(50));
	}
	// A deadline already gone: checks once and returns
	CHECK(!LatteGpuWake_WaitUntil([] { return false; }, clock::now() - std::chrono::seconds(1)));
	CHECK(LatteGpuWake_WaitUntil([] { return true; }, clock::now() - std::chrono::seconds(1)));
	// Made true long before a far deadline: woken by the notify, not the
	// deadline (the GX2Init wait's deadline is its next log line, seconds out)
	{
		std::atomic_bool ready{false};
		const auto start = clock::now();
		std::thread helper([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			ready.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
		});
		const bool r = LatteGpuWake_WaitUntil([&] { return ready.load(std::memory_order_acquire); },
			start + std::chrono::seconds(60));
		helper.join();
		CHECK(r);
		CHECK(clock::now() - start < std::chrono::seconds(30));
	}
}

// The boot as the core runs it with the GPU on a thread of its own: Latte_Start
// waits for the thread's init, the thread waits for the title to start, the
// title's init waits for the GPU's, the GPU waits for GX2Init - each a waiter
// with a predicate of its own on the one wake, and each step notified once.
static void TestGpuWakeBootHandoff()
{
	for (int round = 0; round < 1000; round++)
	{
		std::atomic_bool threadInit{false}, titleRunning{false}, gpuInitFinished{false}, gx2Init{false};
		std::thread gpu([&] {
			threadInit.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
			LatteGpuWake_WaitUntil([&] { return titleRunning.load(std::memory_order_acquire); });
			gpuInitFinished.store(true, std::memory_order_release);
			LatteGpuWake_Notify();
			LatteGpuWake_WaitUntil([&] { return gx2Init.load(std::memory_order_acquire); },
				std::chrono::steady_clock::now() + std::chrono::seconds(5));
		});
		LatteGpuWake_WaitUntil([&] { return threadInit.load(std::memory_order_acquire); });
		titleRunning.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
		LatteGpuWake_WaitUntil([&] { return gpuInitFinished.load(std::memory_order_acquire); });
		gx2Init.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
		gpu.join(); // a lost wake hangs here, and the runner's timeout says so
		CHECK(gpuInitFinished.load());
	}
}

// The command ring's idle wait: the emulated CPU submits bursts with gaps
// between them (TCLSubmitToRing, then one notify), the GPU thread reads until
// the ring is empty, then sleeps until the next submission or its next vsync
// (LatteCP_waitForWork). Every word arrives in order - a lost wake would hold a
// burst back to the vsync deadline, which the elapsed time below would show -
// and the consumer goes round its loop a handful of times per burst, not the
// millions of times a spin does.
static void TestGpuRingIdleWait()
{
	constexpr uint32_t kSize = 4096;
	static std::atomic<uint32_t> ring[kSize];
	std::atomic<uint32_t> readIndex{0}, writeIndex{0};
	std::atomic_bool done{false};
	constexpr int kBursts = 60;
	constexpr int kWords = 100;

	std::thread cpu([&] {
		uint32_t value = 0;
		for (int b = 0; b < kBursts; b++)
		{
			uint32_t w = writeIndex.load(std::memory_order_relaxed);
			for (int i = 0; i < kWords; i++)
			{
				while (((w + 1) & (kSize - 1)) == readIndex.load(std::memory_order_acquire))
					std::this_thread::yield();
				ring[w].store(value++, std::memory_order_relaxed);
				w = (w + 1) & (kSize - 1);
			}
			writeIndex.store(w, std::memory_order_release);
			LatteGpuWake_Notify();
			std::this_thread::sleep_for(std::chrono::milliseconds(3)); // the gap
		}
		done.store(true, std::memory_order_release);
		LatteGpuWake_Notify();
	});

	uint32_t expect = 0;
	uint64_t idleRounds = 0;
	const auto start = std::chrono::steady_clock::now();
	auto nextVSync = start + std::chrono::microseconds(16667);
	for (;;)
	{
		const uint32_t r = readIndex.load(std::memory_order_relaxed);
		if (r != writeIndex.load(std::memory_order_acquire))
		{
			CHECK(ring[r].load(std::memory_order_relaxed) == expect);
			expect++;
			readIndex.store((r + 1) & (kSize - 1), std::memory_order_release);
			continue;
		}
		if (done.load(std::memory_order_acquire) && r == writeIndex.load(std::memory_order_acquire))
			break;
		idleRounds++;
		if (std::chrono::steady_clock::now() >= nextVSync)
			nextVSync += std::chrono::microseconds(16667);
		LatteGpuWake_WaitUntil([&] {
			return readIndex.load(std::memory_order_relaxed) != writeIndex.load(std::memory_order_acquire) ||
				done.load(std::memory_order_acquire);
		}, nextVSync);
	}
	cpu.join();
	CHECK(expect == uint32_t(kBursts * kWords));
	// one wake per burst, one per vsync, and a few spurious ones to spare
	const auto elapsed = std::chrono::steady_clock::now() - start;
	const uint64_t vsyncs = uint64_t(elapsed / std::chrono::microseconds(16667)) + 1;
	CHECK(idleRounds <= uint64_t(kBursts) * 4 + vsyncs * 2);
	if (idleRounds > uint64_t(kBursts) * 4 + vsyncs * 2)
		std::fprintf(stderr, "ring idle wait: %llu rounds for %d bursts\n", (unsigned long long)idleRounds, kBursts);
}

// --- thread-started flag ------------------------------------------------

static void TestThreadStartedFlag()
{
	ThreadStartedFlag flag;
	CHECK(!flag.IsSet());
	// What the starter of a service does, again and again. With the 10 ms
	// sleep it replaced this took 3 s at the very least; woken by the thread
	// it is a few thread starts.
	const auto start = std::chrono::steady_clock::now();
	for (int round = 0; round < 300; round++)
	{
		std::atomic_bool stop{false};
		flag.Reset();
		std::thread service([&] {
			flag.Set();
			while (!stop.load(std::memory_order_acquire))
				std::this_thread::yield();
			flag.Clear();
		});
		flag.Wait();
		CHECK(flag.IsSet());
		stop.store(true, std::memory_order_release);
		service.join();
		CHECK(!flag.IsSet());
	}
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
	// Set before the wait: returns without waiting
	flag.Set();
	flag.Wait();
	CHECK(flag.IsSet());
}

// --- audio ring ---------------------------------------------------------

static void TestAudioRingSpans(unsigned channels)
{
	auto ring = std::make_unique<LibretroAudioRingBuffer>();
	int16_t next = 0, expect = 0;
	// Many laps of the ring in uneven pieces: every span is whole frames,
	// and what comes out is what went in
	for (int round = 0; round < 4000; round++)
	{
		const size_t frames = 1 + (round * 7919) % 3000;
		size_t want = frames * channels;
		while (want)
		{
			int16_t* dst;
			const size_t n = ring->Reserve(&dst, want);
			if (n == 0)
				break;
			CHECK(n % channels == 0);
			for (size_t i = 0; i < n; i++)
				dst[i] = next++;
			ring->Commit(n);
			want -= n;
		}
		size_t drain = ((round % 3) == 0) ? ring->GetReadAvailableSamples() : frames * channels / 2;
		drain -= drain % channels;
		while (drain)
		{
			const int16_t* src;
			const size_t n = ring->Peek(&src, drain);
			if (n == 0)
				break;
			CHECK(n % channels == 0);
			for (size_t i = 0; i < n; i++)
				if (src[i] != expect++)
				{
					s_failures++;
					std::fprintf(stderr, "FAIL audio ring: sample out of order (%u channels)\n", channels);
					return;
				}
			ring->Consume(n);
			drain -= n;
		}
	}
	// Never more than the ring less one frame
	int16_t* dst;
	ring->Reset();
	size_t total = 0;
	for (;;)
	{
		const size_t n = ring->Reserve(&dst, LibretroAudioRingBuffer::kBufferSamples);
		if (!n)
			break;
		ring->Commit(n);
		total += n;
	}
	CHECK(total == LibretroAudioRingBuffer::kBufferSamples - LibretroAudioRingBuffer::kChannels);
}

static void TestAudioRingThreads()
{
	auto ring = std::make_unique<LibretroAudioRingBuffer>();
	constexpr size_t kTotal = 8u * 1024 * 1024;
	std::atomic_bool bad{false};
	std::thread producer([&] {
		int16_t v = 0;
		size_t done = 0;
		while (done < kTotal)
		{
			int16_t* dst;
			size_t n = ring->Reserve(&dst, std::min<size_t>(kTotal - done, 960 * 2));
			n -= n % 2;
			if (!n)
			{
				std::this_thread::yield();
				continue;
			}
			for (size_t i = 0; i < n; i++)
				dst[i] = v++;
			ring->Commit(n);
			done += n;
		}
	});
	int16_t expect = 0;
	size_t got = 0;
	while (got < kTotal)
	{
		const int16_t* src;
		const size_t n = ring->Peek(&src, 1600);
		if (!n)
		{
			std::this_thread::yield();
			continue;
		}
		for (size_t i = 0; i < n; i++)
			if (src[i] != expect++)
				bad = true;
		ring->Consume(n);
		got += n;
	}
	producer.join();
	CHECK(!bad.load());
	CHECK(got == kTotal);
}

int main()
{
	TestWorkQueueStream();
	TestWorkQueueFullAndStop();
	TestGpuWake();
	TestGpuWakeDeadline();
	TestGpuWakeBootHandoff();
	TestGpuRingIdleWait();
	TestThreadStartedFlag();
	TestAudioRingSpans(2);
	TestAudioRingSpans(6);
	TestAudioRingThreads();
	if (s_failures)
	{
		std::fprintf(stderr, "%d check(s) failed\n", s_failures);
		return 1;
	}
	std::printf("handoff: all passed\n");
	return 0;
}
