#include "Cafe/HW/Latte/Core/LatteGpuCothread.h"

#include <cstdlib>
#include <thread>

#ifdef ENABLE_OPENGL

#include <libco.h>

namespace
{
	cothread_t sCothread = nullptr;
	cothread_t sCaller = nullptr;
	bool sFinished = false;
	LatteGpuYield sYield = LatteGpuYield::Idle;
	std::thread::id sOwner;
	void (*sEntry)() = nullptr;
	// Thread-local because it is the one question other threads ask: true
	// only on the frontend's thread, and only while it is running GPU code.
	thread_local bool tInside = false;
	// Deep enough for a driver compiling a shader under the command
	// processor: what a std::thread gets on Linux.
	constexpr unsigned int kStackSize = 8 * 1024 * 1024;

	void Trampoline()
	{
		tInside = true;
		sEntry();
		// An entry that returns is an exit like any other.
		LatteGpuCothread_Exit();
	}
}

bool LatteGpuCothread_Start(void (*entry)())
{
	LatteGpuCothread_Delete(); // the last run's, if nobody freed it
	if (sCothread)
		return false; // one still running: a second would overwrite it
	sEntry = entry;
	sFinished = false;
	sYield = LatteGpuYield::Idle;
	sOwner = std::this_thread::get_id();
	sCothread = co_create(kStackSize, Trampoline);
	return sCothread != nullptr;
}

bool LatteGpuCothread_Exists()
{
	return sCothread != nullptr;
}

bool LatteGpuCothread_IsOwnerThread()
{
	return sCothread != nullptr && std::this_thread::get_id() == sOwner;
}

void LatteGpuCothread_Delete()
{
	if (sCothread && sFinished && !tInside)
	{
		co_delete(sCothread);
		sCothread = nullptr;
	}
}

bool Latte_IsGpuCothread()
{
	return sCothread != nullptr && !sFinished;
}

bool Latte_IsOnGpuCothread()
{
	return tInside;
}

void Latte_GpuYield(LatteGpuYield why)
{
	if (!tInside)
	{
		std::this_thread::yield();
		return;
	}
	sYield = why;
	tInside = false;
	co_switch(sCaller);
	tInside = true;
}

LatteGpuYield Latte_RunGpu()
{
	if (!Latte_IsGpuCothread())
		return LatteGpuYield::Finished;
	// Only the thread whose context the GPU draws with may run it, and never
	// from inside itself.
	if (tInside || std::this_thread::get_id() != sOwner)
		return LatteGpuYield::Wait;
	sCaller = co_active();
	tInside = true;
	co_switch(sCothread);
	tInside = false;
	return sFinished ? LatteGpuYield::Finished : sYield;
}

void Latte_ServeGpu()
{
	if (Latte_IsGpuCothread() && std::this_thread::get_id() == sOwner && !tInside)
	{
		// Whatever it stopped for comes from another thread; give that thread
		// the core before asking again.
		if (Latte_RunGpu() != LatteGpuYield::Frame)
			std::this_thread::yield();
		return;
	}
	std::this_thread::yield();
}

// There is no OS thread to end, so the cothread hands the frontend's thread
// back for the last time. Its stack is freed by whoever stops it; like the
// _endthreadex upstream ends its GPU thread with on Windows, nothing on it is
// unwound.
void LatteGpuCothread_Exit()
{
	sFinished = true;
	tInside = false;
	for (;;)
		co_switch(sCaller);
}

#else // no OpenGL: the GPU is always a thread of its own

bool LatteGpuCothread_Start(void (*)()) { return false; }
bool LatteGpuCothread_Exists() { return false; }
bool LatteGpuCothread_IsOwnerThread() { return false; }
void LatteGpuCothread_Delete() {}
bool Latte_IsGpuCothread() { return false; }
bool Latte_IsOnGpuCothread() { return false; }
void Latte_GpuYield(LatteGpuYield) { std::this_thread::yield(); }
LatteGpuYield Latte_RunGpu() { return LatteGpuYield::Finished; }
void Latte_ServeGpu() { std::this_thread::yield(); }
void LatteGpuCothread_Exit() { std::abort(); }

#endif // ENABLE_OPENGL
