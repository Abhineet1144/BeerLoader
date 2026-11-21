/* First-party extension compiled into the external Valve GNS runtime. Its own certificate store
 * verifies every signature back to Valve's built-in root. No alternate roots or unsigned certs. */
#include "clientlib/csteamnetworkingsockets.h"
#include "steamnetworkingsockets_certstore.h"
#include <json-c/json.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace SteamNetworkingSocketsLib;
#ifndef STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY
#error "Steam authentication requires the runtime's pinned Valve root CA"
#endif
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

static bool fail(SteamNetworkingErrMsg &error, const char *message)
{
    V_strcpy_safe(error, message);
    return false;
}

extern "C" __attribute__((visibility("default"))) unsigned BeerGNS_AuthBridgeVersion()
{ return 1; }

extern "C" __attribute__((visibility("default"))) bool BeerGNS_ConfigureSteam(
    ISteamNetworkingSockets *iface, uint32 appid, const void *json, int size, SteamNetworkingErrMsg &error)
{
    if (!iface || !appid || !json || size <= 0 || size > 1024 * 1024)
        return fail(error, "invalid Steam networking configuration");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new(), json_tokener_free);
    if (!parser) return fail(error, "out of memory parsing Steam configuration");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT);
    Json config(json_tokener_parse_ex(parser.get(), static_cast<const char *>(json), size), json_object_put);
    if (!config || json_tokener_get_error(parser.get()) != json_tokener_success ||
        json_tokener_get_parse_end(parser.get()) != static_cast<size_t>(size) ||
        !json_object_is_type(config.get(), json_type_object))
        return fail(error, "malformed Steam configuration JSON");
    json_object *success = nullptr, *certs = nullptr, *revoked = nullptr;
    if (!json_object_object_get_ex(config.get(), "success", &success) ||
        !json_object_is_type(success, json_type_boolean) || !json_object_get_boolean(success) ||
        !json_object_object_get_ex(config.get(), "certs", &certs) || !json_object_is_type(certs, json_type_array) ||
        !json_object_object_get_ex(config.get(), "revoked_keys", &revoked) || !json_object_is_type(revoked, json_type_array))
        return fail(error, "Steam configuration lacks certificates or revocations");
    size_t count = json_object_array_length(certs), revoked_count = json_object_array_length(revoked);
    if (!count || count > 128 || revoked_count > 4096) return fail(error, "invalid Steam trust-chain count");
    std::vector<std::string> blobs;
    std::vector<uint64> key_ids, revocations;
    /* Validate the complete input before changing the shared store. */
    for (size_t i = 0; i < count; ++i) {
        json_object *item = json_object_array_get_idx(certs, i);
        if (!json_object_is_type(item, json_type_string)) return fail(error, "CA certificate is not a string");
        const char *blob = json_object_get_string(item);
        int length = json_object_get_string_len(item);
        if (length <= 0 || length > 16384 || std::strlen(blob) != static_cast<size_t>(length))
            return fail(error, "invalid CA certificate encoding");
        CMsgSteamDatagramCertificateSigned signed_cert;
        CMsgSteamDatagramCertificate cert;
        if (!ParseCertFromBase64(blob, length, signed_cert, error) || signed_cert.has_private_key_data() ||
            !cert.ParseFromString(signed_cert.cert()) || cert.key_type() != CMsgSteamDatagramCertificate::ED25519 ||
            cert.key_data().size() != 32 || !signed_cert.ca_key_id() || signed_cert.ca_signature().size() != 64)
            return fail(error, "invalid signed CA certificate");
        blobs.emplace_back(blob, length);
        key_ids.push_back(CalculatePublicKeyID_Ed25519(cert.key_data().data(), cert.key_data().size()));
    }
    for (size_t i = 0; i < revoked_count; ++i) {
        json_object *item = json_object_array_get_idx(revoked, i);
        if (!json_object_is_type(item, json_type_string)) return fail(error, "revoked key ID is not a decimal string");
        const char *value = json_object_get_string(item);
        int length = json_object_get_string_len(item);
        if (length <= 0 || length > 20 || std::strspn(value, "0123456789") != static_cast<size_t>(length))
            return fail(error, "invalid revoked key ID");
        errno = 0;
        unsigned long long id = std::strtoull(value, nullptr, 10);
        if (errno || !id) return fail(error, "revoked key ID overflow");
        revocations.push_back(static_cast<uint64>(id));
    }
    SteamNetworkingGlobalLock lock("BeerGNS_ConfigureSteam");
    auto *sockets = static_cast<CSteamNetworkingSockets *>(iface);
    auto *utils = sockets->m_pSteamNetworkingUtils;
    if (utils->GetAppID() && utils->GetAppID() != appid)
        return fail(error, "cannot change a networking instance's Steam app ID");
    if (!utils->GetAppID() && (sockets->BHasAnyConnections() || sockets->BHasAnyListenSockets()))
        return fail(error, "Steam authentication must be configured before opening sockets");
    /* GNS inserts its hardcoded root only while the store is empty. Revocations can create
     * entries themselves; initialize and verify the pinned root BEFORE applying any of them. */
    CECSigningPublicKey root;
    if (!root.SetFromOpenSSHAuthorizedKeys(STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY,
                                          sizeof(STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY)))
        return fail(error, "invalid pinned Valve root CA");
    const time_t now = utils->GetTimeSecure();
    if (!CertStore_CheckPublicKey(CalculatePublicKeyID(root), now, error)) return false;
    for (uint64 id : revocations) CertStore_AddKeyRevocation(id);
    for (const auto &blob : blobs)
        if (!CertStore_AddCertFromBase64(blob.c_str(), error)) return false;
    for (uint64 id : key_ids)
        if (!CertStore_CheckPublicKey(id, now, error)) return false;
    utils->SetAppID(appid);
    return true;
}

extern "C" __attribute__((visibility("default"))) bool BeerGNS_SetSteamCertificate(
    ISteamNetworkingSockets *iface, uint32 appid, const void *blob, int size, SteamNetworkingErrMsg &error)
{
    if (!iface || !appid || !blob || size <= 0 || size > 8192)
        return fail(error, "invalid signed Steam certificate");
    SteamNetworkingGlobalLock lock("BeerGNS_SetSteamCertificate");
    auto *sockets = static_cast<CSteamNetworkingSockets *>(iface);
    auto *utils = sockets->m_pSteamNetworkingUtils;
    if (utils->GetAppID() != appid) return fail(error, "Steam app identity is not configured");
    CMsgSteamDatagramCertificateSigned signed_cert;
    CMsgSteamDatagramCertificate cert;
    if (!signed_cert.ParseFromArray(blob, size) || signed_cert.has_private_key_data() ||
        !signed_cert.ca_key_id() || signed_cert.ca_signature().size() != 64)
        return fail(error, "invalid certificate signature envelope");
    const time_t now = utils->GetTimeSecure();
    const CertAuthScope *scope = CertStore_CheckCert(signed_cert, cert, now, error);
    if (!scope || !CheckCertAppID(cert, scope, appid, error)) return false;
    if (cert.time_expiry() <= now || cert.time_created() > now + 300)
        return fail(error, "Steam certificate is not currently valid");
    SteamNetworkingIdentity identity;
    if (SteamNetworkingIdentityFromCert(identity, cert, error) <= 0 || !(identity == sockets->InternalGetIdentity()))
        return fail(error, "Steam certificate identity does not match this signed-in instance");
    if (!sockets->m_keyPrivateKey.IsValid() || cert.key_data().size() != 32 ||
        std::memcmp(cert.key_data().data(), sockets->m_keyPrivateKey.GetPublicKeyRawData(), 32))
        return fail(error, "Steam certificate public key does not match the instance's private key");
    /* This path is now safe: signature, chain, revocations, validity, identity, app and key all checked. */
    return sockets->SetCertificate(blob, size, error);
}