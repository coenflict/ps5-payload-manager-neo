#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "channel.h"
#include "json_helpers.h"
#include "pldmgr.h"
#include "repository.h"
#include "sha256.h"
#include "slot.h"
#include "upstream.h"

__attribute__((used)) volatile const char pldmgr_channel_sig[] = "PLDMGR_CHANNEL:" PLDMGR_UPDATE_REPO;

#define MARKER "PLDMGR_CHANNEL:"
#define MARKER_LEN 15
#define STATE_PATH CHANNEL_UPDATE_DIR "/state.txt"

static pthread_mutex_t channel_mutex = PTHREAD_MUTEX_INITIALIZER;
static ChannelStatus status;

/* Referenced from startup and status so --gc-sections keeps the marker. */
const char *channel_repo(void) { return (const char *)pldmgr_channel_sig + MARKER_LEN; }

int channel_is_official(void) { return !strcasecmp(PLDMGR_UPDATE_REPO, PLDMGR_OFFICIAL_REPO); }

static int channel_value_valid(const char *v, size_t n) {
    int slashes = 0;
    if (n < 3) return 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] == '/') slashes++;
        else if (!isalnum((unsigned char)v[i]) && v[i] != '-' && v[i] != '_' && v[i] != '.') return 0;
    }
    return slashes == 1 && v[0] != '/' && v[n - 1] != '/';
}

/* The scanning code's own needle is followed by NUL and is never a value. */
static int read_channel_fd(int fd, char *out, size_t size) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > PLDMGR_PAYLOAD_DOWNLOAD_LIMIT) return -1;
    char buffer[8192 + 160];
    size_t carry = 0;
    off_t offset = 0;
    while (offset < st.st_size) {
        ssize_t n = pread(fd, buffer + carry, 8192, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        size_t used = carry + (size_t)n;
        for (size_t i = 0; i + MARKER_LEN < used; i++) {
            if (memcmp(buffer + i, MARKER, MARKER_LEN)) continue;
            const char *v = buffer + i + MARKER_LEN;
            size_t available = used - i - MARKER_LEN;
            const char *end = memchr(v, 0, available < 128 ? available : 128);
            if (!end || (size_t)(end - v) >= size || !channel_value_valid(v, (size_t)(end - v))) continue;
            memcpy(out, v, (size_t)(end - v));
            out[end - v] = 0;
            return 0;
        }
        offset += n;
        carry = used < 160 ? used : 160;
        memmove(buffer, buffer + used - carry, carry);
    }
    return -1;
}

int channel_fd_matches(int fd) {
    char value[128];
    if (read_channel_fd(fd, value, sizeof(value))) snprintf(value, sizeof(value), "%s", PLDMGR_OFFICIAL_REPO);
    return !strcasecmp(value, PLDMGR_UPDATE_REPO);
}

/* ── Handoff health record ────────────────────────────────── */

typedef struct { char attempt[64], healthy[64], failed[1024]; } State;

static void read_state(State *s) {
    memset(s, 0, sizeof(*s));
    char *text = NULL; size_t size = 0, n;
    if (read_file_text(STATE_PATH, &text, &size)) return;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "attempt=", 8) && (n = strlen(line + 8)) < sizeof(s->attempt)) strcpy(s->attempt, line + 8);
        else if (!strncmp(line, "healthy=", 8) && (n = strlen(line + 8)) < sizeof(s->healthy)) strcpy(s->healthy, line + 8);
        else if (!strncmp(line, "failed=", 7) && (n = strlen(line + 7)) < sizeof(s->failed)) strcpy(s->failed, line + 7);
    }
    free(text);
}

static void write_state(const State *s) {
    char text[1280];
    int n = snprintf(text, sizeof(text), "attempt=%s\nhealthy=%s\nfailed=%s\n", s->attempt, s->healthy, s->failed);
    if (n < 0 || (size_t)n >= sizeof(text) || ensure_dir_recursive(CHANNEL_UPDATE_DIR) ||
        write_file_text(STATE_PATH, text, (size_t)n))
        pldmgr_log("[Channel] Could not record self-update state.\n");
}

static int is_failed(const State *s, const char *version) {
    size_t n = strlen(version);
    for (const char *p = s->failed; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !strncmp(p, version, n)) return 1;
        p += len + (end ? 1 : 0);
    }
    return 0;
}

static void add_failed(State *s, const char *version) {
    if (is_failed(s, version)) return;
    size_t used = strlen(s->failed), n = strlen(version);
    /* Oldest entries go first when the bounded list is full. */
    while (used && used + n + 2 > sizeof(s->failed)) {
        char *comma = strchr(s->failed, ',');
        if (!comma) { s->failed[0] = 0; used = 0; break; }
        memmove(s->failed, comma + 1, strlen(comma + 1) + 1);
        used = strlen(s->failed);
    }
    snprintf(s->failed + used, sizeof(s->failed) - used, "%s%s", used ? "," : "", version);
}

static int scan_channel(const State *s, char *path, size_t size, char *version, size_t version_size) {
    if (!path || !version || version_size < 64) return 0;
    path[0] = 0;
    snprintf(version, version_size, "%s", MENU_VERSION);
    DIR *d = opendir(CHANNEL_UPDATE_DIR);
    if (!d) return 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        char candidate_path[1024], candidate[64];
        if (entry->d_name[0] == '.') continue;
        snprintf(candidate_path, sizeof(candidate_path), "%s/%s", CHANNEL_UPDATE_DIR, entry->d_name);
        int fd = slot_open_self_update(candidate_path);
        if (fd < 0) continue;
        int valid = !repository_self_update_version_fd(fd, candidate, sizeof(candidate)) && channel_fd_matches(fd);
        close(fd);
        if (valid && !is_failed(s, candidate) && slot_version_compare(candidate, version) > 0 && strlen(candidate_path) < size) {
            snprintf(version, version_size, "%s", candidate);
            snprintf(path, size, "%s", candidate_path);
        }
    }
    closedir(d);
    return path[0] != 0;
}

int channel_select_handoff(char *path, size_t size, char *version, size_t version_size) {
    if (channel_is_official()) return repository_check_self_update(path, size, version, version_size);
    pthread_mutex_lock(&channel_mutex);
    State s;
    read_state(&s);
    if (s.attempt[0] && slot_version_compare(s.attempt, MENU_VERSION) && strcmp(s.attempt, s.healthy)) {
        /* The previous start handed off to this build and it never came up. */
        add_failed(&s, s.attempt);
        pldmgr_log("[Channel] Payload Manager %s did not start; staying on %s.\n", s.attempt, MENU_VERSION);
        pldmgr_notify("Payload Manager %s failed to start\nUsing %s instead", s.attempt, MENU_VERSION);
        s.attempt[0] = 0;
        write_state(&s);
    }
    int found = scan_channel(&s, path, size, version, version_size);
    pthread_mutex_unlock(&channel_mutex);
    return found;
}

void channel_record_attempt(const char *version) {
    if (channel_is_official() || !version || strlen(version) >= 64) return;
    pthread_mutex_lock(&channel_mutex);
    State s;
    read_state(&s);
    snprintf(s.attempt, sizeof(s.attempt), "%s", version);
    write_state(&s);
    pthread_mutex_unlock(&channel_mutex);
}

void channel_clear_attempt(void) {
    if (channel_is_official()) return;
    pthread_mutex_lock(&channel_mutex);
    State s;
    read_state(&s);
    if (s.attempt[0]) { s.attempt[0] = 0; write_state(&s); }
    pthread_mutex_unlock(&channel_mutex);
}

void channel_mark_healthy(void) {
    if (channel_is_official()) return;
    pthread_mutex_lock(&channel_mutex);
    State s;
    read_state(&s);
    if (s.attempt[0] && !slot_version_compare(s.attempt, MENU_VERSION)) {
        snprintf(s.healthy, sizeof(s.healthy), "%s", MENU_VERSION);
        s.attempt[0] = 0;
        write_state(&s);
    }
    pthread_mutex_unlock(&channel_mutex);
}

int channel_pending_version(char *version, size_t size) {
    char path[1024];
    if (channel_is_official()) return repository_check_self_update(path, sizeof(path), version, size);
    pthread_mutex_lock(&channel_mutex);
    State s;
    read_state(&s);
    int found = scan_channel(&s, path, sizeof(path), version, size);
    pthread_mutex_unlock(&channel_mutex);
    return found;
}

/* ── Release download ─────────────────────────────────────── */

static void set_status(const char *latest, const char *state, const char *message) {
    pthread_mutex_lock(&channel_mutex);
    if (latest) snprintf(status.latest, sizeof(status.latest), "%s", latest);
    snprintf(status.state, sizeof(status.state), "%s", state);
    snprintf(status.message, sizeof(status.message), "%s", message);
    pthread_mutex_unlock(&channel_mutex);
}

static void prune_channel(const char *keep) {
    DIR *d = opendir(CHANNEL_UPDATE_DIR);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        char path[1024], version[64];
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, keep)) continue;
        snprintf(path, sizeof(path), "%s/%s", CHANNEL_UPDATE_DIR, entry->d_name);
        int fd = slot_open_self_update(path);
        if (fd < 0) continue;
        int versioned = !repository_self_update_version_fd(fd, version, sizeof(version));
        close(fd);
        /* The running build stays as the fallback if the new one fails. */
        if (versioned && slot_version_compare(version, MENU_VERSION) && unlink(path))
            pldmgr_log("[Channel] Could not remove superseded %s\n", entry->d_name);
    }
    closedir(d);
}

void channel_check_update(long max_age) {
    if (channel_is_official()) { set_status(NULL, "official", "Official builds update through the official repository"); return; }
    UpstreamSpec spec = {0};
    if (upstream_parse_repo(PLDMGR_UPDATE_REPO, &spec)) { set_status(NULL, "error", "Invalid update channel"); return; }
    snprintf(spec.pattern, sizeof(spec.pattern), "pldmgr_v*.elf");
    UpstreamRelease release;
    char error[96] = "";
    int from_cache = 0;
    int rc = upstream_check(&spec, 0, max_age, UPSTREAM_CACHE_DIR, &release, &from_cache, error, sizeof(error));
    if (rc == UPSTREAM_NO_RELEASE) { set_status(NULL, "current", "No release published in this channel yet"); return; }
    if (rc != UPSTREAM_OK) { set_status(NULL, "error", error[0] ? error : "Channel unavailable"); return; }
    const char *version = release.tag + (release.tag[0] == 'v' || release.tag[0] == 'V');
    if (slot_version_compare(version, MENU_VERSION) <= 0) { set_status(release.tag, "current", "Up to date"); return; }
    char pending[64], message[160];
    if (channel_pending_version(pending, sizeof(pending)) && slot_version_compare(pending, version) >= 0) {
        snprintf(message, sizeof(message), "%s starts next time Payload Manager is launched", pending);
        set_status(release.tag, "ready", message); return;
    }
    State s;
    pthread_mutex_lock(&channel_mutex);
    read_state(&s);
    pthread_mutex_unlock(&channel_mutex);
    if (is_failed(&s, version)) {
        snprintf(message, sizeof(message), "%s failed to start earlier and is skipped", version);
        set_status(release.tag, "failed", message); return;
    }
    if (!release.sha256[0]) { set_status(release.tag, "error", "Release asset has no published checksum"); return; }
    char part[640], hash[65], embedded[64], filename[96];
    if (ensure_dir_recursive(CHANNEL_UPDATE_DIR)) { set_status(release.tag, "error", "Storage unavailable"); return; }
    snprintf(part, sizeof(part), "%s/.download-XXXXXX", CHANNEL_UPDATE_DIR);
    int fd = mkstemp(part);
    if (fd < 0 || close(fd)) { if (fd >= 0) remove(part); set_status(release.tag, "error", "Storage unavailable"); return; }
    const char *failure = NULL;
    if (download_to_file(release.asset_url, part)) failure = "Download failed";
    else if (compute_sha256_file(part, hash) || strcasecmp(hash, release.sha256)) failure = "Checksum mismatch";
    else {
        int file = open(part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (file < 0) failure = "Downloaded file unreadable";
        else {
            if (repository_self_update_version_fd(file, embedded, sizeof(embedded)) ||
                slot_version_compare(embedded, version)) failure = "Embedded version does not match the release";
            else if (!channel_fd_matches(file)) failure = "Release belongs to a different update channel";
            close(file);
        }
    }
    snprintf(filename, sizeof(filename), "pldmgr_v%s.elf", embedded);
    char final_path[768];
    snprintf(final_path, sizeof(final_path), "%s/%s", CHANNEL_UPDATE_DIR, filename);
    if (!failure && rename(part, final_path)) failure = "Could not store update";
    if (failure) {
        remove(part);
        pldmgr_log("[Channel] Self-update %s rejected: %s\n", release.tag, failure);
        set_status(release.tag, "error", failure);
        return;
    }
    prune_channel(filename);
    snprintf(message, sizeof(message), "%s downloaded; starts next time Payload Manager is launched", version);
    pldmgr_log("[Channel] %s\n", message);
    set_status(release.tag, "ready", message);
}

void channel_status(ChannelStatus *out) {
    if (!out) return;
    pthread_mutex_lock(&channel_mutex);
    *out = status;
    pthread_mutex_unlock(&channel_mutex);
    snprintf(out->repo, sizeof(out->repo), "%s", channel_repo());
    out->official = channel_is_official();
    if (!channel_pending_version(out->pending, sizeof(out->pending))) out->pending[0] = 0;
    if (!out->state[0]) snprintf(out->state, sizeof(out->state), "idle");
}
