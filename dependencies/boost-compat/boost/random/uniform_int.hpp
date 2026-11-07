// boost::random::uniform_int_distribution: the standard library's.
#pragma once

#include <random>

namespace boost { namespace random {

template<class T = int>
using uniform_int_distribution = std::uniform_int_distribution<T>;

}} // namespace boost::random
