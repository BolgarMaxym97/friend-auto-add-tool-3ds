/*
 * What the app finds out about the console before it lets an action run, after the 3DS UA
 * installer's checks. Nothing here changes the console. Runs on the worker thread: logging
 * in to the friends server can take seconds.
 */
#ifndef CHECKS_H
#define CHECKS_H

#include <3ds.h>
#include <stdbool.h>

typedef enum { LEVEL_OK, LEVEL_INFO, LEVEL_WARN, LEVEL_FAIL } Level;

#define BATTERY_MIN 2 /* PTMU level 0-5 */
#define LOGIN_TIMEOUT_MS 15000

typedef struct {
    bool done;           /* a check has run at least once */
    bool is_luma;
    u32 luma_version;
    bool game_patching;
    bool frd_admin;      /* frd:a granted */
    u64 my_code;         /* 0 = no friends account */
    bool nimbus;         /* Nimbus' friends module patch is on the card */
    u8 account;          /* friends local account: 2 = Pretendo (Nimbus), 1 = Nintendo */
    bool wifi;
    bool online;         /* logged in to the friends server */
    Result login_result;
    bool sd_ok;
    u8 battery;
    bool charging;
} Console;

typedef enum { ACTION_ADD, ACTION_UPDATE, ACTION_REMOVE } Action;

/* `login` also logs in when the console is not online yet. */
void checks_run(Console *c, bool login);
/* Wi-Fi and the friends server's online state only: cheap, for the main screen. */
void checks_refresh_network(Console *c);
void checks_luma_string(const Console *c, char *out, size_t cap);

#endif
