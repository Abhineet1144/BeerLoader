# Steam Session Provider Design

## Status

Design document only. No Steam login, library, download, or in-game session code exists yet.
Revised 2026-11-01: the provider now authenticates with Steam itself and downloads depots itself, and the same design must serve desktop and Android. This replaces the earlier "official Steam client or SteamCMD only" plan, which could not work on Android.

**Platform scope (2026-11-01):** PC (x86-64 Linux) only for now. Android is on hold. Keep the provider portable, so that Android needs only secure storage, an ARM64 OpenSSL build and UI.

## Objective

Let a user sign in to their own Steam account in Beer, see their owned games, download and update them, and play them with a real Steam session, as on Windows. This includes games that need online Steamworks services, ownership tickets, or third-party DRM such as Denuvo.

Unofficial copies keep working as they do today (`facade` and `guest` modes). Official support is added alongside them, not in place of them.

Rules that do not change:

- Everything Steam reports to a game comes from the user's real, authenticated session: identity, ownership, tickets, cloud and stats. Nothing is forged or invented. When the session cannot provide something (offline, not owned, service not implemented), the game gets the real failure.
- No Steam, Wine, Proton or Windows binaries are copied into or shipped with Beer. Valve's `steamclient64.dll` is not executed.
- One implementation for x86-64 desktop and ARM64 Android. Platform code is limited to secure storage and UI.

## Why not use the official Steam client

- Android has no Steam client to delegate to. A desktop-only bridge to the native Linux client (the Proton `lsteamclient` approach) or a Steam compatibility-tool registration would leave Android without support.
- Running the Windows Steam client under Beer needs its CEF web helper, and it would be a large compatibility target of its own.

Beer therefore speaks to Steam directly, as a Steam client does, from one portable component.

## Components

### Session provider (`beer-steam`)

A separate process written in portable C and built for both architectures. The desktop manager and the Android app start it and talk to it over a local socket. The loader talks to it too, during a game.

It owns:

- **Connection.** Steam's connection-manager protocol, over WebSocket/TLS, selected from Steam's published server list, with reconnects and backoff.
- **Authentication.** Steam's own authentication service, using whichever method Steam asks for:
  - a QR code approved in the Steam mobile app;
  - account name and password, entered by the user and sent only to Steam using Steam's key exchange;
  - Steam Guard by email code, authenticator code, or mobile confirmation.

  The password is never stored or logged. Only the refresh token Steam issues is kept, in secure storage.
- **Secure storage.** On desktop, the Secret Service keyring (libsecret). On Android, a key held in Android Keystore that encrypts the token file. If no secure store is available, the provider does not keep a session and asks the user to sign in each time.
- **Library.** Licenses and app/package metadata (PICS) from the session: owned apps, DLC, depots, branches and launch options.
- **Content.** Downloads depot manifests and chunks from Steam content servers, using the manifest request codes and depot keys Steam issues to this account for apps it owns. Chunks are verified (SHA-1), decrypted and decompressed; installs resume, updates are deltas, verify repairs.
- **Session services for games.** See the bridge below.

### Beer Manager / Android app

UI only:

- sign in and out;
- library, install, update, verify, uninstall and launch;
- per-game Steam mode and Beer settings;
- download progress and errors.

No secrets pass through the UI apart from what the user types into the login prompt, and that is handed straight to the provider.

### In-game bridge (`official` mode)

On Windows, the game's `steam_api64.dll` loads `steamclient64.dll`, which talks to the running Steam client. In Beer:

- The game's own `steam_api64.dll` runs normally as a guest module.
- Beer provides the `steamclient64` interface surface (`CreateInterface` and the versioned `ISteam*` interfaces from the public Steamworks SDK). This is the same layer the offline facade implements today. In `official` mode each call is answered by the provider:
  - user identity and persona;
  - `BIsSubscribedApp` and DLC ownership;
  - `GetAuthSessionTicket`, `GetAuthTicketForWebApi` and `RequestEncryptedAppTicket`, all issued by Steam;
  - Steam Cloud;
  - stats and achievements;
  - friends;
  - later: matchmaking, lobbies and networking.
- Callbacks and call results (`SteamAPI_RunCallbacks` / `ISteamUtils` call results) carry real provider responses.
- An interface or method the provider does not implement yet returns its documented failure value. It never returns success.

The loader gets a per-game, per-launch connection to the provider. It can request services for the running app ID only, and never sees the refresh token.

## Launch modes

| Mode | Use | Steam answers come from |
|---|---|---|
| `official` | Games in the signed-in account's library | Provider session (real) |
| `guest` | Games that ship their own Steam wrapper or emulator | The game's own guest modules |
| `facade` | Offline-compatible games using Beer's implemented surface | Beer's offline facade (existing behaviour) |
| `auto` | Default | `official` if the app is owned by the signed-in account and installed through the provider; otherwise the existing `guest` / `facade` selection |

Selection is policy-based and user-visible. It uses ownership from the session, the install source, and shipped modules. It never uses executable hashes or game-specific RVAs.

## Online play and DRM

- **Ownership checks (Steam DRM, SteamStub).** Satisfied by the real session's ownership and tickets.
- **Denuvo** activates over the network, using the Steam app ticket and its own hardware fingerprint:
  - The ticket is real, so activation can succeed.
  - Anything Beer reports about the hardware (CPUID, GPU identity, disk and volume serials, machine GUIDs) must be **stable between launches**. Otherwise each launch can look like a new machine and use up the daily activation limit. Debugging spoofs (for example the manager's GPU spoof) must stay off for these titles.
- **Multiplayer.** Lobbies and matchmaking go through the session. Peer-to-peer and Steam Datagram Relay networking (`ISteamNetworkingSockets`) is a large separate phase.

## Offline behaviour

- Steam's offline mode needs a previously authenticated session.
- When offline, the provider reports offline. Games that need tickets or online services get Steam's real offline failures.
- No cached ticket is replayed past its validity.

## Provider operations (local API)

Conceptual; not implemented yet.

- `status`: connection, signed-in account (SteamID and persona name only), and secure-storage availability.
- `login-begin` / `login-continue` / `logout`: the authentication flows above, including QR codes, Steam Guard prompts and errors.
- `library`: owned apps with install state.
- `install` / `update` / `verify` / `uninstall`: progress events with states (queued, downloading, verifying, complete, paused, license missing, disk full, network failure, cancelled).
- `launch-plan`: a reviewable descriptor containing:
  - app ID and executable, working directory and launch options from app metadata;
  - Beer prefix and memory cap;
  - the selected Steam mode and the reason;
  - environment variables, none of which carry secrets.
- `launch`: starts Beer with a per-launch session channel.
- Session services used by the loader during a game.

## Failure handling

- **Login required or token expired:** prompt for sign-in. Do not fall back silently.
- **License missing:** show Steam's answer. The game is not offered in `official` mode.
- **Download failure:** keep the content-server error, the depot and manifest IDs, and disk and network state.
- **Provider not running during a game:** session calls fail as Steam does when the client is gone.
- **Game exits before graphics:** classify loader, module, Steam and API failures separately from GPU failures.

## Security rules

- Passwords are typed only into the provider's login prompt, sent only to Steam, and never stored or logged.
- The refresh token lives only in secure storage. Logs redact tokens, tickets, Steam Guard codes and cookies.
- The local socket is user-private (mode 0600, or an Android app-private socket).
- The loader's channel is scoped to one app ID and one launch.
- Nothing is forged: no ownership, licenses, tickets, depot keys or session state. Beer only relays what Steam issued to the signed-in account.
- Loader work follows `docs/windows-loader-compatibility-policy.md`.

## Accepted risk

Steam's connection protocol is not a public API. Third-party clients that use it (SteamKit2, DepotDownloader and the Android launchers) are common, but Valve does not formally sanction them. The protocol can change, and account action is unlikely but possible. Keep the protocol layer isolated, so that protocol changes stay local to it.

## Phase 1 status (2026-11-01)

`beer-steam` (`make beer-steam`, sources in `steam/`) contains:

- `net.c`: TLS (OpenSSL, peer and hostname verified), HTTPS GET, WebSocket client.
- `pb.c`: protobuf encoding and decoding (`make test-steam`).
- `cm.c`: server list, WebSocket CM session on one I/O thread, service calls matched by job ID, gzip'd multi-packets, refresh-token logon, heartbeat.
- `auth.c`: QR sign-in; password sign-in (RSA-encrypted with Steam's key); Steam Guard codes; polling.
- `secret.c`: keyring storage through libsecret loaded at run time, with no build dependency. Steam Guard device data is kept per account after sign-out.
- `provider.c`: the `serve` socket and the `status`, `login-qr`, `login` and `logout` commands. After sign-in it reconnects with backoff, and forgets a token Steam rejects.

Verified against Steam:

- QR sign-in begins and the code rotates.
- The socket protocol works: status, a second sign-in is refused, unknown ops are reported, cancel works.
- The socket is user-only (0600, peer UID checked).
- A keyring save, load and clear round trip works.

Manager side:

- `manager/steam_page.py` adds a Steam tab next to Library, with these views:
  - provider unavailable;
  - connecting;
  - sign-in, by QR code or password;
  - Steam Guard (code entry and/or approval in the app);
  - signed in (persona, account, SteamID, storage, sign out).
- The page starts `beer-steam serve` when no provider is running. The path is the `steam_provider` setting in Loader settings, and the log goes to `~/.beer/logs/beer-steam.log`. The provider keeps running after the manager closes, so the session stays up.
- `manager/qr.py` is a QR encoder (byte mode, level M, versions 1–10) plus a GTK widget. All 123 test strings decoded with OpenCV, including a code rendered on screen.
- `steam_prefix(appid)` creates a `steam-<appid>` prefix for official installs.

Not yet verified (needs a real account): completing QR and password + Steam Guard sign-in, logon with the stored token, persona name.

Socket events, one JSON object per line, broadcast to every client:

- `state`: `state`, `account`, `persona`, `steamid`, `secure_storage`. Sent as the reply to `status` and on every change.
- `qr`: `url`. Sent again whenever Steam rotates the code.
- `guard`: `types`, `hint`, `error`. `error` is set when a submitted code was wrong.
- `login`: `ok`, plus `account` or `error`.
- `warning`: `message`.
- `error`: an unknown op.

Requests: `status`, `login_qr`, `login_password` (with `account` and `password`), `guard_code` (with `code`), `login_cancel`, `logout`.

## Phase 2 status (2026-11-01)

The library works against a real account: 7 licenses resolved to 203 apps in 2.6 s.

- `library.c` follows Steam's chain from licenses to apps:
  1. The `ClientLicenseList` that arrives after logon gives the packages.
  2. PICS package records (binary VDF) give each package's app IDs.
  3. PICS access tokens unlock the app records.
  4. PICS app records (text VDF) give `common/name`, `type` and `oslist`.

  An app owned only through another account's license is marked `shared`.
- Multi-part PICS replies (`response_pending`) are collected by `cm_request`.
- `vdf.c` parses text and binary KeyValues (`make test-steam`).
- The provider resolves the library on a worker thread after every logon. It caches the result in `~/.beer/steam/library-<steamid>.json` (mode 0600), shows the cached list first while refreshing, and removes the cache on sign-out.
- New pieces of the interface:
  - request `library`;
  - event `library`, with `state` (loading, refreshing, ready or error), `error`, and `apps` as `[{appid, name, type, windows, shared}]` sorted by name;
  - terminal command `beer-steam library`.
- The manager's signed-in view:
  - an account header that expands to show the SteamID and storage;
  - the library list with search;
  - games, demos and applications by default, with **Show all** for tools, DLC and configs;
  - per-app notes: type, "No Windows version", "Family shared".
- Not yet handled: app records that Steam serves only over HTTP instead of inline. These are logged as "app records had no inline data"; none appeared in the test account.

## Phase 3 status (2026-11-01)

Tested end to end on a real account with Spacewar (app 480): install, verify and repair, a no-op update, pause and resume, cancel, and uninstall. Also tested from the manager: install dialog → download → Library entry.

Provider:

- **`http.c`:** keep-alive HTTP/1.1 client, plain or TLS. Handles Content-Length, chunked and close-delimited bodies, and reconnects once.
- **`content.c`** handles Steam's content system:
  - Content servers come from `ContentServerDirectory.GetServersForSteamPipe` (only `CDN`/`SteamCache` types; China-only, proxy and app-restricted servers are skipped).
  - Each depot key comes from `ClientGetDepotDecryptionKey`, and the manifest request code from `GetManifestRequestCode`.
  - Manifests (`/depot/<id>/manifest/<gid>/5/<code>`) arrive as PKZip sections. File names are decrypted with Steam's scheme: AES-256 with an ECB-encrypted IV, then CBC with PKCS#7 padding. The ECB step turns padding off, so the CBC step must turn it back on.
  - Chunks (`/depot/<id>/chunk/<sha>`) are decrypted, then decompressed from VZip (LZMA, through liblzma loaded at run time), VSZa (zstd) or PKZip, and every chunk is checked against its SHA-1.
- **`install.c`** is the job queue, one job at a time with 8 workers:
  - **Depot selection:** Windows depots only; 64-bit over 32-bit; the chosen language; no low-violence depots; DLC depots only when owned; shared redistributables skipped; a depot whose key is refused is skipped.
  - **Building:** changed files are built in `beer-steam/staging/<appid>/` and renamed into place on commit. On updates or repair, a chunk that already exists in an installed file (checked by SHA-1) is copied instead of downloaded.
  - **Resume:** `beer-steam/staging/<appid>.done` records finished chunks.
  - **Free space** is checked before downloading.
  - **Bandwidth limit**, averaged over two-second windows.
  - **Verify** hashes every chunk of the installed files and rebuilds only the files that fail.
  - **Removal on update:** files removed from the depot are deleted.
  - **Safety:** manifest paths with `..` or a leading `/` are refused.
  - **Uninstall** deletes only manifest-listed files, so saves and mods stay.
  - The launch option is chosen from `config/launch`, preferring Windows, `default` type and 64-bit. Its executable is matched case-insensitively to the depot's real file name.
- **Install records** are `beer-steam/app_<appid>.vdf`. Plain-name manifests are kept in `beer-steam/depots/` for updates and verify.
- **New socket requests:** `install`, `update`, `verify` (each with `appid`, `folder`, and optional `branch` and `language`), `pause`, `resume`, `cancel`, `uninstall`, `installs` (with `folders`, newline-separated), `downloads`, `bandwidth` (`bytes_per_second`), and `check_updates` (compares build IDs).
- **New events:** `download` (with `state`, `done`, `total`, `rate` and `error`), `installs`, `downloads`, `updates` and `uninstalled`.

Manager:

- Each Steam row shows one of:
  - Install (a dialog with folder and free space);
  - a progress bar with size, speed and time left, plus pause/resume and cancel;
  - Failed, with the reason, Retry and dismiss;
  - Installed, with Update when a newer build exists, and a menu for Open folder, Verify files and Uninstall.
- Finished installs appear under **Steam Games** in the Library. Each gets its own `steam-<appid>` prefix, and `SteamAppId`/`SteamGameId` are set at launch. An entry is removed when its install record is gone, but only if the library folder itself is present (so an unmounted drive does not remove entries).
- Loader settings gained **Steam game language** and **Steam download limit**.

Loader: `beer_steam_app_id_from_install` now falls back to `SteamAppId` from the environment, as Steam sets it for games it launches. The RE8 gate passes.

Not done yet:

- choosing a beta branch in the UI (the provider already accepts `branch`);
- password-protected betas;
- moving an install to another folder;
- the launch option's `workingdir`;
- app records Steam serves only over HTTP.

The in-game `official` mode (real tickets, cloud, achievements) is phase 4.

## Phase 4 status (2026-11-01, first part)

Built and unit-tested. Not yet live-tested: the running provider was busy with a 21 GB download and was not restarted.

- **Provider requests**, answered only when signed in and when the account owns the app:
  - `game_session` returns the SteamID, account, persona and owned app IDs.
  - `app_ticket` returns the Steam-signed app ownership ticket (`ClientGetAppOwnershipTicket`, 857).
  - `encrypted_ticket` (with `userdata` in hex) returns the serialized `EncryptedAppTicket` (`ClientRequestEncryptedAppTicket`, 5526).

  Tickets travel as hex.
- **Loader: `BEER_STEAM_API_MODE=official`.** [platform/beer_steam_session.c](../platform/beer_steam_session.c) is the provider client. It matches replies by request ID and skips broadcast events (`make test-steam` covers it against a fake provider).
  - `SteamAPI_Init` returns true only when the provider confirms an owning, signed-in session. Otherwise it returns false, as Windows does without Steam.
  - SteamID, `BLoggedOn`, persona name and ownership (`BIsSubscribedApp`, DLC) come from the session. In this mode the emulator files next to the game are ignored.
  - `ISteamAppTicket::GetAppOwnershipTicketData` returns the real signed ticket, with the signature offset taken from the ticket's own length field.
  - Each `SteamUser0NN` version gets its own vtable. `RequestEncryptedAppTicket` (call result 154), `GetEncryptedAppTicket` and `UserHasLicenseForApp` sit at that version's slots (022 and later, or 017–021).
- **Manager:** each game's settings have a **Steam API** choice:
  - **Automatic:** official for Steam installs, offline Beer otherwise.
  - **Beer offline Steam:** `compat`.
  - **Your Steam session:** `official`; this also works for local copies.
  - **Game's own Steam DLL:** `guest`.

  An optional **Steam App ID** goes into `SteamAppId`. In official mode, `SteamAppId` takes precedence over `steam_appid.txt` and emulator config, so ownership is checked for the game the user means. The provider refuses tickets for apps the account does not own.
- **`compat` and `guest` are unchanged.** RE8 gate: same local SteamID, BD12 ready, no fatal error.

Next:
- live verification once the provider can restart;
- auth session tickets: GC tokens (`ClientGameConnectTokens`, 779), the session header and ownership ticket, and `ClientAuthList` (5432) registration;
- Steam Cloud, stats and achievements in official mode.

## Gap fixes (2026-11-01)

| Gap | Now | Verified |
|---|---|---|
| Launch `workingdir` | The manager sets `BEER_WORKING_DIR` from the launch option. The loader changes into it after the exe's folder (the `CreateProcess` current-directory contract). | Build only |
| Beta branches | `app_branches` lists public and beta branches (with build, description, and whether a password is needed). The install dialog has **Version**; installed games have **Change version…**, which runs an update on the chosen branch. | UI rendered |
| Password betas | `beta_password` checks the password with `ClientCheckAppBetaPassword` (5450). The branch keys are kept in the keyring (`beta`, `<appid>/<branch>`). The manifest ID comes from `encryptedmanifests/<branch>/encrypted_gid_2` (AES-256-ECB with the key, first 8 bytes). | Build only; needs a game with a password beta |
| Moving an install | The `move` job (in the queue, with progress) renames on the same filesystem. Across filesystems it copies everything, then deletes the source. It also moves the record and manifests. In the manager: **Move to another folder…**. | Live: tmpfs → tmpfs → home drive → tmpfs, SHA-1 unchanged |
| HTTP-served app records | When a PICS reply carries `http_host` and a record has only a SHA, the provider fetches `https://<host>/appinfo/<id>/sha/<sha>.txt.gz` and gunzips it. | Build only; no such app in the test account |
| Sign-out | `Authentication.RevokeToken` (logout action) is sent before the session is forgotten. | Build only |
| Auth session tickets | Provider: game-connect tokens (779) are collected. A ticket is `[n][GC token]`, the 24-byte session header (public IP and time since logon), then `[n][signed ownership ticket]`. The token part and header are registered with `ClientAuthList` (5432), using a CRC32. Loader: `GetAuthSessionTicket` posts `GetAuthSessionTicketResponse_t` (163); `CancelAuthTicket` unregisters. `BeginAuthSession` honestly returns "invalid ticket", because validating other players' tickets is not implemented. `GetAuthTicketForWebApi` returns the invalid handle. | Build only; validity can only be checked by a game server |
| Broadcast callbacks | `SteamAPI_RegisterCallback`/`UnregisterCallback` now keep listeners, mark the object registered as `steam_api` does, and deliver posted events from `SteamAPI_RunCallbacks`. | Build only |

Remaining: Steam Cloud, stats and achievements in official mode; Web API tickets; validating other players' tickets.

## Implementation phases

1. Provider skeleton:
   - connection, QR and password + Steam Guard login, secure token storage on both platforms;
   - `status` / `logout`;
   - manager login page.
2. Library from licenses and PICS; manager library view and Steam downloads folder list.
3. Depot download, update and verify, with resume.
4. `official` launch mode. The facade's interface layer gains a provider backend for:
   - identity;
   - ownership and DLC;
   - auth session tickets and encrypted app tickets.

   Gate: a Steam-DRM title and a ticket-checking title run from a legitimate install.
5. Cloud, stats and achievements, friends and persona.
6. Matchmaking and lobbies.
7. Steam networking sockets and relay.

## Acceptance criteria for phase 1–4

- Sign-in works with QR code and with password + Steam Guard, on desktop and Android. Restarting the provider keeps the session through secure storage, and no secret appears in logs or on disk in plain text.
- The library matches the account's owned apps.
- Downloads are verified, resumable, and pass `verify` afterwards.
- `official` titles receive real tickets. Unowned apps are refused.
- `guest` and `facade` titles behave exactly as before. The RE8 gate passes.
- Unit tests cover:
  - protocol message encoding;
  - login state transitions;
  - manifest and chunk parsing and verification;
  - mode selection;
  - log redaction.

## Steam library folders

- The manager has a **Steam downloads** setting holding a list of one or more library folders. A default folder is created under the Beer home directory.
- The user picks the target folder for each install. Each folder keeps the provider's own install records (app ID, depots, manifest IDs, branch, install state), so a folder on a removable or late-mounted drive is picked up again when it reappears.
- Removing a folder from the list never deletes the games in it.
- Done: the folder list is in the manager's Loader settings as `steam_libraries` in `manager.json`. The default is `~/.beer/steam-library`.

## Manager library sections

The Library page has two headed sections:

- **Local Imports:** games added by hand from an `.exe`. This is the default for existing entries.
- **Steam Games:** games installed by the provider.

Provider installs are marked in `library.json` with `"source": "steam"` and `"appid"`. The row subtitle shows the app ID. Removing an entry still leaves the files on disk; uninstalling goes through the provider.

Done in the manager, but nothing produces `source: "steam"` entries until the provider exists. Steam entries do not set `BEER_STEAM_API_MODE=official` yet, because the loader currently rejects any value other than `compat` and `guest`.

## Official and unofficial copies of the same game

These are kept apart: separate prefixes and save locations. Steam Cloud therefore never syncs saves written by an unofficial copy.

## Open questions

- **TLS and protobuf dependencies.**
  - On desktop, Beer already uses OpenSSL's libcrypto (bcrypt hashes and HMAC, crypt32 Authenticode) together with zlib.
  - The provider also needs OpenSSL's libssl, for TLS to Steam's WebSocket and HTTPS endpoints.
  - The Android build currently has no OpenSSL at all (crypt32 reports "built without OpenSSL"). It would need an ARM64 OpenSSL built and bundled into the APK.
  - Protobuf: protobuf-c, or a small hand-written encoder.
