#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "util/helpers/EventWake.h"

// A value read all the time and replaced now and then, without a lock on the
// way in: a reader counts itself in, takes the current pointer, and reads an
// object nothing will change or free while it is counted in. A writer builds
// the new value, swaps it in, and frees the old one once every reader that
// may still hold it has left - readers counted in before the swap, told apart
// from the ones after it by which of two counters they used. Readers never
// wait; writers take turns with each other and wait only for readers already
// inside. For configuration a game reads every frame and a menu changes - the
// emulated controllers and their mappings.
template<typename T>
class RcuCell
{
public:
	RcuCell() : m_ptr(new T()) {}
	explicit RcuCell(T initial) : m_ptr(new T(std::move(initial))) {}
	~RcuCell() { delete m_ptr.load(std::memory_order_relaxed); }
	RcuCell(const RcuCell&) = delete;
	RcuCell& operator=(const RcuCell&) = delete;

	class Reader
	{
	public:
		explicit Reader(const RcuCell& cell) : m_cell(cell)
		{
			for (;;)
			{
				m_slot = m_cell.m_epoch.load(std::memory_order_seq_cst) & 1;
				m_cell.m_readers[m_slot].fetch_add(1, std::memory_order_seq_cst);
				if ((m_cell.m_epoch.load(std::memory_order_seq_cst) & 1) == m_slot)
					break;
				Leave(); // a writer moved on in between: count in on the new side
			}
			m_ptr = m_cell.m_ptr.load(std::memory_order_seq_cst);
		}
		~Reader() { Leave(); }
		Reader(const Reader&) = delete;
		Reader& operator=(const Reader&) = delete;

		const T& operator*() const { return *m_ptr; }
		const T* operator->() const { return m_ptr; }

	private:
		void Leave()
		{
			if (m_cell.m_readers[m_slot].fetch_sub(1, std::memory_order_seq_cst) == 1)
				m_cell.m_wake.Notify(); // a writer waiting for the last one out
		}

		const RcuCell& m_cell;
		uint32_t m_slot;
		const T* m_ptr;
	};

	Reader Read() const { return Reader(*this); }

	// make() builds the new value, in this writer's turn - so it may read
	// state only writers change, as the previous writer left it
	template<typename TMake>
	void Write(TMake make)
	{
		BeginTurn();
		T* fresh = new T(make());
		T* old = m_ptr.exchange(fresh, std::memory_order_seq_cst);
		// readers that may hold old counted in on the current side: move
		// new readers to the other one, and wait for these to leave
		const uint32_t slot = m_epoch.fetch_add(1, std::memory_order_seq_cst) & 1;
		m_wake.WaitUntil([this, slot] { return m_readers[slot].load(std::memory_order_seq_cst) == 0; });
		EndTurn();
		delete old;
	}

private:
	void BeginTurn()
	{
		for (;;)
		{
			if (!m_writing.exchange(true, std::memory_order_acquire))
				return;
			m_wake.WaitUntil([this] { return !m_writing.load(std::memory_order_acquire); });
		}
	}
	void EndTurn()
	{
		m_writing.store(false, std::memory_order_release);
		m_wake.Notify();
	}

	std::atomic<T*> m_ptr;
	std::atomic<uint32_t> m_epoch{0};
	mutable std::atomic<uint32_t> m_readers[2]{};
	std::atomic_bool m_writing{false};
	mutable EventWake m_wake;
};
