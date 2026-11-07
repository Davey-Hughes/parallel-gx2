// The storage behind boost::container::small_vector and static_vector in this
// tree: up to N elements live inside the object, and a small_vector moves to
// the heap past that while a static_vector stops there (as Boost does, with an
// exception). Only the subset of the Boost interface the emulator uses, with
// the same semantics.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace boost { namespace container { namespace detail {

template<class T, std::size_t N, bool CanGrow>
class inline_vector
{
public:
	using value_type = T;
	using size_type = std::size_t;
	using difference_type = std::ptrdiff_t;
	using reference = T&;
	using const_reference = const T&;
	using pointer = T*;
	using const_pointer = const T*;
	using iterator = T*;
	using const_iterator = const T*;
	using reverse_iterator = std::reverse_iterator<iterator>;
	using const_reverse_iterator = std::reverse_iterator<const_iterator>;

	inline_vector() noexcept : m_data(inline_data()), m_size(0), m_capacity(N) {}

	explicit inline_vector(size_type count) : inline_vector()
	{
		resize(count);
	}

	inline_vector(size_type count, const T& value) : inline_vector()
	{
		assign(count, value);
	}

	template<class It, class = typename std::iterator_traits<It>::iterator_category>
	inline_vector(It first, It last) : inline_vector()
	{
		assign(first, last);
	}

	inline_vector(std::initializer_list<T> list) : inline_vector()
	{
		assign(list.begin(), list.end());
	}

	inline_vector(const inline_vector& other) : inline_vector()
	{
		assign(other.begin(), other.end());
	}

	inline_vector(inline_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) : inline_vector()
	{
		take(std::move(other));
	}

	~inline_vector()
	{
		clear();
		release_heap();
	}

	inline_vector& operator=(const inline_vector& other)
	{
		if (this != &other)
			assign(other.begin(), other.end());
		return *this;
	}

	inline_vector& operator=(inline_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
	{
		if (this != &other)
		{
			clear();
			take(std::move(other));
		}
		return *this;
	}

	inline_vector& operator=(std::initializer_list<T> list)
	{
		assign(list.begin(), list.end());
		return *this;
	}

	void assign(size_type count, const T& value)
	{
		clear();
		reserve(count);
		for (size_type i = 0; i < count; i++)
			::new ((void*)(m_data + i)) T(value);
		m_size = count;
	}

	template<class It, class = typename std::iterator_traits<It>::iterator_category>
	void assign(It first, It last)
	{
		clear();
		if constexpr (std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<It>::iterator_category>)
			reserve((size_type)std::distance(first, last));
		for (; first != last; ++first)
			emplace_back(*first);
	}

	void assign(std::initializer_list<T> list) { assign(list.begin(), list.end()); }

	reference at(size_type pos)
	{
		if (pos >= m_size)
			throw std::out_of_range("inline_vector::at");
		return m_data[pos];
	}

	const_reference at(size_type pos) const
	{
		if (pos >= m_size)
			throw std::out_of_range("inline_vector::at");
		return m_data[pos];
	}

	reference operator[](size_type pos) noexcept { return m_data[pos]; }
	const_reference operator[](size_type pos) const noexcept { return m_data[pos]; }
	reference front() noexcept { return m_data[0]; }
	const_reference front() const noexcept { return m_data[0]; }
	reference back() noexcept { return m_data[m_size - 1]; }
	const_reference back() const noexcept { return m_data[m_size - 1]; }
	T* data() noexcept { return m_data; }
	const T* data() const noexcept { return m_data; }

	iterator begin() noexcept { return m_data; }
	const_iterator begin() const noexcept { return m_data; }
	const_iterator cbegin() const noexcept { return m_data; }
	iterator end() noexcept { return m_data + m_size; }
	const_iterator end() const noexcept { return m_data + m_size; }
	const_iterator cend() const noexcept { return m_data + m_size; }
	reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
	const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
	reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
	const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

	bool empty() const noexcept { return m_size == 0; }
	size_type size() const noexcept { return m_size; }
	size_type capacity() const noexcept { return m_capacity; }
	size_type max_size() const noexcept { return CanGrow ? (size_type)-1 / sizeof(T) : N; }
	static constexpr size_type static_capacity = N;

	void reserve(size_type count)
	{
		if (count > m_capacity)
			grow_to(count);
	}

	void shrink_to_fit() noexcept {}

	void clear() noexcept
	{
		std::destroy(m_data, m_data + m_size);
		m_size = 0;
	}

	template<class... Args>
	reference emplace_back(Args&&... args)
	{
		if (m_size == m_capacity)
			grow_to(next_capacity(m_size + 1));
		T* p = ::new ((void*)(m_data + m_size)) T(std::forward<Args>(args)...);
		m_size++;
		return *p;
	}

	void push_back(const T& value) { emplace_back(value); }
	void push_back(T&& value) { emplace_back(std::move(value)); }

	void pop_back() noexcept
	{
		m_size--;
		std::destroy_at(m_data + m_size);
	}

	template<class... Args>
	iterator emplace(const_iterator pos, Args&&... args)
	{
		const size_type index = (size_type)(pos - m_data);
		if (index == m_size)
		{
			emplace_back(std::forward<Args>(args)...);
			return m_data + index;
		}
		// built before the move, as the argument may refer into this vector
		T value(std::forward<Args>(args)...);
		emplace_back(std::move(m_data[m_size - 1]));
		std::move_backward(m_data + index, m_data + m_size - 2, m_data + m_size - 1);
		m_data[index] = std::move(value);
		return m_data + index;
	}

	iterator insert(const_iterator pos, const T& value) { return emplace(pos, value); }
	iterator insert(const_iterator pos, T&& value) { return emplace(pos, std::move(value)); }

	iterator insert(const_iterator pos, size_type count, const T& value)
	{
		const size_type index = (size_type)(pos - m_data);
		const T copy(value);
		for (size_type i = 0; i < count; i++)
			emplace(m_data + index + i, copy);
		return m_data + index;
	}

	template<class It, class = typename std::iterator_traits<It>::iterator_category>
	iterator insert(const_iterator pos, It first, It last)
	{
		const size_type index = (size_type)(pos - m_data);
		const size_type oldSize = m_size;
		for (; first != last; ++first)
			emplace_back(*first);
		std::rotate(m_data + index, m_data + oldSize, m_data + m_size);
		return m_data + index;
	}

	iterator insert(const_iterator pos, std::initializer_list<T> list)
	{
		return insert(pos, list.begin(), list.end());
	}

	iterator erase(const_iterator pos)
	{
		return erase(pos, pos + 1);
	}

	iterator erase(const_iterator first, const_iterator last)
	{
		T* f = m_data + (first - m_data);
		T* l = m_data + (last - m_data);
		if (f != l)
		{
			T* newEnd = std::move(l, m_data + m_size, f);
			std::destroy(newEnd, m_data + m_size);
			m_size = (size_type)(newEnd - m_data);
		}
		return f;
	}

	void resize(size_type count)
	{
		if (count < m_size)
		{
			std::destroy(m_data + count, m_data + m_size);
			m_size = count;
			return;
		}
		reserve(count);
		for (; m_size < count; m_size++)
			::new ((void*)(m_data + m_size)) T();
	}

	void resize(size_type count, const T& value)
	{
		if (count < m_size)
		{
			std::destroy(m_data + count, m_data + m_size);
			m_size = count;
			return;
		}
		reserve(count);
		for (; m_size < count; m_size++)
			::new ((void*)(m_data + m_size)) T(value);
	}

	void swap(inline_vector& other)
	{
		inline_vector tmp(std::move(other));
		other = std::move(*this);
		*this = std::move(tmp);
	}

	friend bool operator==(const inline_vector& a, const inline_vector& b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
	}

	friend bool operator!=(const inline_vector& a, const inline_vector& b) { return !(a == b); }

	friend bool operator<(const inline_vector& a, const inline_vector& b)
	{
		return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
	}

private:
	static constexpr std::size_t kInline = N ? N : 1;

	T* inline_data() noexcept { return std::launder(reinterpret_cast<T*>(m_inline)); }
	bool is_inline() const noexcept { return m_data == reinterpret_cast<const T*>(m_inline); }

	size_type next_capacity(size_type needed) const
	{
		size_type c = m_capacity ? m_capacity * 2 : 4;
		return c < needed ? needed : c;
	}

	void grow_to(size_type count)
	{
		if constexpr (!CanGrow)
		{
			throw std::bad_alloc();
		}
		else
		{
			T* p = static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t(alignof(T))));
			std::uninitialized_move(m_data, m_data + m_size, p);
			std::destroy(m_data, m_data + m_size);
			release_heap();
			m_data = p;
			m_capacity = count;
		}
	}

	void release_heap() noexcept
	{
		if (!is_inline())
			::operator delete(m_data, std::align_val_t(alignof(T)));
		m_data = inline_data();
		m_capacity = N;
	}

	// other is left empty; this is empty on entry
	void take(inline_vector&& other)
	{
		if (!other.is_inline())
		{
			release_heap();
			m_data = other.m_data;
			m_size = other.m_size;
			m_capacity = other.m_capacity;
			other.m_data = other.inline_data();
			other.m_size = 0;
			other.m_capacity = N;
			return;
		}
		reserve(other.m_size);
		std::uninitialized_move(other.m_data, other.m_data + other.m_size, m_data);
		m_size = other.m_size;
		other.clear();
	}

	T* m_data;
	size_type m_size;
	size_type m_capacity;
	alignas(T) unsigned char m_inline[kInline * sizeof(T)];
};

}}} // namespace boost::container::detail
