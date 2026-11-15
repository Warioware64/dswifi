// SPDX-License-Identifier: MIT

#ifndef DSWIFI_ARM7_TWL_IEEE_TKIP_H__
#define DSWIFI_ARM7_TWL_IEEE_TKIP_H__

// What the WPA handshake needs for a TKIP network ("key descriptor version
// 1"): HMAC-MD5 for the key frames' MIC, and RC4 for the group key the access
// point sends. The bundled mbed TLS has neither (no MD5; 3.x dropped RC4).
// The data itself is encrypted by the wireless chip, given the keys.

#include <stddef.h>
#include <stdint.h>

// MD5 of `len` bytes (RFC 1321).
void tkip_md5(const uint8_t *data, size_t len, uint8_t out[16]);

// HMAC-MD5 (RFC 2104) of `len` bytes under `key`.
void tkip_hmac_md5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                   uint8_t out[16]);

// RC4 under `key`, its first `skip` bytes of keystream thrown away, applied
// to `len` bytes in place (WPA's key data: key = IV || KEK, skip 256).
void tkip_rc4(const uint8_t *key, size_t key_len, size_t skip, uint8_t *data, size_t len);

#endif // DSWIFI_ARM7_TWL_IEEE_TKIP_H__
