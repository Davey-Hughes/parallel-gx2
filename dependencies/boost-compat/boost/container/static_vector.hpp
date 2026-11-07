// boost::container::static_vector, on the storage in detail/inline_vector.hpp:
// never more than Capacity elements, and no heap.
#pragma once

#include "detail/inline_vector.hpp"

namespace boost { namespace container {

template<class T, std::size_t Capacity, class Options = void>
class static_vector : public detail::inline_vector<T, Capacity, false>
{
	using base = detail::inline_vector<T, Capacity, false>;
public:
	using base::base;
	static_vector() = default;
	static_vector(const static_vector&) = default;
	static_vector(static_vector&&) = default;
	static_vector& operator=(const static_vector&) = default;
	static_vector& operator=(static_vector&&) = default;
	static_vector& operator=(std::initializer_list<T> list)
	{
		base::assign(list.begin(), list.end());
		return *this;
	}
};

}} // namespace boost::container
