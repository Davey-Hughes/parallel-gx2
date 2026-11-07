#include "Cafe/HW/Espresso/Recompiler/PPCRecompilerWorkQueue.h"

bool PPCRecompilerWorkQueue::Init()
{
	if (m_initialized)
		return true;
	if (!retro_spsc_init(&m_ring, kCapacity * sizeof(uint32_t)))
		return false;
	if (!retro_eventcount_init(&m_wake))
	{
		retro_eventcount_free(&m_wake);
		retro_spsc_free(&m_ring);
		return false;
	}
	m_initialized = true;
	return true;
}

void PPCRecompilerWorkQueue::Free()
{
	if (!m_initialized)
		return;
	retro_eventcount_free(&m_wake);
	retro_spsc_free(&m_ring);
	m_initialized = false;
}

bool PPCRecompilerWorkQueue::Push(uint32_t address)
{
	if (!m_initialized || retro_spsc_write_avail(&m_ring) < sizeof(address))
		return false;
	retro_spsc_write(&m_ring, &address, sizeof(address));
	retro_eventcount_notify(&m_wake);
	return true;
}

bool PPCRecompilerWorkQueue::Pop(uint32_t& address)
{
	if (!m_initialized)
		return false;
	return retro_spsc_read(&m_ring, &address, sizeof(address)) == sizeof(address);
}

void PPCRecompilerWorkQueue::WaitForWork(const std::atomic_bool& stop)
{
	if (!m_initialized)
		return;
	for (;;)
	{
		if (stop.load(std::memory_order_acquire) || retro_spsc_read_avail(&m_ring) >= sizeof(uint32_t))
			return;
		const int key = retro_eventcount_prepare_wait(&m_wake);
		if (stop.load(std::memory_order_acquire) || retro_spsc_read_avail(&m_ring) >= sizeof(uint32_t))
		{
			retro_eventcount_cancel_wait(&m_wake);
			return;
		}
		retro_eventcount_commit_wait(&m_wake, key);
	}
}

void PPCRecompilerWorkQueue::Wake()
{
	if (m_initialized)
		retro_eventcount_notify(&m_wake);
}

void PPCRecompilerWorkQueue::Clear()
{
	if (m_initialized)
		retro_spsc_clear(&m_ring);
}
