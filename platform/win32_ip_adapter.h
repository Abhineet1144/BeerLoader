#ifndef BEER_WIN32_IP_ADAPTER_H
#define BEER_WIN32_IP_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

/* Windows 10 x64 IP Helper layouts. Keep these pointer fields as uint64_t so
 * the contract can be tested on the Linux host without importing Windows SDK
 * headers. */
typedef struct BeerSocketAddress {
    uint64_t sockaddr;
    int32_t length;
    uint32_t padding;
} BeerSocketAddress;

typedef struct BeerIpAdapterUnicastAddress {
    uint32_t length;
    uint32_t flags;
    uint64_t next;
    BeerSocketAddress address;
    uint32_t prefix_origin;
    uint32_t suffix_origin;
    uint32_t dad_state;
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t lease_lifetime;
    uint8_t on_link_prefix_length;
    uint8_t padding[7];
} BeerIpAdapterUnicastAddress;

typedef struct BeerIpAdapterAddresses {
    uint32_t length;
    uint32_t if_index;
    uint64_t next;
    uint64_t adapter_name;
    uint64_t first_unicast_address;
    uint64_t first_anycast_address;
    uint64_t first_multicast_address;
    uint64_t first_dns_server_address;
    uint64_t dns_suffix;
    uint64_t description;
    uint64_t friendly_name;
    uint8_t physical_address[8];
    uint32_t physical_address_length;
    uint32_t flags;
    uint32_t mtu;
    uint32_t if_type;
    uint32_t oper_status;
    uint32_t ipv6_if_index;
    uint32_t zone_indices[16];
    uint64_t first_prefix;
    uint64_t transmit_link_speed;
    uint64_t receive_link_speed;
    uint64_t first_wins_server_address;
    uint64_t first_gateway_address;
    uint32_t ipv4_metric;
    uint32_t ipv6_metric;
    uint64_t luid;
    BeerSocketAddress dhcpv4_server;
    uint32_t compartment_id;
    uint8_t network_guid[16];
    uint32_t connection_type;
    uint32_t tunnel_type;
    uint8_t dhcpv6_server[28];
    uint8_t dhcpv6_client_duid[130];
    uint16_t dhcpv6_client_duid_padding;
    uint32_t dhcpv6_client_duid_length;
    uint32_t dhcpv6_iaid;
    uint64_t first_dns_suffix;
} BeerIpAdapterAddresses;

enum {
    BEER_IF_TYPE_ETHERNET_CSMACD = 6,
    BEER_IF_TYPE_IEEE80211 = 71,
    BEER_IF_OPER_STATUS_UP = 1,
    BEER_IP_PREFIX_ORIGIN_DHCP = 3,
    BEER_IP_SUFFIX_ORIGIN_DHCP = 3,
    BEER_IP_DAD_STATE_PREFERRED = 4,
};

_Static_assert(sizeof(BeerSocketAddress) == 16, "SOCKET_ADDRESS x64 size");
_Static_assert(sizeof(BeerIpAdapterUnicastAddress) == 64,
               "IP_ADAPTER_UNICAST_ADDRESS_LH x64 size");
_Static_assert(offsetof(BeerIpAdapterAddresses, first_unicast_address) == 24,
               "IP_ADAPTER_ADDRESSES FirstUnicastAddress offset");
_Static_assert(offsetof(BeerIpAdapterAddresses, physical_address) == 80,
               "IP_ADAPTER_ADDRESSES PhysicalAddress offset");
_Static_assert(offsetof(BeerIpAdapterAddresses, mtu) == 96,
               "IP_ADAPTER_ADDRESSES Mtu offset");
_Static_assert(offsetof(BeerIpAdapterAddresses, oper_status) == 104,
               "IP_ADAPTER_ADDRESSES OperStatus offset");
_Static_assert(offsetof(BeerIpAdapterAddresses, first_prefix) == 176,
               "IP_ADAPTER_ADDRESSES FirstPrefix offset");
_Static_assert(sizeof(BeerIpAdapterAddresses) == 456,
               "IP_ADAPTER_ADDRESSES_LH x64 size");

#endif
