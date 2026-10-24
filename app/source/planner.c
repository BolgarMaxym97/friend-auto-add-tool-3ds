#include "planner.h"

#include <stdbool.h>
#include <string.h>

static bool in_list(uint64_t code, const uint64_t *friends, int count)
{
    for (int i = 0; i < count; i++)
        if (friends[i] == code)
            return true;
    return false;
}

void plan_make(Plan *plan, const Db *db, uint64_t my_code, const uint64_t *friends, int friend_count)
{
    memset(plan, 0, sizeof(*plan));
    int room = FRIENDS_MAX - friend_count;
    for (int i = db->count - 1; i >= 0; i--) {
        uint64_t code = db->entries[i].code;
        PlanItem *item = &plan->items[plan->count++];
        item->entry = i;
        if (my_code && code == my_code) {
            item->reason = PLAN_SELF;
            plan->self++;
        } else if (in_list(code, friends, friend_count)) {
            item->reason = PLAN_EXISTS;
            plan->exists++;
        } else if (plan->add >= room) {
            item->reason = PLAN_NO_ROOM;
            plan->no_room++;
        } else {
            item->reason = PLAN_ADD;
            plan->add++;
        }
    }
}
