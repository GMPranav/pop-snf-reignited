#ifndef DYNARMIC_HOST_H
#define DYNARMIC_HOST_H

#include <stdint.h>
#include <stddef.h>
#include "../elf32/elf32_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dynarmic_host_s dynarmic_host_t;

/* Create and initialize Dynarmic ARMv7 JIT for the guest image */
dynarmic_host_t *dynarmic_host_create(elf32_image_t *img);
void dynarmic_host_destroy(dynarmic_host_t *host);

/* Call guest function at given virtual address, returning R0 */
uint32_t dynarmic_call(dynarmic_host_t *host, uint32_t entry_point, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);

/* Call guest function with stack parameters */
uint32_t dynarmic_call_stack(dynarmic_host_t *host, uint32_t entry_point, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, const uint32_t *stack_args, uint32_t stack_count);

/* Host-call profiler: times every guest->host bridge call (file reads, GL uploads, audio...) so slow frames can be
 * attributed. Off by default (adds two timer reads per call); enabled with PerfProfile = 1 in config.ini. */
void    host_prof_enable(int on);
int     host_prof_enabled(void);
void    host_prof_frame_reset(void);                 /* call at the start of each frame */
int64_t host_prof_frame_total_us(void);              /* time spent inside host calls this frame */
uint32_t host_prof_frame_calls(void);
/* Writes "name=ms/calls ..." for the n most expensive host calls of this frame; returns the number written */
int     host_prof_frame_top(int n, char *buf, size_t bufsz);
/* Folds this frame into the aggregate (used for per-level-load breakdowns) */
void    host_prof_aggregate_frame(void);
int     host_prof_aggregate_top(int n, char *buf, size_t bufsz, int64_t *total_us, uint32_t *calls);
int     host_prof_sample_top(int n, char *buf, size_t bufsz);
void    host_prof_aggregate_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DYNARMIC_HOST_H */
