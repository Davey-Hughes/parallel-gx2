#include "Cafe/OS/common/OSCommon.h"
#include "zlib125.h"
#include "zlib125_stream.h"

typedef struct {
	/* +0x00 */ MEMPTR<uint8>		next_in;     /* next input byte */
	/* +0x04 */ uint32be			avail_in;  /* number of bytes available at next_in */
	/* +0x08 */ uint32be			total_in;  /* total number of input bytes read so far */

	/* +0x0C */ MEMPTR<uint8>		next_out; /* next output byte should be put there */
	/* +0x10 */ uint32be			avail_out; /* remaining free space at next_out */
	/* +0x14 */ uint32be			total_out; /* total number of bytes output so far */

	/* +0x18 */ MEMPTR<char>		msg;  /* last error message, NULL if no error */
	/* +0x1C */ MEMPTR<void>		state; /* not visible by applications */

	/* +0x20 */ MEMPTR<void>		zalloc;  /* used to allocate the internal state */
	/* +0x24 */ MEMPTR<void>		zfree;   /* used to free the internal state */
	/* +0x28 */ MEMPTR<void>		opaque;  /* private data object passed to zalloc and zfree */

	/* +0x2C */ uint32be			data_type;  /* best guess about the data type: binary or text */
	/* +0x30 */ uint32be			adler;      /* adler32 value of the uncompressed data */
	/* +0x34 */ uint32be			reserved;   /* reserved for future use */
}z_stream_ppc2;

static_assert(sizeof(z_stream_ppc2) == 0x38);

void zlib125_zcalloc(PPCInterpreter_t* hCPU);
void zlib125_zcfree(PPCInterpreter_t* hCPU);

// The host stream behind a guest z_stream is held in a small block the
// guest's zalloc gives, where zlib kept its own state: the guest sees the
// allocation it would have, and frees it through zfree at the end.
namespace
{
	constexpr uint32 HOST_STATE_MAGIC = 0x5A484F53; // 'ZHOS'
	constexpr uint32 HOST_STATE_SIZE = 16;          // magic, pad, host pointer

	void* zlib125_guestAlloc(z_stream_ppc2* zstream, uint32 size)
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		hCPU->gpr[3] = zstream->opaque.GetMPTR();
		hCPU->gpr[4] = 1;
		hCPU->gpr[5] = size;
		PPCCore_executeCallbackInternal(zstream->zalloc.GetMPTR());
		if (hCPU->gpr[3] == MPTR_NULL)
			return nullptr;
		void* p = memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
		memset(p, 0, size);
		return p;
	}

	void zlib125_guestFree(z_stream_ppc2* zstream, void* p)
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		hCPU->gpr[3] = zstream->opaque.GetMPTR();
		hCPU->gpr[4] = memory_getVirtualOffsetFromPointer(p);
		PPCCore_executeCallbackInternal(zstream->zfree.GetMPTR());
	}

	zlib125::HostStream* zlib125_getHost(z_stream_ppc2* zstream)
	{
		if (!zstream || !zstream->state)
			return nullptr;
		uint8* block = (uint8*)zstream->state.GetPtr();
		uint32 magic;
		memcpy(&magic, block, 4);
		if (magic != HOST_STATE_MAGIC)
			return nullptr;
		zlib125::HostStream* host;
		memcpy(&host, block + 8, sizeof(host));
		return host;
	}

	// the default allocators, for a stream whose zalloc/zfree are null
	void zlib125_setupAllocators(z_stream_ppc2* zstream)
	{
		if (zstream->zalloc == nullptr)
			zstream->zalloc = PPCInterpreter_makeCallableExportDepr(zlib125_zcalloc);
		if (zstream->zfree == nullptr)
			zstream->zfree = PPCInterpreter_makeCallableExportDepr(zlib125_zcfree);
	}

	sint32 zlib125_attach(z_stream_ppc2* zstream, zlib125::HostStream* host, sint32 result)
	{
		if (!host)
		{
			zstream->state = nullptr;
			return result;
		}
		uint8* block = (uint8*)zlib125_guestAlloc(zstream, HOST_STATE_SIZE);
		if (!block)
		{
			delete host;
			zstream->state = nullptr;
			return zlib125::Z_MEM_ERROR;
		}
		memcpy(block, &HOST_STATE_MAGIC, 4);
		memcpy(block + 8, &host, sizeof(host));
		zstream->state = block;
		zstream->msg = nullptr;
		zstream->data_type = zlib125::Z_UNKNOWN;
		zlib125::StreamFields f = {};
		host->Reset(f);
		zstream->total_in = f.totalIn;
		zstream->total_out = f.totalOut;
		zstream->adler = f.adler;
		return zlib125::Z_OK;
	}

	sint32 zlib125_detach(z_stream_ppc2* zstream)
	{
		zlib125::HostStream* host = zlib125_getHost(zstream);
		if (!host)
			return zlib125::Z_STREAM_ERROR;
		delete host;
		zlib125_guestFree(zstream, zstream->state.GetPtr());
		zstream->state = nullptr;
		return zlib125::Z_OK;
	}

	zlib125::StreamFields zlib125_fields(z_stream_ppc2* zstream)
	{
		zlib125::StreamFields f;
		f.nextIn = zstream->next_in.GetPtr();
		f.availIn = zstream->avail_in;
		f.totalIn = zstream->total_in;
		f.nextOut = zstream->next_out.GetPtr();
		f.availOut = zstream->avail_out;
		f.totalOut = zstream->total_out;
		f.adler = zstream->adler;
		return f;
	}

	void zlib125_store(z_stream_ppc2* zstream, const zlib125::StreamFields& f)
	{
		zstream->next_in = (uint8*)f.nextIn;
		zstream->avail_in = f.availIn;
		zstream->total_in = f.totalIn;
		zstream->next_out = f.nextOut;
		zstream->avail_out = f.availOut;
		zstream->total_out = f.totalOut;
		zstream->adler = f.adler;
	}
}

void zlib125_zcalloc(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(opaque, 0);
	ppcDefineParamU32(items, 1);
	ppcDefineParamU32(size, 2);

	hCPU->gpr[3] = items*size;
	hCPU->instructionPointer = gCoreinitData->MEMAllocFromDefaultHeap.GetMPTR();
}

void zlib125_zcfree(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(opaque, 0);
	ppcDefineParamMPTR(baseIndex, 1);

	hCPU->gpr[3] = baseIndex;
	hCPU->instructionPointer = gCoreinitData->MEMFreeToDefaultHeap.GetMPTR();
}

void zlib125Export_inflateInit_(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamStr(version, 1);

	zlib125_setupAllocators(zstream);
	sint32 r;
	zlib125::HostStream* host = zlib125::HostStream::CreateInflate(15, &r);
	osLib_returnFromFunction(hCPU, zlib125_attach(zstream, host, r));
}

void zlib125Export_inflateInit2_(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamS32(windowBits, 1);
	ppcDefineParamStr(version, 2);

	zlib125_setupAllocators(zstream);
	sint32 r;
	zlib125::HostStream* host = zlib125::HostStream::CreateInflate(windowBits, &r);
	osLib_returnFromFunction(hCPU, zlib125_attach(zstream, host, r));
}

void zlib125Export_inflate(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamS32(flushParam, 1);

	zlib125::HostStream* host = zlib125_getHost(zstream);
	if (!host || host->IsDeflate())
	{
		osLib_returnFromFunction(hCPU, zlib125::Z_STREAM_ERROR);
		return;
	}
	zlib125::StreamFields f = zlib125_fields(zstream);
	sint32 r = host->Inflate(f, flushParam);
	zlib125_store(zstream, f);
	osLib_returnFromFunction(hCPU, r);
}

void zlib125Export_inflateEnd(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	osLib_returnFromFunction(hCPU, zlib125_detach(zstream));
}

void zlib125Export_inflateReset(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);

	zlib125::HostStream* host = zlib125_getHost(zstream);
	if (!host)
	{
		osLib_returnFromFunction(hCPU, zlib125::Z_STREAM_ERROR);
		return;
	}
	zlib125::StreamFields f = zlib125_fields(zstream);
	sint32 r = host->InflateReset(f);
	zlib125_store(zstream, f);
	zstream->msg = nullptr;
	osLib_returnFromFunction(hCPU, r);
}

void zlib125Export_inflateReset2(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamS32(windowBits, 1);

	zlib125::HostStream* host = zlib125_getHost(zstream);
	if (!host)
	{
		osLib_returnFromFunction(hCPU, zlib125::Z_STREAM_ERROR);
		return;
	}
	zlib125::StreamFields f = zlib125_fields(zstream);
	sint32 r = host->InflateReset(f, windowBits);
	zlib125_store(zstream, f);
	zstream->msg = nullptr;
	osLib_returnFromFunction(hCPU, r);
}

void zlib125Export_deflateInit_(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamS32(level, 1);
	ppcDefineParamStr(version, 2);
	ppcDefineParamS32(streamsize, 3);

	zlib125_setupAllocators(zstream);
	cemu_assert_debug(streamsize == sizeof(z_stream_ppc2));
	sint32 r;
	zlib125::HostStream* host = zlib125::HostStream::CreateDeflate(level, zlib125::Z_DEFLATED, 15, &r);
	osLib_returnFromFunction(hCPU, zlib125_attach(zstream, host, r));
}

void zlib125Export_deflateInit2_(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamS32(level, 1);
	ppcDefineParamS32(method, 2);
	ppcDefineParamS32(windowBits, 3);
	ppcDefineParamS32(memLevel, 4);
	ppcDefineParamS32(strategy, 5);
	ppcDefineParamStr(version, 6);
	ppcDefineParamS32(streamsize, 7);

	// Splatoon's zalloc hands out too little for zlib's own state; the
	// default heap is what it has always been given here
	zstream->zalloc = PPCInterpreter_makeCallableExportDepr(zlib125_zcalloc);
	zstream->zfree = PPCInterpreter_makeCallableExportDepr(zlib125_zcfree);
	cemu_assert_debug(streamsize == sizeof(z_stream_ppc2));
	sint32 r;
	zlib125::HostStream* host = zlib125::HostStream::CreateDeflate(level, method, windowBits, &r);
	osLib_returnFromFunction(hCPU, zlib125_attach(zstream, host, r));
}

void zlib125Export_deflateBound(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamU32(sourceLen, 1);

	zlib125::HostStream* host = zlib125_getHost(zstream);
	uint32 r;
	if (host && host->IsDeflate())
		r = host->DeflateBound(sourceLen);
	else // zlib's answer for a stream it does not know
		r = sourceLen + ((sourceLen + 7) >> 3) + ((sourceLen + 63) >> 6) + 5 + 6;
	osLib_returnFromFunction(hCPU, r);
}

void zlib125Export_deflate(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	ppcDefineParamS32(flushParam, 1);

	zlib125::HostStream* host = zlib125_getHost(zstream);
	if (!host || !host->IsDeflate())
	{
		osLib_returnFromFunction(hCPU, zlib125::Z_STREAM_ERROR);
		return;
	}
	if (flushParam >= zlib125::Z_PARTIAL_FLUSH && flushParam <= zlib125::Z_FULL_FLUSH)
	{
		static std::atomic_bool s_warned{false};
		if (!s_warned.exchange(true, std::memory_order_relaxed))
			cemuLog_log(LogType::Force, "zlib125: deflate() flush {} is taken as Z_NO_FLUSH", flushParam);
	}
	zlib125::StreamFields f = zlib125_fields(zstream);
	sint32 r = host->Deflate(f, flushParam);
	zlib125_store(zstream, f);
	osLib_returnFromFunction(hCPU, r);
}

void zlib125Export_deflateEnd(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(zstream, z_stream_ppc2, 0);
	osLib_returnFromFunction(hCPU, zlib125_detach(zstream));
}

void zlib125Export_uncompress(PPCInterpreter_t* hCPU)
{
	// Bytef * dest, uLongf * destLen, const Bytef * source, uLong sourceLen
	uint8* memDst = memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	uint8* memSrc = memory_getPointerFromVirtualOffset(hCPU->gpr[5]);
	uint32be* pDestLen = (uint32be*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
	uint32 sourceLen = hCPU->gpr[6];
	uint32 destLen = *pDestLen;
	sint32 r = zlib125::Uncompress(memDst, &destLen, memSrc, sourceLen);
	*pDestLen = destLen;
	osLib_returnFromFunction(hCPU, r);
}

void zlib125Export_compress(PPCInterpreter_t* hCPU)
{
	// Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen
	uint8* memDst = memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	uint8* memSrc = memory_getPointerFromVirtualOffset(hCPU->gpr[5]);
	uint32be* pDestLen = (uint32be*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
	uint32 sourceLen = hCPU->gpr[6];
	uint32 destLen = *pDestLen;
	sint32 r = zlib125::Compress(memDst, &destLen, memSrc, sourceLen, zlib125::Z_DEFAULT_COMPRESSION);
	*pDestLen = destLen;
	osLib_returnFromFunction(hCPU, r);
}

void zlib125Export_crc32(PPCInterpreter_t* hCPU)
{
	uint32 crc = hCPU->gpr[3];
	uint8* buf = (uint8*)memory_getPointerFromVirtualOffsetAllowNull(hCPU->gpr[4]);
	uint32 len = hCPU->gpr[5];
	osLib_returnFromFunction(hCPU, zlib125::Crc32(crc, buf, len));
}

void zlib125Export_compressBound(PPCInterpreter_t* hCPU)
{
	osLib_returnFromFunction(hCPU, zlib125::CompressBound(hCPU->gpr[3]));
}

namespace zlib
{
	void load()
	{

	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "zlib125";
		}

		void RPLMapped() override
		{
			osLib_addFunction("zlib125", "inflateInit2_", zlib125Export_inflateInit2_);
			osLib_addFunction("zlib125", "inflateInit_", zlib125Export_inflateInit_);
			osLib_addFunction("zlib125", "inflateEnd", zlib125Export_inflateEnd);
			osLib_addFunction("zlib125", "inflate", zlib125Export_inflate);
			osLib_addFunction("zlib125", "inflateReset", zlib125Export_inflateReset);
			osLib_addFunction("zlib125", "inflateReset2", zlib125Export_inflateReset2);

			osLib_addFunction("zlib125", "deflateInit_", zlib125Export_deflateInit_);
			osLib_addFunction("zlib125", "deflateInit2_", zlib125Export_deflateInit2_);
			osLib_addFunction("zlib125", "deflateBound", zlib125Export_deflateBound);
			osLib_addFunction("zlib125", "deflate", zlib125Export_deflate);
			osLib_addFunction("zlib125", "deflateEnd", zlib125Export_deflateEnd);

			osLib_addFunction("zlib125", "uncompress", zlib125Export_uncompress);
			osLib_addFunction("zlib125", "compress", zlib125Export_compress);

			osLib_addFunction("zlib125", "crc32", zlib125Export_crc32);
			osLib_addFunction("zlib125", "compressBound", zlib125Export_compressBound);
		};

	}s_COSZlib125Module;

	COSModule* GetModule()
	{
		return &s_COSZlib125Module;
	}
}