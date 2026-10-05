#include "audio_backend.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static atomic_uint completions;

static void packet_done(void *opaque)
{
    (void)opaque;
    atomic_fetch_add_explicit(&completions, 1, memory_order_relaxed);
}

int main(void)
{
    enum { RATE = 48000, CHANNELS = 2, FRAMES = 480 };
    int16_t pcm[FRAMES * CHANNELS];
    memset(pcm, 0, sizeof(pcm));

    if (!beer_audio_init(RATE, CHANNELS)) return 1;
    for (unsigned i = 0; i < 8; ++i)
        if (!beer_audio_submit(pcm, sizeof(pcm), packet_done, NULL)) return 2;

    /* A single packet may legitimately exceed the latency target (for example
     * a DirectSound ring segment). It must be admitted when the queue is empty
     * rather than deadlocking forever on back-pressure. */
    int16_t oversized[RATE * CHANNELS / 2];
    memset(oversized, 0, sizeof(oversized));
    if (!beer_audio_submit(oversized, sizeof(oversized), packet_done, NULL)) return 3;

    for (unsigned i = 0; i < 300 && atomic_load_explicit(&completions, memory_order_relaxed) < 9; ++i)
        usleep(10000);

    unsigned done = atomic_load_explicit(&completions, memory_order_relaxed);
    uint64_t expected = sizeof(pcm) * 8u + sizeof(oversized);
    uint64_t played = beer_audio_bytes_played();
    beer_audio_shutdown();
    if (done != 9 || played != expected) {
        fprintf(stderr, "audio backend mismatch: done=%u bytes=%llu expected=%llu\n",
                done, (unsigned long long)played, (unsigned long long)expected);
        return 4;
    }
    return 0;
}
