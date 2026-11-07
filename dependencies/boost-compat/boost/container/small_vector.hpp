// boost::container::small_vector, on the storage in detail/inline_vector.hpp.
#pragma once

#include "detail/inline_vector.hpp"

namespace boost { namespace container {

template<class T, std::size_t N, class Allocator = void, class Options = void>
class small_vector : public detail::inline_vector<T, N, true>
{
	using base = detail::inline_vector<T, N, true>;
public:
	using base::base;
	small_vector() = default;
	small_vector(const small_vector&) = default;
	small_vector(small_vector&&) = default;
	small_vector& operator=(const small_vector&) = default;
	small_vector& operator=(small_vector&&) = default;
	small_vector& operator=(std::initializer_list<T> list)
	{
		base::assign(list.begin(), list.end());
		return *this;
	}
};

}} // namespace boost::container
