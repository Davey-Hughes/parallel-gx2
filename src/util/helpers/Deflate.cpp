#include "util/helpers/Deflate.h"

#include <memory>

#include <encodings/crc32.h>
#include <encodings/deflate.h>

namespace Deflate
{
	size_t ZlibCompressBound(size_t len)
	{
		return len + (len >> 12) + (len >> 14) + (len >> 25) + 13;
	}

	size_t CompressBound(size_t len)
	{
		// stored blocks at worst: five bytes every 16 KiB, header, trailer
		return len + (len >> 11) + 64;
	}

	bool Compress(uint8_t* dst, size_t& dstLen, const uint8_t* src, size_t srcLen, int level)
	{
		void* z = rdeflate_new(level < 0 ? 6 : level, 15);
		if (!z)
			return false;
		size_t out = 0;
		bool ok = false;
		rdeflate_set_in(z, src, srcLen);
		rdeflate_set_out(z, dst, dstLen);
		rdeflate_finish(z);
		for (;;)
		{
			size_t wrote = 0;
			int st = rdeflate_process(z, nullptr, &wrote);
			out += wrote;
			if (st == RDEFLATE_PROCESS_END)
			{
				ok = true;
				break;
			}
			if (st != RDEFLATE_PROCESS_NEXT || out == dstLen)
				break;
			rdeflate_set_out(z, dst + out, dstLen - out);
		}
		rdeflate_free(z);
		if (ok)
			dstLen = out;
		return ok;
	}

	Result Uncompress(uint8_t* dst, size_t& dstLen, const uint8_t* src, size_t& srcLen, int windowBits)
	{
		struct Free
		{
			void operator()(void* z) const { rinflate_free(z); }
		};
		thread_local std::unique_ptr<void, Free> s_inflater;
		void* z = s_inflater.get();
		if (z)
			rinflate_reset(z, windowBits);
		else
		{
			z = rinflate_new(windowBits);
			if (!z)
				return Result::OutOfMemory;
			s_inflater.reset(z);
		}
		size_t in = 0, out = 0;
		Result result = Result::DataError;
		rinflate_set_in(z, src, srcLen);
		rinflate_set_out(z, dst, dstLen);
		for (;;)
		{
			size_t read = 0, wrote = 0;
			int st = rinflate_process(z, &read, &wrote);
			in += read;
			out += wrote;
			if (st == RDEFLATE_PROCESS_END)
			{
				result = Result::Ok;
				break;
			}
			if (st == RDEFLATE_PROCESS_ERROR)
				break;
			if (!read && !wrote)
			{
				// as zlib's uncompress: out of room, or out of input
				if (out == dstLen)
					result = Result::BufferTooSmall;
				break;
			}
		}
		dstLen = out;
		srcLen = in;
		return result;
	}

	uint32_t Adler32(uint32_t adler, const uint8_t* data, size_t len)
	{
		constexpr uint32_t BASE = 65521;
		uint32_t a = adler & 0xFFFF, b = adler >> 16;
		while (len)
		{
			// 5552 bytes is the most that cannot overflow 32 bits (zlib's NMAX)
			size_t n = len < 5552 ? len : 5552;
			len -= n;
			while (n--)
			{
				a += *data++;
				b += a;
			}
			a %= BASE;
			b %= BASE;
		}
		return (b << 16) | a;
	}

	uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len)
	{
		return len ? encoding_crc32(crc, data, len) : crc;
	}
}
