#ifndef UPDATER_H
#define UPDATER_H

#include <stddef.h>

/* Automatic payload updates.
 *
 * At startup one background pass checks the payloads referenced by the
 * autoload list first (in sequence order), then the other installed payloads.
 * Each payload is compared against its maintainer's latest release and the
 * curated catalog; a verified newer version is installed into the same slot
 * (the replaced version is retained for rollback). Autoload waits for an
 * entry's own check before launching it, bounded by the boot budget, so
 * updates apply to the current boot while failures, timeouts and missing
 * network always fall back to the installed payload.
 *
 * Policies per payload: "auto" installs any newer verified release, "safe"
 * (default) holds major-version changes for approval, "notify" only reports,
 * "off" skips the payload entirely. Prereleases are opt-in per payload. */

int updater_start(int boot);   /* 0 started, 1 already running, -1 failure */
void updater_stop(void);       /* cancel and join; never from the worker */
void updater_cancel(void);     /* cancel without waiting for a running download */
int updater_check_now(void);   /* manual pass: fresh conditional requests */

/* Map an autoload entry to its managed slot key (out empty when unmanaged)
 * and wait until that slot's update job finished, the boot budget expired or
 * cancelled() returns nonzero. Returns 0 ready/unmanaged, 1 timed out,
 * -1 cancelled. */
typedef int (*UpdaterCancelled)(void *context);
int updater_wait_entry(const char *entry, char *slot_key, size_t size,
                       UpdaterCancelled cancelled, void *context);
int updater_boot_waiting(void); /* autoload status: waiting on updates */

size_t updater_status_json(char *buf, size_t size);
/* Partial settings update. 0, CONFIG_INVALID or -1 (persistence failure). */
int updater_settings_json(const char *json);
/* Explicit approval: install the available version of one slot now. */
int updater_apply(const char *slot_key, char *out_json, size_t size);
/* Restore the retained version and skip the version it replaced. */
int updater_rollback(const char *slot_key, char *out_json, size_t size);
/* Never install this exact version automatically again. */
int updater_skip_version(const char *slot_key, const char *version);

#endif
