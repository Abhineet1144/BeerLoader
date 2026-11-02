#include "win32_static_tls.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t slots[128] = {0};
    /* Finish the deepest dependency first, then its parents, as recursive
     * import loading does. Every module must retain a distinct slot. */
    for (size_t count = 32; count; --count) {
        uint32_t slot = beer_static_tls_slot(count - 1, 128);
        assert(slot == count && !slots[slot]);
        slots[slot] = 1;
    }
    assert(!slots[0]);
    assert(beer_static_tls_slot(0, 128) == 1);
    assert(beer_static_tls_slot(126, 128) == 127);
    assert(beer_static_tls_slot(127, 128) == UINT32_MAX);
    assert(beer_static_tls_slot(0, 0) == UINT32_MAX);
    assert(beer_static_tls_slot(0, 1) == UINT32_MAX);
    assert(beer_static_tls_slot(SIZE_MAX, 128) == UINT32_MAX);
    puts("static TLS slot contract: PASS");
    return 0;
}