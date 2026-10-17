#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "updater.h"
#include "upstream.h"
#include "boot_guard.h"
#include "channel.h"
#include "config.h"
#include "json_helpers.h"
#include "pldmgr.h"
#include "repository.h"
#include "sha256.h"
#include "slot.h"
#include "sources.h"

#ifndef PLDMGR_NETWORK_WAIT_MS
#define PLDMGR_NETWORK_WAIT_MS 5000
#endif
#define DEFAULT_BUDGET 20
#define MAX_BUDGET 300
#define MAX_PREFS 512
#define MAX_ITEMS 512
#define BOOT_RECHECK_SECONDS 600L         /* autoload payloads, at most every 10 minutes */
#define BACKGROUND_RECHECK_SECONDS 43200L /* other installed payloads, twice a day */

/* ── Settings (updates.json) ──────────────────────────────── */

typedef struct {
    char slot[128];
    char policy[8];   /* auto | safe | notify | off */
    int prerelease, unverified;
    char repo[256], asset[128], skip[64];
} Prefs;

typedef struct {
    int enabled, budget, self_update;
    Prefs *prefs;
    size_t count;
} Settings;

static pthread_mutex_t settings_mutex = PTHREAD_MUTEX_INITIALIZER;

static int policy_valid(const char *p) {
    return !strcmp(p, "auto") || !strcmp(p, "safe") || !strcmp(p, "notify") || !strcmp(p, "off");
}
static int slot_key_valid(const char *key) {
    return key && (!strncmp(key, "repo/", 5) || !strncmp(key, "local/", 6)) && strlen(key) < 128 &&
           !strstr(key, "..") && !strpbrk(key, "\r\n,\\") && key[strlen(key) - 1] != '/';
}
static int text_valid(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < 32 || *s == 127) return 0;
    return 1;
}
static void default_prefs(Prefs *p, const char *slot) {
    memset(p, 0, sizeof(*p));
    snprintf(p->slot, sizeof(p->slot), "%s", slot ? slot : "");
    strcpy(p->policy, "safe");
}
static Prefs *find_prefs(Settings *s, const char *slot) {
    for (size_t i = 0; i < s->count; i++) if (!strcmp(s->prefs[i].slot, slot)) return &s->prefs[i];
    return NULL;
}
static Prefs *add_prefs(Settings *s, const char *slot) {
    Prefs *p = find_prefs(s, slot);
    if (p) return p;
    if (s->count >= MAX_PREFS) return NULL;
    Prefs *grown = realloc(s->prefs, (s->count + 1) * sizeof(Prefs));
    if (!grown) return NULL;
    s->prefs = grown;
    p = &s->prefs[s->count++];
    default_prefs(p, slot);
    return p;
}
static void free_settings(Settings *s) { free(s->prefs); s->prefs = NULL; s->count = 0; }

static int parse_bool(const char *start, const char *end, int *out) {
    if (end - start == 4 && !memcmp(start, "true", 4)) { *out = 1; return 0; }
    if (end - start == 5 && !memcmp(start, "false", 5)) { *out = 0; return 0; }
    return -1;
}
static int parse_string(const char *start, const char *end, char *out, size_t size) {
    return json_decode_string(start, end, out, size) || !text_valid(out) ? -1 : 0;
}

typedef struct { Settings *settings; Prefs *prefs; } ParseContext;

static int prefs_member(const char *key, const char *start, const char *end, void *context) {
    Prefs *p = context;
    char value[256];
    if (!strcmp(key, "policy")) {
        if (parse_string(start, end, value, sizeof(value)) || !policy_valid(value)) return -1;
        strcpy(p->policy, value);
    } else if (!strcmp(key, "prerelease")) return parse_bool(start, end, &p->prerelease);
    else if (!strcmp(key, "unverified")) return parse_bool(start, end, &p->unverified);
    else if (!strcmp(key, "repo")) {
        if (parse_string(start, end, value, sizeof(value))) return -1;
        UpstreamSpec spec = {0};
        if (value[0] && upstream_parse_repo(value, &spec)) return -1;
        strcpy(p->repo, value);
    } else if (!strcmp(key, "asset")) {
        if (parse_string(start, end, value, sizeof(p->asset)) || strchr(value, '/')) return -1;
        strcpy(p->asset, value);
    } else if (!strcmp(key, "skip")) {
        if (parse_string(start, end, value, sizeof(p->skip)) || strchr(value, ',')) return -1;
        strcpy(p->skip, value);
    }
    return 0;
}

static int payloads_member(const char *key, const char *start, const char *end, void *context) {
    ParseContext *ctx = context;
    if (!slot_key_valid(key)) return -1;
    if (end - start == 4 && !memcmp(start, "null", 4)) {
        Prefs *p = find_prefs(ctx->settings, key);
        if (p) { *p = ctx->settings->prefs[--ctx->settings->count]; }
        return 0;
    }
    if (*start != '{') return -1;
    Prefs *p = add_prefs(ctx->settings, key);
    return p ? json_visit_object(start, end, prefs_member, p) : -1;
}

static int settings_member(const char *key, const char *start, const char *end, void *context) {
    ParseContext *ctx = context;
    Settings *s = ctx->settings;
    if (!strcmp(key, "enabled")) return parse_bool(start, end, &s->enabled);
    if (!strcmp(key, "self_update")) return parse_bool(start, end, &s->self_update);
    if (!strcmp(key, "budget")) {
        long value = 0;
        if (start == end || end - start > 4) return -1;
        for (const char *p = start; p < end; p++) { if (!isdigit((unsigned char)*p)) return -1; value = value * 10 + *p - '0'; }
        if (value > MAX_BUDGET) return -1;
        s->budget = (int)value;
        return 0;
    }
    if (!strcmp(key, "payloads")) return *start == '{' ? json_visit_object(start, end, payloads_member, ctx) : -1;
    return 0;
}

static int apply_settings_json(Settings *s, const char *json, size_t size) {
    ParseContext ctx = { s, NULL };
    return json_visit_object(json, json + size, settings_member, &ctx);
}

static void load_settings_unlocked(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->enabled = 1; s->budget = DEFAULT_BUDGET; s->self_update = 1;
    char *json = NULL; size_t size = 0;
    if (read_file_text(UPDATES_CONFIG_PATH, &json, &size)) return;
    Settings parsed = *s;
    if (apply_settings_json(&parsed, json, size)) {
        pldmgr_log("[Updater] Ignoring unreadable %s; using defaults.\n", UPDATES_CONFIG_PATH);
        free_settings(&parsed);
    } else *s = parsed;
    free(json);
}

static int save_settings_unlocked(const Settings *s) {
    size_t capacity = 512 + s->count * (6 * (128 + 256 + 128 + 64) + 256);
    char *buf = malloc(capacity);
    if (!buf) return -1;
    JsonListBuilder jb = { buf, capacity, 0, 1, 0 };
    json_append(&jb, "{\n  \"enabled\": %s,\n  \"budget\": %d,\n  \"self_update\": %s,\n  \"payloads\": {",
                s->enabled ? "true" : "false", s->budget, s->self_update ? "true" : "false");
    for (size_t i = 0; i < s->count; i++) {
        const Prefs *p = &s->prefs[i];
        char slot[6 * 128], repo[6 * 256], asset[6 * 128], skip[6 * 64];
        json_escape_checked(&jb, p->slot, slot, sizeof(slot));
        json_escape_checked(&jb, p->repo, repo, sizeof(repo));
        json_escape_checked(&jb, p->asset, asset, sizeof(asset));
        json_escape_checked(&jb, p->skip, skip, sizeof(skip));
        json_append(&jb, "%s\n    \"%s\": {\"policy\": \"%s\", \"prerelease\": %s, \"unverified\": %s, "
                         "\"repo\": \"%s\", \"asset\": \"%s\", \"skip\": \"%s\"}",
                    i ? "," : "", slot, p->policy, p->prerelease ? "true" : "false",
                    p->unverified ? "true" : "false", repo, asset, skip);
    }
    json_append(&jb, "\n  }\n}\n");
    int rc = jb.error || ensure_dir_recursive(BASE_DATA_DIR) ? -1 : write_file_text(UPDATES_CONFIG_PATH, buf, jb.pos);
    free(buf);
    return rc;
}

static void load_settings(Settings *s) {
    pthread_mutex_lock(&settings_mutex);
    load_settings_unlocked(s);
    pthread_mutex_unlock(&settings_mutex);
}

static Prefs prefs_for(const Settings *s, const char *slot) {
    Prefs p;
    for (size_t i = 0; i < s->count; i++) if (!strcmp(s->prefs[i].slot, slot)) return s->prefs[i];
    default_prefs(&p, slot);
    return p;
}

int updater_settings_json(const char *json) {
    if (!json || !json[0]) return CONFIG_INVALID;
    pthread_mutex_lock(&settings_mutex);
    Settings s;
    load_settings_unlocked(&s);
    int rc = apply_settings_json(&s, json, strlen(json)) ? CONFIG_INVALID : save_settings_unlocked(&s);
    free_settings(&s);
    pthread_mutex_unlock(&settings_mutex);
    return rc;
}

int updater_skip_version(const char *slot_key, const char *version) {
    if (!slot_key_valid(slot_key) || !version || strlen(version) >= 64 || strchr(version, ',')) return -1;
    pthread_mutex_lock(&settings_mutex);
    Settings s;
    load_settings_unlocked(&s);
    Prefs *p = add_prefs(&s, slot_key);
    int rc = -1;
    if (p) { snprintf(p->skip, sizeof(p->skip), "%s", version); rc = save_settings_unlocked(&s); }
    free_settings(&s);
    pthread_mutex_unlock(&settings_mutex);
    return rc;
}

/* ── Pass state ───────────────────────────────────────────── */

typedef struct {
    char version[64], url[1024], sha256[65], filename[256], published[16], source[12];
} Candidate;

typedef struct {
    char slot[128], name[128], installed[64], latest[64], source[12];
    char upstream[256], pattern[256], state[16], message[160];
    char entry[512]; /* autoload entry resolved to this slot when the pass began */
    int autoload, done, has_candidate;
    Candidate candidate;
} Item;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t install_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker_thread;
static int worker_owned, running, cancel_requested, boot_gate, waiters;
static long long gate_deadline;
static long last_run;
static char network_state[16] = "unknown";
static Item *items;
static size_t item_count;

static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int is_cancelled(void) {
    pthread_mutex_lock(&state_mutex);
    int c = cancel_requested;
    pthread_mutex_unlock(&state_mutex);
    return c;
}
static Item *find_item_locked(const char *slot) {
    for (size_t i = 0; i < item_count; i++) if (!strcmp(items[i].slot, slot)) return &items[i];
    return NULL;
}
static void set_item(const char *slot, const char *state, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_item(const char *slot, const char *state, const char *fmt, ...) {
    pthread_mutex_lock(&state_mutex);
    Item *item = find_item_locked(slot);
    if (item) {
        if (state) snprintf(item->state, sizeof(item->state), "%s", state);
        if (fmt) {
            va_list args; va_start(args, fmt);
            vsnprintf(item->message, sizeof(item->message), fmt, args);
            va_end(args);
        }
    }
    pthread_mutex_unlock(&state_mutex);
}
static void finish_item(const char *slot) {
    pthread_mutex_lock(&state_mutex);
    Item *item = find_item_locked(slot);
    if (item) item->done = 1;
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_mutex);
}

/* ── Autoload entries → slots ─────────────────────────────── */

static int entry_slot_key(const char *entry, const SlotMeta *slots, size_t count, char *out, size_t size) {
    out[0] = 0;
    if (!entry || !entry[0] || entry[0] == '!' || entry[0] == '#') return -1;
    if (!strncmp(entry, "repo/", 5) || !strncmp(entry, "local/", 6)) {
        if (!slot_key_valid(entry)) return -1;
        snprintf(out, size, "%s", entry);
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if ((entry[0] == '/' && !strcmp(slots[i].payload_path, entry)) ||
            (entry[0] != '/' && !strcmp(slots[i].filename, entry))) {
            snprintf(out, size, "%s", slots[i].slot_key);
            return 0;
        }
    }
    return -1;
}

typedef struct { char (*keys)[512]; size_t count; } EntryList;

static void read_autoload_entries(EntryList *list) {
    list->keys = NULL; list->count = 0;
    char *text = NULL; size_t size = 0;
    if (read_file_text(AUTOLOAD_CONFIG_PATH, &text, &size) || size > 4095) { free(text); return; }
    size_t lines = 1;
    for (size_t i = 0; i < size; i++) lines += text[i] == '\n';
    list->keys = calloc(lines, sizeof(*list->keys));
    if (!list->keys) { free(text); return; }
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (isspace((unsigned char)*line)) line++;
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) *--end = 0;
        if (*line && *line != '!' && strlen(line) < 512) strcpy(list->keys[list->count++], line);
    }
    free(text);
}

/* ── Verification and installation ────────────────────────── */

/* PS5 payloads are x86-64 little-endian ELF64 files; .bin payloads may be raw
 * images, but are never accepted as an HTML or JSON error document. */
static int payload_file_sane(const char *path, const char *filename) {
    unsigned char header[64] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (!n) return 0;
    const char *ext = strrchr(filename, '.');
    int elf_name = ext && !strcasecmp(ext, ".elf");
    if (n >= 20 && !memcmp(header, "\177ELF", 4))
        return header[4] == 2 && header[5] == 1 && (header[18] | header[19] << 8) == 62;
    if (elf_name) return 0;
    return header[0] != '<' && header[0] != '{' && header[0] != '[';
}

static void safe_component(const char *in, char *out, size_t size) {
    size_t pos = 0;
    for (; *in && pos + 1 < size; in++) {
        unsigned char c = (unsigned char)*in;
        out[pos++] = (isalnum(c) || c == '-' || c == '.') ? (char)c : '_';
    }
    out[pos] = 0;
    while (pos && (out[pos - 1] == '.' || out[pos - 1] == '_')) out[--pos] = 0;
}

static int filename_valid(const char *name) {
    const char *ext = strrchr(name, '.');
    return name[0] && name[0] != '.' && !strchr(name, '/') && !strstr(name, "..") && strlen(name) < 200 &&
           ext && (!strcasecmp(ext, ".elf") || !strcasecmp(ext, ".bin")) && !is_pldmgr_name(name);
}

/* Mirror-style "<name>_<version><ext>" keeps installed files recognisable. */
static void upstream_filename(const char *base, const UpstreamRelease *release, char *out, size_t size) {
    char name[96], version[64];
    const char *ext = strrchr(release->asset_name, '.');
    safe_component(base, name, sizeof(name));
    safe_component(release->tag, version, sizeof(version));
    snprintf(out, size, "%s_%s%s", name[0] ? name : "payload", version, ext ? ext : ".elf");
    if (!filename_valid(out)) snprintf(out, size, "%s", release->asset_name);
}

typedef struct { int ok; char message[160]; char new_filename[256]; } InstallResult;

static InstallResult install_candidate(const SlotMeta *current, const Candidate *c, const RepoPayload *catalog) {
    InstallResult result = {0};
    if (!filename_valid(c->filename)) { snprintf(result.message, sizeof(result.message), "Invalid payload file name"); return result; }
    char part[640];
    if (ensure_dir_recursive(PAYLOADS_STORAGE_DIR)) { snprintf(result.message, sizeof(result.message), "Storage unavailable"); return result; }
    snprintf(part, sizeof(part), "%s/.download-XXXXXX", PAYLOADS_STORAGE_DIR);
    int fd = mkstemp(part);
    if (fd < 0 || close(fd)) {
        if (fd >= 0) remove(part);
        snprintf(result.message, sizeof(result.message), "Storage unavailable");
        return result;
    }
    char hash[65] = "";
    SlotMeta *meta = NULL;
    if (download_to_file(c->url, part)) { snprintf(result.message, sizeof(result.message), "Download failed"); goto done; }
    if (compute_sha256_file(part, hash) || (c->sha256[0] && strcasecmp(hash, c->sha256))) {
        snprintf(result.message, sizeof(result.message), "Checksum mismatch; download discarded"); goto done;
    }
    if (!payload_file_sane(part, c->filename)) {
        snprintf(result.message, sizeof(result.message), "Downloaded file is not a PS5 payload"); goto done;
    }
    meta = malloc(sizeof(*meta));
    if (!meta) { snprintf(result.message, sizeof(result.message), "Out of memory"); goto done; }
    *meta = *current;
    snprintf(meta->filename, sizeof(meta->filename), "%s", c->filename);
    snprintf(meta->version, sizeof(meta->version), "%s", c->version);
    snprintf(meta->checksum, sizeof(meta->checksum), "%s", hash);
    snprintf(meta->url, sizeof(meta->url), "%s", c->url);
    if (c->published[0]) snprintf(meta->last_update, sizeof(meta->last_update), "%s", c->published);
    if (catalog && !strcmp(c->source, "catalog")) {
        snprintf(meta->name, sizeof(meta->name), "%s", catalog->name);
        snprintf(meta->last_update, sizeof(meta->last_update), "%s", catalog->last_update);
        snprintf(meta->source, sizeof(meta->source), "%s", catalog->source);
        snprintf(meta->source_direct, sizeof(meta->source_direct), "%s", catalog->source_direct);
        snprintf(meta->category, sizeof(meta->category), "%s", catalog->category);
        snprintf(meta->description, sizeof(meta->description), "%s", catalog->description);
        snprintf(meta->author, sizeof(meta->author), "%s", catalog->author);
        snprintf(meta->details_url, sizeof(meta->details_url), "%s", catalog->details_url);
    } else if (!strcmp(c->source, "upstream")) {
        snprintf(meta->source_direct, sizeof(meta->source_direct), "%s", c->url);
    }
    if (!strcmp(meta->kind, SLOT_KIND_LOCAL)) {
        snprintf(meta->origin, sizeof(meta->origin), "upstream");
        snprintf(meta->origin_detail, sizeof(meta->origin_detail), "%s", c->url);
    }
    time_t now = time(NULL); struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(meta->installed_at, sizeof(meta->installed_at), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    char key[128];
    pthread_mutex_lock(&install_mutex);
    int committed = slot_commit_update(current->slot_key, part, meta, key, sizeof(key));
    pthread_mutex_unlock(&install_mutex);
    if (committed) { snprintf(result.message, sizeof(result.message), "Could not store the update"); goto done; }
    result.ok = 1;
    snprintf(result.new_filename, sizeof(result.new_filename), "%s", c->filename);
    /* Bare-filename list entries keep resolving after the file name changes. */
    if (strcmp(current->filename, c->filename)) {
        config_update_list_entry(AUTOLOAD_CONFIG_PATH, current->filename, current->slot_key);
        config_update_list_entry(FAVORITES_CONFIG_PATH, current->filename, current->slot_key);
    }
    snprintf(result.message, sizeof(result.message), "Updated %s → %s (%s)",
             current->version[0] ? current->version : "unknown", c->version,
             !strcmp(c->source, "upstream") ? "upstream release" : "catalog");
done:
    free(meta);
    remove(part);
    return result;
}

/* ── Per-payload decision ─────────────────────────────────── */

typedef struct {
    int found;
    Candidate best, alternate;
    int has_alternate;
    char note[160];
    char upstream[256], pattern[256];
} Decision;

static int newer_than_installed(const Candidate *c, const SlotMeta *meta) {
    if (meta->version[0]) return slot_version_compare(c->version, meta->version) > 0;
    /* Unversioned (uploaded) payloads: replace only when the bytes differ. */
    char hash[65];
    if (!c->sha256[0] || compute_sha256_file(meta->payload_path, hash)) return 1;
    return strcasecmp(hash, c->sha256) != 0;
}

static void decide(const SlotMeta *meta, const Prefs *p, const RepoPayload *catalog, long max_age, Decision *d) {
    memset(d, 0, sizeof(*d));
    UpstreamSpec spec = {0};
    const char *sources[] = { p->repo,
        catalog ? catalog->source_direct : "", catalog ? catalog->source : "",
        meta->source_direct, meta->source };
    int have_repo = 0;
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]) && !have_repo; i++)
        have_repo = sources[i][0] && !upstream_parse_repo(sources[i], &spec);
    if (p->asset[0]) snprintf(spec.pattern, sizeof(spec.pattern), "%s", p->asset);
    else if (catalog && catalog->extract_file[0]) spec.pattern[0] = 0; /* archive upstream: catalog only */
    else if (catalog && !upstream_derive_pattern(catalog->source_direct, catalog->version, spec.pattern, sizeof(spec.pattern)))
        snprintf(spec.require, sizeof(spec.require), "%s", catalog->asset_pattern);
    else if (upstream_derive_pattern(meta->source_direct, meta->version, spec.pattern, sizeof(spec.pattern)) && p->repo[0])
        snprintf(spec.pattern, sizeof(spec.pattern), "*");
    if (have_repo) snprintf(d->upstream, sizeof(d->upstream), "%s/%s", spec.owner, spec.repo);
    snprintf(d->pattern, sizeof(d->pattern), "%s", spec.pattern);

    Candidate up = {0}, cat = {0};
    int have_up = 0, have_cat = 0;
    if (have_repo && spec.pattern[0]) {
        UpstreamRelease release; int from_cache = 0; char error[96] = "";
        /* Someone already running a prerelease follows newer ones. */
        int prerelease = p->prerelease || upstream_version_is_prerelease(meta->version);
        int rc = upstream_check(&spec, prerelease, max_age, UPSTREAM_CACHE_DIR, &release, &from_cache, error, sizeof(error));
        if (rc == UPSTREAM_OK) {
            if (!release.sha256[0] && !p->unverified)
                snprintf(d->note, sizeof(d->note), "Upstream %s publishes no checksum", release.tag);
            else if (strcmp(release.tag, p->skip)) {
                have_up = 1;
                snprintf(up.version, sizeof(up.version), "%s", release.tag);
                snprintf(up.url, sizeof(up.url), "%s", release.asset_url);
                snprintf(up.sha256, sizeof(up.sha256), "%s", release.sha256);
                snprintf(up.published, sizeof(up.published), "%s", release.published);
                snprintf(up.source, sizeof(up.source), "upstream");
                upstream_filename(catalog ? catalog->name : meta->name[0] ? meta->name : meta->slot_key + (meta->slot_key[0] == 'r' ? 5 : 6),
                                  &release, up.filename, sizeof(up.filename));
            }
        } else if (rc == UPSTREAM_NO_RELEASE) snprintf(d->note, sizeof(d->note), "No matching upstream release asset");
        else snprintf(d->note, sizeof(d->note), "%s", error[0] ? error : "Upstream unavailable");
    } else if (catalog && catalog->extract_file[0]) {
        snprintf(d->note, sizeof(d->note), "Upstream ships an archive; using catalog");
    }
    if (catalog && catalog->version[0] && repository_entry_valid(catalog) &&
        (catalog->checksum[0] || p->unverified) && strcmp(catalog->version, p->skip)) {
        have_cat = 1;
        snprintf(cat.version, sizeof(cat.version), "%s", catalog->version);
        snprintf(cat.url, sizeof(cat.url), "%s", catalog->url);
        snprintf(cat.sha256, sizeof(cat.sha256), "%s", catalog->checksum);
        snprintf(cat.filename, sizeof(cat.filename), "%s", catalog->filename);
        snprintf(cat.published, sizeof(cat.published), "%s", catalog->last_update);
        snprintf(cat.source, sizeof(cat.source), "catalog");
    }
    if (have_up && have_cat) {
        int cmp = slot_version_compare(up.version, cat.version);
        int prefer_up = cmp > 0 || (cmp == 0 && up.sha256[0]);
        d->best = prefer_up ? up : cat;
        d->alternate = prefer_up ? cat : up;
        d->has_alternate = 1;
    } else if (have_up) d->best = up;
    else if (have_cat) d->best = cat;
    d->found = have_up || have_cat;
}

static void process_slot(const char *slot_key, long max_age) {
    char dir[640];
    snprintf(dir, sizeof(dir), "%s/%s", PAYLOADS_STORAGE_DIR, slot_key);
    SlotMeta *meta = malloc(sizeof(*meta));
    RepoPayload *catalog = malloc(sizeof(*catalog));
    Decision *d = malloc(sizeof(*d));
    Settings settings;
    load_settings(&settings);
    Prefs prefs = prefs_for(&settings, slot_key);
    free_settings(&settings);
    if (!meta || !catalog || !d) { set_item(slot_key, "failed", "Out of memory"); goto done; }
    if (slot_read_meta(dir, meta) || !meta->payload_path[0]) { set_item(slot_key, "failed", "Payload not found"); goto done; }
    if (!strcmp(prefs.policy, "off")) { set_item(slot_key, "disabled", "Automatic updates are off"); goto done; }
    set_item(slot_key, "checking", "%s", "");
    int have_catalog = !strcmp(meta->kind, SLOT_KIND_REPO) && !sources_cached_entry(meta->source_url, meta->id, catalog);
    decide(meta, &prefs, have_catalog ? catalog : NULL, max_age, d);
    pthread_mutex_lock(&state_mutex);
    Item *item = find_item_locked(slot_key);
    if (item) {
        snprintf(item->installed, sizeof(item->installed), "%s", meta->version);
        snprintf(item->upstream, sizeof(item->upstream), "%s", d->upstream);
        snprintf(item->pattern, sizeof(item->pattern), "%s", d->pattern);
        item->has_candidate = d->found;
        if (d->found) {
            item->candidate = d->best;
            snprintf(item->latest, sizeof(item->latest), "%s", d->best.version);
            snprintf(item->source, sizeof(item->source), "%s", d->best.source);
        }
    }
    pthread_mutex_unlock(&state_mutex);
    if (!d->found && !d->upstream[0] && !have_catalog) {
        set_item(slot_key, "untracked", "No update source; set an upstream repository to track it");
        goto done;
    }
    if (!d->found || !newer_than_installed(&d->best, meta)) {
        set_item(slot_key, "current", "%s", d->note);
        pthread_mutex_lock(&state_mutex);
        if ((item = find_item_locked(slot_key))) item->has_candidate = 0;
        pthread_mutex_unlock(&state_mutex);
        goto done;
    }
    long from = upstream_major_version(meta->version), to = upstream_major_version(d->best.version);
    if (!strcmp(prefs.policy, "notify")) {
        set_item(slot_key, "available", "%s available", d->best.version); goto done;
    }
    if (!strcmp(prefs.policy, "safe") && from >= 0 && to >= 0 && from != to) {
        set_item(slot_key, "approval", "Major version change to %s; approve to install", d->best.version); goto done;
    }
    set_item(slot_key, "downloading", "Installing %s", d->best.version);
    InstallResult result = install_candidate(meta, &d->best, have_catalog ? catalog : NULL);
    if (!result.ok && d->has_alternate && newer_than_installed(&d->alternate, meta)) {
        pldmgr_log("[Updater] %s: %s; trying %s %s\n", slot_key, result.message, d->alternate.source, d->alternate.version);
        result = install_candidate(meta, &d->alternate, have_catalog ? catalog : NULL);
        if (result.ok) d->best = d->alternate;
    }
    if (result.ok) {
        boot_guard_updated(slot_key, d->best.version);
        pldmgr_log("[Updater] %s: %s\n", slot_key, result.message);
        set_item(slot_key, "updated", "%s", result.message);
        pthread_mutex_lock(&state_mutex);
        if ((item = find_item_locked(slot_key))) {
            snprintf(item->installed, sizeof(item->installed), "%s", d->best.version);
            item->has_candidate = 0;
        }
        pthread_mutex_unlock(&state_mutex);
    } else {
        pldmgr_log("[Updater] %s: %s; keeping %s\n", slot_key, result.message, meta->version[0] ? meta->version : "installed version");
        set_item(slot_key, "failed", "%s; keeping installed version", result.message);
    }
done:
    finish_item(slot_key);
    free(meta); free(catalog); free(d);
}

/* ── Worker ───────────────────────────────────────────────── */

static int wait_for_network(void) {
    long long end = now_ms() + PLDMGR_NETWORK_WAIT_MS;
    char ip[64];
    while (pldmgr_get_local_ip(ip, sizeof(ip))) {
        if (now_ms() >= end || is_cancelled()) return -1;
        usleep(250000);
    }
    return 0;
}

static void build_items(int boot) {
    SlotMeta *slots = NULL; size_t count = 0;
    EntryList entries;
    if (slot_list_all(&slots, &count)) { slots = NULL; count = 0; }
    read_autoload_entries(&entries);
    Item *list = calloc(count ? count : 1, sizeof(Item));
    size_t n = 0;
    if (list) {
        /* Autoload order first: the sequence launches them in this order. */
        for (size_t e = 0; e < entries.count && n < MAX_ITEMS; e++) {
            char key[128];
            if (entry_slot_key(entries.keys[e], slots, count, key, sizeof(key))) continue;
            int seen = 0;
            for (size_t i = 0; i < n; i++) if (!strcmp(list[i].slot, key)) seen = 1;
            int exists = 0;
            for (size_t i = 0; i < count; i++) if (!strcmp(slots[i].slot_key, key)) exists = 1;
            if (seen || !exists) continue;
            snprintf(list[n].slot, sizeof(list[n].slot), "%s", key);
            snprintf(list[n].entry, sizeof(list[n].entry), "%s", entries.keys[e]);
            list[n].autoload = 1;
            strcpy(list[n].state, "pending");
            n++;
        }
        for (size_t i = 0; i < count && n < MAX_ITEMS; i++) {
            int seen = 0;
            for (size_t k = 0; k < n; k++) if (!strcmp(list[k].slot, slots[i].slot_key)) seen = 1;
            if (seen) continue;
            snprintf(list[n].slot, sizeof(list[n].slot), "%s", slots[i].slot_key);
            strcpy(list[n].state, "pending");
            n++;
        }
        for (size_t k = 0; k < n; k++)
            for (size_t i = 0; i < count; i++)
                if (!strcmp(list[k].slot, slots[i].slot_key))
                    snprintf(list[k].name, sizeof(list[k].name), "%s", slots[i].name[0] ? slots[i].name : slots[i].filename);
    }
    (void)boot;
    pthread_mutex_lock(&state_mutex);
    free(items);
    items = list; item_count = list ? n : 0;
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_mutex);
    free(entries.keys);
    slot_free_all(slots);
}

static void refresh_catalogs(void) {
    SlotMeta *slots = NULL; size_t count = 0;
    repository_ensure_fresh(0);
    if (slot_list_all(&slots, &count)) return;
    for (size_t i = 0; i < count && !is_cancelled(); i++) {
        if (strcmp(slots[i].kind, SLOT_KIND_REPO) || !slots[i].source_url[0] || is_pldmgr_official_source(slots[i].source_url)) continue;
        int seen = 0;
        for (size_t k = 0; k < i; k++) if (!strcmp(slots[k].source_url, slots[i].source_url)) seen = 1;
        if (!seen) sources_ensure_fresh(slots[i].source_url, 0);
    }
    slot_free_all(slots);
}

static void mark_remaining(const char *state, const char *message) {
    pthread_mutex_lock(&state_mutex);
    for (size_t i = 0; i < item_count; i++) {
        if (items[i].done) continue;
        snprintf(items[i].state, sizeof(items[i].state), "%s", state);
        snprintf(items[i].message, sizeof(items[i].message), "%s", message);
        items[i].done = 1;
    }
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_mutex);
}

static void set_network(const char *state) {
    pthread_mutex_lock(&state_mutex);
    snprintf(network_state, sizeof(network_state), "%s", state);
    pthread_mutex_unlock(&state_mutex);
}

typedef struct { int boot, manual; } PassOptions;

static void *worker(void *argument) {
    PassOptions options = *(PassOptions *)argument;
    free(argument);
    Settings settings;
    load_settings(&settings);
    int enabled = settings.enabled || options.manual;
    int self_update = settings.self_update;
    free_settings(&settings);
    if (!enabled) {
        mark_remaining("disabled", "Automatic updates are disabled");
        pthread_mutex_lock(&state_mutex); boot_gate = 0; pthread_cond_broadcast(&state_cond); pthread_mutex_unlock(&state_mutex);
    }
    if (wait_for_network()) {
        set_network("offline");
        pldmgr_log("[Updater] No network; launching installed payloads.\n");
        mark_remaining("offline", "No network; installed version used");
        goto finished;
    }
    set_network("online");
    refresh_catalogs();
    if (!enabled) goto finished;
    for (size_t i = 0;; i++) {
        char slot[128]; int autoload;
        pthread_mutex_lock(&state_mutex);
        int more = i < item_count && !cancel_requested;
        if (more) { snprintf(slot, sizeof(slot), "%s", items[i].slot); autoload = items[i].autoload; }
        pthread_mutex_unlock(&state_mutex);
        if (!more) break;
        process_slot(slot, options.manual ? 0 : autoload ? BOOT_RECHECK_SECONDS : BACKGROUND_RECHECK_SECONDS);
    }
    mark_remaining("skipped", "Update check cancelled");
    if (self_update && !is_cancelled()) channel_check_update(options.manual ? 0 : BOOT_RECHECK_SECONDS);
finished:
    pthread_mutex_lock(&state_mutex);
    running = 0; boot_gate = 0; last_run = (long)time(NULL);
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_mutex);
    pldmgr_log("[Updater] Update pass finished.\n");
    return NULL;
}

static int start_pass(int boot, int manual) {
    pthread_mutex_lock(&lifecycle_mutex);
    pthread_mutex_lock(&state_mutex);
    int busy = running;
    pthread_mutex_unlock(&state_mutex);
    if (busy) { pthread_mutex_unlock(&lifecycle_mutex); return 1; }
    if (worker_owned) { pthread_join(worker_thread, NULL); worker_owned = 0; }
    PassOptions *options = malloc(sizeof(*options));
    if (!options) { pthread_mutex_unlock(&lifecycle_mutex); return -1; }
    options->boot = boot; options->manual = manual;
    Settings settings;
    load_settings(&settings);
    /* Items exist before autoload can ask for one: an unknown entry never waits. */
    build_items(boot);
    pthread_mutex_lock(&state_mutex);
    running = 1; cancel_requested = 0;
    boot_gate = boot && settings.enabled && settings.budget > 0;
    gate_deadline = now_ms() + (long long)settings.budget * 1000;
    pthread_mutex_unlock(&state_mutex);
    free_settings(&settings);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int rc = pthread_create(&worker_thread, &attr, worker, options);
    pthread_attr_destroy(&attr);
    if (rc) {
        free(options);
        pthread_mutex_lock(&state_mutex); running = 0; boot_gate = 0; pthread_cond_broadcast(&state_cond); pthread_mutex_unlock(&state_mutex);
        pldmgr_log("[Updater] Could not start update worker: %s\n", strerror(rc));
    } else worker_owned = 1;
    pthread_mutex_unlock(&lifecycle_mutex);
    return rc ? -1 : 0;
}

int updater_start(int boot) { return start_pass(boot, 0); }

void updater_cancel(void) {
    pthread_mutex_lock(&state_mutex);
    cancel_requested = 1; boot_gate = 0;
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_mutex);
}

void updater_stop(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    pthread_mutex_lock(&state_mutex);
    cancel_requested = 1; boot_gate = 0;
    pthread_cond_broadcast(&state_cond);
    pthread_mutex_unlock(&state_mutex);
    if (worker_owned) { pthread_join(worker_thread, NULL); worker_owned = 0; }
    pthread_mutex_unlock(&lifecycle_mutex);
}

int updater_wait_entry(const char *entry, char *slot_key, size_t size, UpdaterCancelled cancelled, void *context) {
    if (!slot_key || size < 128) return 0;
    slot_key[0] = 0;
    if (!entry || entry[0] == '!') return 0;
    /* An update may already have renamed the file a bare entry refers to. */
    pthread_mutex_lock(&state_mutex);
    for (size_t i = 0; i < item_count && !slot_key[0]; i++)
        if (items[i].autoload && !strcmp(items[i].entry, entry)) snprintf(slot_key, size, "%s", items[i].slot);
    pthread_mutex_unlock(&state_mutex);
    SlotMeta *slots = NULL; size_t count = 0;
    if (!slot_key[0] && !slot_list_all(&slots, &count)) {
        entry_slot_key(entry, slots, count, slot_key, size);
        slot_free_all(slots);
    }
    if (!slot_key[0]) return 0;
    int result = 0, announced = 0;
    pthread_mutex_lock(&state_mutex);
    waiters++;
    for (;;) {
        Item *item = find_item_locked(slot_key);
        if (!boot_gate || !item || item->done) break;
        if (now_ms() >= gate_deadline) { result = 1; break; }
        if (!announced) { pldmgr_log("[Autoload] Waiting for update check: %s\n", slot_key); announced = 1; }
        pthread_mutex_unlock(&state_mutex);
        int stop = cancelled && cancelled(context);
        pthread_mutex_lock(&state_mutex);
        if (stop) { result = -1; break; }
        struct timespec wake;
        clock_gettime(CLOCK_REALTIME, &wake);
        wake.tv_nsec += 100000000;
        if (wake.tv_nsec >= 1000000000) { wake.tv_sec++; wake.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&state_cond, &state_mutex, &wake);
    }
    waiters--;
    pthread_mutex_unlock(&state_mutex);
    if (result == 1) pldmgr_log("[Autoload] Update budget expired; launching installed %s\n", slot_key);
    return result;
}

int updater_boot_waiting(void) {
    pthread_mutex_lock(&state_mutex);
    int w = waiters > 0;
    pthread_mutex_unlock(&state_mutex);
    return w;
}

/* ── API ──────────────────────────────────────────────────── */

size_t updater_status_json(char *buf, size_t size) {
    if (!buf || !size) return 0;
    buf[0] = 0;
    SlotMeta *slots = NULL; size_t count = 0;
    if (slot_list_all(&slots, &count)) return 0;
    Settings settings;
    load_settings(&settings);
    EntryList entries;
    read_autoload_entries(&entries);
    JsonListBuilder jb = { buf, size, 0, 1, 0 };
    ChannelStatus channel;
    channel_status(&channel);
    char repo_e[6 * 128], pending_e[6 * 64], latest_e[6 * 64], cstate_e[6 * 16], cmsg_e[6 * 160];
    json_escape_checked(&jb, channel.repo, repo_e, sizeof(repo_e));
    json_escape_checked(&jb, channel.pending, pending_e, sizeof(pending_e));
    json_escape_checked(&jb, channel.latest, latest_e, sizeof(latest_e));
    json_escape_checked(&jb, channel.state, cstate_e, sizeof(cstate_e));
    json_escape_checked(&jb, channel.message, cmsg_e, sizeof(cmsg_e));
    pthread_mutex_lock(&state_mutex);
    json_append(&jb, "{\"enabled\":%s,\"budget\":%d,\"self_update\":%s,\"running\":%s,\"last_run\":%ld,\"network\":\"%s\","
                     "\"channel\":{\"repo\":\"%s\",\"official\":%s,\"running\":\"%s\",\"pending\":\"%s\",\"latest\":\"%s\",\"state\":\"%s\",\"message\":\"%s\"},"
                     "\"items\":[",
                settings.enabled ? "true" : "false", settings.budget, settings.self_update ? "true" : "false",
                running ? "true" : "false", last_run, network_state,
                repo_e, channel.official ? "true" : "false", MENU_VERSION, pending_e, latest_e, cstate_e, cmsg_e);
    for (size_t i = 0; i < count; i++) {
        const SlotMeta *s = &slots[i];
        Item *item = find_item_locked(s->slot_key);
        Prefs p = prefs_for(&settings, s->slot_key);
        int autoload = 0;
        char key[128];
        for (size_t e = 0; e < entries.count && !autoload; e++)
            autoload = !entry_slot_key(entries.keys[e], slots, count, key, sizeof(key)) && !strcmp(key, s->slot_key);
        SlotMeta previous;
        int has_previous = !slot_previous_version(s->slot_key, &previous);
        char slot_e[6 * 128], name_e[6 * 256], inst_e[6 * 64], latest[6 * 64], src_e[6 * 16], up_e[6 * 256];
        char pat_e[6 * 256], state_e[6 * 16], msg_e[6 * 160], repo2_e[6 * 256], asset_e[6 * 128], skip_e[6 * 64], prev_e[6 * 64];
        json_escape_checked(&jb, s->slot_key, slot_e, sizeof(slot_e));
        json_escape_checked(&jb, s->name[0] ? s->name : s->filename, name_e, sizeof(name_e));
        json_escape_checked(&jb, s->version, inst_e, sizeof(inst_e));
        json_escape_checked(&jb, item && item->has_candidate ? item->latest : "", latest, sizeof(latest));
        json_escape_checked(&jb, item && item->has_candidate ? item->source : "", src_e, sizeof(src_e));
        json_escape_checked(&jb, item ? item->upstream : "", up_e, sizeof(up_e));
        json_escape_checked(&jb, item ? item->pattern : "", pat_e, sizeof(pat_e));
        json_escape_checked(&jb, item ? item->state : "idle", state_e, sizeof(state_e));
        json_escape_checked(&jb, item ? item->message : "", msg_e, sizeof(msg_e));
        json_escape_checked(&jb, p.repo, repo2_e, sizeof(repo2_e));
        json_escape_checked(&jb, p.asset, asset_e, sizeof(asset_e));
        json_escape_checked(&jb, p.skip, skip_e, sizeof(skip_e));
        json_escape_checked(&jb, has_previous ? previous.version : "", prev_e, sizeof(prev_e));
        json_append(&jb, "%s{\"slot\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"installed\":\"%s\",\"latest\":\"%s\",\"source\":\"%s\","
                         "\"upstream\":\"%s\",\"pattern\":\"%s\",\"state\":\"%s\",\"message\":\"%s\",\"autoload\":%s,"
                         "\"policy\":\"%s\",\"prerelease\":%s,\"unverified\":%s,\"repo\":\"%s\",\"asset\":\"%s\",\"skip\":\"%s\","
                         "\"has_previous\":%s,\"previous\":\"%s\"}",
                    i ? "," : "", slot_e, name_e, !strcmp(s->kind, SLOT_KIND_REPO) ? "repo" : "local", inst_e, latest, src_e,
                    up_e, pat_e, state_e, msg_e, autoload ? "true" : "false",
                    p.policy, p.prerelease ? "true" : "false", p.unverified ? "true" : "false", repo2_e, asset_e, skip_e,
                    has_previous ? "true" : "false", prev_e);
    }
    pthread_mutex_unlock(&state_mutex);
    json_append(&jb, "]}\n");
    free(entries.keys);
    free_settings(&settings);
    slot_free_all(slots);
    return jb.error ? 0 : jb.pos;
}

int updater_check_now(void) { return start_pass(0, 1); }

static int respond(char *out, size_t size, int ok, const char *message) {
    char escaped[6 * 256];
    pldmgr_json_escape(message ? message : "", escaped, sizeof(escaped));
    json_format(out, size, "{\"ok\":%s,\"message\":\"%s\"}", ok ? "true" : "false", escaped);
    return ok ? 0 : -1;
}

int updater_apply(const char *slot_key, char *out, size_t size) {
    if (!slot_key_valid(slot_key)) return respond(out, size, 0, "Invalid payload");
    Candidate candidate;
    pthread_mutex_lock(&state_mutex);
    Item *item = find_item_locked(slot_key);
    int have = item && item->has_candidate;
    if (have) candidate = item->candidate;
    pthread_mutex_unlock(&state_mutex);
    if (!have) return respond(out, size, 0, "No update available; check for updates first");
    char dir[640];
    snprintf(dir, sizeof(dir), "%s/%s", PAYLOADS_STORAGE_DIR, slot_key);
    SlotMeta *meta = malloc(sizeof(*meta));
    RepoPayload *catalog = malloc(sizeof(*catalog));
    int rc = -1;
    if (!meta || !catalog || slot_read_meta(dir, meta) || !meta->payload_path[0]) {
        respond(out, size, 0, "Payload not found");
        goto done;
    }
    int have_catalog = !strcmp(meta->kind, SLOT_KIND_REPO) && !sources_cached_entry(meta->source_url, meta->id, catalog);
    if (!newer_than_installed(&candidate, meta)) { respond(out, size, 0, "Already up to date"); goto done; }
    InstallResult result = install_candidate(meta, &candidate, have_catalog ? catalog : NULL);
    set_item(slot_key, result.ok ? "updated" : "failed", "%s", result.message);
    if (result.ok) {
        pthread_mutex_lock(&state_mutex);
        if ((item = find_item_locked(slot_key))) {
            item->has_candidate = 0;
            snprintf(item->installed, sizeof(item->installed), "%s", candidate.version);
        }
        pthread_mutex_unlock(&state_mutex);
        pldmgr_log("[Updater] %s: %s (approved)\n", slot_key, result.message);
    }
    rc = respond(out, size, result.ok, result.message);
done:
    free(meta); free(catalog);
    return rc;
}

int updater_rollback(const char *slot_key, char *out, size_t size) {
    if (!slot_key_valid(slot_key)) return respond(out, size, 0, "Invalid payload");
    char dir[640];
    snprintf(dir, sizeof(dir), "%s/%s", PAYLOADS_STORAGE_DIR, slot_key);
    SlotMeta *current = malloc(sizeof(*current)), *restored = malloc(sizeof(*restored));
    int rc = -1;
    if (!current || !restored || slot_read_meta(dir, current)) { respond(out, size, 0, "Payload not found"); goto done; }
    pthread_mutex_lock(&install_mutex);
    int failed = slot_restore_previous(slot_key, restored);
    pthread_mutex_unlock(&install_mutex);
    if (failed) { respond(out, size, 0, "No previous version to restore"); goto done; }
    if (current->version[0]) updater_skip_version(slot_key, current->version);
    char message[256];
    snprintf(message, sizeof(message), "Restored %s; %s will not be installed automatically",
             restored->version[0] ? restored->version : "previous version", current->version[0] ? current->version : "the replaced file");
    set_item(slot_key, "current", "%s", message);
    pldmgr_log("[Updater] %s: %s\n", slot_key, message);
    rc = respond(out, size, 1, message);
done:
    free(current); free(restored);
    return rc;
}
