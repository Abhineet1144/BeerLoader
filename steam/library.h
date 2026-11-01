/* Owned-app library: licenses -> packages -> apps, resolved through Steam's PICS product info. */
#ifndef BEER_STEAM_LIBRARY_H
#define BEER_STEAM_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#include "cm.h"

typedef struct {
    uint32_t package_id;
    uint64_t access_token;
    uint32_t owner_id;           /* account that owns the license (another one when family-shared) */
} SteamLicense;

typedef struct {
    uint32_t appid;
    char *name;
    char *type;                  /* lowercase PICS type: game, demo, application, tool, dlc, ... */
    int windows;                 /* oslist names Windows (or is empty, which Steam treats as Windows) */
    int shared;                  /* only owned through another account's license */
} SteamApp;

/* Parses CMsgClientLicenseList. Returns the EResult it carries. */
int library_parse_licenses(const uint8_t *body, size_t len, SteamLicense **licenses, size_t *count);

/* Resolves the licenses into apps. account_id is the low 32 bits of the signed-in SteamID.
 * Returns 1 on success; err describes a failure. */
int library_fetch(Cm *cm, uint32_t account_id, const SteamLicense *licenses, size_t license_count,
                  SteamApp **apps, size_t *app_count, char *err, size_t errlen);
void library_free(SteamApp *apps, size_t count);

/* Full PICS record of one app (root block; its "appinfo" child holds common, config, depots...).
 * NULL when Steam did not return it. Caller frees with vdf_free. */
struct VdfNode;
struct VdfNode *library_app_info(Cm *cm, uint32_t appid);

/* JSON array of the apps, sorted by name. Caller frees. */
char *library_json(const SteamApp *apps, size_t count);

#endif
