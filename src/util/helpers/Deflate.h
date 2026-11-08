#pragma once

#include <cstddef>
#include <cstdint>

// zlib streams (RFC 1950) through libretro-common's deflate, in place of
// zlib's compress and uncompress. The streams are zlib's: zlib reads what
// Compress writes, and Uncompress reads what zlib writes.
namespace Deflate
{
	enum class Result
	{
		Ok,
		BufferTooSmall, // the output filled before the stream ended
		DataError,      // malformed, or the input ended before the stream
		OutOfMemory,
	};

	// zlib's compressBound(): what a game sizes its buffer by
	size_t ZlibCompressBound(size_t len);
	// room enough for Compress() to never run out, whatever the data
	size_t CompressBound(size_t len);

	// A zlib stream of src into dst, dstLen bytes of room; dstLen becomes
	// its length. False when it does not fit, or out of memory.
	bool Compress(uint8_t* dst, size_t& dstLen, const uint8_t* src, size_t srcLen, int level = 6);

	// The stream at src, srcLen bytes, into dst, dstLen bytes of room;
	// dstLen becomes what was decoded and srcLen what the stream took.
	// windowBits as zlib's: 8..15 a zlib stream, -8..-15 raw deflate,
	// 16+ gzip, 32+ either. One inflater per thread is kept and reused.
	Result Uncompress(uint8_t* dst, size_t& dstLen, const uint8_t* src, size_t& srcLen, int windowBits = 15);

	uint32_t Adler32(uint32_t adler, const uint8_t* data, size_t len);
	uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len);
}
