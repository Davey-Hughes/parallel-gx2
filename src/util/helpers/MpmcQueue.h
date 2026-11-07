#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "util/helpers/EventWake.h"
#include "util/helpers/MpscList.h"

// ConcurrentQueue for the queues several threads take from - the pipeline
// compile pools - without its mutex and condition variable. A bounded ring of
// cells with sequence numbers (Vyukov's), which any thread pushes to and pops
// from with a compare and exchange; a push that finds it full goes to a
// lock-free overflow list instead, which a pop that finds the ring empty takes
// back - its first item for itself, the rest into the ring, what does not fit
// back on the list. In order while the ring has room; past 1024 queued, the
// overflow keeps every item but not always their order. A pop with nothing to
// give sleeps on an eventcount the push notifies. The same names as
// ConcurrentQueue, so a queue moves over by its declaration alone.
template<typename T>
class MpmcQueue
{
	static constexpr size_t kCapacity = 1024;
	static constexpr size_t kMask = kCapacity - 1;

	struct Cell
	{
		std::atomic<size_t> seq;
		T data;
	};

public:
	MpmcQueue() : m_cells(new Cell[kCapacity])
	{
		for (size_t i = 0; i < kCapacity; i++)
			m_cells[i].seq.store(i, std::memory_order_relaxed);
	}
	MpmcQueue(const MpmcQueue&) = delete;
	MpmcQueue& operator=(const MpmcQueue&) = delete;

	size_t push(T item)
	{
		const size_t count = m_count.fetch_add(1, std::memory_order_relaxed) + 1;
		if (!RingPush(item))
			m_overflow.Push(std::move(item));
		m_wake.Notify();
		return count;
	}


	// Takes the oldest item if there is one; never waits
	bool peek2(T& item)
	{
		if (!RingPop(item) && !TakeOverflow(item))
			return false;
		m_count.fetch_sub(1, std::memory_order_relaxed);
		return true;
	}

	void pop(T& item)
	{
		bool got = false;
		m_wake.WaitUntil([&] { return got || (got = peek2(item)); });
	}

	T pop()
	{
		T item;
		pop(item);
		return item;
	}

	void clear()
	{
		T item;
		while (peek2(item))
			;
	}

	size_t size() const
	{
		const ptrdiff_t n = (ptrdiff_t)m_count.load(std::memory_order_relaxed);
		return n > 0 ? (size_t)n : 0;
	}

	bool empty() const { return size() == 0; }

private:
	bool RingPush(T& item)
	{
		size_t pos = m_enqueue.load(std::memory_order_relaxed);
		for (;;)
		{
			Cell& cell = m_cells[pos & kMask];
			const size_t seq = cell.seq.load(std::memory_order_acquire);
			const ptrdiff_t dif = (ptrdiff_t)seq - (ptrdiff_t)pos;
			if (dif == 0)
			{
				if (m_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
				{
					cell.data = std::move(item);
					cell.seq.store(pos + 1, std::memory_order_release);
					return true;
				}
			}
			else if (dif < 0)
				return false; // full
			else
				pos = m_enqueue.load(std::memory_order_relaxed);
		}
	}

	bool RingPop(T& item)
	{
		size_t pos = m_dequeue.load(std::memory_order_relaxed);
		for (;;)
		{
			Cell& cell = m_cells[pos & kMask];
			const size_t seq = cell.seq.load(std::memory_order_acquire);
			const ptrdiff_t dif = (ptrdiff_t)seq - (ptrdiff_t)(pos + 1);
			if (dif == 0)
			{
				if (m_dequeue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
				{
					item = std::move(cell.data);
					cell.data = T();
					cell.seq.store(pos + kMask + 1, std::memory_order_release);
					return true;
				}
			}
			else if (dif < 0)
				return false; // empty
			else
				pos = m_dequeue.load(std::memory_order_relaxed);
		}
	}

	bool TakeOverflow(T& item)
	{
		bool got = false, more = false;
		m_overflow.TakeAll([&](T&& value) {
			if (!got)
			{
				item = std::move(value);
				got = true;
				return;
			}
			if (!RingPush(value))
				m_overflow.Push(std::move(value));
			more = true;
		});
		// the rest is somewhere another pop can take it: wake those asleep
		if (more)
			m_wake.Notify();
		return got;
	}

	std::unique_ptr<Cell[]> m_cells;
	alignas(64) std::atomic<size_t> m_enqueue{0};
	alignas(64) std::atomic<size_t> m_dequeue{0};
	MpscList<T> m_overflow;
	std::atomic<size_t> m_count{0};
	EventWake m_wake;
};
