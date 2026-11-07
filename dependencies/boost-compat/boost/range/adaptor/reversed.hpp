// range | boost::adaptors::reversed: the range walked back to front.
#pragma once

#include <iterator>

namespace boost { namespace adaptors {

namespace detail {

struct reversed_tag {};

template<class R>
struct reversed_range
{
	R& r;
	auto begin() const { return std::rbegin(r); }
	auto end() const { return std::rend(r); }
};

template<class R>
inline reversed_range<R> operator|(R& r, reversed_tag) { return reversed_range<R>{r}; }

template<class R>
inline reversed_range<const R> operator|(const R& r, reversed_tag) { return reversed_range<const R>{r}; }

} // namespace detail

inline constexpr detail::reversed_tag reversed{};

}} // namespace boost::adaptors
