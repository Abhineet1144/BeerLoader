# Steam Library Broker Design

## Status

Design document only. No Steam download, authentication, ownership, or runtime-broker implementation exists yet.

## Objective

Add a GameHub-style library and launcher experience around Beer while preserving a strict trust boundary:

- Users authenticate with an official Steam client or SteamCMD installation.
- Official Steam software performs account authentication, ownership checks, depot downloads, updates, and content decryption.
- Beer discovers installed applications, stores per-game compatibility profiles, and launches Windows executables.
- Beer does not forge licenses, ownership tickets, download tokens, or authenticated Steam sessions.
- Steam binaries are not copied into or embedded in the Beer loader.

The broker is a separate launcher-side process. The Beer loader remains a Windows compatibility runtime and does not become a Steam client implementation.

## Components

### Beer Manager

The existing `manager/beer_manager.py` UI remains the user-facing library and settings application. A future Steam page may provide:

- Steam installation and login status.
- Owned/installed game discovery.
- Download, update, verify, launch, and open-install-folder actions.
- Per-title Beer prefix, executable, environment, graphics, memory-cap, and Steam-runtime settings.
- Logs showing which Steam mode was selected and why.

### Steam broker

A separate process controls official Steam or SteamCMD through supported command-line/process behavior. It must:

- Never request a Steam password through Beer UI.
- Never read or copy Steam credential files.
- Keep secrets in Steam's own credential storage.
- Report process/readiness state to Beer Manager.
- Delegate downloads and updates to official Steam software.
- Keep Steam running when a game requires authenticated Steamworks services.

### Library scanner

The scanner reads metadata only:

- Steam library roots from `libraryfolders.vdf`.
- Installed application state from `appmanifest_<appid>.acf`.
- Executable candidates from application manifests and Beer profiles.
- Download/update state exposed by official Steam metadata.

Malformed or inaccessible files must produce a visible, non-fatal diagnostic. Discovery must not modify a Steam library.

### Launch planner

The launch planner selects one of these modes per game:

| Mode | Intended use | Runtime source |
|---|---|---|
| `facade` | Offline-compatible games that need only Beer's implemented Steam surface | Beer-owned compatibility implementation |
| `guest` | Games shipping an application-local Steam wrapper or emulator that should execute normally | Game-shipped guest PE modules |
| `official` | Games requiring authenticated ownership tickets, callbacks, lobbies, networking, achievements, or other live Steamworks services | Running official Steam session plus a future supported bridge |
| `auto` | Default policy | Select from manifest, shipped modules, prior diagnostics, and a user override |

Selection must be policy-based. It must not depend on executable hashes, game-specific RVAs, or forged API results.

## Proposed broker operations

These are conceptual operations, not implemented endpoints.

### `status`

Returns:

- Whether official Steam/SteamCMD is installed.
- Whether the process is running and ready.
- Whether a user is logged in, when that state can be determined without accessing credentials.
- Discovered library roots.

Failure examples:

- Steam executable not found.
- Steam is updating and not ready.
- User interaction/login required.
- Metadata is malformed or inaccessible.

### `list-installed`

Returns app ID, title, install directory, manifest state, update state, and executable candidates derived from official metadata.

It must not claim ownership based only on an arbitrary local directory or `steam_appid.txt`.

### `install` / `update` / `verify`

Delegates the requested operation to official Steam or SteamCMD. Beer records process output and final status but does not download depot content itself.

Expected states include queued, downloading, verifying, complete, login required, license missing, disk full, network failure, and cancelled.

### `launch-plan`

Produces a reviewable descriptor containing:

- App ID and executable.
- Working directory.
- Beer prefix and memory cap.
- Selected Steam mode and reason.
- Required official Steam readiness.
- Environment variables that contain no credentials.
- Compatibility warnings and pending regression gates.

### `launch`

Checks the plan, starts or verifies official Steam when required, and then starts Beer. The broker must keep any required official process alive for the game session.

## Official-session limitation

Downloading and launching through official Steam can be integrated without reimplementing authentication. Authenticated Steamworks runtime support is a separate problem. Many games communicate with Steam through private IPC and expect signed ownership/session tickets. Beer must not fabricate these.

Until a lawful, maintainable official-session bridge exists, `official` mode may support installation and process orchestration while reporting that authenticated in-game Steamworks functionality is unavailable. Games that validate signed tickets must fail clearly rather than receiving fake success.

## Failure handling

- **Not installed:** offer Steam discovery/configuration; do not silently substitute a foreign runtime file.
- **Login required:** open official Steam and wait for user action.
- **License missing:** show the official failure; do not retry with a fabricated ticket.
- **Download failed:** preserve official output, exit code, and disk/network diagnostics.
- **Manifest missing:** rescan configured libraries and allow manual executable import without claiming ownership.
- **Steam runtime unavailable:** allow `facade` or `guest` only when the selected game profile supports it.
- **Game exits before graphics:** classify loader, module, Steam, and API failures separately from GPU failures.
- **Beer regression:** retain the log, command, selected policy, app ID, and memory-cap/OOM events.

## Troubleshooting checklist

1. Confirm the app ID and install directory from `appmanifest_<appid>.acf`.
2. Confirm official Steam is logged in and the title is owned when `official` mode is selected.
3. Confirm the executable and working directory in the generated launch plan.
4. Check whether the title ships `steam_api64.dll` or another local wrapper and whether `guest` mode is intentional.
5. Check whether Beer loaded a host Steam DLL directly; this is not allowed as an implicit loader dependency.
6. Distinguish signed-ticket rejection from missing app-ID metadata.
7. Inspect the Beer log before changing runtime policy.
8. Run under a memory cgroup and record `oom_kill` events for large titles.
9. Keep per-title overrides user-visible and reversible.

## Security and dependency rules

- No credential collection or storage by Beer.
- No ownership, license, session-ticket, or depot-key forgery.
- No direct depot download implementation in Beer.
- No copying or embedding Steam, Wine, Proton, or Windows runtime files in Beer.
- Official tools remain separate processes selected by the user.
- The Beer loader may use only project-approved runtime dependencies; the shader translator remains the existing exception.
- Loader and module-policy work must follow `docs/windows-loader-compatibility-policy.md`: emulate reusable Windows contracts, never executable-specific patches or entitlement bypasses.
- Logs must redact command-line secrets and authentication tokens.

## Implementation phases

1. Read-only Steam installation/library/manifest discovery.
2. Launch-plan generation and per-game Steam-mode UI.
3. Official Steam process readiness and launch orchestration.
4. Delegated install/update/verify operations.
5. Compatibility reporting and troubleshooting UI.
6. Investigate authenticated runtime integration separately; do not block the safe library-management phases on it.

## Acceptance criteria for a first implementation

- Lists installed games without modifying Steam data.
- Generates a deterministic, inspectable launch plan.
- Delegates downloads to official Steam software.
- Never receives a Steam password or fabricates ownership.
- Preserves Beer's standalone `facade` and application-local `guest` modes.
- Reports unsupported authenticated runtime requirements explicitly.
- Includes unit tests for VDF/ACF parsing, mode selection, malformed metadata, and command construction.
