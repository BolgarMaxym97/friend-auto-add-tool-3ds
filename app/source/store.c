#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool store_writable(void)
{
    mkdir(STORE_DIR, 0777);
    const char *probe = STORE_DIR "/.probe";
    FILE *f = fopen(probe, "w");
    if (!f)
        return false;
    bool ok = fputs("ok", f) >= 0;
    ok = fclose(f) == 0 && ok;
    remove(probe);
    return ok;
}

bool store_load(Db *db, time_t *updated)
{
    static char text[DB_FILE_MAX];
    memset(db, 0, sizeof(*db));
    *updated = 0;

    FILE *f = fopen(STORE_DB, "rb");
    if (!f)
        return false;
    size_t len = fread(text, 1, sizeof(text), f);
    fclose(f);
    db_parse(db, text, len);

    FILE *meta = fopen(STORE_META, "r");
    if (meta) {
        long long t = 0;
        if (fscanf(meta, "%lld", &t) == 1)
            *updated = (time_t)t;
        fclose(meta);
    }
    return true;
}

bool store_save(const char *text, size_t len)
{
    mkdir(STORE_DIR, 0777);
    FILE *f = fopen(STORE_DB_TMP, "wb");
    if (!f)
        return false;
    bool ok = fwrite(text, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(STORE_DB_TMP);
        return false;
    }
    /* FAT will not rename onto an existing file */
    remove(STORE_DB);
    if (rename(STORE_DB_TMP, STORE_DB) != 0)
        return false;

    FILE *meta = fopen(STORE_META, "w");
    if (meta) {
        fprintf(meta, "%lld\n", (long long)time(NULL));
        fclose(meta);
    }
    return true;
}
