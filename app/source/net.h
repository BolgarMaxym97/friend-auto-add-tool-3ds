/*
 * Downloading the code base over HTTPS: libcurl with mbedTLS, the peer checked against the
 * CA bundle in the RomFS (the author's build turned verification off).
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>

#define DB_URL "https://raw.githubusercontent.com/BolgarMaxym97/friend-auto-add-tool-3ds/main/db/friend_code.txt"
#define CA_BUNDLE "romfs:/cacert.pem"

bool net_init(void);
void net_exit(void);
/* Fetches `url` into `buf` (at most `cap` bytes; a larger answer fails). `err` gets a short
 * reason on failure. */
bool net_get(const char *url, char *buf, size_t cap, size_t *len, char *err, size_t err_cap);

#endif
