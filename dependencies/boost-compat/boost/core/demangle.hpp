// boost::core::demangle: the C++ ABI's demangler where there is one, the name
// as it is elsewhere.
#pragma once

#include <cstdlib>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#endif

namespace boost { namespace core {

inline std::string demangle(const char* name)
{
#if defined(__GNUC__) || defined(__clang__)
	int status = 0;
	char* d = abi::__cxa_demangle(name, nullptr, nullptr, &status);
	if (d)
	{
		std::string out(d);
		std::free(d);
		return out;
	}
#endif
	return name;
}

}} // namespace boost::core
