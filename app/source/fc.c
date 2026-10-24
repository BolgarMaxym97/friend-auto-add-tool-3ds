#include "fc.h"

#include <stdio.h>
#include <string.h>

#define FC_MAX 999999999999ull

static uint32_t rol(uint32_t v, int n)
{
    return v << n | v >> (32 - n);
}

/* SHA-1 of a message shorter than 56 bytes: one block. */
static void sha1_short(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint8_t block[64] = {0};
    uint32_t w[80];
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    memcpy(block, msg, len);
    block[len] = 0x80;
    block[63] = (uint8_t)(len * 8);
    block[62] = (uint8_t)(len * 8 >> 8);
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 |
               block[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

static uint8_t checksum(uint32_t pid)
{
    uint8_t le[4] = {(uint8_t)pid, (uint8_t)(pid >> 8), (uint8_t)(pid >> 16), (uint8_t)(pid >> 24)};
    uint8_t digest[20];
    sha1_short(le, sizeof(le), digest);
    return digest[0] >> 1;
}

bool fc_parse(const char *s, size_t len, uint64_t *out)
{
    uint64_t v = 0;
    int digits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (++digits > 12)
                return false;
            v = v * 10 + (uint64_t)(c - '0');
        } else if (c != ' ' && c != '\t' && c != '-') {
            return false;
        }
    }
    if (digits != 12)
        return false;
    *out = v;
    return true;
}

bool fc_valid(uint64_t fc)
{
    if (fc == 0 || fc > FC_MAX || fc >> 39)
        return false;
    uint32_t pid = fc_pid(fc);
    return pid != 0 && (fc >> 32) == checksum(pid);
}

uint32_t fc_pid(uint64_t fc)
{
    return (uint32_t)fc;
}

uint64_t fc_from_pid(uint32_t pid)
{
    return (uint64_t)checksum(pid) << 32 | pid;
}

void fc_format(uint64_t fc, char out[FC_STR_LEN])
{
    snprintf(out, FC_STR_LEN, "%04u-%04u-%04u", (unsigned)(fc / 100000000ull % 10000),
             (unsigned)(fc / 10000ull % 10000), (unsigned)(fc % 10000));
}
