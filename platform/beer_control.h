#ifndef BEER_CONTROL_H
#define BEER_CONTROL_H

#include <stddef.h>
#include <stdint.h>

/* Live control channel for the manager app: a per-process unix socket at
 * $XDG_RUNTIME_DIR/beer/<pid>.sock (mode 0600) speaking one JSON object per line.
 *
 * Requests:  {"cmd":"hello"} {"cmd":"status"} {"cmd":"settings"}
 *            {"cmd":"set","name":"time_scale","value":0.5}
 * Replies:   {"ok":true,...} or {"ok":false,"error":"..."}
 *
 * Floating point values travel as decimals but are stored as fixed point (milli units), because the
 * loader is built without SSE. */
void beer_control_start(const char *exe_path);
void beer_control_note_frame(void);   /* call once per presented frame */
void beer_control_frame_limit_wait(void);   /* applies the live frame cap; call once per presented frame */

/* JSON helpers shared with beer_memory.c */
const char *beer_json_find(const char *json, const char *key);
int beer_json_string(const char *json, const char *key, char *out, size_t size);
int beer_json_milli(const char *json, const char *key, int32_t *out);   /* number or bool as milli units */

/* Stats the overlay shows */
uint64_t beer_control_frames(void);
uint64_t beer_control_uptime_s(void);
int beer_control_time_scale_milli(void);
int beer_control_paused(void);
void beer_control_proc_stats(uint64_t *cpu_ticks, uint64_t *rss_kb, uint64_t *threads);

#endif
