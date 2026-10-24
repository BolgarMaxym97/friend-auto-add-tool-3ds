/*
 * The friends module (frd:a), the way the system's own Friend List registers a friend code:
 * AddFriendOnline with the code's principal id - the server makes it a provisional friend,
 * or a full one when the other console has our code already - then the screen name from the
 * base, then Save. Pretendo only: Nintendo Network for the 3DS is gone (April 2024).
 */
#ifndef FRIENDS_H
#define FRIENDS_H

#include <3ds.h>
#include <stdbool.h>

#include "db.h"
#include "planner.h"

typedef struct {
    FriendKey key;
    u64 code;
    u8 relationship; /* RelationshipType */
    u32 attributes;  /* FriendAttributes */
    char name[DB_NAME_BYTES];
} Friend;

typedef struct {
    Friend items[FRIENDS_MAX];
    int count;
    int mutual, requests; /* complete vs. incomplete or local */
} FriendList;

/* frd:a, which only a title granted it (the CIA) gets. */
Result friends_init(void);
void friends_exit(void);

Result friends_my_code(u64 *code);
/* Local account 2 is the one Nimbus switches to for Pretendo, 1 is Nintendo's. */
Result friends_account(u8 *id);
bool friends_online(void);
/* HasLoggedIn: this session has called Login. */
bool friends_logged_in(void);
/* Logs in unless already online; waits up to `timeout_ms`. 0 once the module is online. */
Result friends_login(u32 timeout_ms);
Result friends_list(FriendList *list);
/* Registers `code` and names it `name` (empty: "FC" and its last 8 digits). On success
 * `relationship` is what the server made of it. */
Result friends_add(u64 code, const char *name, u8 *relationship);
Result friends_remove(const FriendKey *key);
Result friends_save(void);

/* Event signalled on FRIEND_REGISTERED_USER (someone accepted a request of ours). */
Result friends_watch(Handle *event);
/* Empties the notification queue; true if a friend registered us. */
bool friends_drain_notifications(void);

#endif
