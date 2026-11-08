#pragma once

#include <cstddef>
#include <cstdint>

// The console's zlib 1.2.5 (zlib125.rpl), over libretro-common's deflate:
// what a guest z_stream's calls do, on the host side of its fields.
// zlib125.cpp moves the fields to and from the guest's struct.
namespace zlib125
{
	// zlib.h's values, which the guest passes and expects back
	enum : int
	{
		Z_NO_FLUSH = 0,
		Z_PARTIAL_FLUSH = 1,
		Z_SYNC_FLUSH = 2,
		Z_FULL_FLUSH = 3,
		Z_FINISH = 4,
		Z_BLOCK = 5,

		Z_OK = 0,
		Z_STREAM_END = 1,
		Z_NEED_DICT = 2,
		Z_STREAM_ERROR = -2,
		Z_DATA_ERROR = -3,
		Z_MEM_ERROR = -4,
		Z_BUF_ERROR = -5,

		Z_DEFLATED = 8,
		Z_DEFAULT_COMPRESSION = -1,
		Z_UNKNOWN = 2,
	};

	// the z_stream fields a call reads and moves on
	struct StreamFields
	{
		const uint8_t* nextIn;
		uint32_t availIn;
		uint32_t totalIn;
		uint8_t* nextOut;
		uint32_t availOut;
		uint32_t totalOut;
		uint32_t adler;
	};

	class HostStream
	{
	public:
		// nullptr with *result Z_STREAM_ERROR for parameters zlib refuses,
		// or Z_MEM_ERROR
		static HostStream* CreateInflate(int windowBits, int* result);
		static HostStream* CreateDeflate(int level, int method, int windowBits, int* result);
		~HostStream();

		bool IsDeflate() const { return m_deflate; }

		// what inflateInit/deflateInit leave in the fields they set
		void Reset(StreamFields& f);

		int Inflate(StreamFields& f, int flush);
		// windowBits as inflateReset2's; inflateReset keeps the stream's
		int InflateReset(StreamFields& f, int windowBits);
		int InflateReset(StreamFields& f) { return InflateReset(f, m_windowBits); }

		// Z_PARTIAL_FLUSH, Z_SYNC_FLUSH and Z_FULL_FLUSH are taken as
		// Z_NO_FLUSH: the output catches up at the next block or at Z_FINISH
		int Deflate(StreamFields& f, int flush);
		uint32_t DeflateBound(uint32_t sourceLen) const;

	private:
		HostStream() = default;
		enum class Wrap { Raw, Zlib, Gzip, Detect };
		static bool ParseWindowBits(int windowBits, bool inflate, Wrap* wrap);
		void UpdateCheck(const uint8_t* data, size_t len, StreamFields& f);

		void* m_stream = nullptr;
		bool m_deflate = false;
		int m_windowBits = 15;
		Wrap m_wrap = Wrap::Zlib;
		bool m_ended = false;     // the stream's end was read or written
		bool m_finishing = false; // deflate: Z_FINISH was asked
		uint32_t m_check = 0;     // adler32 (zlib) or crc32 (gzip) so far
	};

	uint32_t CompressBound(uint32_t sourceLen);
	// zlib's compress() and uncompress(): Z_OK, Z_BUF_ERROR, Z_DATA_ERROR or Z_MEM_ERROR
	int Compress(uint8_t* dst, uint32_t* dstLen, const uint8_t* src, uint32_t srcLen, int level);
	int Uncompress(uint8_t* dst, uint32_t* dstLen, const uint8_t* src, uint32_t srcLen);
	// zlib's crc32(): 0 for a null buffer, the start value
	uint32_t Crc32(uint32_t crc, const uint8_t* buf, uint32_t len);
}
