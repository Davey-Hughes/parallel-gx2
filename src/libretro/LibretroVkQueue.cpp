#include "LibretroVkQueue.h"

#include <atomic>
#include <cstdint>

#include "libretro.h"
#include "libretro_vulkan.h"
#include "util/helpers/EventWake.h"

namespace LibretroVkQueue
{
	namespace
	{
		// Written by the frontend thread (context_reset, and again every frame
		// in retro_run), read by the GPU thread on every submit. No lock: the
		// frontend thread used to take one exclusively every frame, and wait
		// behind a submit that was itself waiting for the frontend's queue.
		std::atomic<const retro_hw_render_interface_vulkan*> s_iface{nullptr};
		// Submits that read s_iface and have not let go of it yet
		std::atomic<uint32_t> s_inFlight{0};
		ImmortalEventWake s_released;
	} // namespace

	void SetInterface(const void* hwRenderInterfaceVulkan)
	{
		const auto* iface = static_cast<const retro_hw_render_interface_vulkan*>(hwRenderInterfaceVulkan);
		// The same one as last frame - every frame but the rare one - changes
		// nothing and waits for nothing
		if (s_iface.load(std::memory_order_relaxed) == iface)
			return;
		s_iface.exchange(iface, std::memory_order_seq_cst);
		// A submit that took the queue lock through the old one releases it
		// through the old one; the change is not done until it has
		s_released->WaitUntil([] { return s_inFlight.load(std::memory_order_seq_cst) == 0; });
	}

	Lock::Lock() : m_iface(nullptr)
	{
		s_inFlight.fetch_add(1, std::memory_order_seq_cst);
		const retro_hw_render_interface_vulkan* iface = s_iface.load(std::memory_order_seq_cst);
		// Older frontends leave these null even though they hand over a queue,
		// so both have to be there before either is called.
		if (iface && iface->lock_queue && iface->unlock_queue)
		{
			iface->lock_queue(iface->handle);
			m_iface = iface;
		}
	}

	Lock::~Lock()
	{
		if (m_iface)
		{
			const auto* iface = static_cast<const retro_hw_render_interface_vulkan*>(m_iface);
			iface->unlock_queue(iface->handle);
		}
		if (s_inFlight.fetch_sub(1, std::memory_order_seq_cst) == 1)
			s_released->Notify(); // SetInterface, waiting for this one
	}
} // namespace LibretroVkQueue
