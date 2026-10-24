#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fc.h"

static LogLine lines[LOG_MAX];
static int head, count; /* lines[head] is the next slot */
static LightLock lock;
static char file_path[64];

void log_init(const char *path)
{
    LightLock_Init(&lock);
    snprintf(file_path, sizeof(file_path), "%s", path);
}

const char *log_badge(LogKind kind)
{
    switch (kind) {
    case LOG_MUTUAL: return "взаємний";
    case LOG_REQUEST: return "заявка";
    case LOG_NOW_MUTUAL: return "тепер взаємний";
    case LOG_SKIP: return "пропущено";
    case LOG_ERROR: return "помилка";
    case LOG_REMOVED: return "видалено";
    case LOG_INFO:
    case LOG_DONE: break;
    }
    return NULL;
}

void log_add(LogKind kind, u64 code, const char *fmt, ...)
{
    LogLine line = {.kind = kind, .code = code};
    va_list args;
    va_start(args, fmt);
    vsnprintf(line.text, sizeof(line.text), fmt, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now); /* the console clock is local time already */
    char stamp[20] = "";
    if (tm) {
        strftime(line.time, sizeof(line.time), "%H:%M", tm);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
    }

    LightLock_Lock(&lock);
    lines[head] = line;
    head = (head + 1) % LOG_MAX;
    if (count < LOG_MAX)
        count++;
    FILE *f = file_path[0] ? fopen(file_path, "a") : NULL;
    if (f) {
        char fc[FC_STR_LEN] = "";
        if (code)
            fc_format(code, fc);
        const char *badge = log_badge(kind);
        fprintf(f, "%s %s%s%s%s%s\n", stamp, fc, code ? " " : "", badge ? badge : "", badge ? ": " : "", line.text);
        fclose(f);
    }
    LightLock_Unlock(&lock);
}

int log_count(void)
{
    LightLock_Lock(&lock);
    int n = count;
    LightLock_Unlock(&lock);
    return n;
}

bool log_get(int i, LogLine *out)
{
    bool ok = false;
    LightLock_Lock(&lock);
    if (i >= 0 && i < count) {
        *out = lines[(head - 1 - i + LOG_MAX) % LOG_MAX];
        ok = true;
    }
    LightLock_Unlock(&lock);
    return ok;
}
