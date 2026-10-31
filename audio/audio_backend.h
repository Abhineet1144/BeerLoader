#ifndef BEER_AUDIO_BACKEND_H
#define BEER_AUDIO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

typedef void (*BeerAudioDoneFn)(void *opaque);

/* One sound-server stream per guest audio source (a WASAPI client, a DirectSound buffer, waveOut): the server mixes
 * them, and each source's play position and reset stay its own. */
typedef struct BeerAudioStream BeerAudioStream;

BeerAudioStream *beer_audio_stream_open(unsigned rate, unsigned channels, const char *name);
/* block: wait while more than ~400 ms is queued (WASAPI, waveOut); DirectSound Unlock never blocks. */
int beer_audio_stream_submit(BeerAudioStream *stream, const void *pcm, size_t bytes, int block,
                             BeerAudioDoneFn done, void *opaque);
void beer_audio_stream_reset(BeerAudioStream *stream);
uint64_t beer_audio_stream_bytes_played(BeerAudioStream *stream);   /* handed to the sound server */
/* Bytes the sound server has actually played (handed over minus its current latency); falls back to
 * bytes_played when the server cannot report latency.  *exact tells which one it was. */
uint64_t beer_audio_stream_bytes_heard(BeerAudioStream *stream, int *exact);
void beer_audio_stream_close(BeerAudioStream *stream);

/* The default stream (waveOut). */
int beer_audio_init(unsigned rate, unsigned channels);
int beer_audio_submit_nowait(const void *pcm, size_t bytes);
int beer_audio_submit(const void *pcm, size_t bytes, BeerAudioDoneFn done, void *opaque);
void beer_audio_reset(void);
void beer_audio_shutdown(void);
int beer_audio_available(void);
uint64_t beer_audio_bytes_played(void);

#endif
