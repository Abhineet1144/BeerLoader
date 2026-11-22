#ifndef BEER_MEDIA_SESSION_H
#define BEER_MEDIA_SESSION_H

#include <stddef.h>
#include <stdint.h>

/* Media Foundation's playback pipeline: media session, source resolver and media source, topologies, sample grabber
 * sinks, event queues and work queues (media_session.c). Decoding runs in ffmpeg, as for the source reader
 * (media_foundation.c). Unreal Engine 4's WMF media player plays its videos through this pipeline. */

/* Host services set by the loader before any of the exports below is used. */
typedef int (*BeerMfPathResolveFn)(const char *windows_path, char *host_path, size_t size);
typedef uint64_t (*BeerMfWaitHandleFn)(uint64_t handle, uint32_t milliseconds);
void beer_mf_session_set_host(BeerMfPathResolveFn resolve_path, BeerMfWaitHandleFn wait_handle);

#define BEER_MF_MSABI __attribute__((ms_abi))
uint64_t BEER_MF_MSABI beer_mfs_create_media_session(void *configuration, void **session);
uint64_t BEER_MF_MSABI beer_mfs_create_source_resolver(void **resolver);
uint64_t BEER_MF_MSABI beer_mfs_create_topology(void **topology);
uint64_t BEER_MF_MSABI beer_mfs_create_topology_node(uint32_t type, void **node);
uint64_t BEER_MF_MSABI beer_mfs_create_sample_grabber_sink_activate(void *media_type, void *callback, void **activate);
uint64_t BEER_MF_MSABI beer_mfs_create_audio_renderer_activate(void **activate);
uint64_t BEER_MF_MSABI beer_mfs_create_video_renderer_activate(uint64_t window, void **activate);
uint64_t BEER_MF_MSABI beer_mfs_get_service(void *object, const void *service, const void *iid, void **out);
uint64_t BEER_MF_MSABI beer_mfs_create_event_queue(void **queue);
uint64_t BEER_MF_MSABI beer_mfs_create_media_event(uint32_t type, const void *extended_type, uint32_t status,
                                                 const void *value, void **event);
uint64_t BEER_MF_MSABI beer_mfs_create_async_result(void *object, void *callback, void *state, void **result);
uint64_t BEER_MF_MSABI beer_mfs_invoke_callback(void *result);
uint64_t BEER_MF_MSABI beer_mfs_put_work_item(uint32_t queue, void *callback, void *state);
uint64_t BEER_MF_MSABI beer_mfs_put_work_item_ex(uint32_t queue, void *result);
uint64_t BEER_MF_MSABI beer_mfs_put_waiting_work_item(uint64_t event, int32_t priority, void *result, uint64_t *key);
uint64_t BEER_MF_MSABI beer_mfs_cancel_work_item(uint64_t key);
uint64_t BEER_MF_MSABI beer_mfs_create_sample(void **sample);
uint64_t BEER_MF_MSABI beer_mfs_create_memory_buffer(uint32_t max_length, void **buffer);
uint64_t BEER_MF_MSABI beer_mfs_create_aligned_memory_buffer(uint32_t max_length, uint32_t alignment, void **buffer);
uint64_t BEER_MF_MSABI beer_mfs_create_collection(void **collection);
uint64_t BEER_MF_MSABI beer_mfs_get_system_time(void);
uint64_t BEER_MF_MSABI beer_mfs_enum_device_sources(void *attributes, void ***sources, uint32_t *count);

#endif
