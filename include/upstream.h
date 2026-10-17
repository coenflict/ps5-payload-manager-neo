#ifndef UPSTREAM_H
#define UPSTREAM_H

#include <stddef.h>

/* Release lookup directly from a payload maintainer's forge, used ahead of
 * (and cross-checked against) curated catalogs. GitHub and Gitea/Forgejo
 * release APIs share the fields used here. */

enum { UPSTREAM_NONE = 0, UPSTREAM_GITHUB = 1, UPSTREAM_GITEA = 2 };

typedef struct UpstreamSpec {
    int kind;
    char base[256];      /* Gitea: scheme://host[:port]; unused for GitHub */
    char owner[100];
    char repo[100];
    char pattern[256];   /* case-insensitive asset glob; '*' is the only wildcard */
    char require[128];   /* optional case-insensitive substring of the asset name */
} UpstreamSpec;

typedef struct UpstreamRelease {
    char tag[64];
    char published[16];  /* YYYY-MM-DD */
    int prerelease;
    char asset_name[256];
    char asset_url[1024];
    char sha256[65];     /* published digest, lower-case hex; empty when absent */
    long long size;
} UpstreamRelease;

#ifndef PLDMGR_GITHUB_API
#define PLDMGR_GITHUB_API "https://api.github.com"
#endif

/* Parse "owner/repo", "github.com/owner/repo" or a forge URL such as
 * https://host/owner/repo/releases[/download/...]. Returns 0 or -1. */
int upstream_parse_repo(const char *text, UpstreamSpec *spec);

/* Derive an asset glob from a known asset file name and the version it was
 * released as: the last occurrence of the version becomes '*'. Archives and
 * non-payload extensions return -1. */
int upstream_derive_pattern(const char *asset_url_or_name, const char *version,
                            char *out, size_t size);

int upstream_glob_match(const char *pattern, const char *name);

int upstream_api_url(const UpstreamSpec *spec, char *out, size_t size);

/* Select the highest-versioned non-draft release (stable unless allowed)
 * that carries exactly one payload asset matching the spec.
 * Returns 0 when found, 1 when none qualifies, -1 for malformed input. */
int upstream_select_release(const char *json, size_t size, const UpstreamSpec *spec,
                            int allow_prerelease, UpstreamRelease *out);

enum {
    UPSTREAM_OK = 0,          /* release selected */
    UPSTREAM_NO_RELEASE = 1,  /* response valid, nothing qualifies */
    UPSTREAM_ERROR = -1,      /* network/HTTP/parse failure */
    UPSTREAM_RATE_LIMITED = -2
};

/* Look up the newest qualifying release. Responses are cached under
 * cache_dir with their ETag: within max_age seconds the cache is used without
 * a request, afterwards a conditional request is made (a 304 does not count
 * against GitHub's rate limit). A stale cache is used when the network fails
 * and *from_cache is set. */
int upstream_check(const UpstreamSpec *spec, int allow_prerelease, long max_age,
                   const char *cache_dir, UpstreamRelease *out, int *from_cache,
                   char *error, size_t error_size);

/* Tag names with alpha/beta/rc/pre/dev/test/... words are prereleases even
 * when the release is not flagged; "2.5B"-style suffixes are not. */
int upstream_version_is_prerelease(const char *version);

/* Major component used to gate potentially breaking updates; -1 when the
 * version has no leading number or looks like a build number/date. */
long upstream_major_version(const char *version);

#endif
