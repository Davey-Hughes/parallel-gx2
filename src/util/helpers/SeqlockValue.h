#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// A value one thread publishes and any number of threads read, without a lock
// and without either side waiting for the other. The writer fills the next of
// four slots and then says it is the latest; a reader copies the latest slot
// and checks it was not rewritten while it read, which takes the writer four
// more publishes in that time - with one a frame, never - and is a retry, not
// a wait. A reader never waits on a write under way. The value is kept as
// atomic words, so even that retry is not a data race. For small, trivially
// copyable values: an input sample once a frame, read by the emulated CPU.
template<typename T>
class SeqlockValue
{
	static_assert(std::is_trivially_copyable_v<T>, "SeqlockValue holds plain values");
	static constexpr size_t kWords = (sizeof(T) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
	static constexpr uint32_t kSlots = 4;

public:
	SeqlockValue() { Store(T{}); }

	// One writer at a time
	void Store(const T& value)
	{
		uint64_t words[kWords] = {};
		std::memcpy(words, &value, sizeof(T));
		const uint64_t n = m_latest.load(std::memory_order_relaxed) + 1;
		Slot& slot = m_slots[n % kSlots];
		slot.seq.store(n * 2 + 1, std::memory_order_relaxed); // being written
		// Release on each word, not a fence (which TSan cannot follow): a
		// reader that sees one of them sees the slot marked as being written
		for (size_t i = 0; i < kWords; i++)
			slot.words[i].store(words[i], std::memory_order_release);
		slot.seq.store(n * 2, std::memory_order_release);
		m_latest.store(n, std::memory_order_release);
	}

	T Load() const
	{
		uint64_t words[kWords];
		for (;;)
		{
			const uint64_t n = m_latest.load(std::memory_order_acquire);
			const Slot& slot = m_slots[n % kSlots];
			if (slot.seq.load(std::memory_order_acquire) != n * 2)
				continue; // rewritten since: there is a newer latest
			for (size_t i = 0; i < kWords; i++)
				words[i] = slot.words[i].load(std::memory_order_acquire);
			if (slot.seq.load(std::memory_order_relaxed) == n * 2)
				break;
		}
		T value;
		std::memcpy(&value, words, sizeof(T));
		return value;
	}

private:
	struct Slot
	{
		std::atomic<uint64_t> seq{0};
		std::atomic<uint64_t> words[kWords]{};
	};
	Slot m_slots[kSlots];
	std::atomic<uint64_t> m_latest{0};
};
