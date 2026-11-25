#ifndef BEER_STEAM_VOICE_H
#define BEER_STEAM_VOICE_H

#include <stdint.h>

/* Steam voice chat for ISteamUser (StartVoiceRecording .. GetVoiceOptimalSampleRate): microphone capture through
 * PulseAudio/PipeWire, Opus encoding into Steam's voice packet format, and decoding of received packets. libopus and
 * libpulse-simple are loaded at run time; without them recording reports no data and decoding fails cleanly.
 * Results are EVoiceResult values. */

enum {
    BEER_VOICE_OK = 0,
    BEER_VOICE_NOT_INITIALIZED = 1,
    BEER_VOICE_NOT_RECORDING = 2,
    BEER_VOICE_NO_DATA = 3,
    BEER_VOICE_BUFFER_TOO_SMALL = 4,
    BEER_VOICE_DATA_CORRUPTED = 5
};

#define BEER_VOICE_SAMPLE_RATE 24000u   /* Steam's Opus voice rate; also GetVoiceOptimalSampleRate */

void beer_steam_voice_start(void);
void beer_steam_voice_stop(void);
/* Bytes of compressed voice ready for beer_steam_voice_get. */
int beer_steam_voice_available(uint64_t steamid, uint32_t *compressed_bytes);
int beer_steam_voice_get(uint64_t steamid, void *dest, uint32_t dest_size, uint32_t *written);
/* Decodes one packet (from any sender) to 16-bit mono PCM at `rate` (11025..48000). */
int beer_steam_voice_decompress(const void *packet, uint32_t size, void *dest, uint32_t dest_size, uint32_t *written,
                                uint32_t rate);

/* For tests: encodes PCM (24 kHz mono, whole 20 ms frames) into one packet without a microphone. */
int beer_steam_voice_encode_for_test(uint64_t steamid, const int16_t *pcm, uint32_t samples, void *dest,
                                     uint32_t dest_size, uint32_t *written);

#endif
