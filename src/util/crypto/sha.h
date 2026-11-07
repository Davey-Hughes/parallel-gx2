#pragma once
// SHA-1, SHA-256 and HMAC-SHA256 over libretro-common's lrc_hash and kdf.

#include <cstddef>
#include <cstdint>

#include <lrc_hash.h>
#include <crypto/kdf.h>

#define SHA_DIGEST_LENGTH 20
#define SHA1_DIGEST_LENGTH 20
#define SHA256_DIGEST_LENGTH 32

inline void SHA1(const void* data, size_t len, uint8_t* digest)
{
	SHA1Digest((const uint8_t*)data, len, digest);
}

inline void SHA256(const void* data, size_t len, uint8_t* digest)
{
	struct sha256_state st;
	sha256_stream_init(&st, 0);
	sha256_stream_update(&st, (const uint8_t*)data, len);
	sha256_stream_final(&st, digest);
}

// Returns mac, and its length (32) in *macLen when given.
inline uint8_t* HMAC_SHA256(const void* key, size_t keyLen, const void* data, size_t len, uint8_t* mac, unsigned int* macLen = nullptr)
{
	hmac_sha256((const uint8_t*)key, keyLen, (const uint8_t*)data, len, mac);
	if (macLen)
		*macLen = 32;
	return mac;
}

// An incremental SHA-1 or SHA-256.
class ShaStream
{
public:
	explicit ShaStream(bool isSha1 = false) { Init(isSha1); }

	void Init(bool isSha1)
	{
		m_isSha1 = isSha1;
		if (isSha1)
			sha1_stream_init(&m_sha1);
		else
			sha256_stream_init(&m_sha256, 0);
	}

	void Update(const void* data, size_t len)
	{
		if (m_isSha1)
			sha1_stream_update(&m_sha1, (const uint8_t*)data, len);
		else
			sha256_stream_update(&m_sha256, (const uint8_t*)data, len);
	}

	// 20 or 32 bytes
	void Final(uint8_t* digest)
	{
		if (m_isSha1)
			sha1_stream_final(&m_sha1, digest);
		else
			sha256_stream_final(&m_sha256, digest);
	}

	size_t Size() const { return m_isSha1 ? 20 : 32; }

private:
	bool m_isSha1;
	struct sha1_state m_sha1;
	struct sha256_state m_sha256;
};
