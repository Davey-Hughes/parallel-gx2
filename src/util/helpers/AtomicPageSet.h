#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

// A set of page numbers many threads add to and one thread drains, without a
// lock: a bit per page, a bit per 64 pages saying where to look, and one flag
// saying whether to look at all. Adding a page that is already in costs three
// loads and no write, which is the common case for something called on every
// cache-line flush the emulated CPU makes.
//
// Adding sets the page's bit, then its group's, then the flag; draining clears
// them the other way round - the flag, then each group, then its pages - so a
// page added before a drain begins is always reported, by that drain or by
// one before it.
template<size_t kPages>
class AtomicPageSet
{
	static_assert(kPages % (64 * 64) == 0, "whole summary words");
	static constexpr size_t kWords = kPages / 64;
	static constexpr size_t kSummaryWords = kWords / 64;

public:
	AtomicPageSet()
		: m_words(new std::atomic<uint64_t>[kWords]), m_summary(new std::atomic<uint64_t>[kSummaryWords])
	{
		for (size_t i = 0; i < kWords; i++)
			m_words[i].store(0, std::memory_order_relaxed);
		for (size_t i = 0; i < kSummaryWords; i++)
			m_summary[i].store(0, std::memory_order_relaxed);
	}

	// Pages [first, last], any thread
	void Add(uint32_t first, uint32_t last)
	{
		bool any = false;
		for (uint64_t page = first; page <= last && page < kPages;)
		{
			const size_t word = (size_t)(page / 64);
			const uint32_t bitFirst = (uint32_t)(page % 64);
			const uint64_t lastInWord = std::min<uint64_t>(last, (uint64_t)word * 64 + 63);
			const uint32_t bitLast = (uint32_t)(lastInWord % 64);
			const uint64_t mask = (bitLast == 63 ? ~0ull : ((1ull << (bitLast + 1)) - 1)) & ~((1ull << bitFirst) - 1);
			const uint64_t summaryBit = 1ull << (word % 64);
			// Already in, and its group and the flag still say so: nothing
			// to do (see the order above for why all three are needed)
			if ((m_words[word].load(std::memory_order_acquire) & mask) != mask ||
				!(m_summary[word / 64].load(std::memory_order_acquire) & summaryBit) ||
				!m_any.load(std::memory_order_acquire))
			{
				m_words[word].fetch_or(mask, std::memory_order_seq_cst);
				m_summary[word / 64].fetch_or(summaryBit, std::memory_order_seq_cst);
				any = true;
			}
			page = lastInWord + 1;
		}
		if (any)
			m_any.store(true, std::memory_order_seq_cst);
	}

	bool MaybeEmpty() const { return !m_any.load(std::memory_order_acquire); }

	// One thread. Calls fn(page) for every page added, and removes it.
	template<typename TFn>
	void Drain(TFn fn)
	{
		if (!m_any.exchange(false, std::memory_order_seq_cst))
			return;
		for (size_t s = 0; s < kSummaryWords; s++)
		{
			if (m_summary[s].load(std::memory_order_relaxed) == 0)
				continue;
			uint64_t groups = m_summary[s].exchange(0, std::memory_order_seq_cst);
			while (groups)
			{
				const unsigned g = CountTrailingZeros(groups);
				groups &= groups - 1;
				const size_t word = s * 64 + g;
				uint64_t bits = m_words[word].exchange(0, std::memory_order_seq_cst);
				while (bits)
				{
					const unsigned b = CountTrailingZeros(bits);
					bits &= bits - 1;
					fn((uint32_t)(word * 64 + b));
				}
			}
		}
	}

private:
	static unsigned CountTrailingZeros(uint64_t v)
	{
#if defined(_MSC_VER) && !defined(__clang__)
		unsigned long i;
		_BitScanForward64(&i, v);
		return (unsigned)i;
#else
		return (unsigned)__builtin_ctzll(v);
#endif
	}

	std::unique_ptr<std::atomic<uint64_t>[]> m_words;
	std::unique_ptr<std::atomic<uint64_t>[]> m_summary;
	std::atomic_bool m_any{false};
};
