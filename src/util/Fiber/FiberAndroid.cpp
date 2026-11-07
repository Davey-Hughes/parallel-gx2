#include "Fiber.h"
#include <atomic>
#include <cstdlib>
#include <string>

// bionic ships no ucontext functions - getcontext() and friends were never part
// of Android's libc - so the stack switching goes through libretro-common's
// libco instead. libco keeps the registers in the cothread it switches away
// from, so unlike swapcontext() nothing has to be stored on arrival.
#include <libco.h>

namespace
{
	struct FiberImpl
	{
		cothread_t thread{};
		void (*entryPoint)(void* userParam){};
		void* userParam{};
		bool ownsThread{};
		bool running{};
		std::string debugName;
	};

	thread_local FiberImpl* sCurrentImpl{};

	constexpr size_t kFiberStackSize = 2 * 1024 * 1024;

	// Runs on the fiber's own stack the first time it is entered.
	void FiberTrampoline()
	{
		FiberImpl* self = sCurrentImpl;
		self->entryPoint(self->userParam);
		// Cemu's fiber entry points do not return. There is no context to go
		// back to if one does, so stop here instead of running off the stack.
		cemu_assert_unimplemented();
		std::abort();
	}

	void CheckSwitchTarget(FiberImpl* leaving, FiberImpl* target)
	{
		if (!target->running) [[likely]]
			return;
		cemuLog_log(LogType::Force,
			"[Fiber] refusing to switch to \"{}\": it is running on another host thread. leaving \"{}\"",
			target->debugName.empty() ? "unnamed" : target->debugName,
			leaving->debugName.empty() ? "unnamed" : leaving->debugName);
		cemuLog_waitForFlush();
		std::abort();
	}
} // namespace

thread_local Fiber* sCurrentFiber{};

Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	FiberImpl* impl = new FiberImpl();
	impl->entryPoint = FiberEntryPoint;
	impl->userParam = userParam;
	impl->thread = co_create(kFiberStackSize, &FiberTrampoline);
	if (!impl->thread)
	{
		// there is no way back from this - the caller is about to switch to a
		// fiber that has nowhere to run - so say so rather than fault later
		cemuLog_log(LogType::Force, "[Fiber] out of memory for a {} KiB stack", kFiberStackSize / 1024);
		cemuLog_waitForFlush();
		std::abort();
	}
	impl->ownsThread = true;
	m_stackPtr = impl->thread;
	this->m_implData = impl;
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
	// fiber for the current thread: it runs on the thread's own stack
	FiberImpl* impl = new FiberImpl();
	impl->thread = co_active();
	this->m_implData = impl;
	m_stackPtr = nullptr;
}

Fiber::~Fiber()
{
	FiberImpl* impl = static_cast<FiberImpl*>(m_implData);
	if (impl->ownsThread)
		co_delete(impl->thread);
	delete impl;
}

void Fiber::SetDebugName(const char* name)
{
	FiberImpl* impl = static_cast<FiberImpl*>(m_implData);
	impl->debugName = name ? name : "";
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	cemu_assert_debug(sCurrentFiber == nullptr);
	sCurrentFiber = new Fiber(privateData);
	sCurrentImpl = static_cast<FiberImpl*>(sCurrentFiber->m_implData);
	sCurrentImpl->running = true;
	return sCurrentFiber;
}

void Fiber::Switch(Fiber& targetFiber)
{
	Fiber* leavingFiber = sCurrentFiber;
	FiberImpl* leavingImpl = static_cast<FiberImpl*>(leavingFiber->m_implData);
	FiberImpl* targetImpl = static_cast<FiberImpl*>(targetFiber.m_implData);

	// The scheduler does switch a thread to itself: when a timeslice ends the
	// thread goes back on the run queue, and __OSGetNextRunableThread() can
	// hand it straight back if it is still the best candidate on that core.
	// swapcontext() absorbs that - it saves into the context it then restores
	// from, so the call returns and the thread simply carries on. Return
	// instead of switching, which is what the callers already expect.
	if (targetImpl == leavingImpl)
		return;

	CheckSwitchTarget(leavingImpl, targetImpl);
	sCurrentFiber = &targetFiber;
	sCurrentImpl = targetImpl;
	leavingImpl->running = false;
	targetImpl->running = true;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	co_switch(targetImpl->thread);
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

void* Fiber::GetFiberPrivateData()
{
	return sCurrentFiber->m_privateData;
}
