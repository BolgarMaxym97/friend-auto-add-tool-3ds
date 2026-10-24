/*
 * The operation log: the last LOG_MAX lines in memory for the Log screen, every line also
 * appended to sdmc:/FriendAutoAddTool/log.txt. Written from the worker thread and the UI
 * thread alike, so every access takes the lock.
 */
#ifndef LOG_H
#define LOG_H

#include <3ds.h>
#include <stdbool.h>

#define LOG_MAX 200
#define LOG_TEXT 160

typedef enum {
    LOG_INFO,       /* plain message, no code */
    LOG_DONE,       /* the summary line of a finished job */
    LOG_MUTUAL,     /* added, and they had added us: friends now */
    LOG_REQUEST,    /* added as a provisional friend */
    LOG_NOW_MUTUAL, /* an earlier request turned into a friend */
    LOG_SKIP,
    LOG_ERROR,
    LOG_REMOVED,
} LogKind;

typedef struct {
    char time[6]; /* "14:02" */
    LogKind kind;
    u64 code;     /* 0 for a message line */
    char text[LOG_TEXT];
} LogLine;

void log_init(const char *path);
void log_add(LogKind kind, u64 code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int log_count(void);
/* `i` 0 is the newest line. */
bool log_get(int i, LogLine *out);
/* The short word on a line's badge ("заявка"), or NULL for a message line. */
const char *log_badge(LogKind kind);

#endif
