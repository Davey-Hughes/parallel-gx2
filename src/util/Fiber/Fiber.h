#pragma once

#if BOOST_OS_WINDOWS

#endif

class Fiber
{
public:
	Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData);
	~Fiber();

	static Fiber* PrepareCurrentThread(void* privateData = nullptr);
	static void Switch(Fiber& targetFiber);
	static void* GetFiberPrivateData();

	// Labels the fiber for crash reports. On Android it names the fiber in the
	// report of a switch to one that is already running; the other backends
	// ignore it.
	void SetDebugName(const char* name);
private:
	Fiber(void* privateData); // fiber from current thread

	void* m_implData{nullptr};
	void* m_privateData;
	void* m_stackPtr{ nullptr };
};