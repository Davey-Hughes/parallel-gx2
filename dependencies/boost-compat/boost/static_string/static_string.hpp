// boost::static_string<N>: a string of at most N characters stored inline.
#pragma once

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace boost {

template<std::size_t N, class CharT = char>
class basic_static_string
{
public:
	using size_type = std::size_t;

	basic_static_string() noexcept { m_data[0] = 0; }
	basic_static_string(const CharT* s) { assign(s); }

	size_type size() const noexcept { return m_size; }
	size_type length() const noexcept { return m_size; }
	static constexpr size_type capacity() noexcept { return N; }
	static constexpr size_type max_size() noexcept { return N; }
	bool empty() const noexcept { return m_size == 0; }
	const CharT* c_str() const noexcept { return m_data; }
	const CharT* data() const noexcept { return m_data; }
	CharT* data() noexcept { return m_data; }
	CharT& back() noexcept { return m_data[m_size - 1]; }
	const CharT& back() const noexcept { return m_data[m_size - 1]; }
	CharT& front() noexcept { return m_data[0]; }
	CharT& operator[](size_type i) noexcept { return m_data[i]; }
	const CharT& operator[](size_type i) const noexcept { return m_data[i]; }
	const CharT* begin() const noexcept { return m_data; }
	const CharT* end() const noexcept { return m_data + m_size; }
	operator std::basic_string_view<CharT>() const noexcept { return {m_data, m_size}; }

	void clear() noexcept { m_size = 0; m_data[0] = 0; }

	void push_back(CharT c)
	{
		if (m_size >= N)
			throw std::length_error("static_string::push_back");
		m_data[m_size++] = c;
		m_data[m_size] = 0;
	}

	void pop_back() noexcept
	{
		m_data[--m_size] = 0;
	}

	basic_static_string& assign(std::basic_string_view<CharT> s)
	{
		if (s.size() > N)
			throw std::length_error("static_string::assign");
		std::memcpy(m_data, s.data(), s.size() * sizeof(CharT));
		m_size = s.size();
		m_data[m_size] = 0;
		return *this;
	}

	basic_static_string& assign(const CharT* s) { return assign(std::basic_string_view<CharT>(s)); }

	int compare(std::basic_string_view<CharT> s) const noexcept
	{
		return std::basic_string_view<CharT>(m_data, m_size).compare(s);
	}

	int compare(const CharT* s) const noexcept { return compare(std::basic_string_view<CharT>(s)); }

	friend bool operator==(const basic_static_string& a, std::basic_string_view<CharT> b) { return a.compare(b) == 0; }
	friend bool operator!=(const basic_static_string& a, std::basic_string_view<CharT> b) { return a.compare(b) != 0; }

private:
	CharT m_data[N + 1];
	size_type m_size = 0;
};

template<std::size_t N>
using static_string = basic_static_string<N, char>;

} // namespace boost
