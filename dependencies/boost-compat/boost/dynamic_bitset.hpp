// boost::dynamic_bitset: a bit set sized at run time, with the operations the
// recompiler uses. Bits past size() are kept clear.
#pragma once

#include <cstddef>
#include <vector>

namespace boost {

template<class Block = unsigned long>
class dynamic_bitset
{
	static constexpr std::size_t kBits = sizeof(Block) * 8;
public:
	using size_type = std::size_t;

	dynamic_bitset() = default;
	explicit dynamic_bitset(size_type n, unsigned long value = 0) { resize(n); if (n && value) m_blocks[0] = (Block)value; trim(); }

	size_type size() const noexcept { return m_size; }
	bool empty() const noexcept { return m_size == 0; }

	void resize(size_type n, bool value = false)
	{
		const size_type old = m_size;
		m_blocks.resize((n + kBits - 1) / kBits, value ? ~(Block)0 : (Block)0);
		m_size = n;
		if (value)
			for (size_type i = old; i < n && i < ((old + kBits - 1) / kBits) * kBits; i++)
				set(i);
		trim();
	}

	bool test(size_type i) const { return (m_blocks[i / kBits] >> (i % kBits)) & 1; }
	bool operator[](size_type i) const { return test(i); }

	dynamic_bitset& set(size_type i, bool value = true)
	{
		if (value)
			m_blocks[i / kBits] |= (Block)1 << (i % kBits);
		else
			m_blocks[i / kBits] &= ~((Block)1 << (i % kBits));
		return *this;
	}

	dynamic_bitset& set()
	{
		for (auto& b : m_blocks)
			b = ~(Block)0;
		trim();
		return *this;
	}

	dynamic_bitset& reset(size_type i) { return set(i, false); }

	dynamic_bitset& reset()
	{
		for (auto& b : m_blocks)
			b = 0;
		return *this;
	}

	bool any() const
	{
		for (auto b : m_blocks)
			if (b)
				return true;
		return false;
	}

	bool none() const { return !any(); }

	dynamic_bitset& operator&=(const dynamic_bitset& o)
	{
		for (size_type i = 0; i < m_blocks.size() && i < o.m_blocks.size(); i++)
			m_blocks[i] &= o.m_blocks[i];
		for (size_type i = o.m_blocks.size(); i < m_blocks.size(); i++)
			m_blocks[i] = 0;
		return *this;
	}

	dynamic_bitset& operator|=(const dynamic_bitset& o)
	{
		for (size_type i = 0; i < m_blocks.size() && i < o.m_blocks.size(); i++)
			m_blocks[i] |= o.m_blocks[i];
		return *this;
	}

	bool operator==(const dynamic_bitset& o) const { return m_size == o.m_size && m_blocks == o.m_blocks; }
	bool operator!=(const dynamic_bitset& o) const { return !(*this == o); }

	size_type count() const
	{
		size_type n = 0;
		for (auto b : m_blocks)
			for (; b; b &= b - 1)
				n++;
		return n;
	}

private:
	void trim()
	{
		if (m_size % kBits && !m_blocks.empty())
			m_blocks.back() &= ((Block)1 << (m_size % kBits)) - 1;
	}

	std::vector<Block> m_blocks;
	size_type m_size = 0;
};

} // namespace boost
