# Steam-authenticated networking

Beer obtains networking identity and certificates from its signed-in Steam provider,
not from a synthetic Steam ID, local ownership override or unsigned ticket.

## Runtime bridge

[The first-party build wrapper](../tools/steam_networking/CMakeLists.txt) compiles
[the authentication extension](../tools/steam_networking/steam_auth_bridge.cpp) into
an **external** Valve GameNetworkingSockets checkout. No Valve source is copied into Beer.
Verified upstream revision: d534c19aa760df3fb75fd20db13ba1932b8a5463.

The extension has a versioned, narrow C ABI:

- `BeerGNS_AuthBridgeVersion`: ABI version 1.
- `BeerGNS_ConfigureSteam`: parse Valve's HTTPS GetSDRConfig response, initialize the
  built-in Valve root **before** revocation entries, import signed CA certificates,
  verify each chain, apply revocations and set the app identity. Reconfiguration is
  allowed only for the same app; initial setup precedes connections/listeners.
- `BeerGNS_SetSteamCertificate`: validate the signature envelope, Valve CA chain,
  revocations, validity, app, identity and matching public/private key before
  invoking GNS's certificate installation. Caller-supplied private keys are rejected.

The provider restricts both operations to owned apps on the signed-in session. It
fetches configuration only from api.steampowered.com over hostname-verified TLS.
Only a 32-byte Ed25519 **public** key is sent in EMsg 5621. The instance's private
key stays inside GNS. Steam's signed reply is forwarded unchanged.

The adapter configures and authenticates before creating P2P connections. A separate
worker renews certificates five minutes before expiry, retries failures no more
than once a minute, and refreshes trust configuration hourly. It never performs
these network requests under GNS's global lock or on the ICE signaling worker.
Authentication availability and callback 1222 come from the runtime's real status;
the guest callback runs only from the game's callback pump. Authenticated operation
requires peer certificates, including localhost. New peer connections and listeners
are refused until authentication is Current; certificate failure must not produce
a self-signed connection request. Stock unextended libraries retain
the previous custom-peer backend; they cannot claim Steam authentication.

## Build and deployment

[The build/install script](../tools/build_steam_networking.sh) accepts an external
checkout through `GNS_SOURCE_DIR`, optional `GNS_BUILD_DIR` and `GNS_INSTALL_DIR`,
and additional CMake arguments. Dependencies: C/C++17 toolchain, CMake, OpenSSL
development files, matching protobuf headers/runtime/protoc, json-c development
files and Python 3. Dependencies can be extracted into user-owned directories;
the script does not require or invoke elevated privileges.

Native ICE is enabled; Google's WebRTC backend and SDR are not. Installation backs
up the previous runtime and atomically replaces its inode so existing running games
retain their old mapping safely. The manager's saved networking path is unchanged.
Restart the game to load a newly installed runtime.

## Validation and limits

- [Offline bridge tests](../tools/steam_networking/bridge_test.py): malformed JSON,
  wrong app, altered CA signatures and revoked CA keys. Optional
  `BEER_STEAM_CONFIG_FIXTURE` is a previously HTTPS-fetched public Valve configuration.
- [Real-session authentication test](../tests/steam_networking_auth_live_test.c):
  actual certificate installation, Current availability/callback, altered signature,
  wrong app and independently generated, correctly Steam-signed wrong-key controls.
  Explicit `make test-steam-auth-live`; it contacts no other player.
- `make test-steam`: existing two-process reliable P2P, listener lifetime, resolver,
  identity, protobuf, matchmaking and provider IPC tests; an offline extended-runtime
  fixture verifies certificate/config refusal cannot fall back to self-signed connections.

**Authentication success alone does not prove Windows multiplayer interoperability.**
A real public-session join is now user-confirmed: the live game reached Connected,
with end-to-end IPv6 ICE connectivity and a delivered connection callback. The
remote host's operating system was not independently confirmed. This does not
establish compatibility with every Windows version or NAT topology, nor a long soak.
**Valve SDR transport is not implemented** in this open-source build;
hosts relying on relays/IP privacy are not covered by direct ICE support. Relay
availability remains CannotTry, not a fabricated success.

One real public Windows-style host replied with ConnectOK and ICE candidates,
then closed the connection with rendezvous timeout 5008. Local host/reflexive
candidates are gathered; this is not proof that NAT traversal or data exchange works.
Lobby browsing retains Steam's advertised player count without granting membership.
Relay ping APIs report unavailable using correct Win64 return conventions (including
the float-return local-location age); no placeholder is reported as a measurement.

### Session-browser ping: confirmed missing implementation

The Chained Together browser's 9999 remains unfixed. A separate refresh trace confirms
that host `P2PPING` lobby metadata is present and `ParsePingLocationString` is called,
but parsing fails. The game therefore does not reach a ping-estimation call. Both
Beer's fallback and the external non-SDR GNS runtime lack the required parser and
relay measurement/estimation implementation. This is independent of successful
direct IPv6 joining and of the real RTT available on an established connection.

`BEER_STEAM_DIAGNOSTICS` now identifies ping methods once per process and records
ping-key presence during lobby reads/enumeration. A proper fix needs compatible
ping-location decoding, actual local relay measurements and a supported estimator;
fixed numbers or CM-request duration would not be valid browser ping estimates.