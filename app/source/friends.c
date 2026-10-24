#include "friends.h"

#include <stdio.h>
#include <string.h>

#include "fc.h"

/* Pretendo can take over 10 s per code when busy (New 3DS log, 2026-10-24) */
#define ADD_TIMEOUT_NS (30ull * 1000 * 1000 * 1000)
/* what svcWaitSynchronization returns on timeout: not a failure by R_FAILED */
#define WAIT_TIMED_OUT 0x09401BFE

static bool ready;

Result friends_init(void)
{
    if (ready)
        return 0;
    Result res = frdInit(false);
    if (R_FAILED(res))
        return res;
    ready = true;
    return 0;
}

void friends_exit(void)
{
    if (ready)
        frdExit();
    ready = false;
}

Result friends_my_code(u64 *code)
{
    FriendKey key;
    Result res = FRD_GetMyFriendKey(&key);
    if (R_FAILED(res))
        return res;
    *code = key.principalId ? fc_from_pid(key.principalId) : 0;
    return 0;
}

Result friends_account(u8 *id)
{
    return FRD_GetMyLocalAccountId(id);
}

bool friends_online(void)
{
    bool online = false;
    return R_SUCCEEDED(FRD_IsOnline(&online)) && online;
}

bool friends_logged_in(void)
{
    bool logged = false;
    return R_SUCCEEDED(FRD_HasLoggedIn(&logged)) && logged;
}

/* 0xC960C501 here is Pretendo's NASC 122: the console's uidhmac is stale, which Nimbus 2.2.0
 * repairs. The result is a hint, not a verdict: online is what counts, and the friends module
 * may get there a moment after the event. */
Result friends_login(u32 timeout_ms)
{
    if (friends_online())
        return 0;
    Handle event;
    Result res = svcCreateEvent(&event, RESET_ONESHOT);
    if (R_FAILED(res))
        return res;
    res = FRD_Login(event);
    if (R_SUCCEEDED(res)) {
        res = svcWaitSynchronization(event, (s64)timeout_ms * 1000000);
        if (R_SUCCEEDED(res))
            res = FRD_GetLastResponseResult();
    }
    svcCloseHandle(event);
    for (int i = 0; i < 6 && !friends_online(); i++)
        svcSleepThread(500ull * 1000 * 1000);
    if (friends_online())
        return 0;
    return R_FAILED(res) ? res : MAKERESULT(RL_PERMANENT, RS_NOTFOUND, RM_APPLICATION, RD_NOT_FOUND);
}

static void name_to_utf8(const MiiScreenName name, char out[DB_NAME_BYTES])
{
    u16 units[MII_NAME_LEN];
    memcpy(units, name, sizeof(units));
    units[MII_NAME_LEN - 1] = 0;
    ssize_t n = utf16_to_utf8((u8 *)out, units, DB_NAME_BYTES - 1);
    out[n > 0 ? n : 0] = 0;
    out[DB_NAME_BYTES - 1] = 0;
}

/* Names and relationships come from FRD_GetFriendInfo, not FRD_GetMiiScreenName: the friends
 * module answers GetMiiScreenName in two static buffers (names, character sets) and libctru
 * sets up only the first, so the reply to a non-empty list panics the kernel (crash dumps in
 * the friends process on the test console, 2026-10-23). */
Result friends_list(FriendList *list)
{
    static FriendKey keys[FRIENDS_MAX];
    static FriendInfo infos[FRIENDS_MAX];
    static u32 attributes[FRIENDS_MAX];
    u32 n = 0;

    memset(list, 0, sizeof(*list));
    Result res = FRD_GetFriendKeyList(keys, &n, 0, FRIENDS_MAX);
    if (R_FAILED(res))
        return res;
    if (n > FRIENDS_MAX)
        n = FRIENDS_MAX;
    if (!n)
        return 0;
    memset(infos, 0, sizeof(infos));
    memset(attributes, 0, sizeof(attributes));
    res = FRD_GetFriendInfo(infos, keys, n, false, false);
    if (R_FAILED(res))
        return res;
    FRD_GetFriendAttributeFlags(attributes, keys, n);

    for (u32 i = 0; i < n; i++) {
        Friend *f = &list->items[i];
        f->key = keys[i];
        f->code = fc_from_pid(keys[i].principalId);
        f->relationship = infos[i].relationship;
        f->attributes = attributes[i];
        name_to_utf8(infos[i].screenName, f->name);
        if (f->relationship == RELATIONSHIP_COMPLETE)
            list->mutual++;
        else if (f->relationship == RELATIONSHIP_INCOMPLETE || f->relationship == RELATIONSHIP_LOCAL)
            list->requests++;
    }
    list->count = (int)n;
    return 0;
}

static void screen_name(u64 code, const char *name, MiiScreenName out)
{
    char fallback[16];
    memset(out, 0, sizeof(MiiScreenName));
    if (!name || !name[0]) {
        snprintf(fallback, sizeof(fallback), "FC%08llu", (unsigned long long)(code % 100000000ull));
        name = fallback;
    }
    /* utf8_to_utf16 stops at the units it is given; db.c already cut the name to 10 characters */
    ssize_t n = utf8_to_utf16(out, (const u8 *)name, MII_NAME_LEN - 1);
    if (n < 0)
        n = 0;
    out[n < MII_NAME_LEN ? n : MII_NAME_LEN - 1] = 0;
}

static bool find_key(u32 pid, FriendKey *key, u8 *relationship)
{
    static FriendList list;
    if (R_FAILED(friends_list(&list)))
        return false;
    for (int i = 0; i < list.count; i++) {
        if (list.items[i].key.principalId == pid) {
            *key = list.items[i].key;
            *relationship = list.items[i].relationship;
            return true;
        }
    }
    return false;
}

Result friends_add(u64 code, const char *name, u8 *relationship)
{
    u32 pid = fc_pid(code);
    FriendKey key;
    /* An add that reported an error may still have reached the server and the list (New 3DS
     * log, 2026-10-24): a retry finds the code there and only names it. */
    if (!find_key(pid, &key, relationship)) {
        Handle event;
        Result res = svcCreateEvent(&event, RESET_ONESHOT);
        if (R_FAILED(res))
            return res;
        res = FRDA_AddFriendOnline(event, pid);
        if (R_SUCCEEDED(res)) {
            res = svcWaitSynchronization(event, ADD_TIMEOUT_NS);
            if (res == WAIT_TIMED_OUT)
                res = MAKERESULT(RL_TEMPORARY, RS_STATUSCHANGED, RM_APPLICATION, RD_TIMEOUT);
            else if (R_SUCCEEDED(res))
                res = FRD_GetLastResponseResult();
        }
        svcCloseHandle(event);
        if (R_FAILED(res))
            return res;
        if (!find_key(pid, &key, relationship))
            return MAKERESULT(RL_PERMANENT, RS_NOTFOUND, RM_APPLICATION, RD_NOT_FOUND);
    }

    MiiScreenName mii_name;
    screen_name(code, name, mii_name);
    FRDA_UpdateMiiScreenName(&key, &mii_name, CHARSET_JPN_USA_EUR);
    return FRDA_Save();
}

Result friends_remove(const FriendKey *key)
{
    return FRDA_RemoveFriend(key->principalId, key->localFriendCode);
}

Result friends_save(void)
{
    return FRDA_Save();
}

Result friends_watch(Handle *event)
{
    Result res = svcCreateEvent(event, RESET_ONESHOT);
    if (R_FAILED(res))
        return res;
    res = FRD_AttachToEventNotification(*event);
    if (R_SUCCEEDED(res))
        res = FRD_SetNotificationMask(MASK_FRIEND_REGISTERED_USER);
    if (R_FAILED(res)) {
        svcCloseHandle(*event);
        *event = 0;
    }
    return res;
}

bool friends_drain_notifications(void)
{
    NotificationEvent events[8];
    bool registered = false;
    for (int guard = 0; guard < 8; guard++) {
        u32 n = 0;
        if (R_FAILED(FRD_GetEventNotification(events, 8, &n)) || n == 0)
            break;
        for (u32 i = 0; i < n && i < 8; i++)
            if (events[i].type == FRIEND_REGISTERED_USER)
                registered = true;
    }
    return registered;
}
