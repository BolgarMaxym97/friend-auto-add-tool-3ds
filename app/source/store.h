/*
 * The app's folder on the SD card: the code base, when it was last downloaded, and the log.
 */
#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "db.h"

#define STORE_DIR "sdmc:/FriendAutoAddTool"
#define STORE_DB STORE_DIR "/friend_code.txt"
#define STORE_DB_TMP STORE_DIR "/friend_code.tmp"
#define STORE_META STORE_DIR "/db_updated.txt"
#define STORE_LOG STORE_DIR "/log.txt"

/* Makes the folder and proves a file can be written in it. */
bool store_writable(void);
/* False when there is no base yet (or it cannot be read); `db` is empty then. */
bool store_load(Db *db, time_t *updated);
/* Writes `text` as the new base through a temporary file, so a cut-off write never leaves
 * half a base behind, and records the time. */
bool store_save(const char *text, size_t len);

#endif
