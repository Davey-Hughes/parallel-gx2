// The Boost.StringAlgo predicates and case helpers the emulator uses. Case is
// folded per character with the C library's tolower/towlower, which is what
// Boost does under the default ("C") locale.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>

namespace boost { namespace algorithm {

namespace detail {

template<class C>
inline C fold(C c)
{
	if constexpr (sizeof(C) == 1)
		return (C)std::tolower((unsigned char)c);
	else
		return (C)std::towlower((std::wint_t)c);
}

template<class C>
inline C fold_upper(C c)
{
	if constexpr (sizeof(C) == 1)
		return (C)std::toupper((unsigned char)c);
	else
		return (C)std::towupper((std::wint_t)c);
}

template<class T>
struct char_of
{
	using type = std::remove_cv_t<std::remove_reference_t<decltype(*std::begin(std::declval<const T&>()))>>;
};
template<class C> struct char_of<C*> { using type = std::remove_cv_t<C>; };
template<class C> struct char_of<const C*> { using type = std::remove_cv_t<C>; };
template<class C, std::size_t N> struct char_of<C[N]> { using type = std::remove_cv_t<C>; };

// Every argument as a view: strings, views, string literals and char pointers.
template<class T>
inline auto view(const T& s)
{
	using C = typename char_of<std::decay_t<T>>::type;
	if constexpr (std::is_pointer_v<std::decay_t<T>>)
		return std::basic_string_view<C>(s);
	else
		return std::basic_string_view<C>(std::data(s), std::size(s));
}

// The two sides may differ in character type (a std::string_view against a
// wide literal); characters are compared by value, as Boost does.
template<class C1, class C2, class Eq>
inline bool equal_with(std::basic_string_view<C1> a, std::basic_string_view<C2> b, Eq eq)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), eq);
}

template<class C1, class C2, class Eq>
inline bool find_with(std::basic_string_view<C1> haystack, std::basic_string_view<C2> needle, Eq eq)
{
	return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), eq) != haystack.end();
}

struct is_equal { template<class A, class B> bool operator()(A a, B b) const { return (wchar_t)a == (wchar_t)b; } };
struct is_iequal { template<class A, class B> bool operator()(A a, B b) const { return (wchar_t)fold(a) == (wchar_t)fold(b); } };

} // namespace detail

template<class A, class B>
inline bool equals(const A& a, const B& b)
{
	return detail::equal_with(detail::view(a), detail::view(b), detail::is_equal());
}

template<class A, class B>
inline bool iequals(const A& a, const B& b)
{
	return detail::equal_with(detail::view(a), detail::view(b), detail::is_iequal());
}

template<class A, class B>
inline bool starts_with(const A& s, const B& prefix)
{
	auto v = detail::view(s);
	auto p = detail::view(prefix);
	return v.size() >= p.size() && detail::equal_with(v.substr(0, p.size()), p, detail::is_equal());
}

template<class A, class B>
inline bool istarts_with(const A& s, const B& prefix)
{
	auto v = detail::view(s);
	auto p = detail::view(prefix);
	return v.size() >= p.size() && detail::equal_with(v.substr(0, p.size()), p, detail::is_iequal());
}

template<class A, class B>
inline bool ends_with(const A& s, const B& suffix)
{
	auto v = detail::view(s);
	auto p = detail::view(suffix);
	return v.size() >= p.size() && detail::equal_with(v.substr(v.size() - p.size()), p, detail::is_equal());
}

template<class A, class B>
inline bool iends_with(const A& s, const B& suffix)
{
	auto v = detail::view(s);
	auto p = detail::view(suffix);
	return v.size() >= p.size() && detail::equal_with(v.substr(v.size() - p.size()), p, detail::is_iequal());
}

template<class A, class B>
inline bool contains(const A& s, const B& needle)
{
	return detail::find_with(detail::view(s), detail::view(needle), detail::is_equal());
}

template<class A, class B>
inline bool icontains(const A& s, const B& needle)
{
	return detail::find_with(detail::view(s), detail::view(needle), detail::is_iequal());
}

template<class S>
inline void to_lower(S& s)
{
	for (auto& c : s)
		c = detail::fold(c);
}

template<class S>
inline void to_upper(S& s)
{
	for (auto& c : s)
		c = detail::fold_upper(c);
}

template<class S>
inline S to_lower_copy(S s)
{
	to_lower(s);
	return s;
}

template<class S>
inline S to_upper_copy(S s)
{
	to_upper(s);
	return s;
}

} // namespace algorithm

using algorithm::contains;
using algorithm::ends_with;
using algorithm::equals;
using algorithm::icontains;
using algorithm::iends_with;
using algorithm::iequals;
using algorithm::istarts_with;
using algorithm::starts_with;
using algorithm::to_lower;
using algorithm::to_lower_copy;
using algorithm::to_upper;
using algorithm::to_upper_copy;

} // namespace boost
