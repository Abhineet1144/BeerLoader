#include "win32_ip_adapter.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(sizeof(BeerSocketAddress) == 16);
    assert(sizeof(BeerIpAdapterUnicastAddress) == 64);
    assert(sizeof(BeerIpAdapterAddresses) == 456);
    assert(offsetof(BeerIpAdapterAddresses, first_unicast_address) == 24);
    assert(offsetof(BeerIpAdapterAddresses, mtu) == 96);
    assert(offsetof(BeerIpAdapterAddresses, if_type) == 100);
    assert(offsetof(BeerIpAdapterAddresses, oper_status) == 104);
    assert(offsetof(BeerIpAdapterAddresses, first_prefix) == 176);
    assert(BEER_IF_OPER_STATUS_UP == 1);
    assert(BEER_IF_TYPE_IEEE80211 == 71);
    puts("win32 IP adapter layout tests passed");
    return 0;
}
