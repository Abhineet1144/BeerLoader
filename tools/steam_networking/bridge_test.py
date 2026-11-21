"""Offline negative tests for the first-party GNS authentication bridge.

Set BEER_STEAM_NETWORKING_LIBRARY and optionally BEER_STEAM_CONFIG_FIXTURE to a
previously HTTPS-fetched Valve config. Each case uses its own process/trust store.
No account session, ticket or private key is needed.
"""
import base64
import ctypes as c
import json
import os
import subprocess
import sys


def case(name):
    lib = c.CDLL(os.environ["BEER_STEAM_NETWORKING_LIBRARY"])
    assert lib.BeerGNS_AuthBridgeVersion() == 1
    lib.GameNetworkingSockets_Init.argtypes = [c.c_void_p, c.c_void_p]
    lib.GameNetworkingSockets_Init.restype = c.c_bool
    lib.SteamNetworkingSockets_LibV12.restype = c.c_void_p
    lib.BeerGNS_ConfigureSteam.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_int, c.c_void_p]
    lib.BeerGNS_ConfigureSteam.restype = c.c_bool
    lib.BeerGNS_SetSteamCertificate.argtypes = lib.BeerGNS_ConfigureSteam.argtypes
    lib.BeerGNS_SetSteamCertificate.restype = c.c_bool
    identity = (c.c_ubyte * 136)()
    identity[0] = 16
    identity[4] = 8
    for i, byte in enumerate((76561198000000011).to_bytes(8, "little")):
        identity[8 + i] = byte
    error = c.create_string_buffer(1024)
    assert lib.GameNetworkingSockets_Init(identity, error), error.value
    sockets = lib.SteamNetworkingSockets_LibV12()

    def configure(config, app=480):
        blob = config if isinstance(config, bytes) else json.dumps(config, separators=(",", ":")).encode()
        return lib.BeerGNS_ConfigureSteam(sockets, app, blob, len(blob), error)

    if name == "malformed":
        for config in (b"{", b"[]", b'{}', b'{"success":true,"certs":[],"revoked_keys":[]}',
                       b'{"success":true,"certs":["abc"],"revoked_keys":[]}',
                       b'{"success":true,"certs":[12],"revoked_keys":[]}'):
            assert not configure(config)
        assert not lib.BeerGNS_SetSteamCertificate(sockets, 480, b"bad", 3, error)
        print("malformed inputs rejected")
        return
    with open(os.environ["BEER_STEAM_CONFIG_FIXTURE"], encoding="utf-8") as source:
        config = json.load(source)
    if name == "valid":
        assert configure(config), error.value
        assert configure(config), error.value  # same app refresh is allowed
        assert not configure(config, 481)  # an instance cannot silently change apps
        assert not lib.BeerGNS_SetSteamCertificate(sockets, 481, b"bad", 3, error)
    elif name == "tampered":
        damaged = bytearray(base64.b64decode(config["certs"][0]))
        damaged[-1] ^= 1
        config["certs"][0] = base64.b64encode(damaged).decode()
        assert not configure(config), "tampered CA signature accepted"
        # Previously imported untrusted data must not become an alternate trusted root.
        assert not lib.BeerGNS_SetSteamCertificate(sockets, 480, b"bad", 3, error)
    elif name == "revoked":
        # Public key ID of the main intermediate, computed from the public CA certificate.
        blob = base64.b64decode(config["certs"][0])
        import hashlib
        # The cert begins at field 4 (length-delimited); parse the tiny public envelope.
        def fields(data):
            position = 0
            def varint():
                nonlocal position
                value = shift = 0
                while True:
                    byte = data[position]
                    position += 1
                    value |= (byte & 127) << shift
                    if byte < 128:
                        return value
                    shift += 7
            result = {}
            while position < len(data):
                tag = varint()
                wire = tag & 7
                if wire == 0:
                    value = varint()
                elif wire == 2:
                    length = varint()
                    value = data[position:position + length]
                    position += length
                else:
                    length = {1: 8, 5: 4}[wire]
                    value = data[position:position + length]
                    position += length
                result[tag >> 3] = value
            return result
        key = fields(fields(blob)[4])[2]
        key_id = int.from_bytes(hashlib.sha256(key).digest()[:8], "little")
        config["revoked_keys"].append(str(key_id))
        assert not configure(config), "revoked CA accepted"
    else:
        raise AssertionError(name)
    print(name, "PASS")


if __name__ == "__main__":
    if len(sys.argv) == 2:
        case(sys.argv[1])
    else:
        cases = ["malformed"]
        if os.environ.get("BEER_STEAM_CONFIG_FIXTURE"):
            cases += ["valid", "tampered", "revoked"]
        for name in cases:
            subprocess.run([sys.executable, __file__, name], check=True)
        print("Steam authentication bridge tests passed")