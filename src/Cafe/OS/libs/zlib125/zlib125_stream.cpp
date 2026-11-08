#include "Cafe/OS/libs/zlib125/zlib125_stream.h"

#include <encodings/deflate.h>

#include "util/helpers/Deflate.h"

namespace zlib125
{
	// The container windowBits asks for, as zlib 1.2.5 reads it: negative
	// raw, 8..15 zlib, plus 16 gzip, plus 32 (inflate only) either; 0 is
	// inflate's "as the zlib header says"
	bool HostStream::ParseWindowBits(int windowBits, bool inflate, Wrap* wrap)
	{
		int bits = windowBits;
		if (bits < 0)
		{
			*wrap = Wrap::Raw;
			bits = -bits;
		}
		else if (bits >= 32 && inflate)
		{
			*wrap = Wrap::Detect;
			bits -= 32;
		}
		else if (bits >= 16)
		{
			*wrap = Wrap::Gzip;
			bits -= 16;
		}
		else
			*wrap = Wrap::Zlib;
		if (bits == 0 && inflate && *wrap != Wrap::Raw)
			return true;
		return bits >= 8 && bits <= 15;
	}

	HostStream* HostStream::CreateInflate(int windowBits, int* result)
	{
		Wrap wrap;
		if (!ParseWindowBits(windowBits, true, &wrap))
		{
			*result = Z_STREAM_ERROR;
			return nullptr;
		}
		HostStream* s = new HostStream();
		s->m_windowBits = windowBits;
		s->m_wrap = wrap;
		// the decoder's window is always 32 KiB, so only the container matters
		s->m_stream = rinflate_new(wrap == Wrap::Raw ? -15 : wrap == Wrap::Gzip ? 31 : wrap == Wrap::Detect ? 47 : 15);
		if (!s->m_stream)
		{
			delete s;
			*result = Z_MEM_ERROR;
			return nullptr;
		}
		*result = Z_OK;
		return s;
	}

	HostStream* HostStream::CreateDeflate(int level, int method, int windowBits, int* result)
	{
		Wrap wrap;
		if (level == Z_DEFAULT_COMPRESSION)
			level = 6;
		if (level < 0 || level > 9 || method != Z_DEFLATED || !ParseWindowBits(windowBits, false, &wrap))
		{
			*result = Z_STREAM_ERROR;
			return nullptr;
		}
		HostStream* s = new HostStream();
		s->m_deflate = true;
		s->m_windowBits = windowBits;
		s->m_wrap = wrap;
		// the window bits go through: a match never reaches further back than
		// the window the receiver was told of
		s->m_stream = rdeflate_new(level, windowBits);
		if (!s->m_stream)
		{
			delete s;
			*result = Z_MEM_ERROR;
			return nullptr;
		}
		*result = Z_OK;
		return s;
	}

	HostStream::~HostStream()
	{
		if (m_deflate)
			rdeflate_free(m_stream);
		else
			rinflate_free(m_stream);
	}

	void HostStream::Reset(StreamFields& f)
	{
		f.totalIn = 0;
		f.totalOut = 0;
		m_check = m_wrap == Wrap::Gzip ? 0 : 1;
		f.adler = m_check;
		if (!m_deflate)
			f.adler = 1; // zlib 1.2.5's inflateReset, whatever the container
		m_ended = false;
		m_finishing = false;
	}

	void HostStream::UpdateCheck(const uint8_t* data, size_t len, StreamFields& f)
	{
		if (m_wrap == Wrap::Zlib)
			m_check = Deflate::Adler32(m_check, data, len);
		else if (m_wrap == Wrap::Gzip)
			m_check = Deflate::Crc32(m_check, data, len);
		else
			return;
		f.adler = m_check;
	}

	int HostStream::Inflate(StreamFields& f, int flush)
	{
		if (m_deflate || !f.nextOut || (!f.nextIn && f.availIn))
			return Z_STREAM_ERROR;
		if (m_ended)
			return Z_STREAM_END;
		if (m_wrap == Wrap::Detect && f.availIn)
		{
			m_wrap = f.nextIn[0] == 0x1f ? Wrap::Gzip : Wrap::Zlib;
			m_check = m_wrap == Wrap::Gzip ? 0 : 1;
		}
		rinflate_set_in(m_stream, f.nextIn, f.availIn);
		rinflate_set_out(m_stream, f.nextOut, f.availOut);
		size_t in = 0, out = 0;
		int st;
		for (;;)
		{
			size_t read = 0, wrote = 0;
			st = rinflate_process(m_stream, &read, &wrote);
			in += read;
			out += wrote;
			if (st != RDEFLATE_PROCESS_NEXT || (!read && !wrote))
				break;
		}
		UpdateCheck(f.nextOut, out, f);
		f.nextIn += in;
		f.availIn -= (uint32_t)in;
		f.totalIn += (uint32_t)in;
		f.nextOut += out;
		f.availOut -= (uint32_t)out;
		f.totalOut += (uint32_t)out;
		if (st == RDEFLATE_PROCESS_ERROR)
			return Z_DATA_ERROR;
		if (st == RDEFLATE_PROCESS_END)
		{
			m_ended = true;
			return Z_STREAM_END;
		}
		// as zlib: nothing could be done, or Z_FINISH and not all of it
		if ((!in && !out) || flush == Z_FINISH)
			return Z_BUF_ERROR;
		return Z_OK;
	}

	int HostStream::InflateReset(StreamFields& f, int windowBits)
	{
		Wrap wrap;
		if (m_deflate || !ParseWindowBits(windowBits, true, &wrap))
			return Z_STREAM_ERROR;
		m_windowBits = windowBits;
		m_wrap = wrap;
		rinflate_reset(m_stream, wrap == Wrap::Raw ? -15 : wrap == Wrap::Gzip ? 31 : wrap == Wrap::Detect ? 47 : 15);
		Reset(f);
		return Z_OK;
	}

	int HostStream::Deflate(StreamFields& f, int flush)
	{
		if (!m_deflate || flush < Z_NO_FLUSH || flush > Z_BLOCK || !f.nextOut || (!f.nextIn && f.availIn))
			return Z_STREAM_ERROR;
		if (m_ended)
			return flush == Z_FINISH ? Z_STREAM_END : Z_STREAM_ERROR;
		if (!f.availOut)
			return Z_BUF_ERROR;
		rdeflate_set_in(m_stream, f.nextIn, f.availIn);
		if (flush == Z_FINISH)
			m_finishing = true;
		// what is left of the input given now is the last: the flag stays set
		if (m_finishing)
			rdeflate_finish(m_stream);
		rdeflate_set_out(m_stream, f.nextOut, f.availOut);
		size_t in = 0, out = 0;
		int st;
		for (;;)
		{
			size_t read = 0, wrote = 0;
			st = rdeflate_process(m_stream, &read, &wrote);
			in += read;
			out += wrote;
			if (st != RDEFLATE_PROCESS_NEXT || (!read && !wrote))
				break;
		}
		UpdateCheck(f.nextIn, in, f);
		f.nextIn += in;
		f.availIn -= (uint32_t)in;
		f.totalIn += (uint32_t)in;
		f.nextOut += out;
		f.availOut -= (uint32_t)out;
		f.totalOut += (uint32_t)out;
		if (st == RDEFLATE_PROCESS_ERROR)
			return Z_STREAM_ERROR;
		if (st == RDEFLATE_PROCESS_END)
		{
			m_ended = true;
			return Z_STREAM_END;
		}
		if (!in && !out)
			return Z_BUF_ERROR;
		return Z_OK;
	}

	uint32_t HostStream::DeflateBound(uint32_t sourceLen) const
	{
		// zlib 1.2.5's for parameters other than the defaults, which also
		// covers those
		uint32_t wrapLen = m_wrap == Wrap::Raw ? 0 : m_wrap == Wrap::Gzip ? 18 : 6;
		return sourceLen + ((sourceLen + 7) >> 3) + ((sourceLen + 63) >> 6) + 5 + wrapLen;
	}

	uint32_t CompressBound(uint32_t sourceLen)
	{
		return (uint32_t)Deflate::ZlibCompressBound(sourceLen);
	}

	int Compress(uint8_t* dst, uint32_t* dstLen, const uint8_t* src, uint32_t srcLen, int level)
	{
		size_t len = *dstLen;
		if (!Deflate::Compress(dst, len, src, srcLen, level < 0 ? 6 : level))
			return Z_BUF_ERROR;
		*dstLen = (uint32_t)len;
		return Z_OK;
	}

	int Uncompress(uint8_t* dst, uint32_t* dstLen, const uint8_t* src, uint32_t srcLen)
	{
		size_t len = *dstLen, read = srcLen;
		Deflate::Result r = Deflate::Uncompress(dst, len, src, read);
		*dstLen = (uint32_t)len;
		switch (r)
		{
		case Deflate::Result::Ok:
			return Z_OK;
		case Deflate::Result::BufferTooSmall:
			return Z_BUF_ERROR;
		case Deflate::Result::OutOfMemory:
			return Z_MEM_ERROR;
		default:
			return Z_DATA_ERROR;
		}
	}

	uint32_t Crc32(uint32_t crc, const uint8_t* buf, uint32_t len)
	{
		if (!buf)
			return 0;
		return Deflate::Crc32(crc, buf, len);
	}
}
