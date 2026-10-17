/* Drives the actual updater, upstream resolver, boot guard and release channel
 * against a local HTTP fixture. Only the network-presence probe is replaced. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "pldmgr.h"
#include "config.h"
#include "updater.h"
#include "upstream.h"
#include "boot_guard.h"
#include "channel.h"
#include "json_helpers.h"
#include "slot.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

void pldmgr_log(const char *fmt, ...) {
    if (!getenv("UPDATER_VERBOSE")) return;
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
}
void pldmgr_notify(const char *fmt, ...) { (void)fmt; }
int pldmgr_autoload_update_config_entry(const char *old_key, const char *new_key) {
    return config_update_list_entry(AUTOLOAD_CONFIG_PATH, old_key, new_key);
}
int updater_test_ip(char *buf, size_t size) {
    if (getenv("UPDATER_OFFLINE")) return -1;
    snprintf(buf, size, "192.0.2.10");
    return 0;
}

static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int never_cancelled(void *context) { (void)context; return 0; }

static void print_status(void) {
    char *buf = malloc(1 << 20);
    CHECK(buf);
    CHECK(updater_status_json(buf, 1 << 20) > 0);
    printf("%s", buf);
    free(buf);
}

/* pass [entry...]: boot pass; each entry is waited for like autoload would. */
static void pass(int argc, char **argv, int manual) {
    CHECK(manual ? updater_check_now() == 0 : updater_start(1) == 0);
    for (int i = 0; i < argc; i++) {
        char key[128];
        long long begin = now_ms();
        int rc = updater_wait_entry(argv[i], key, sizeof(key), never_cancelled, NULL);
        fprintf(stderr, "WAIT %s -> %s rc=%d ms=%lld\n", argv[i], key, rc, now_ms() - begin);
    }
    /* Join the pass so results and files are final. */
    char *buf = malloc(1 << 20);
    CHECK(buf);
    long long end = now_ms() + 30000;
    for (;;) {
        CHECK(updater_status_json(buf, 1 << 20) > 0);
        if (strstr(buf, "\"running\":false")) break;
        CHECK(now_ms() < end);
        usleep(20000);
    }
    free(buf);
    updater_stop();
    print_status();
}

int main(int argc, char **argv) {
    CHECK(argc >= 2);
    CHECK(!ensure_dir_recursive(BASE_DATA_DIR));
    const char *cmd = argv[1];
    if (!strcmp(cmd, "pass")) pass(argc - 2, argv + 2, 0);
    else if (!strcmp(cmd, "manual")) pass(argc - 2, argv + 2, 1);
    else if (!strcmp(cmd, "status")) print_status();
    else if (!strcmp(cmd, "settings")) printf("%d\n", updater_settings_json(argv[2]));
    else if (!strcmp(cmd, "apply") || !strcmp(cmd, "rollback")) {
        char out[1024];
        if (!strcmp(cmd, "apply")) pass(0, NULL, 1); /* candidates come from a pass */
        int rc = !strcmp(cmd, "apply") ? updater_apply(argv[2], out, sizeof(out)) : updater_rollback(argv[2], out, sizeof(out));
        printf("\nRESULT %d %s\n", rc, out);
    } else if (!strcmp(cmd, "guard-launch")) {
        /* Updated payload launched, then the "console crashes" (process exits). */
        boot_guard_updated(argv[2], argv[3]);
        boot_guard_launching(argv[2]);
        if (argc > 4 && !strcmp(argv[4], "done")) boot_guard_sequence_done();
        if (argc > 4 && !strcmp(argv[4], "healthy")) { boot_guard_sequence_done(); sleep(2); boot_guard_tick(); }
    } else if (!strcmp(cmd, "guard-eval")) printf("%d\n", boot_guard_evaluate(atoi(argv[2])));
    else if (!strcmp(cmd, "handoff")) {
        char path[1024], version[64];
        int found = channel_select_handoff(path, sizeof(path), version, sizeof(version));
        printf("%d %s %s\n", found, found ? version : "-", found ? path : "-");
    } else if (!strcmp(cmd, "attempt")) channel_record_attempt(argv[2]);
    else if (!strcmp(cmd, "healthy")) channel_mark_healthy();
    else if (!strcmp(cmd, "channel-check")) {
        channel_check_update(0);
        ChannelStatus s; channel_status(&s);
        printf("%s|%s|%s|%s|%d\n", s.state, s.latest, s.pending, s.message, s.official);
    } else if (!strcmp(cmd, "fd-matches")) {
        int fd = open(argv[2], O_RDONLY);
        CHECK(fd >= 0);
        printf("%d\n", channel_fd_matches(fd));
        close(fd);
    } else if (!strcmp(cmd, "parse")) {
        UpstreamSpec spec = {0};
        int rc = upstream_parse_repo(argv[2], &spec);
        printf("%d %d %s %s %s\n", rc, spec.kind, spec.owner[0] ? spec.owner : "-", spec.repo[0] ? spec.repo : "-", spec.base[0] ? spec.base : "-");
    } else if (!strcmp(cmd, "derive")) {
        char out[256];
        int rc = upstream_derive_pattern(argv[2], argv[3], out, sizeof(out));
        printf("%d %s\n", rc, rc ? "-" : out);
    } else if (!strcmp(cmd, "glob")) printf("%d\n", upstream_glob_match(argv[2], argv[3]));
    else if (!strcmp(cmd, "major")) printf("%ld\n", upstream_major_version(argv[2]));
    else if (!strcmp(cmd, "select")) {
        char *json = NULL; size_t size = 0;
        CHECK(!read_file_text(argv[2], &json, &size));
        UpstreamSpec spec = {0};
        snprintf(spec.pattern, sizeof(spec.pattern), "%s", argv[3]);
        if (argc > 4) snprintf(spec.require, sizeof(spec.require), "%s", argv[4]);
        UpstreamRelease release;
        int rc = upstream_select_release(json, size, &spec, argc > 5 && !strcmp(argv[5], "pre"), &release);
        printf("%d %s %s %s\n", rc, rc ? "-" : release.tag, rc ? "-" : release.asset_name, rc || !release.sha256[0] ? "-" : release.sha256);
        free(json);
    } else return 2;
    return 0;
}
