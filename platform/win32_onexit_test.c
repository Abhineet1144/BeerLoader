#include "win32_onexit.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

unsigned beer_test_onexit_calls;
extern int __attribute__((ms_abi)) beer_test_onexit_callback(void);

/* Real Win64 callback which consumes every home slot. The old SysV callback
 * cast fails this test by overwriting its caller's live frame. */
__asm__(
    ".pushsection .text\n"
    ".global beer_test_onexit_callback\n"
    "beer_test_onexit_callback:\n"
    "    movaps %xmm0, 8(%rsp)\n"
    "    movq $0, 24(%rsp)\n"
    "    movq $0, 32(%rsp)\n"
    "    incl beer_test_onexit_calls(%rip)\n"
    "    mov $17, %eax\n"
    "    ret\n"
    ".popsection\n"
);

static BeerOnExitTable table;
static unsigned sequence[8], length;

static int __attribute__((ms_abi)) first(void)
{ sequence[length++] = 1; return 123; }
static int __attribute__((ms_abi)) second(void)
{ sequence[length++] = 2; return 0; }
static int __attribute__((ms_abi)) adding(void)
{
    sequence[length++] = 3;
    /* Grow during execution, replacing the table allocation while a callback
     * is active. The executor must re-read its cursor after returning. */
    for (unsigned i = 0; i < 80; ++i)
        assert(!beer_onexit_register(&table, beer_test_onexit_callback));
    assert(!beer_onexit_register(&table, second));
    return 0;
}

static void *register_worker(void *arg)
{
    BeerOnExitTable *target = arg;
    for (unsigned i = 0; i < 1000; ++i)
        assert(!beer_onexit_register(target, beer_test_onexit_callback));
    return NULL;
}

int main(void)
{
    assert(beer_onexit_initialize(NULL) < 0);
    assert(beer_onexit_register(NULL, first) < 0);
    assert(beer_onexit_execute(NULL) < 0);
    assert(!beer_onexit_initialize(&table));
    assert(beer_onexit_register(&table, NULL) < 0);
    assert(!beer_onexit_execute(&table));
    assert(!beer_onexit_initialize(&table));
    assert(!beer_onexit_register(&table, first));
    assert(!beer_onexit_register(&table, adding));
    assert(!beer_onexit_execute(&table));
    assert(length == 3 && sequence[0] == 3 && sequence[1] == 2 && sequence[2] == 1);
    assert(beer_test_onexit_calls == 80);
    assert(!table.first && !table.last && !table.end);

    assert(!beer_onexit_initialize(&table));
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&workers[i], NULL, register_worker, &table));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
    assert(table.last - table.first == 4000);
    assert(!beer_onexit_execute(&table));
    assert(beer_test_onexit_calls == 4080);
    assert(!table.first && !table.last && !table.end);
    puts("Win64 onexit callback ABI and table tests passed");
    return 0;
}