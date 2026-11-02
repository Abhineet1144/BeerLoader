#ifndef BEER_WIN32_ADAPTER_INFO_H
#define BEER_WIN32_ADAPTER_INFO_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>

typedef struct {
    uint64_t next;
    char address[16], mask[16];
    uint32_t context, padding;
} BeerIpAddrString64;
typedef struct {
    uint64_t next;
    uint32_t combo_index;
    char name[260], description[132];
    uint32_t address_length;
    uint8_t address[8];
    uint32_t index, type, dhcp_enabled, padding;
    uint64_t current_ip_address;
    BeerIpAddrString64 ips, gateways, dhcp_server;
    uint32_t have_wins, padding2;
    BeerIpAddrString64 primary_wins, secondary_wins;
    int64_t lease_obtained, lease_expires;
} BeerIpAdapterInfo64;
_Static_assert(sizeof(BeerIpAddrString64) == 48, "Win64 IP_ADDR_STRING");
_Static_assert(offsetof(BeerIpAdapterInfo64, address_length) == 404, "adapter MAC length");
_Static_assert(offsetof(BeerIpAdapterInfo64, ips) == 440, "adapter IP list");
_Static_assert(offsetof(BeerIpAdapterInfo64, lease_obtained) == 688, "Win64 lease time");
_Static_assert(sizeof(BeerIpAdapterInfo64) == 704, "Win64 IP_ADAPTER_INFO");

static int beer_adapter_link(const struct ifaddrs *p)
{
    return p->ifa_name && p->ifa_addr && !(p->ifa_flags & IFF_LOOPBACK) &&
           p->ifa_addr->sa_family == AF_PACKET;
}
static int beer_adapter_ip(const struct ifaddrs *p, const char *name)
{
    return p->ifa_name && p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
           !strcmp(p->ifa_name, name);
}
static void beer_adapter_default_gateway(const char *name, BeerIpAddrString64 *out)
{
    FILE *routes = fopen("/proc/net/route", "r");
    if (!routes) return;
    char line[512], interface[64];
    unsigned destination, gateway, flags;
    while (fgets(line, sizeof(line), routes)) {
        if (sscanf(line, "%63s %x %x %x", interface, &destination, &gateway, &flags) == 4 &&
            !strcmp(interface, name) && !destination && (flags & 3u) == 3u) {
            uint32_t raw = gateway;
            inet_ntop(AF_INET, &raw, out->address, sizeof(out->address));
            strcpy(out->mask, "0.0.0.0");
            break;
        }
    }
    fclose(routes);
}
/* Project the host's IPv4 configuration as static guest addresses. Beer does
 * not run a guest DHCP or WINS client, so no lease/server state is advertised.
 * All list nodes and strings belong to the caller's one buffer. */
static uint32_t beer_adapter_info_encode(const struct ifaddrs *list, void *buffer, uint32_t *size)
{
    if (!size) return 87;
    size_t adapters = 0, extra_ips = 0;
    for (const struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!beer_adapter_link(p)) continue;
        ++adapters;
        size_t ips = 0;
        for (const struct ifaddrs *ip = list; ip; ip = ip->ifa_next)
            if (beer_adapter_ip(ip, p->ifa_name)) ++ips;
        if (ips > 1) extra_ips += ips - 1;
    }
    if (!adapters) { *size = 0; return 232; }
    if (adapters > UINT32_MAX / sizeof(BeerIpAdapterInfo64) ||
        extra_ips > UINT32_MAX / sizeof(BeerIpAddrString64)) return 8;
    size_t required = adapters * sizeof(BeerIpAdapterInfo64) + extra_ips * sizeof(BeerIpAddrString64);
    if (required > UINT32_MAX) return 8;
    if (!buffer || *size < required) { *size = (uint32_t)required; return 111; }
    memset(buffer, 0, required);
    BeerIpAdapterInfo64 *out = buffer;
    BeerIpAddrString64 *extra = (void *)(out + adapters);
    size_t index = 0;
    for (const struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!beer_adapter_link(p)) continue;
        BeerIpAdapterInfo64 *a = &out[index++];
        if (index < adapters) a->next = (uint64_t)(uintptr_t)&out[index];
        snprintf(a->name, sizeof(a->name), "%s", p->ifa_name);
        snprintf(a->description, sizeof(a->description), "%s", p->ifa_name);
        const struct sockaddr_ll *link = (const struct sockaddr_ll *)p->ifa_addr;
        if (link->sll_halen > sizeof(a->address)) return 13; /* ERROR_INVALID_DATA */
        a->address_length = link->sll_halen;
        memcpy(a->address, link->sll_addr, link->sll_halen);
        a->index = (uint32_t)link->sll_ifindex;
        a->type = link->sll_hatype == 1 ? 6u : 1u; /* Ethernet or OTHER, not a guessed capability */
        BeerIpAddrString64 *last = NULL;
        for (const struct ifaddrs *ip = list; ip; ip = ip->ifa_next) {
            if (!beer_adapter_ip(ip, p->ifa_name)) continue;
            BeerIpAddrString64 *node = last ? extra++ : &a->ips;
            if (last) last->next = (uint64_t)(uintptr_t)node;
            const struct sockaddr_in *address = (const struct sockaddr_in *)ip->ifa_addr;
            inet_ntop(AF_INET, &address->sin_addr, node->address, sizeof(node->address));
            if (ip->ifa_netmask) {
                const struct sockaddr_in *mask = (const struct sockaddr_in *)ip->ifa_netmask;
                inet_ntop(AF_INET, &mask->sin_addr, node->mask, sizeof(node->mask));
            } else strcpy(node->mask, "0.0.0.0");
            last = node;
        }
        if (!last) { strcpy(a->ips.address, "0.0.0.0"); strcpy(a->ips.mask, "0.0.0.0"); }
        strcpy(a->gateways.address, "0.0.0.0"); strcpy(a->gateways.mask, "0.0.0.0");
        beer_adapter_default_gateway(p->ifa_name, &a->gateways);
    }
    *size = (uint32_t)required;
    return 0;
}
static uint32_t beer_get_adapters_info(void *buffer, uint32_t *size)
{
    if (!size) return 87;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) { *size = 0; return 232; }
    uint32_t result = beer_adapter_info_encode(list, buffer, size);
    freeifaddrs(list);
    return result;
}
#endif