#ifndef BEER_OBJECTS_H
#define BEER_OBJECTS_H

#include <stddef.h>

/* Object scanner for the manager: finds game objects by their transform and groups them by C++ type.
 *
 *   obj_scan  {max_offset?}            -> scans writable memory for transforms (4x4 / 3x4 matrices, float and double
 *                                         quaternion+translation+scale), finds each owner object (nearest vtable
 *                                         pointer before it) and groups them by (vtable, transform offset, kind).
 *                                         Replies with the groups, largest first, named from MSVC RTTI when present.
 *   obj_list  {group,offset,limit}     -> instances of a group with their live transform
 *   obj_moved {limit,min,max}          -> instances whose position changed since the previous obj_moved (or the
 *                                         scan) by min..max metres, most-moved first
 *   obj_find_player {hold_ms}          -> holds W, then S, in the game window: the controlled character is what
 *                                         moved forward and then back; candidates best first
 *   obj_near {x,y,z,radius,limit,untyped} -> objects within radius of a point (the player), nearest first;
 *                                         untyped: also bare transforms (physics bodies, caches)
 *   obj_copies {x,y,z,tol}             -> every place holding this position (3 consecutive floats or doubles):
 *                                         physics body, render matrix, previous frame...; remembered for obj_shift
 *   obj_shift {dx,dy,dz,hold_ms}       -> moves every remembered copy by the offset and keeps writing it for
 *                                         hold_ms, so the engine's per-frame rebuilds take the new position
 * Entities found through a pointer to their transform holder come before the holder itself, one entry per object.
 *
 * Transform encodings in replies ("kind"):
 *   mat4  16 floats, rows = local X/Y/Z axes (length = scale) then position, w = 0,0,0,1
 *   mat34 the same rows with unused w lanes
 *   qts   float quaternion x,y,z,w | translation x,y,z,pad | scale x,y,z,pad   (Unreal FTransform)
 *   qts64 the same in doubles                                                  (UE5 FTransform)
 * Edits go through mem_write / mem_freeze. */
void beer_objects_handle(const char *cmd, const char *request, char *out, size_t size);

#endif
