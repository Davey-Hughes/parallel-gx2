// The OpenGL GPU as a cothread of the frontend's thread (LatteGpuCothread.h).
// Each case is something the core relies on: retro_run gets its thread back at
// every kind of wait and learns which one it was, the GPU's way out leaves a
// cothread that can be freed and started again, nothing but the owning thread
// ever runs it, and libco keeps its state per thread - the emulated cores
// switch fibers through it on Android at the same time as the GPU does here.
#include "Cafe/HW/Latte/Core/LatteGpuCothread.h"

#include <libco.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- the protocol --------------------------------------------------------

static int s_steps = 0;
static bool s_insideSeen = true;
static LatteGpuYield s_nestedRun = LatteGpuYield::Idle;

static void ProtocolEntry()
{
	s_insideSeen = Latte_IsOnGpuCothread();
	s_nestedRun = Latte_RunGpu(); // from inside itself: must not run
	s_steps = 1;
	Latte_GpuYield(LatteGpuYield::Idle);
	s_steps = 2;
	Latte_GpuYield(LatteGpuYield::Wait);
	s_steps = 3;
	Latte_GpuYield(LatteGpuYield::Frame);
	s_steps = 4;
	Latte_GpuYield(LatteGpuYield::Parked);
	s_steps = 5;
	LatteGpuCothread_Exit();
}

static void TestProtocol()
{
	CHECK(!Latte_IsGpuCothread());
	CHECK(Latte_RunGpu() == LatteGpuYield::Finished); // none to run
	CHECK(LatteGpuCothread_Start(ProtocolEntry));
	CHECK(LatteGpuCothread_Exists());
	CHECK(LatteGpuCothread_IsOwnerThread());
	CHECK(Latte_IsGpuCothread());
	CHECK(!Latte_IsOnGpuCothread());
	CHECK(s_steps == 0); // created, not run

	CHECK(Latte_RunGpu() == LatteGpuYield::Idle && s_steps == 1);
	CHECK(s_insideSeen);
	CHECK(s_nestedRun == LatteGpuYield::Wait);
	CHECK(!Latte_IsOnGpuCothread());
	CHECK(Latte_RunGpu() == LatteGpuYield::Wait && s_steps == 2);
	CHECK(Latte_RunGpu() == LatteGpuYield::Frame && s_steps == 3);
	CHECK(Latte_RunGpu() == LatteGpuYield::Parked && s_steps == 4);
	// A second start while this one lives would orphan it.
	CHECK(!LatteGpuCothread_Start(ProtocolEntry));
	CHECK(Latte_RunGpu() == LatteGpuYield::Finished && s_steps == 5);
	CHECK(!Latte_IsGpuCothread());
	CHECK(LatteGpuCothread_Exists()); // finished, not yet freed
	CHECK(Latte_RunGpu() == LatteGpuYield::Finished && s_steps == 5); // stays finished
	LatteGpuCothread_Delete();
	CHECK(!LatteGpuCothread_Exists());
}

// An entry that returns, rather than leaving through the exit, ends the same way.
static void ReturningEntry()
{
	Latte_GpuYield(LatteGpuYield::Idle);
}

static void TestReturningEntry()
{
	CHECK(LatteGpuCothread_Start(ReturningEntry));
	CHECK(Latte_RunGpu() == LatteGpuYield::Idle);
	CHECK(Latte_RunGpu() == LatteGpuYield::Finished);
	LatteGpuCothread_Delete();
	CHECK(!LatteGpuCothread_Exists());
}

// --- restart: a reset stops the GPU and starts another --------------------

static int s_runs = 0;
static void CountingEntry()
{
	s_runs++;
	for (int i = 0; i < 3; i++)
		Latte_GpuYield(LatteGpuYield::Frame);
	LatteGpuCothread_Exit();
}

static void TestRestart()
{
	for (int run = 0; run < 50; run++)
	{
		CHECK(LatteGpuCothread_Start(CountingEntry));
		int frames = 0;
		LatteGpuYield why;
		while ((why = Latte_RunGpu()) != LatteGpuYield::Finished)
			frames += why == LatteGpuYield::Frame;
		CHECK(frames == 3);
		// Not deleted here: the next start frees the last run's, as
		// Latte_Start does after a stop that could not free it.
	}
	CHECK(s_runs == 50);
	LatteGpuCothread_Delete();
	CHECK(!LatteGpuCothread_Exists());
}

// --- other threads -------------------------------------------------------

static std::atomic<int> s_otherSaw{-1};
static std::atomic<int> s_otherRun{-1};

static void WhileOtherThreadAsks()
{
	// The frontend's thread is inside the GPU now. Another thread asking
	// whether it is the cothread must hear no, and must not get to run it.
	std::thread other([] {
		s_otherSaw = Latte_IsOnGpuCothread() ? 1 : 0;
		s_otherRun = (int)Latte_RunGpu();
		Latte_ServeGpu(); // steps aside, runs nothing
		Latte_GpuYield(LatteGpuYield::Idle); // a plain yield off the cothread
	});
	other.join();
	Latte_GpuYield(LatteGpuYield::Idle);
	LatteGpuCothread_Exit();
}

static void TestOtherThreads()
{
	CHECK(LatteGpuCothread_Start(WhileOtherThreadAsks));
	std::thread outside([] {
		CHECK(!LatteGpuCothread_IsOwnerThread());
		CHECK(Latte_RunGpu() == LatteGpuYield::Wait);
	});
	outside.join();
	CHECK(Latte_RunGpu() == LatteGpuYield::Idle);
	CHECK(s_otherSaw == 0);
	CHECK(s_otherRun == (int)LatteGpuYield::Wait);
	CHECK(Latte_RunGpu() == LatteGpuYield::Finished);
	LatteGpuCothread_Delete();
}

// --- Latte_ServeGpu: the step inside a frontend-side wait ------------------

static std::atomic<bool> s_ready{false};
static void WaitsForAnotherThread()
{
	// What the launch waits for (g_isGPUInitFinished) is set by the GPU, and
	// what the GPU waits for is set by the emulated CPU.
	while (!s_ready.load())
		Latte_GpuYield(LatteGpuYield::Idle);
	LatteGpuCothread_Exit();
}

static void TestServe()
{
	CHECK(LatteGpuCothread_Start(WaitsForAnotherThread));
	std::thread cpu([] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		s_ready = true;
	});
	while (Latte_IsGpuCothread())
		Latte_ServeGpu();
	cpu.join();
	LatteGpuCothread_Delete();
	CHECK(!LatteGpuCothread_Exists());
	Latte_ServeGpu(); // nothing to serve: a yield
}

// --- stack ---------------------------------------------------------------

static volatile unsigned char s_sink;
static void UseStack(int depth)
{
	volatile unsigned char buffer[64 * 1024];
	buffer[0] = (unsigned char)depth;
	buffer[sizeof(buffer) - 1] = (unsigned char)depth;
	if (depth > 0)
		UseStack(depth - 1);
	s_sink = buffer[0] + buffer[sizeof(buffer) - 1];
}

static void DeepEntry()
{
	UseStack(80); // about 5 MiB of the 8 the GPU's stack has, as a driver's compiler might
	Latte_GpuYield(LatteGpuYield::Frame);
	LatteGpuCothread_Exit();
}

static void TestStack()
{
	CHECK(LatteGpuCothread_Start(DeepEntry));
	CHECK(Latte_RunGpu() == LatteGpuYield::Frame);
	CHECK(Latte_RunGpu() == LatteGpuYield::Finished);
	LatteGpuCothread_Delete();
}

// --- libco on several threads at once (LIBCO_MP) --------------------------
//
// Three threads ping-pong with a cothread of their own, as the emulated cores
// do through FiberAndroid, while this thread runs the GPU. Built without
// LIBCO_MP, libco keeps the running cothread in one variable for the whole
// process, and a switch on one thread saves its registers into another
// thread's cothread.

struct PingPong
{
	cothread_t main = nullptr;
	cothread_t co = nullptr;
	long count = 0;
};
static thread_local PingPong* t_pingPong = nullptr;
static constexpr long kSwitches = 200000;

static void PingPongEntry()
{
	for (;;)
	{
		t_pingPong->count++;
		co_switch(t_pingPong->main);
	}
}

static long s_gpuSpins = 0;
static void SpinningGpu()
{
	for (;;)
	{
		s_gpuSpins++;
		Latte_GpuYield(LatteGpuYield::Idle);
	}
}

static void TestMultiThreaded()
{
	std::atomic<bool> go{false};
	std::atomic<int> done{0};
	std::vector<std::thread> cores;
	std::vector<long> counts(3, -1);
	for (int i = 0; i < 3; i++)
	{
		cores.emplace_back([&, i] {
			PingPong p;
			t_pingPong = &p;
			p.main = co_active();
			p.co = co_create(256 * 1024, PingPongEntry);
			while (!go.load())
				std::this_thread::yield();
			for (long n = 0; n < kSwitches; n++)
				co_switch(p.co);
			counts[i] = p.count;
			co_delete(p.co);
			done++;
		});
	}
	CHECK(LatteGpuCothread_Start(SpinningGpu));
	go = true;
	long runs = 0;
	while (done.load() < 3)
	{
		Latte_RunGpu();
		runs++;
	}
	for (auto& t : cores)
		t.join();
	for (int i = 0; i < 3; i++)
		CHECK(counts[i] == kSwitches);
	CHECK(s_gpuSpins == runs);
	// Never exits: a cothread abandoned mid-run is what a stop off the
	// frontend's thread leaves, and the next start must refuse rather than
	// orphan it.
	CHECK(!LatteGpuCothread_Start(SpinningGpu));
}

int main()
{
	TestProtocol();
	TestReturningEntry();
	TestRestart();
	TestOtherThreads();
	TestServe();
	TestStack();
	TestMultiThreaded();
	if (s_failures)
	{
		std::fprintf(stderr, "%d check(s) failed\n", s_failures);
		return 1;
	}
	std::printf("gpu_cothread: all passed\n");
	return 0;
}
