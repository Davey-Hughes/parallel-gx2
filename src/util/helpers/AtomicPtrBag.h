#pragma once

#include <atomic>
#include <cstddef>

// A set of pointers threads add to and remove from, without a lock - for the
// pipelines that use a shader or a framebuffer, which the pipeline cache's
// compile threads register all at once while a title loads. Slots live in a
// list that only ever grows at its head and is freed with the bag, so walking
// it is always safe; a removed pointer leaves its slot empty, and the next add
// takes an empty slot before it makes a new one, so the list stays as long as
// the most pointers it has held at once.
//
// ForEachTaken is for the owner's destruction, when it deletes what is left:
// it takes each pointer out of its slot before handing it over, so a pointer
// is handed over once even if its own destructor removes it again.
template<typename T>
class AtomicPtrBag
{
	struct Slot
	{
		std::atomic<T*> ptr;
		Slot* next;
	};

public:
	AtomicPtrBag() = default;
	AtomicPtrBag(const AtomicPtrBag&) = delete;
	AtomicPtrBag& operator=(const AtomicPtrBag&) = delete;
	~AtomicPtrBag()
	{
		Slot* s = m_head.load(std::memory_order_acquire);
		while (s)
		{
			Slot* next = s->next;
			delete s;
			s = next;
		}
	}

	void Add(T* p)
	{
		for (Slot* s = m_head.load(std::memory_order_acquire); s; s = s->next)
		{
			T* expected = nullptr;
			if (s->ptr.load(std::memory_order_relaxed) == nullptr &&
				s->ptr.compare_exchange_strong(expected, p, std::memory_order_acq_rel))
				return;
		}
		Slot* slot = new Slot{{p}, m_head.load(std::memory_order_relaxed)};
		while (!m_head.compare_exchange_weak(slot->next, slot, std::memory_order_release, std::memory_order_relaxed))
			;
	}

	// False if p was not in it
	bool Remove(T* p)
	{
		for (Slot* s = m_head.load(std::memory_order_acquire); s; s = s->next)
		{
			T* expected = p;
			if (s->ptr.load(std::memory_order_relaxed) == p &&
				s->ptr.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel))
				return true;
		}
		return false;
	}

	bool Empty() const
	{
		for (Slot* s = m_head.load(std::memory_order_acquire); s; s = s->next)
		{
			if (s->ptr.load(std::memory_order_acquire))
				return false;
		}
		return true;
	}

	template<typename TFn>
	void ForEachTaken(TFn fn)
	{
		for (Slot* s = m_head.load(std::memory_order_acquire); s; s = s->next)
		{
			if (T* p = s->ptr.exchange(nullptr, std::memory_order_acq_rel))
				fn(p);
		}
	}

private:
	std::atomic<Slot*> m_head{nullptr};
};
