// boost::bind and its placeholders: the standard library's.
#pragma once

#include <functional>

namespace boost {

namespace placeholders {
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;
} // namespace placeholders

template<class F, class... Args>
inline auto bind(F&& f, Args&&... args)
{
	return std::bind(std::forward<F>(f), std::forward<Args>(args)...);
}

} // namespace boost
