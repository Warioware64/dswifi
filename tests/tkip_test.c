// Host test of the TKIP handshake's crypto (source/arm7/twl/ieee/tkip.twl.c)
// against published vectors: RFC 1321 (MD5), RFC 2202 (HMAC-MD5), and the
// usual RC4 examples.
//
//   cc -I source tests/tkip_test.c source/arm7/twl/ieee/tkip.twl.c -o /tmp/tkip_test && /tmp/tkip_test

#include <stdio.h>
#include <string.h>

#include "arm7/twl/ieee/tkip.h"

static int failures = 0;

static void expect(const char *what, const uint8_t *got, const char *hex, size_t len)
{
    char out[2 * 64 + 1];
    for (size_t i = 0; i < len; i++)
        sprintf(out + 2 * i, "%02x", got[i]);
    if (strcmp(out, hex) != 0)
    {
        printf("FAIL %s: %s, wanted %s\n", what, out, hex);
        failures++;
    }
}

int main(void)
{
    uint8_t out[16];

    tkip_md5((const uint8_t *)"", 0, out);
    expect("md5 empty", out, "d41d8cd98f00b204e9800998ecf8427e", 16);
    tkip_md5((const uint8_t *)"abc", 3, out);
    expect("md5 abc", out, "900150983cd24fb0d6963f7d28e17f72", 16);
    tkip_md5((const uint8_t *)"message digest", 14, out);
    expect("md5 message digest", out, "f96b697d7cb7938d525a2f31aaf161d0", 16);
    const char *eighty = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    tkip_md5((const uint8_t *)eighty, 80, out);
    expect("md5 80 digits", out, "57edf4a22be3c955ac49da2e2107b67a", 16);

    uint8_t key1[16];
    memset(key1, 0x0b, 16);
    tkip_hmac_md5(key1, 16, (const uint8_t *)"Hi There", 8, out);
    expect("hmac-md5 case 1", out, "9294727a3638bb1c13f48ef8158bfc9d", 16);
    const char *data2 = "what do ya want for nothing?";
    tkip_hmac_md5((const uint8_t *)"Jefe", 4, (const uint8_t *)data2, strlen(data2), out);
    expect("hmac-md5 case 2", out, "750c783e6ab0b503eaa86e310a5db738", 16);
    uint8_t key3[16], data3[50];
    memset(key3, 0xaa, 16);
    memset(data3, 0xdd, 50);
    tkip_hmac_md5(key3, 16, data3, 50, out);
    expect("hmac-md5 case 3", out, "56be34521d144c88dbb8c733f0e8b3f6", 16);
    uint8_t key6[80];
    memset(key6, 0xaa, 80);
    const char *data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    tkip_hmac_md5(key6, 80, (const uint8_t *)data6, strlen(data6), out);
    expect("hmac-md5 case 6", out, "6b1ab7fe4bd7bf8f0b62e6ce61b9d0cd", 16);

    uint8_t text1[] = "Plaintext";
    tkip_rc4((const uint8_t *)"Key", 3, 0, text1, 9);
    expect("rc4 Key", text1, "bbf316e8d940af0ad3", 9);
    uint8_t text2[] = "pedia";
    tkip_rc4((const uint8_t *)"Wiki", 4, 0, text2, 5);
    expect("rc4 Wiki", text2, "1021bf0420", 5);
    uint8_t text3[] = "Attack at dawn";
    tkip_rc4((const uint8_t *)"Secret", 6, 0, text3, 14);
    expect("rc4 Secret", text3, "45a01f645fc35b383552544b9bf5", 14);
    // (A skip is the same stream, later: the 4th byte on is the 4th byte on.)
    uint8_t whole[12] = { 0 }, later[9] = { 0 };
    tkip_rc4((const uint8_t *)"Key", 3, 0, whole, 12);
    tkip_rc4((const uint8_t *)"Key", 3, 3, later, 9);
    if (memcmp(whole + 3, later, 9) != 0)
    {
        printf("FAIL rc4 skip\n");
        failures++;
    }

    if (failures == 0)
        printf("tkip_test: ok\n");
    return failures == 0 ? 0 : 1;
}
