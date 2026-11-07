#pragma once

#include <atomic>

// What a work queue holds for an item, rather than the item: whoever is to do
// the work - a pool thread that took the ticket off the queue, or the thread
// that needs it done now, or the item's own destructor when it was never done
// - takes the item out of the ticket with one exchange, and whoever finds it
// empty knows someone else has it. So an item is never taken out of the
// middle of a queue, which is what a queue would need a lock for, and a ticket
// left in the queue for an item that has gone points at nothing rather than at
// freed memory. Two owners, the queue and the item; the last to let go frees
// it.
template<typename T>
struct ClaimTicket
{
	explicit ClaimTicket(T* t) : item(t) {}

	// The item if this is the one that takes it, nullptr if it was taken
	T* Claim() { return item.exchange(nullptr, std::memory_order_acq_rel); }

	void Release()
	{
		if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
			delete this;
	}

	std::atomic<T*> item;
	std::atomic<int> refs{2};
};
