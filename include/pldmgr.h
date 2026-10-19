#pragma once

/* Network Settings */
#define MENU_PORT 8084
#define ELFLDR_PORT 9021

/* Routes */
#define ROUTE_INDEX "/"
#define ROUTE_INDEX_HTML "/index.html"
#define ROUTE_LIST_PAYLOADS "/list_payloads"
#define ROUTE_UPLOAD "/manage:upload"
#define ROUTE_CHECK "/manage:check"
#define ROUTE_DELETE "/manage:delete"
#define ROUTE_LOAD_PAYLOAD "/loadpayload:"
#define ROUTE_SHUTDOWN "/shutdown"
#define ROUTE_LOG "/log"
#define ROUTE_VERSION "/version"
#define ROUTE_GETIP "/getip"
#define ROUTE_GET_CONFIG "/get_config"
#define ROUTE_SET_CONFIG "/set_config"
#define ROUTE_ABORT "/abort"
#define ROUTE_AUTOLOAD_STATUS "/autoload_status"
#define ROUTE_AUTOLOAD_CLEAR "/autoload_clear"
#define ROUTE_REPO_ICONS "/repository_icons"
#define ROUTE_REPO_CACHE_CLEAR "/repository_cache_clear"
#define ROUTE_REPO_LIST "/repository_payloads"
#define ROUTE_REPO_REFRESH "/repository_refresh"
#define ROUTE_REPO_INSTALL "/repository_install"
#define ROUTE_UPDATE_CHECK "/update_check"
#define ROUTE_SOURCES_LIST "/sources_list"
#define ROUTE_SOURCES_SET "/sources_set"
#define ROUTE_SOURCES_ADD "/sources_add"
#define ROUTE_SOURCES_REMOVE "/sources_remove"
#define ROUTE_USB_MOVE_CHECK "/usb_move_check"
#define ROUTE_USB_MOVE_PERFORM "/usb_move_perform"
#define ROUTE_CACHE_MANIFEST "/cache.appcache"

#define ROUTE_PROCESSES_LIST "/processes_list"
#define ROUTE_PROCESS_KILL "/process_kill"
#define ROUTE_HISTORY_LIST "/history_list"
#define ROUTE_UPDATES_STATUS "/updates_status"
#define ROUTE_UPDATES_CHECK "/updates_check"
#define ROUTE_UPDATES_APPLY "/updates_apply"
#define ROUTE_UPDATES_ROLLBACK "/updates_rollback"
#define ROUTE_UPDATES_SETTINGS "/updates_settings"

/* Payload Manager Neo: a fork of PS5 Payload Manager by PLK. */
#define PRODUCT_NAME "Payload Manager Neo"
#define MENU_VERSION "0.7.1"
#define AUTOLOAD_CONFIG_PATH "/data/pldmgr/autoload.txt"
#define FAVORITES_CONFIG_PATH "/data/pldmgr/favorites.txt"
#define PLDMGR_CONFIG_PATH "/data/pldmgr/pldmgr_config.txt"
#define REPOSITORY_CACHE_PATH "/data/pldmgr/repository_cache.json"
#define PAYLOADS_STORAGE_DIR "/data/pldmgr/payloads"
#define REPOSITORY_SOURCE_URL                                                  \
  "https://itsplk.github.io/ps5-payloads-mirror/payloads_v2.json"
#define LEGACY_REPOSITORY_SOURCE_URL                                           \
  "https://itsplk.github.io/ps5-payloads-mirror/payloads.json"
#define REPOSITORY_REFRESH_INTERVAL_SEC 10800

/* Automatic updates */
#define UPDATES_CONFIG_PATH "/data/pldmgr/updates.json"
#define UPSTREAM_CACHE_DIR "/data/pldmgr/upstream"
#define BOOT_GUARD_PATH "/data/pldmgr/boot_guard.txt"
/* Self-updates of non-official builds live outside the legacy directories,
 * which official launchers scan and official self-updates prune. */
#define CHANNEL_UPDATE_DIR "/data/pldmgr/self"
#define PLDMGR_OFFICIAL_REPO "itsPLK/ps5-payload-manager"
/* GitHub repository whose releases update this build. Builds only ever hand
 * off to, or install, a manager carrying the same channel marker. Override
 * with `make PLDMGR_UPDATE_REPO=owner/repo`; release CI uses its repository. */
#ifndef PLDMGR_UPDATE_REPO
#define PLDMGR_UPDATE_REPO "coenflict/ps5-payload-manager-neo"
#endif

/* Logging (implementation in log_server.c) */
void pldmgr_log(const char *fmt, ...);
int pldmgr_server_is_active();

#include "autoload.h"
#include "notification.h"
#include "utils.h"

/* Paths */
#define BASE_DATA_DIR "/data/pldmgr"
#define SOURCES_CONFIG_PATH "/data/pldmgr/sources.json"
#define MAX_SOURCES 50

/* Scan Locations (Internal + 8 USB ports) */
static const char *SCAN_DIRS[] = {
    "/data/pldmgr",     "/mnt/usb0/pldmgr", "/mnt/usb1/pldmgr",
    "/mnt/usb2/pldmgr", "/mnt/usb3/pldmgr", "/mnt/usb4/pldmgr",
    "/mnt/usb5/pldmgr", "/mnt/usb6/pldmgr", "/mnt/usb7/pldmgr"};
#define SCAN_DIRS_COUNT 9

/* Messages */
#define MSG_OK "OK"
