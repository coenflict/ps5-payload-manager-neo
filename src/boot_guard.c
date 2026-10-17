#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "boot_guard.h"
#include "json_helpers.h"
#include "pldmgr.h"
#include "slot.h"
#include "updater.h"

#define MAX_TRACKED 64

typedef struct { char slot[128]; char version[64]; } Tracked;

static pthread_mutex_t guard_mutex = PTHREAD_MUTEX_INITIALIZER;
static Tracked updated[MAX_TRACKED], launched[MAX_TRACKED];
static int updated_count, launched_count, probation;
static long long probation_end;

static long long monotonic_seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

static void write_guard(const char *state) {
    char text[MAX_TRACKED * 200 + 64];
    size_t pos = (size_t)snprintf(text, sizeof(text), "state=%s\nlaunched=", state);
    for (int i = 0; i < launched_count && pos < sizeof(text); i++)
        pos += (size_t)snprintf(text + pos, sizeof(text) - pos, "%s%s|%s", i ? "," : "", launched[i].slot, launched[i].version);
    if (pos < sizeof(text)) pos += (size_t)snprintf(text + pos, sizeof(text) - pos, "\n");
    if (pos >= sizeof(text) || write_file_text(BOOT_GUARD_PATH, text, pos))
        pldmgr_log("[BootGuard] Could not record launch state.\n");
}

static int restore(const char *slot, const char *bad_version) {
    char dir[640];
    SlotMeta *current = malloc(sizeof(*current)), *restored = malloc(sizeof(*restored));
    int rc = -1;
    snprintf(dir, sizeof(dir), "%s/%s", PAYLOADS_STORAGE_DIR, slot);
    /* Only undo the exact version this guard installed; a manual change wins. */
    if (current && restored && !slot_read_meta(dir, current) && !strcmp(current->version, bad_version) &&
        !slot_restore_previous(slot, restored)) {
        updater_skip_version(slot, bad_version);
        pldmgr_log("[BootGuard] Previous boot stopped after launching %s %s; restored %s.\n",
                   slot, bad_version, restored->version[0] ? restored->version : "previous version");
        pldmgr_notify("Payload Manager\nRestored %s %s after an interrupted boot",
                      restored->name[0] ? restored->name : slot, restored->version);
        rc = 0;
    }
    free(current); free(restored);
    return rc;
}

int boot_guard_evaluate(int replaced_running_instance) {
    char *text = NULL; size_t size = 0;
    if (read_file_text(BOOT_GUARD_PATH, &text, &size)) return 0;
    int rolled_back = 0, active = 0;
    char *save = NULL, *list = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strcmp(line, "state=launching") || !strcmp(line, "state=probation")) active = 1;
        else if (!strncmp(line, "launched=", 9)) list = line + 9;
    }
    if (active && !replaced_running_instance && list) {
        char *item_save = NULL;
        for (char *item = strtok_r(list, ",", &item_save); item; item = strtok_r(NULL, ",", &item_save)) {
            char *bar = strchr(item, '|');
            if (!bar || bar == item || !bar[1]) continue;
            *bar = 0;
            if (!restore(item, bar + 1)) rolled_back++;
        }
    }
    free(text);
    pthread_mutex_lock(&guard_mutex);
    launched_count = 0; probation = 0;
    write_guard("clean");
    pthread_mutex_unlock(&guard_mutex);
    return rolled_back;
}

void boot_guard_updated(const char *slot_key, const char *version) {
    if (!slot_key || !version || strlen(slot_key) >= 128 || strlen(version) >= 64 ||
        strpbrk(slot_key, ",|\n") || strpbrk(version, ",|\n")) return;
    pthread_mutex_lock(&guard_mutex);
    int i = 0;
    while (i < updated_count && strcmp(updated[i].slot, slot_key)) i++;
    if (i < MAX_TRACKED) {
        snprintf(updated[i].slot, sizeof(updated[i].slot), "%s", slot_key);
        snprintf(updated[i].version, sizeof(updated[i].version), "%s", version);
        if (i == updated_count) updated_count++;
    }
    pthread_mutex_unlock(&guard_mutex);
}

void boot_guard_launching(const char *slot_key) {
    if (!slot_key) return;
    pthread_mutex_lock(&guard_mutex);
    for (int i = 0; i < updated_count; i++) {
        if (strcmp(updated[i].slot, slot_key)) continue;
        int seen = 0;
        for (int k = 0; k < launched_count; k++) if (!strcmp(launched[k].slot, slot_key)) seen = 1;
        /* Persist before the launch: a crash during it must be attributable. */
        if (!seen && launched_count < MAX_TRACKED) {
            launched[launched_count++] = updated[i];
            probation = 0;
            write_guard("launching");
        }
    }
    pthread_mutex_unlock(&guard_mutex);
}

void boot_guard_sequence_done(void) {
    pthread_mutex_lock(&guard_mutex);
    if (launched_count) {
        probation = 1;
        probation_end = monotonic_seconds() + BOOT_GUARD_PROBATION_SECONDS;
        write_guard("probation");
    }
    pthread_mutex_unlock(&guard_mutex);
}

void boot_guard_tick(void) {
    pthread_mutex_lock(&guard_mutex);
    if (probation && monotonic_seconds() >= probation_end) {
        probation = 0;
        launched_count = 0;
        write_guard("clean");
        pldmgr_log("[BootGuard] Updated payloads stayed up; boot marked healthy.\n");
    }
    pthread_mutex_unlock(&guard_mutex);
}
