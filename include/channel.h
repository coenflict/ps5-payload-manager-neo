#ifndef CHANNEL_H
#define CHANNEL_H

#include <stddef.h>

/* Manager self-updates scoped to a release channel (PLDMGR_UPDATE_REPO).
 *
 * Every build embeds "PLDMGR_CHANNEL:<owner/repo>"; official upstream builds
 * carry no marker and count as the official channel. A build only hands off
 * to or installs a manager from its own channel, so a fork is never replaced
 * by an upstream release (or the reverse). Official-channel builds keep the
 * legacy catalog-based mechanism unchanged; other channels download releases
 * from their own GitHub repository into CHANNEL_UPDATE_DIR. A newly installed
 * build that fails to come up is skipped on the next start. */

typedef struct ChannelStatus {
    char repo[128];
    int official;
    char pending[64];   /* installed, newer than running, starts next time */
    char latest[64];    /* newest release seen in the channel */
    char state[16];     /* idle | current | ready | failed | error | official */
    char message[160];
} ChannelStatus;

const char *channel_repo(void); /* embedded marker value */
int channel_is_official(void);
/* 1 when the ELF at fd carries this build's channel (offset preserved). */
int channel_fd_matches(int fd);
/* Startup: newest healthy same-channel build newer than this one. */
int channel_select_handoff(char *path, size_t size, char *version, size_t version_size);
void channel_record_attempt(const char *version);
void channel_clear_attempt(void);
void channel_mark_healthy(void); /* call once services are up */
/* Pending same-channel version for /list_payloads; 1 when present. */
int channel_pending_version(char *version, size_t size);
/* Background: download a verified newer channel release (next start). */
void channel_check_update(long max_age);
void channel_status(ChannelStatus *out);

#endif
