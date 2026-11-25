/* Steam voice packet roundtrip: a 440 Hz tone is encoded into Steam's voice format, checked for its layout and CRC,
 * decoded at several output rates, and must come back as a tone of similar loudness. A corrupted packet must be
 * rejected. Skips (exit 0) when libopus is not installed. */
#include "beer_steam_voice.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static double rms(const int16_t *p, size_t n)
{
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += (double)p[i] * p[i];
    return n ? sqrt(sum / n) : 0;
}

int main(void)
{
    enum { SAMPLES = 480 * 10 };   /* 200 ms */
    static int16_t tone[SAMPLES];
    for (int i = 0; i < SAMPLES; ++i) tone[i] = (int16_t)(8000.0 * sin(2 * M_PI * 440.0 * i / BEER_VOICE_SAMPLE_RATE));
    static unsigned char packet[32768];
    uint32_t size = 0;
    const uint64_t steamid = 76561199688062658ull;
    const int r = beer_steam_voice_encode_for_test(steamid, tone, SAMPLES, packet, sizeof(packet), &size);
    if (r == BEER_VOICE_NOT_INITIALIZED) { puts("steam voice tests skipped (no libopus)"); return 0; }
    assert(r == BEER_VOICE_OK && size > 8 + 3 + 3 + 4);
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id |= (uint64_t)packet[i] << (8 * i);
    assert(id == steamid);
    assert(packet[8] == 0x0B && (packet[9] | packet[10] << 8) == (int)BEER_VOICE_SAMPLE_RATE);
    assert(packet[11] == 0x06);
    const unsigned length = packet[12] | packet[13] << 8;
    assert(14 + length + 4 == size);

    static int16_t out[48000];
    const uint32_t rates[] = { 24000, 48000, 11025, 44100 };
    for (size_t k = 0; k < sizeof(rates) / sizeof(*rates); ++k) {
        uint32_t written = 0;
        assert(beer_steam_voice_decompress(packet, size, out, sizeof(out), &written, rates[k]) == BEER_VOICE_OK);
        const size_t n = written / 2, expected = (size_t)SAMPLES * rates[k] / BEER_VOICE_SAMPLE_RATE;
        assert(n + 8 >= expected && n <= expected + 8);
        /* Opus has a short start-up delay: compare the loudness of the second half. */
        const double level = rms(out + n / 2, n / 2), source = rms(tone, SAMPLES);
        assert(level > source * 0.5 && level < source * 1.5);
    }
    uint32_t written = 0;
    assert(beer_steam_voice_decompress(packet, size, out, 16, &written, 24000) == BEER_VOICE_BUFFER_TOO_SMALL);
    assert(written > 16);
    packet[20] ^= 0x55;   /* corrupt one byte: the CRC must catch it */
    assert(beer_steam_voice_decompress(packet, size, out, sizeof(out), &written, 24000) == BEER_VOICE_DATA_CORRUPTED);
    puts("steam voice tests passed");
    return 0;
}
