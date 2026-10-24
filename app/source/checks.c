#include "checks.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "friends.h"
#include "store.h"

/* Luma's svcGetSystemInfo(0x10000, ...): 0 = version, 3 = the config bitfield, bit 2 of
 * which is "Enable game patching" - also what loads Nimbus' patches (3DS UA checks.c). */
#define LUMA_INFO 0x10000
#define LUMA_INFO_VERSION 0
#define LUMA_INFO_CONFIG 3
#define LUMA_CFG_GAME_PATCHING (1u << 2)

/* Nimbus patches the friends module (0004013000003202) to talk to Pretendo: older Luma reads
 * the patch from titles/, Luma 13+ from sysmodules/. */
static const char *const NIMBUS_PATCHES[] = {
    "sdmc:/luma/titles/0004013000003202/code.ips",
    "sdmc:/luma/sysmodules/0004013000003202.ips",
};

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

void checks_refresh_network(Console *c)
{
    u32 wifi = 0;
    c->wifi = R_SUCCEEDED(ACU_GetWifiStatus(&wifi)) && wifi != 0;
    c->online = c->frd_admin && friends_online();
}

void checks_run(Console *c, bool login)
{
    memset(c, 0, sizeof(*c));

    s64 out = 0;
    if (R_SUCCEEDED(svcGetSystemInfo(&out, LUMA_INFO, LUMA_INFO_VERSION))) {
        c->is_luma = true;
        c->luma_version = (u32)out;
        if (R_SUCCEEDED(svcGetSystemInfo(&out, LUMA_INFO, LUMA_INFO_CONFIG)))
            c->game_patching = out & LUMA_CFG_GAME_PATCHING;
    }

    for (size_t i = 0; i < sizeof(NIMBUS_PATCHES) / sizeof(*NIMBUS_PATCHES); i++)
        c->nimbus |= exists(NIMBUS_PATCHES[i]);

    c->frd_admin = R_SUCCEEDED(friends_init());
    if (c->frd_admin) {
        friends_my_code(&c->my_code);
        friends_account(&c->account);
    }

    checks_refresh_network(c);
    if (login && c->frd_admin && c->my_code && c->wifi && !c->online) {
        c->login_result = friends_login(LOGIN_TIMEOUT_MS);
        c->online = R_SUCCEEDED(c->login_result);
    }

    c->sd_ok = store_writable();

    PTMU_GetBatteryLevel(&c->battery);
    bool adapter = false;
    u8 charge = 0;
    PTMU_GetAdapterState(&adapter);
    PTMU_GetBatteryChargeState(&charge);
    c->charging = adapter || charge;
    c->done = true;
}

void checks_luma_string(const Console *c, char *out, size_t cap)
{
    u32 v = c->luma_version;
    if (GET_VERSION_REVISION(v))
        snprintf(out, cap, "v%lu.%lu.%lu", GET_VERSION_MAJOR(v), GET_VERSION_MINOR(v), GET_VERSION_REVISION(v));
    else
        snprintf(out, cap, "v%lu.%lu", GET_VERSION_MAJOR(v), GET_VERSION_MINOR(v));
}
