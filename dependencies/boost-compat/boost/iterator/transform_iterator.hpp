// boost::make_transform_iterator: an iterator that hands out f(*it).
#pragma once

#include <iterator>
#include <type_traits>
#include <utility>

namespace boost {

template<class F, class It>
class transform_iterator
{
public:
	using iterator_category = std::forward_iterator_tag;
	using reference = decltype(std::declval<const F&>()(*std::declval<It>()));
	using value_type = std::remove_cv_t<std::remove_reference_t<reference>>;
	using difference_type = typename std::iterator_traits<It>::difference_type;
	using pointer = void;

	transform_iterator() = default;
	transform_iterator(It it, F f) : m_it(std::move(it)), m_f(std::move(f)) {}

	reference operator*() const { return m_f(*m_it); }
	transform_iterator& operator++() { ++m_it; return *this; }
	transform_iterator operator++(int) { transform_iterator t = *this; ++m_it; return t; }
	bool operator==(const transform_iterator& o) const { return m_it == o.m_it; }
	bool operator!=(const transform_iterator& o) const { return m_it != o.m_it; }
	const It& base() const { return m_it; }

private:
	It m_it{};
	F m_f{};
};

template<class It, class F>
inline transform_iterator<F, It> make_transform_iterator(It it, F f)
{
	return transform_iterator<F, It>(std::move(it), std::move(f));
}

} // namespace boost
