#include "win32_static_tls.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* Modules with TLS are numbered 1, 2, 3, ... in the order their TLS is set up; slot 0 is the executable's. */
    uint32_t next = 0;
    for (uint32_t expected = 1; expected < 128; ++expected)
        assert(beer_static_tls_next_slot(&next, 128) == expected);
    assert(beer_static_tls_next_slot(&next, 128) == UINT32_MAX);
    next = 0;
    assert(beer_static_tls_next_slot(&next, 1) == UINT32_MAX);
    assert(beer_static_tls_next_slot(&next, 0) == UINT32_MAX);
    assert(beer_static_tls_next_slot(NULL, 128) == UINT32_MAX);
    puts("static TLS slot contract: PASS");
    return 0;
}
