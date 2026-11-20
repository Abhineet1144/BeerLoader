#ifndef BEER_CAPTURE_H
#define BEER_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

/* Recording and live streaming. The post pass (renderer/vulkan/vulkan_post.c) scales the finished picture (effects
 * applied, overlay left out) on the GPU and reads it back into a small ring of host buffers; this module feeds those
 * frames, plus the game's own audio, to an ffmpeg child process that encodes them into a file or an RTMP stream.
 *
 * The output is constant frame rate on one of two timelines:
 *   real time  - frames are placed by the wall clock (what you saw, at the speed you saw it);
 *   game time  - frames are placed by the game's (scaled) clock, so a game slowed down in the manager still records at
 *                normal speed. The game's audio does not follow the game clock, so this timeline records no sound.
 *
 * Control commands (beer_control.c): capture_start, capture_stop, capture_status. Integer math only (the loader is
 * built without SSE). */

/* Nonzero while a capture runs: the presenter routes every frame through the post pass. */
int beer_capture_active(void);

/* Post pass, once per presented frame: returns 1 when this frame is due on the capture timeline. *out_w and *out_h are the
 * encode size (fixed for the whole capture), *due the frame's timeline index. Nothing changes until commit. */
int beer_capture_poll(uint32_t src_w, uint32_t src_h, uint32_t *out_w, uint32_t *out_h, uint64_t *due);
/* The frame for `due` was recorded into a readback buffer. */
void beer_capture_commit(uint64_t due);
/* No readback buffer was free for a due frame (the encoder is behind); counted in the status. */
void beer_capture_note_busy(void);

/* A recorded frame's pixels are complete: tightly packed RGBA, w*h*4 bytes. The encoder thread calls
 * release(slot) when it no longer reads them (also when the frame is dropped). Returns 0 when nothing takes the frame
 * (release was already called). */
typedef void (*BeerCaptureRelease)(int slot);
int beer_capture_push(int slot, const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t due, BeerCaptureRelease release);

/* Game audio tap (audio/audio_backend.c): interleaved S16LE exactly as handed to the sound server. */
void beer_capture_audio(const void *stream, const int16_t *pcm, size_t frames, unsigned rate, unsigned channels);

void beer_capture_handle(const char *cmd, const char *request, char *out, size_t size);
void beer_capture_init(const char *exe_path);

#endif
