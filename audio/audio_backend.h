#ifndef BEER_AUDIO_BACKEND_H
#define BEER_AUDIO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

typedef void (*BeerAudioDoneFn)(void *opaque);

int beer_audio_init(unsigned rate, unsigned channels);
int beer_audio_submit(const void *pcm, size_t bytes, BeerAudioDoneFn done, void *opaque);
void beer_audio_reset(void);
void beer_audio_shutdown(void);
int beer_audio_available(void);
uint64_t beer_audio_bytes_played(void);

#endif
