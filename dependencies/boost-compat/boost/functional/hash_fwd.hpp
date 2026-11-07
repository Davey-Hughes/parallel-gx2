// boost::hash_combine, with Boost's mixing.
#pragma once

#include <cstddef>
#include <functional>

namespace boost {

template<class T>
inline void hash_combine(std::size_t& seed, const T& v)
{
	seed ^= std::hash<T>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

} // namespace boost
