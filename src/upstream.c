#include <ctype.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "upstream.h"
#include "repository.h"
#include "json_helpers.h"
#include "sha256.h"
#include "slot.h"
#include "pldmgr.h"

#define RELEASES_LIMIT ((size_t)4 * 1024 * 1024)

static int name_part_valid(const char *s, size_t n) {
    if (!n || n >= 100 || (n == 1 && s[0] == '.') || (n == 2 && !strncmp(s, "..", 2))) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_' && s[i] != '.') return 0;
    return 1;
}

/* Copy owner and repository from "owner/repo[/...]". */
static int take_owner_repo(const char *path, UpstreamSpec *spec) {
    while (*path == '/') path++;
    const char *slash = strchr(path, '/');
    if (!slash) return -1;
    const char *repo = slash + 1, *end = repo + strcspn(repo, "/?#");
    size_t owner_len = (size_t)(slash - path), repo_len = (size_t)(end - repo);
    if (repo_len > 4 && !strncasecmp(end - 4, ".git", 4)) repo_len -= 4;
    if (!name_part_valid(path, owner_len) || !name_part_valid(repo, repo_len)) return -1;
    memcpy(spec->owner, path, owner_len); spec->owner[owner_len] = 0;
    memcpy(spec->repo, repo, repo_len); spec->repo[repo_len] = 0;
    return 0;
}

int upstream_parse_repo(const char *text, UpstreamSpec *spec) {
    if (!text || !spec) return -1;
    UpstreamSpec parsed = {0};
    while (isspace((unsigned char)*text)) text++;
    char buffer[1024];
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) n--;
    if (!n || n >= sizeof(buffer)) return -1;
    memcpy(buffer, text, n); buffer[n] = 0;
    const char *rest = buffer;
    int https = !strncasecmp(rest, "https://", 8), http = !strncasecmp(rest, "http://", 7);
    if (https || http) {
        const char *host = rest + (https ? 8 : 7);
        size_t host_len = strcspn(host, "/?#");
        if (!host_len || host_len >= 200 || memchr(host, '@', host_len)) return -1;
        for (size_t i = 0; i < host_len; i++)
            if (!isalnum((unsigned char)host[i]) && !strchr(".-:[]", host[i])) return -1;
        if ((host_len == 10 && !strncasecmp(host, "github.com", 10)) ||
            (host_len == 14 && !strncasecmp(host, "www.github.com", 14))) {
            if (!https) return -1;
            parsed.kind = UPSTREAM_GITHUB;
        } else {
            parsed.kind = UPSTREAM_GITEA;
            snprintf(parsed.base, sizeof(parsed.base), "%.*s%.*s",
                     https ? 8 : 7, rest, (int)host_len, host);
        }
        rest = host + host_len;
    } else {
        if (!strncasecmp(rest, "github.com/", 11)) rest += 11;
        parsed.kind = UPSTREAM_GITHUB;
        /* A bare reference must be exactly owner/repo. */
        const char *slash = strchr(rest, '/');
        if (!slash || strchr(slash + 1, '/')) return -1;
    }
    if (take_owner_repo(rest, &parsed)) return -1;
    memcpy(parsed.pattern, spec->pattern, sizeof(parsed.pattern));
    memcpy(parsed.require, spec->require, sizeof(parsed.require));
    *spec = parsed;
    return 0;
}

static int payload_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".elf") || !strcasecmp(dot, ".bin"));
}

static const char *last_case_occurrence(const char *haystack, const char *needle) {
    size_t n = strlen(needle), h = strlen(haystack);
    if (!n || n > h) return NULL;
    for (size_t i = h - n + 1; i-- > 0;)
        if (!strncasecmp(haystack + i, needle, n)) return haystack + i;
    return NULL;
}

int upstream_derive_pattern(const char *asset, const char *version, char *out, size_t size) {
    if (!asset || !out || !size) return -1;
    out[0] = 0;
    const char *slash = strrchr(asset, '/');
    const char *name = slash ? slash + 1 : asset;
    size_t length = strcspn(name, "?#");
    char file[256];
    if (!length || length >= sizeof(file)) return -1;
    memcpy(file, name, length); file[length] = 0;
    if (strchr(file, '*') || strchr(file, '%') || !payload_extension(file)) return -1;
    const char *at = NULL; size_t cut = 0;
    if (version && version[0]) {
        if ((at = last_case_occurrence(file, version))) cut = strlen(version);
        else if ((version[0] == 'v' || version[0] == 'V') && version[1] &&
                 (at = last_case_occurrence(file, version + 1))) cut = strlen(version + 1);
    }
    int written = at ? snprintf(out, size, "%.*s*%s", (int)(at - file), file, at + cut)
                     : snprintf(out, size, "%s", file);
    if (written < 0 || (size_t)written >= size) { out[0] = 0; return -1; }
    return 0;
}

int upstream_glob_match(const char *pattern, const char *name) {
    const char *star = NULL, *resume = NULL;
    while (*name) {
        if (*pattern == '*') { star = pattern++; resume = name; continue; }
        if (*pattern && tolower((unsigned char)*pattern) == tolower((unsigned char)*name)) {
            pattern++; name++; continue;
        }
        if (!star) return 0;
        pattern = star + 1; name = ++resume;
    }
    while (*pattern == '*') pattern++;
    return *pattern == 0;
}

int upstream_api_url(const UpstreamSpec *spec, char *out, size_t size) {
    if (!spec || !out || !size || !spec->owner[0] || !spec->repo[0]) return -1;
    int n = -1;
    if (spec->kind == UPSTREAM_GITHUB)
        n = snprintf(out, size, "%s/repos/%s/%s/releases?per_page=20", PLDMGR_GITHUB_API, spec->owner, spec->repo);
    else if (spec->kind == UPSTREAM_GITEA && spec->base[0])
        n = snprintf(out, size, "%s/api/v1/repos/%s/%s/releases?limit=20", spec->base, spec->owner, spec->repo);
    if (n < 0 || (size_t)n >= size) { out[0] = 0; return -1; }
    return 0;
}

int upstream_version_is_prerelease(const char *version) {
    static const char *words[] = { "alpha", "beta", "rc", "pre", "preview", "dev", "test", "nightly", "canary", "snapshot" };
    if (!version) return 0;
    for (const char *p = version; *p;) {
        if (!isalpha((unsigned char)*p)) { p++; continue; }
        const char *start = p;
        while (isalpha((unsigned char)*p)) p++;
        size_t n = (size_t)(p - start);
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
            if (strlen(words[i]) == n && !strncasecmp(start, words[i], n)) return 1;
    }
    return 0;
}

long upstream_major_version(const char *version) {
    if (!version) return -1;
    while (isspace((unsigned char)*version)) version++;
    if (*version == 'v' || *version == 'V') version++;
    if (!isdigit((unsigned char)*version)) return -1;
    long major = 0;
    while (isdigit((unsigned char)*version)) {
        major = major * 10 + (*version++ - '0');
        if (major >= 1000) return -1; /* build numbers and dates are not semantic */
    }
    return major;
}

/* ── Release JSON ─────────────────────────────────────────── */

#define MAX_MATCHES 8

typedef struct {
    const UpstreamSpec *spec;
    const char *tag;
    UpstreamRelease matches[MAX_MATCHES];
    int count, overflow;
} AssetScan;

static int literal_true(const char *start, const char *end) {
    return end - start == 4 && !memcmp(start, "true", 4);
}

static int contains_case(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++) if (!strncasecmp(haystack, needle, n)) return 1;
    return !n;
}

static int asset_size_member(const char *key, const char *start, const char *end, void *context) {
    if (strcmp(key, "size")) return 0;
    long long *size = context, value = 0;
    for (const char *p = start; p < end; p++) {
        if (!isdigit((unsigned char)*p) || value > (long long)1 << 50) return 0;
        value = value * 10 + (*p - '0');
    }
    *size = value;
    return 0;
}

/* Whether the wildcard of a single-star pattern is exactly the release tag. */
static int exact_for_tag(const char *pattern, const char *tag, const char *name) {
    const char *star = strchr(pattern, '*');
    if (!star || strchr(star + 1, '*')) return 0;
    const char *forms[2] = { tag, (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag };
    for (int i = 0; i < 2; i++) {
        char expected[512];
        int n = snprintf(expected, sizeof(expected), "%.*s%s%s", (int)(star - pattern), pattern, forms[i], star + 1);
        if (n > 0 && (size_t)n < sizeof(expected) && !strcasecmp(expected, name)) return 1;
    }
    return 0;
}

static int visit_asset(const char *start, const char *end, void *context) {
    AssetScan *scan = context;
    UpstreamRelease asset = {0};
    char digest[96];
    if (json_extract_string(start, end, "name", asset.asset_name, sizeof(asset.asset_name)) ||
        json_extract_string(start, end, "browser_download_url", asset.asset_url, sizeof(asset.asset_url))) return 0;
    if (!payload_extension(asset.asset_name) || !upstream_glob_match(scan->spec->pattern, asset.asset_name) ||
        !contains_case(asset.asset_name, scan->spec->require) ||
        (strncasecmp(asset.asset_url, "https://", 8) && strncasecmp(asset.asset_url, "http://", 7))) return 0;
    if (!json_extract_string(start, end, "digest", digest, sizeof(digest)) &&
        !strncasecmp(digest, "sha256:", 7) && strlen(digest + 7) == 64 && repository_checksum_valid(digest + 7)) {
        for (int i = 0; i < 64; i++) asset.sha256[i] = (char)tolower((unsigned char)digest[7 + i]);
    }
    json_visit_object(start, end, asset_size_member, &asset.size);
    if (scan->count == MAX_MATCHES) { scan->overflow = 1; return 0; }
    scan->matches[scan->count++] = asset;
    return 0;
}

/* One match is used as is. Several are resolved only by an exact tag
 * substitution; anything else is ambiguous and never guessed. */
static const UpstreamRelease *chosen_asset(const AssetScan *scan) {
    if (scan->overflow || !scan->count) return NULL;
    if (scan->count == 1) return &scan->matches[0];
    const UpstreamRelease *exact = NULL;
    for (int i = 0; i < scan->count; i++) {
        if (!exact_for_tag(scan->spec->pattern, scan->tag, scan->matches[i].asset_name)) continue;
        if (exact) return NULL;
        exact = &scan->matches[i];
    }
    return exact;
}

typedef struct {
    const UpstreamSpec *spec;
    int allow_prerelease, found;
    UpstreamRelease *best;
} ReleaseScan;

typedef struct { const char *assets_start, *assets_end; int draft, prerelease; } ReleaseFields;

static int release_member(const char *key, const char *start, const char *end, void *context) {
    ReleaseFields *fields = context;
    if (!strcmp(key, "assets") && *start == '[') { fields->assets_start = start; fields->assets_end = end; }
    else if (!strcmp(key, "draft")) fields->draft = literal_true(start, end);
    else if (!strcmp(key, "prerelease")) fields->prerelease = literal_true(start, end);
    return 0;
}

static int visit_release(const char *start, const char *end, void *context) {
    ReleaseScan *scan = context;
    ReleaseFields fields = {0};
    if (*start != '{' || json_visit_object(start, end, release_member, &fields)) return -1;
    if (fields.draft || (fields.prerelease && !scan->allow_prerelease) || !fields.assets_start) return 0;
    UpstreamRelease candidate = {0};
    if (json_extract_string(start, end, "tag_name", candidate.tag, sizeof(candidate.tag)) ||
        !candidate.tag[0] || strpbrk(candidate.tag, "\r\n,")) return 0;
    /* Many maintainers publish betas without the prerelease flag. */
    if (!scan->allow_prerelease && upstream_version_is_prerelease(candidate.tag)) return 0;
    char published[64] = "";
    json_extract_string(start, end, "published_at", published, sizeof(published));
    if (strlen(published) >= 10) { memcpy(candidate.published, published, 10); candidate.published[10] = 0; }
    candidate.prerelease = fields.prerelease;
    AssetScan *assets = calloc(1, sizeof(*assets));
    if (!assets) return -1;
    assets->spec = scan->spec; assets->tag = candidate.tag;
    const UpstreamRelease *asset = NULL;
    if (!json_visit_array(fields.assets_start, fields.assets_end, visit_asset, assets))
        asset = chosen_asset(assets);
    if (asset) {
        memcpy(candidate.asset_name, asset->asset_name, sizeof(candidate.asset_name));
        memcpy(candidate.asset_url, asset->asset_url, sizeof(candidate.asset_url));
        memcpy(candidate.sha256, asset->sha256, sizeof(candidate.sha256));
        candidate.size = asset->size;
    }
    free(assets);
    if (!asset) return 0;
    if (!scan->found || slot_version_compare(candidate.tag, scan->best->tag) > 0) {
        *scan->best = candidate;
        scan->found = 1;
    }
    return 0;
}

int upstream_select_release(const char *json, size_t size, const UpstreamSpec *spec,
                            int allow_prerelease, UpstreamRelease *out) {
    if (!json || !spec || !out || !spec->pattern[0]) return -1;
    memset(out, 0, sizeof(*out));
    const char *start = json;
    while (start < json + size && isspace((unsigned char)*start)) start++;
    if (start == json + size || *start != '[') return -1;
    UpstreamRelease best = {0};
    ReleaseScan scan = { spec, allow_prerelease, 0, &best };
    if (json_visit_array(start, json + size, visit_release, &scan)) return -1;
    if (!scan.found) return 1;
    *out = best;
    return 0;
}

/* ── Cached, conditional lookup ───────────────────────────── */

static void cache_paths(const char *dir, const char *url, char body[512], char meta[512]) {
    SHA256_CTX ctx; unsigned char hash[32]; char hex[17];
    sha256_init(&ctx); sha256_update(&ctx, (const unsigned char *)url, strlen(url)); sha256_final(&ctx, hash);
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    snprintf(body, 512, "%s/upstream_%s.json", dir, hex);
    snprintf(meta, 512, "%s/upstream_%s.meta", dir, hex);
}

static void read_meta(const char *path, char *etag, size_t etag_size, long *checked) {
    char *text = NULL; size_t size = 0;
    etag[0] = 0; *checked = 0;
    if (read_file_text(path, &text, &size)) return;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "etag=", 5) && strlen(line + 5) < etag_size) strcpy(etag, line + 5);
        else if (!strncmp(line, "checked=", 8)) *checked = strtol(line + 8, NULL, 10);
    }
    free(text);
}

static void write_meta(const char *path, const char *etag, long checked) {
    char text[384];
    int n = snprintf(text, sizeof(text), "etag=%s\nchecked=%ld\n", etag ? etag : "", checked);
    if (n > 0 && (size_t)n < sizeof(text)) write_file_text(path, text, (size_t)n);
}

static long read_rate_limit(const char *dir) {
    char path[512], *text = NULL; size_t size = 0;
    snprintf(path, sizeof(path), "%s/upstream_ratelimit", dir);
    if (read_file_text(path, &text, &size)) return 0;
    long reset = strtol(text, NULL, 10);
    free(text);
    return reset;
}

static void write_rate_limit(const char *dir, long reset) {
    char path[512], text[32];
    snprintf(path, sizeof(path), "%s/upstream_ratelimit", dir);
    int n = snprintf(text, sizeof(text), "%ld\n", reset);
    write_file_text(path, text, (size_t)n);
}

static int parse_cached(const char *path, const UpstreamSpec *spec, int allow_prerelease, UpstreamRelease *out) {
    char *json = NULL; size_t size = 0;
    if (read_file_text(path, &json, &size)) return UPSTREAM_ERROR;
    int rc = upstream_select_release(json, size, spec, allow_prerelease, out);
    free(json);
    return rc < 0 ? UPSTREAM_ERROR : rc;
}

/* After a transport failure (DNS, connect, timeout) GitHub is skipped briefly,
 * so one boot never stacks a connect timeout per payload. */
static pthread_mutex_t reachability_mutex = PTHREAD_MUTEX_INITIALIZER;
static long github_unreachable_until;

int upstream_check(const UpstreamSpec *spec, int allow_prerelease, long max_age,
                   const char *cache_dir, UpstreamRelease *out, int *from_cache,
                   char *error, size_t error_size) {
    int dummy = 0;
    if (!from_cache) from_cache = &dummy;
    *from_cache = 0;
    if (error && error_size) error[0] = 0;
    char url[640], body[512], meta[512], etag[256];
    if (!spec || !out || !cache_dir || upstream_api_url(spec, url, sizeof(url))) {
        if (error) snprintf(error, error_size, "No upstream repository");
        return UPSTREAM_ERROR;
    }
    memset(out, 0, sizeof(*out));
    ensure_dir_recursive(cache_dir);
    cache_paths(cache_dir, url, body, meta);
    long checked = 0, now = (long)time(NULL);
    read_meta(meta, etag, sizeof(etag), &checked);
    int cached = access(body, F_OK) == 0;
    if (cached && max_age > 0 && now >= checked && now - checked < max_age) {
        int rc = parse_cached(body, spec, allow_prerelease, out);
        if (rc != UPSTREAM_ERROR) { *from_cache = 1; return rc; }
        cached = 0;
    }
    if (spec->kind == UPSTREAM_GITHUB && read_rate_limit(cache_dir) > now) {
        if (cached) { *from_cache = 1; return parse_cached(body, spec, allow_prerelease, out); }
        if (error) snprintf(error, error_size, "GitHub rate limit reached");
        return UPSTREAM_RATE_LIMITED;
    }
    pthread_mutex_lock(&reachability_mutex);
    int unreachable = spec->kind == UPSTREAM_GITHUB && github_unreachable_until > now;
    pthread_mutex_unlock(&reachability_mutex);
    if (unreachable) {
        if (cached) { *from_cache = 1; return parse_cached(body, spec, allow_prerelease, out); }
        if (error) snprintf(error, error_size, "Upstream unreachable");
        return UPSTREAM_ERROR;
    }
    char temp[544];
    snprintf(temp, sizeof(temp), "%s/.upstream-XXXXXX", cache_dir);
    int fd = mkstemp(temp);
    if (fd < 0 || close(fd)) {
        if (fd >= 0) remove(temp);
        if (error) snprintf(error, error_size, "Storage error");
        return UPSTREAM_ERROR;
    }
    HttpFetch request = { .url = url, .out_path = temp, .limit = RELEASES_LIMIT,
                          .if_none_match = cached ? etag : NULL, .api = spec->kind == UPSTREAM_GITHUB,
                          .connect_timeout = 4, .timeout = 10 };
    int fetched = http_fetch(&request);
    if (!fetched && request.status == 304 && cached) {
        write_meta(meta, etag, now);
        return parse_cached(body, spec, allow_prerelease, out);
    }
    if (!fetched && request.status == 200) {
        char *json = NULL; size_t size = 0;
        int rc = read_file_text(temp, &json, &size) ? UPSTREAM_ERROR
               : upstream_select_release(json, size, spec, allow_prerelease, out);
        free(json);
        if (rc >= 0 && !rename(temp, body)) {
            write_meta(meta, request.etag, now);
            return rc;
        }
        remove(temp);
        if (error) snprintf(error, error_size, "Malformed release data");
        return UPSTREAM_ERROR;
    }
    remove(temp);
    if (!request.status && spec->kind == UPSTREAM_GITHUB) {
        pthread_mutex_lock(&reachability_mutex);
        github_unreachable_until = now + 60;
        pthread_mutex_unlock(&reachability_mutex);
    }
    int limited = spec->kind == UPSTREAM_GITHUB &&
        ((request.status == 403 && request.ratelimit_remaining == 0) || request.status == 429 ||
         (request.status == 403 && request.retry_after > 0));
    if (limited) {
        long reset = request.ratelimit_reset > now ? request.ratelimit_reset
                   : now + (request.retry_after > 0 ? request.retry_after : 600);
        write_rate_limit(cache_dir, reset);
        pldmgr_log("[Updater] GitHub rate limit reached; upstream checks paused until %ld.\n", reset);
    }
    if (error) {
        if (limited) snprintf(error, error_size, "GitHub rate limit reached");
        else if (request.status) snprintf(error, error_size, "Upstream returned HTTP %ld", request.status);
        else snprintf(error, error_size, "Upstream unreachable");
    }
    if (cached) {
        int rc = parse_cached(body, spec, allow_prerelease, out);
        if (rc != UPSTREAM_ERROR) { *from_cache = 1; return rc; }
    }
    return limited ? UPSTREAM_RATE_LIMITED : UPSTREAM_ERROR;
}
