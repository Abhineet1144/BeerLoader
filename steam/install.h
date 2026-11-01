/* Install, update, verify and uninstall Steam apps into Beer library folders.
 *
 * Layout inside a library folder:
 *   common/<installdir>/               the game files
 *   beer-steam/app_<appid>.vdf         install record (KeyValues text)
 *   beer-steam/depots/<depot>_<gid>.manifest   plain-name manifests of what is installed
 *   beer-steam/staging/<appid>/        files being built; renamed into place on commit
 *   beer-steam/staging/<appid>.done    SHA-1s of chunks already written (resume) */
#ifndef BEER_STEAM_INSTALL_H
#define BEER_STEAM_INSTALL_H

#include <stddef.h>
#include <stdint.h>

#include "cm.h"

enum { INSTALL_ACTION_INSTALL, INSTALL_ACTION_UPDATE, INSTALL_ACTION_VERIFY, INSTALL_ACTION_MOVE };

typedef struct Installer Installer;
/* Receives one JSON event object (without newline) about a job. */
typedef void (*InstallEventFn)(void *context, const char *json);

Installer *installer_new(Cm *cm, InstallEventFn event, void *context);

typedef struct {
    uint32_t appid;
    char name[256];
    char folder[1024];           /* library folder */
    char branch[64];             /* "public" unless a beta is chosen */
    char language[32];           /* Steam language name, e.g. "english" */
    char beta_key[65];           /* hex key for a password-protected branch, or empty */
    char target[1024];           /* INSTALL_ACTION_MOVE: the library folder to move to */
    int action;
    const uint32_t *owned;       /* owned app IDs (for DLC depots); copied */
    size_t owned_count;
} InstallRequest;

/* Queues a job. Returns 0 (err set) if the app is already queued or the folder is unusable. */
int installer_enqueue(Installer *in, const InstallRequest *request, char *err, size_t errlen);
void installer_pause(Installer *in, uint32_t appid);
void installer_resume(Installer *in, uint32_t appid);
void installer_cancel(Installer *in, uint32_t appid);
/* Download limit in bytes per second, 0 for none. */
void installer_set_bandwidth(Installer *in, uint64_t bytes_per_second);
/* JSON array of queued and running jobs. Caller frees. */
char *installer_queue_json(Installer *in);
int installer_busy_with(Installer *in, uint32_t appid);

/* JSON array of install records found in the folders. Caller frees. */
char *installs_json(const char *const *folders, size_t count);
/* Removes the files the manifests list (other files in the game folder are kept). */
int install_remove(const char *folder, uint32_t appid, char *err, size_t errlen);

#endif
