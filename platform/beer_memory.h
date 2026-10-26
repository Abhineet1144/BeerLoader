#ifndef BEER_MEMORY_H
#define BEER_MEMORY_H

#include <stddef.h>

/* Cheat-Engine-style memory access for the manager, over the control socket. Commands (JSON, one per line):
 *   mem_regions                                   -> readable regions of this process (/proc/self/maps)
 *   mem_read   {addr,len}                         -> hex bytes
 *   mem_write  {addr,hex}                         -> writes bytes (also into read-only pages)
 *   mem_scan   {type,value,writable}              -> first scan, starts a session
 *   mem_next   {mode,value}                       -> filter the session: exact|changed|unchanged|increased|decreased
 *   mem_results{offset,limit}                     -> page of the session with live values
 *   mem_set_all{value}                            -> write a value to EVERY result of the scan list
 *   mem_remove {addrs:[...]}                      -> drop results from the list (up to 64 per call)
 *   mem_reset                                     -> drop the session
 *   mem_values {type,addrs:[...]}                 -> live values for a watch list
 *   mem_set    {addr,type,value}                  -> write a typed value
 *   mem_freeze {addr,type,value} / mem_unfreeze {addr} / mem_frozen
 * Types: i8 i16 i32 i64 f32 f64 str bytes. All access goes through /proc/self/mem, so a bad address
 * returns an error instead of faulting the game. This file is built with SSE because it handles floats. */
/* Formats the value at addr as `type` (i8..f64, str is not supported here) into out; returns 0 if unreadable. */
int beer_memory_peek(unsigned long long addr, const char *type, char *out, size_t size);
void beer_memory_handle(const char *cmd, const char *request, char *out, size_t size);

#endif
