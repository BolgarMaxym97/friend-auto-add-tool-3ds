/*
 * The code base, friend_code.txt: one friend per line, oldest first, new ones appended.
 *
 *     # a comment
 *     1234-5678-9012|nick
 *     2204 1225 1663
 *
 * The name after '|' is optional; it becomes the provisional friend's screen name (at most
 * 10 characters, as the console keeps them). Pure C, built into the host tests too.
 */
#ifndef DB_H
#define DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DB_MAX 1000
#define DB_NAME_CHARS 10
#define DB_NAME_BYTES (DB_NAME_CHARS * 4 + 1) /* UTF-8, up to 4 bytes a character */
/* The file the app downloads and reads is never larger. */
#define DB_FILE_MAX (64 * 1024)

typedef struct {
    uint64_t code;
    char name[DB_NAME_BYTES];
} DbEntry;

typedef struct {
    DbEntry entries[DB_MAX];
    int count;
    int bad;       /* lines that are neither a code, a comment nor blank, or a code that fails its checksum */
    int dupes;     /* codes listed again further down; the first line wins */
    int overflow;  /* valid codes past DB_MAX */
} Db;

void db_parse(Db *db, const char *text, size_t len);
/* Index of `code` in the base, or -1. */
int db_find(const Db *db, uint64_t code);

#endif
