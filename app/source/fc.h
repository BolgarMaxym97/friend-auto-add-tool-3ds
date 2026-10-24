/*
 * Friend codes. A code is 12 decimal digits holding (checksum << 32) | principal id, the
 * checksum being the top 7 bits of SHA-1 over the little-endian principal id - the same
 * rule FRD_IsValidFriendCode() applies. Pure C, built into the host tests too.
 */
#ifndef FC_H
#define FC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FC_STR_LEN 15 /* "1234-5678-9012" + NUL */

/* 12 digits, with any spaces, tabs and dashes between them; nothing else. */
bool fc_parse(const char *s, size_t len, uint64_t *out);
/* A code the friends server could have issued: a principal id and its checksum. */
bool fc_valid(uint64_t fc);
uint32_t fc_pid(uint64_t fc);
uint64_t fc_from_pid(uint32_t pid);
void fc_format(uint64_t fc, char out[FC_STR_LEN]);

#endif
