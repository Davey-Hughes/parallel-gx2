// boost::property_tree::ptree and read_ini, for flat INI files: a key is
// "Section.name" (or "name" before the first section). get<std::string> throws
// when the key is missing and get_optional returns nothing, as in Boost.
#pragma once

#include <istream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace boost { namespace property_tree {

class ptree_bad_path : public std::runtime_error
{
public:
	explicit ptree_bad_path(const std::string& path) : std::runtime_error("No such node (" + path + ")") {}
};

namespace ini_parser {
class ini_parser_error : public std::runtime_error
{
public:
	ini_parser_error(const std::string& message, unsigned long line)
		: std::runtime_error("<unspecified file>(" + std::to_string(line) + "): " + message) {}
};
} // namespace ini_parser

class ptree
{
public:
	template<class T>
	T get(const std::string& path) const
	{
		auto it = m_values.find(path);
		if (it == m_values.end())
			throw ptree_bad_path(path);
		return convert<T>(it->second, path);
	}

	template<class T>
	T get(const std::string& path, const T& fallback) const
	{
		auto it = m_values.find(path);
		if (it == m_values.end())
			return fallback;
		return convert<T>(it->second, path);
	}

	template<class T>
	std::optional<T> get_optional(const std::string& path) const
	{
		auto it = m_values.find(path);
		if (it == m_values.end())
			return std::nullopt;
		return convert<T>(it->second, path);
	}

	void put(const std::string& path, const std::string& value) { m_values[path] = value; }

	bool empty() const { return m_values.empty(); }

private:
	template<class T>
	static T convert(const std::string& s, const std::string& path)
	{
		if constexpr (std::is_same_v<T, std::string>)
			return s;
		else
		{
			std::istringstream in(s);
			T v{};
			in >> v;
			if (in.fail())
				throw ptree_bad_path(path);
			return v;
		}
	}

	std::map<std::string, std::string> m_values;
};

namespace ini_parser {

inline void read_ini(std::istream& in, ptree& pt)
{
	auto trim = [](std::string s) {
		const char* ws = " \t\r\n";
		const auto b = s.find_first_not_of(ws);
		if (b == std::string::npos)
			return std::string();
		const auto e = s.find_last_not_of(ws);
		return s.substr(b, e - b + 1);
	};
	ptree result;
	std::string section;
	std::string line;
	unsigned long lineNo = 0;
	while (std::getline(in, line))
	{
		lineNo++;
		line = trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#')
			continue;
		if (line[0] == '[')
		{
			const auto end = line.find(']');
			if (end == std::string::npos)
				throw ini_parser_error("unmatched '['", lineNo);
			section = trim(line.substr(1, end - 1));
			continue;
		}
		const auto eq = line.find('=');
		if (eq == std::string::npos)
			throw ini_parser_error("'=' character not found in line", lineNo);
		const std::string key = trim(line.substr(0, eq));
		if (key.empty())
			throw ini_parser_error("key expected", lineNo);
		result.put(section.empty() ? key : section + "." + key, trim(line.substr(eq + 1)));
	}
	pt = std::move(result);
}

} // namespace ini_parser

using ini_parser::read_ini;

}} // namespace boost::property_tree
