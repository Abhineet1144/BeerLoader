/* Session storage in the desktop keyring (Secret Service through libsecret, loaded at run time).
 * Without a keyring nothing is stored and the user signs in each time the provider starts. */
#ifndef BEER_STEAM_SECRET_H
#define BEER_STEAM_SECRET_H

int secret_available(void);
/* The signed-in session: account name and refresh token. */
int secret_save_session(const char *account, const char *refresh_token);
/* 1 when found (caller frees both), 0 when none is stored, -1 when the keyring is unavailable. */
int secret_load_session(char **account, char **refresh_token);
int secret_clear_session(void);
/* Steam Guard device data per account; kept after sign-out, as the Steam client does. */
int secret_save_guard(const char *account, const char *guard_data);
char *secret_load_guard(const char *account);
/* Beta branch keys ("<appid>/<branch>"), returned by Steam for a correct beta password. */
int secret_save_beta(const char *app_branch, const char *key_hex);
char *secret_load_beta(const char *app_branch);
/* Overwrites and frees a secret string. */
void secret_wipe(char *s);

#endif
