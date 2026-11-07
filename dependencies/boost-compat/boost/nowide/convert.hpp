// boost::nowide::narrow and widen: UTF-8 to and from wchar_t text (UTF-16
// where wchar_t is 16 bits, UTF-32 elsewhere). As in Boost.Nowide, a malformed
// sequence becomes U+FFFD rather than an error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <type_traits>

namespace boost { namespace nowide {

namespace detail {

constexpr char32_t kReplacement = 0xFFFD;

// Decodes one code point at s[i], moving i past it.
inline char32_t decode_utf8(const char* s, std::size_t n, std::size_t& i)
{
	const unsigned char c = (unsigned char)s[i++];
	if (c < 0x80)
		return c;
	int extra;
	char32_t cp;
	char32_t min;
	if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min = 0x80; }
	else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min = 0x800; }
	else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min = 0x10000; }
	else return kReplacement;
	for (int k = 0; k < extra; k++)
	{
		if (i >= n || ((unsigned char)s[i] & 0xC0) != 0x80)
			return kReplacement;
		cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
	}
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return kReplacement;
	return cp;
}

inline void encode_utf8(std::string& out, char32_t cp)
{
	if (cp < 0x80)
		out.push_back((char)cp);
	else if (cp < 0x800)
	{
		out.push_back((char)(0xC0 | (cp >> 6)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	}
	else if (cp < 0x10000)
	{
		out.push_back((char)(0xE0 | (cp >> 12)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	}
	else
	{
		out.push_back((char)(0xF0 | (cp >> 18)));
		out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	}
}

inline std::wstring widen(const char* s, std::size_t n)
{
	std::wstring out;
	out.reserve(n);
	std::size_t i = 0;
	while (i < n)
	{
		const char32_t cp = decode_utf8(s, n, i);
		if constexpr (sizeof(wchar_t) == 2)
		{
			if (cp >= 0x10000)
			{
				const char32_t v = cp - 0x10000;
				out.push_back((wchar_t)(0xD800 + (v >> 10)));
				out.push_back((wchar_t)(0xDC00 + (v & 0x3FF)));
				continue;
			}
		}
		out.push_back((wchar_t)cp);
	}
	return out;
}

inline std::string narrow(const wchar_t* s, std::size_t n)
{
	std::string out;
	out.reserve(n);
	for (std::size_t i = 0; i < n; i++)
	{
		char32_t cp = (char32_t)(std::make_unsigned_t<wchar_t>)s[i];
		if constexpr (sizeof(wchar_t) == 2)
		{
			if (cp >= 0xD800 && cp <= 0xDBFF)
			{
				if (i + 1 < n && (char32_t)s[i + 1] >= 0xDC00 && (char32_t)s[i + 1] <= 0xDFFF)
				{
					cp = 0x10000 + ((cp - 0xD800) << 10) + ((char32_t)s[i + 1] - 0xDC00);
					i++;
				}
				else
					cp = kReplacement;
			}
			else if (cp >= 0xDC00 && cp <= 0xDFFF)
				cp = kReplacement;
		}
		else if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			cp = kReplacement;
		encode_utf8(out, cp);
	}
	return out;
}

} // namespace detail

inline std::wstring widen(const char* s) { return detail::widen(s, std::strlen(s)); }
inline std::wstring widen(const char* s, std::size_t n) { return detail::widen(s, n); }
inline std::wstring widen(const char* begin, const char* end) { return detail::widen(begin, (std::size_t)(end - begin)); }
inline std::wstring widen(std::string_view s) { return detail::widen(s.data(), s.size()); }
inline std::wstring widen(const std::string& s) { return detail::widen(s.data(), s.size()); }

// Into a buffer the caller owns, terminator included: the buffer, or null if
// it is too small.
inline wchar_t* widen(wchar_t* output, std::size_t output_size, const char* source)
{
	const std::wstring w = widen(source);
	if (w.size() + 1 > output_size)
		return nullptr;
	std::memcpy(output, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
	return output;
}

inline std::string narrow(const wchar_t* s) { return detail::narrow(s, std::wcslen(s)); }
inline std::string narrow(const wchar_t* s, std::size_t n) { return detail::narrow(s, n); }
inline std::string narrow(const wchar_t* begin, const wchar_t* end) { return detail::narrow(begin, (std::size_t)(end - begin)); }
inline std::string narrow(std::wstring_view s) { return detail::narrow(s.data(), s.size()); }
inline std::string narrow(const std::wstring& s) { return detail::narrow(s.data(), s.size()); }

inline char* narrow(char* output, std::size_t output_size, const wchar_t* source)
{
	const std::string n = narrow(source);
	if (n.size() + 1 > output_size)
		return nullptr;
	std::memcpy(output, n.c_str(), n.size() + 1);
	return output;
}

}} // namespace boost::nowide
