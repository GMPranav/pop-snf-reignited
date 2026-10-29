#ifndef BRIDGE_LIBC_H
#define BRIDGE_LIBC_H

#include <stdint.h>
#include "../elf32/elf32_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set game working root directory */
void bridge_libc_set_root_dir(const char *path);

/* Initialize heap upper bound dynamically based on guest image size (reserves 32 MB for stack) */
void bridge_libc_init_heap(elf32_image_t *img);

/* Set debug mode: enables/disables call-site leak tracking and diagnostic logs */
void bridge_libc_set_debug_mode(int enabled);

/* Function signature for SVC host handler: returns R0 */
typedef uint32_t (*svc_handler_fn)(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp);

/* Extended return registers for ARM EABI functions like __aeabi_idivmod */
extern uint32_t g_guest_ret_r1;
extern int g_has_guest_ret_r1;
extern uint32_t g_guest_lr;
extern uint32_t g_guest_pc;

/* Look up libc/system handler by symbol name */
svc_handler_fn bridge_libc_lookup(const char *name);

/* Telemetry getters */
uint32_t bridge_libc_get_heap_used_mb(void);
uint64_t bridge_libc_get_malloc_count(void);
uint64_t bridge_libc_get_free_count(void);
uint32_t bridge_libc_get_stk_vfd_count(void);

/* Clean up in-memory temporary VFS buffers */
void bridge_libc_cleanup_mem_vfs(void);

/* Diagnostics: call periodically (e.g. once per second) from the main loop.
 * Prints net malloc/free/mmap accounting - cheap, safe to call often. */
void bridge_libc_dump_mem_report(void);

/* Diagnostics: prints the top call-sites (by guest link-register value)
 * currently holding the most live (unfreed) heap bytes. Walks a small
 * fixed-size table, so it's more expensive than dump_mem_report - call it
 * at OOM time or on-demand, not every frame. addr2line/objdump the printed
 * lr values against the guest .so to map them back to functions. */
void bridge_libc_dump_leak_report(int top_n);

/* Diagnostics: prints the top call-sites by TOTAL bytes ever allocated
 * (gross throughput), regardless of how much of that is still live. A site
 * with huge throughput but small live footprint allocates-and-frees
 * constantly - that's what fragments a segregated free-list heap, and it's
 * invisible to dump_leak_report's live-bytes ranking no matter how much
 * churn it's responsible for. Same cost/calling convention as
 * dump_leak_report - call at OOM time or on-demand. */
void bridge_libc_dump_churn_report(int top_n);

/* Diagnostics: prints every asset path that failed to fopen() during this
 * run (deduplicated), each with the guest link-register value of its first
 * failing call-site. Intended to be called once at shutdown. */
void bridge_libc_dump_missing_assets(void);

/* Diagnostics: call once per frame from the main loop with whatever "where
 * are we in the game" identifiers are cheaply readable (scene/game object
 * pointers, engine state, frame number). Nothing here needs to be a name -
 * raw pointers are enough to correlate "this changed" with log timestamps.
 * Every new leak-tracking / missing-asset table entry stamps itself with the
 * most recent values passed in here, so reports can say which scene an
 * allocation site or a missing asset was FIRST seen in, not just what was
 * active whenever a report happened to print. A scene-pointer change is
 * also logged immediately as a `[SCENE]` transition line. */
void bridge_libc_set_scene_context(uint32_t scene_ptr, uint32_t game_ptr, uint32_t ce_state, uint32_t frame_no);

/* Request game termination: sets exit requested flag and code */
void bridge_request_exit(int exit_code);
int bridge_is_exit_requested(void);
int bridge_get_exit_code(void);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_LIBC_H */
