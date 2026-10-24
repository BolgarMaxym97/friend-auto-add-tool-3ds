#include "db.h"

#include <string.h>

#include "fc.h"

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static void trim(const char **s, const char **e)
{
    while (*s < *e && is_space(**s))
        (*s)++;
    while (*e > *s && is_space((*e)[-1]))
        (*e)--;
}

/* Copies at most DB_NAME_CHARS UTF-8 characters, never cutting one in half; a stray byte
 * that does not start a character ends the name. */
static void copy_name(char *out, const char *s, const char *e)
{
    size_t n = 0;
    int chars = 0;
    while (s < e && chars < DB_NAME_CHARS) {
        unsigned char c = (unsigned char)*s;
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (!len || (size_t)(e - s) < len)
            break;
        memcpy(out + n, s, len);
        n += len;
        s += len;
        chars++;
    }
    out[n] = 0;
}

static void parse_line(Db *db, const char *s, const char *e)
{
    trim(&s, &e);
    if (s == e || *s == '#')
        return;

    const char *bar = memchr(s, '|', (size_t)(e - s));
    const char *code_end = bar ? bar : e;
    uint64_t code;
    if (!fc_parse(s, (size_t)(code_end - s), &code) || !fc_valid(code)) {
        db->bad++;
        return;
    }
    if (db_find(db, code) >= 0) {
        db->dupes++;
        return;
    }
    if (db->count >= DB_MAX) {
        db->overflow++;
        return;
    }

    DbEntry *entry = &db->entries[db->count++];
    entry->code = code;
    entry->name[0] = 0;
    if (bar) {
        const char *ns = bar + 1, *ne = e;
        trim(&ns, &ne);
        copy_name(entry->name, ns, ne);
    }
}

void db_parse(Db *db, const char *text, size_t len)
{
    memset(db, 0, sizeof(*db));
    const char *p = text, *end = text + len;
    /* a UTF-8 byte order mark from a Windows editor */
    if (len >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3))
        p += 3;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        parse_line(db, p, line_end);
        p = nl ? nl + 1 : end;
    }
}

int db_find(const Db *db, uint64_t code)
{
    for (int i = 0; i < db->count; i++)
        if (db->entries[i].code == code)
            return i;
    return -1;
}
