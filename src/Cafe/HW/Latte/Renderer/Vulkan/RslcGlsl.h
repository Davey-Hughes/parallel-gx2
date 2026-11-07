#pragma once
#include <string>

// What RendererShaderVk hands rslc for a stage's GLSL.
//
// rslc takes #version 450 and nothing else; the decompiler writes 430, the
// output shaders 420 to 450, the graphic packs anything. Their sources test
// VULKAN, which glslang predefined for a Vulkan target. #line keeps a
// message's line numbers the source's.
//
// A round-to-even mode (spirv_execution_mode 4462) stays only for 32 bit, and
// only where the device takes it (@rte32), as the decompiler writes it: the
// graphic packs carry the 16 and 64 bit ones older decompilers wrote, whose
// device features nothing checks. glslang kept the last mode a stage
// declared alone; rslc would keep every one.
inline bool RslcGlslDropMode(const std::string& line, bool rte32)
{
	const size_t at = line.find_first_not_of(" \t");
	if (at == std::string::npos || line.compare(at, 21, "spirv_execution_mode(") != 0)
		return false;
	const size_t close = line.rfind(')');
	const size_t comma = line.rfind(',', close);
	if (close == std::string::npos || comma == std::string::npos)
		return false;
	const size_t mode = line.rfind(',', comma - 1);
	const auto field = [&line](size_t from, size_t to) {
		const size_t b = line.find_first_not_of(" \t(", from + 1);
		return b < to ? line.substr(b, line.find_last_not_of(" \t", to - 1) + 1 - b) : std::string();
	};
	if (field(mode == std::string::npos ? line.find('(') : mode, comma) != "4462")
		return false;
	return !rte32 || field(comma, close) != "32";
}

inline std::string RslcGlsl(const std::string& glsl, bool rte32)
{
	std::string out = "#version 450\n#define VULKAN 100\n#line 1\n";
	out.reserve(out.size() + glsl.size());
	size_t at = 0;
	bool versionSeen = false;
	while (at < glsl.size())
	{
		size_t end = glsl.find('\n', at);
		if (end == std::string::npos)
			end = glsl.size();
		const std::string line = glsl.substr(at, end - at);
		const size_t first = line.find_first_not_of(" \t");
		if (!versionSeen && first != std::string::npos && line.compare(first, 8, "#version") == 0)
			versionSeen = true; // its line stays, empty
		else if (!RslcGlslDropMode(line, rte32))
			out.append(line);
		out.push_back('\n');
		at = end + 1;
	}
	return out;
}
