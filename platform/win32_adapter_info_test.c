#include "win32_adapter_info.h"
#include <assert.h>
#include <stdlib.h>
int main(void)
{
    struct sockaddr_ll links[3] = {{ .sll_family = AF_PACKET, .sll_ifindex = 11, .sll_hatype = 1, .sll_halen = 6 },
        { .sll_family = AF_PACKET, .sll_ifindex = 12, .sll_hatype = 1, .sll_halen = 6 },
        { .sll_family = AF_PACKET, .sll_ifindex = 1 }};
    memcpy(links[0].sll_addr, "ABCDEF", 6);
    struct sockaddr_in ips[3] = {{ .sin_family = AF_INET }, { .sin_family = AF_INET }, { .sin_family = AF_INET }};
    struct sockaddr_in mask = { .sin_family = AF_INET };
    assert(inet_pton(AF_INET, "192.0.2.1", &ips[0].sin_addr) == 1);
    assert(inet_pton(AF_INET, "192.0.2.2", &ips[1].sin_addr) == 1);
    assert(inet_pton(AF_INET, "198.51.100.1", &ips[2].sin_addr) == 1);
    assert(inet_pton(AF_INET, "255.255.255.0", &mask.sin_addr) == 1);
    struct ifaddrs nodes[6] = {0};
    for (size_t i = 0; i < 5; ++i) nodes[i].ifa_next = &nodes[i+1];
    nodes[0].ifa_name = "beer-test0"; nodes[0].ifa_addr = (void *)&links[0];
    nodes[1].ifa_name = "beer-test0"; nodes[1].ifa_addr = (void *)&ips[0]; nodes[1].ifa_netmask = (void *)&mask;
    nodes[2].ifa_name = "beer-test0"; nodes[2].ifa_addr = (void *)&ips[1]; nodes[2].ifa_netmask = (void *)&mask;
    nodes[3].ifa_name = "beer-test1"; nodes[3].ifa_addr = (void *)&links[1];
    nodes[4].ifa_name = "beer-test1"; nodes[4].ifa_addr = (void *)&ips[2]; nodes[4].ifa_netmask = (void *)&mask;
    nodes[5].ifa_name = "lo"; nodes[5].ifa_addr = (void *)&links[2]; nodes[5].ifa_flags = IFF_LOOPBACK;
    uint32_t size = 0;
    assert(beer_adapter_info_encode(nodes, NULL, NULL) == 87);
    assert(beer_adapter_info_encode(NULL, NULL, &size) == 232 && !size);
    assert(beer_adapter_info_encode(nodes, NULL, &size) == 111 && size == 2 * 704 + 48);
    unsigned char *buffer = malloc(size + 16);
    memset(buffer, 0xa5, size + 16);
    uint32_t small = size - 1;
    assert(beer_adapter_info_encode(nodes, buffer, &small) == 111 && small == size && buffer[0] == 0xa5);
    assert(!beer_adapter_info_encode(nodes, buffer, &size));
    BeerIpAdapterInfo64 *a = (void *)buffer;
    assert(a[0].next == (uint64_t)(uintptr_t)&a[1] && !a[1].next);
    assert(a[0].index == 11 && a[1].index == 12 && a[0].type == 6);
    assert(a[0].address_length == 6 && !memcmp(a[0].address, "ABCDEF", 6));
    assert(!strcmp(a[0].ips.address, "192.0.2.1") && !strcmp(a[0].ips.mask, "255.255.255.0"));
    BeerIpAddrString64 *extra = (void *)(uintptr_t)a[0].ips.next;
    assert(extra == (void *)&a[2] && !extra->next && !strcmp(extra->address, "192.0.2.2"));
    assert(!strcmp(a[1].ips.address, "198.51.100.1"));
    assert(!a[0].dhcp_enabled && !a[0].have_wins && !a[0].current_ip_address);
    for (unsigned i = 0; i < 16; ++i) assert(buffer[size + i] == 0xa5);
    free(buffer);
    assert(beer_get_adapters_info(NULL, NULL) == 87);
    puts("IPv4 adapter sizing, Win64 layout and caller-owned lists: PASS");
}