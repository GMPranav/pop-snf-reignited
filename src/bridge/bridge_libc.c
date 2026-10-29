#include "bridge_libc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#define PATH_MAX 260
#else
#include <unistd.h>
#endif

uint32_t g_guest_ret_r1 = 0;
int g_has_guest_ret_r1 = 0;

static char g_root_dir[PATH_MAX] = ".";

static void stk_slots_init(void); /* forward declaration */
static void mem_vfs_init(void);   /* forward declaration */

void bridge_libc_set_root_dir(const char *path) {
    strncpy(g_root_dir, path, sizeof(g_root_dir) - 1);
#ifdef _WIN32
    _setmaxstdio(2048);
#endif
    stk_slots_init();
    mem_vfs_init();
}

#define G_PTR(addr) (((addr) && (uint32_t)(addr) < img->mem_size) ? (img->mem + (addr)) : NULL)
#define READ_STACK(idx) (*(uint32_t*)(img->mem + sp + (idx) * 4))

/* Segregated Free-List Heap allocator (0x02000000 to g_heap_max)
 *
 * Block header (16 bytes, 16-byte aligned):
 *   uint32_t size      payload capacity in bytes (multiple of 16)
 *   uint32_t magic     HEAP_BLOCK_MAGIC (allocated) or HEAP_FREE_MAGIC (free)
 *   uint32_t next      free-list link (0 when allocated)
 *   uint32_t owner_lr  guest lr that allocated this block (valid only while
 *                      the block is allocated; meaningless on a free block).
 *                      Used solely for the per-call-site leak report below.
 *
 * Physical adjacency: because all blocks are bump-allocated contiguously and
 * splits preserve adjacency, the block immediately after blk at addr A is
 * always at A + 16 + blk->size, provided that address is < g_heap_cur.
 * We use this to forward-coalesce on free without any stored "next" pointer.
 */
static uint32_t g_heap_cur = 0x02000000u;
static uint32_t g_heap_max = 0x1E000000u;  /* Defaults to 480 MB (512 MB guest image minus 32 MB stack) */

void bridge_libc_init_heap(elf32_image_t *img) {
    if (!img) return;
    const uint32_t stack_reserve = 32u * 1024u * 1024u;
    if (img->mem_size > stack_reserve + 0x02000000u) {
        g_heap_max = img->mem_size - stack_reserve;
    }
}

static int s_libc_debug_mode = 0;

void bridge_libc_set_debug_mode(int enabled) {
    s_libc_debug_mode = enabled;
}

/* --- Scene/level context -------------------------------------------------
 * main.cpp already reverse-engineers a handful of engine object pointers
 * every single frame (current scene, current "game" object, engine state).
 * It feeds the current values in here once per frame via
 * bridge_libc_set_scene_context() so that anything logged from in here -
 * a new leak-tracking entry, a missing-asset lookup, an OOM print - can be
 * stamped with exactly which scene/level was active, instead of only being
 * cross-referenceable against the nearest once-per-second [FRAME] line. */
static uint32_t g_ctx_scene    = 0;
static uint32_t g_ctx_game     = 0;
static uint32_t g_ctx_ce_state = 0;
static uint32_t g_ctx_frame    = 0;

void bridge_libc_set_scene_context(uint32_t scene_ptr, uint32_t game_ptr, uint32_t ce_state, uint32_t frame_no) {
    if (s_libc_debug_mode && scene_ptr != g_ctx_scene) {
        fprintf(stderr, "[SCENE] frame=%u scene 0x%08X -> 0x%08X (game=0x%08X ce_state=%u)\n",
                frame_no, g_ctx_scene, scene_ptr, game_ptr, ce_state);
    }
    g_ctx_scene    = scene_ptr;
    g_ctx_game     = game_ptr;
    g_ctx_ce_state = ce_state;
    g_ctx_frame    = frame_no;
}

/* Allocation accounting - printed periodically to diagnose leaks */
static uint64_t g_acc_malloc_calls  = 0;
static uint64_t g_acc_malloc_bytes  = 0;
static uint64_t g_acc_free_calls    = 0;
static uint64_t g_acc_free_bytes    = 0;
static uint64_t g_acc_mmap_bytes    = 0;
static uint64_t g_acc_munmap_bytes  = 0;

/* Print alloc summary - call periodically */
static void print_alloc_summary(void) {
    int64_t net_malloc = (int64_t)(g_acc_malloc_bytes - g_acc_free_bytes);
    int64_t net_mmap   = (int64_t)(g_acc_mmap_bytes   - g_acc_munmap_bytes);
    uint32_t bump_mb   = (g_heap_cur - 0x02000000u) >> 20;
    fprintf(stderr,
        "[MEM] malloc=%lluk free=%lluk (net=%lldk) mmap=%lluk munmap=%lluk (net=%lldk) bump=%uMB\n",
        (unsigned long long)(g_acc_malloc_bytes  >> 10),
        (unsigned long long)(g_acc_free_bytes    >> 10),
        (long long)(net_malloc >> 10),
        (unsigned long long)(g_acc_mmap_bytes    >> 10),
        (unsigned long long)(g_acc_munmap_bytes  >> 10),
        (long long)(net_mmap   >> 10),
        bump_mb);
}

void bridge_libc_dump_mem_report(void) {
    if (s_libc_debug_mode) {
        print_alloc_summary();
    }
}

/* --- Per-call-site leak tracking -----------------------------------------
 * Every live (unfreed) heap block remembers the guest link-register value
 * (g_guest_lr) of the call that allocated it, stashed in the block header's
 * owner_lr field. This table aggregates live count/bytes per lr so a leak
 * can be attributed to a call-site without needing guest symbols - dump the
 * top offenders and addr2line/objdump the printed lr against the guest .so.
 */
#define LR_TABLE_SIZE 4096

typedef struct {
    uint32_t lr;          /* 0 until first use of this slot */
    uint32_t live_count;
    uint64_t live_bytes;
    uint64_t total_calls;  /* all-time allocations from this site; 0 = empty slot */
    uint64_t total_bytes;  /* all-time bytes EVER allocated here - never decremented
                             * on free, unlike live_bytes. A site with huge total_bytes
                             * but small live_bytes allocates-and-frees constantly: that
                             * churn is exactly what fragments the heap without ever
                             * showing up as a "leak" in the live-bytes ranking. */
    uint32_t first_scene;  /* g_ctx_scene the FIRST time this lr ever allocated */
    uint32_t first_game;   /* g_ctx_game  at that same moment */
    uint32_t first_frame;  /* g_ctx_frame at that same moment */
} lr_stat_t;

static lr_stat_t g_lr_stats[LR_TABLE_SIZE];

/* Open-addressing (linear probe from hash) find-or-insert. total_calls==0
 * marks an empty slot, so lr==0 is a perfectly valid key. Returns -1 only
 * if the table is completely full (256+ distinct call sites), in which
 * case that site's allocations simply won't be individually attributed. */
static int lr_stat_find_or_insert(uint32_t lr) {
    uint32_t h = (lr ^ (lr >> 15)) % LR_TABLE_SIZE;
    for (uint32_t i = 0; i < LR_TABLE_SIZE; ++i) {
        uint32_t idx = (h + i) % LR_TABLE_SIZE;
        if (g_lr_stats[idx].total_calls == 0) {
            g_lr_stats[idx].lr           = lr;
            g_lr_stats[idx].first_scene  = g_ctx_scene;
            g_lr_stats[idx].first_game   = g_ctx_game;
            g_lr_stats[idx].first_frame  = g_ctx_frame;
            return (int)idx;
        }
        if (g_lr_stats[idx].lr == lr) return (int)idx;
    }
    return -1;
}

static void lr_track_alloc(uint32_t lr, uint32_t size) {
    if (!s_libc_debug_mode) return;
    int slot = lr_stat_find_or_insert(lr);
    if (slot < 0) return;
    g_lr_stats[slot].live_count++;
    g_lr_stats[slot].live_bytes  += size;
    g_lr_stats[slot].total_calls++;
    g_lr_stats[slot].total_bytes += size;
}

static void lr_track_free(uint32_t lr, uint32_t size) {
    if (!s_libc_debug_mode) return;
    uint32_t h = (lr ^ (lr >> 15)) % LR_TABLE_SIZE;
    for (uint32_t i = 0; i < LR_TABLE_SIZE; ++i) {
        uint32_t idx = (h + i) % LR_TABLE_SIZE;
        if (g_lr_stats[idx].total_calls == 0) return; /* never tracked - nothing to do */
        if (g_lr_stats[idx].lr == lr) {
            if (g_lr_stats[idx].live_count > 0) g_lr_stats[idx].live_count--;
            if (g_lr_stats[idx].live_bytes >= size) g_lr_stats[idx].live_bytes -= size;
            else g_lr_stats[idx].live_bytes = 0;
            return;
        }
    }
}

void bridge_libc_dump_leak_report(int top_n) {
    if (!s_libc_debug_mode) return;
    int idx[LR_TABLE_SIZE];
    int n = 0;
    for (int i = 0; i < LR_TABLE_SIZE; ++i) {
        if (g_lr_stats[i].total_calls > 0) idx[n++] = i;
    }
    if (top_n > n) top_n = n;
    /* Partial selection sort - n is at most 256, top_n is small. */
    for (int i = 0; i < top_n; ++i) {
        int best = i;
        for (int j = i + 1; j < n; ++j) {
            if (g_lr_stats[idx[j]].live_bytes > g_lr_stats[idx[best]].live_bytes) best = j;
        }
        int tmp = idx[i]; idx[i] = idx[best]; idx[best] = tmp;
    }
    fprintf(stderr, "[MEM] Top %d live allocation call-site(s) by unfreed bytes (of %d tracked):\n", top_n, n);
    for (int i = 0; i < top_n; ++i) {
        lr_stat_t *s = &g_lr_stats[idx[i]];
        fprintf(stderr, "  lr=0x%08X  live=%u allocs, %llu KB live  (%llu total calls this run)"
                        "  first_seen=(frame=%u scene=0x%08X game=0x%08X)\n",
                s->lr, s->live_count,
                (unsigned long long)(s->live_bytes >> 10),
                (unsigned long long)s->total_calls,
                s->first_frame, s->first_scene, s->first_game);
    }
}

/* A site with huge total_bytes but small live_bytes allocates-and-frees
 * constantly without ever leaking - invisible to dump_leak_report (which
 * sorts by live bytes) no matter how much churn it's responsible for. That
 * churn is exactly what fragments a segregated free-list heap: memory gets
 * freed correctly but into pieces the wrong size for whatever comes next.
 * This ranks by gross bytes-ever-allocated instead, to surface it. */
void bridge_libc_dump_churn_report(int top_n) {
    if (!s_libc_debug_mode) return;
    int idx[LR_TABLE_SIZE];
    int n = 0;
    for (int i = 0; i < LR_TABLE_SIZE; ++i) {
        if (g_lr_stats[i].total_calls > 0) idx[n++] = i;
    }
    if (top_n > n) top_n = n;
    for (int i = 0; i < top_n; ++i) {
        int best = i;
        for (int j = i + 1; j < n; ++j) {
            if (g_lr_stats[idx[j]].total_bytes > g_lr_stats[idx[best]].total_bytes) best = j;
        }
        int tmp = idx[i]; idx[i] = idx[best]; idx[best] = tmp;
    }
    fprintf(stderr, "[MEM] Top %d allocation call-site(s) by total throughput (of %d tracked):\n", top_n, n);
    for (int i = 0; i < top_n; ++i) {
        lr_stat_t *s = &g_lr_stats[idx[i]];
        fprintf(stderr, "  lr=0x%08X  %llu KB EVER allocated (%llu calls), only %llu KB (%u allocs) still live"
                        "  first_seen=(frame=%u scene=0x%08X game=0x%08X)\n",
                s->lr,
                (unsigned long long)(s->total_bytes >> 10),
                (unsigned long long)s->total_calls,
                (unsigned long long)(s->live_bytes >> 10), s->live_count,
                s->first_frame, s->first_scene, s->first_game);
    }
}

/* --- Missing-asset tracking -----------------------------------------------
 * Every fopen() that fails gets recorded here (deduplicated by sanitized
 * path) along with the guest lr of its first failing call, so a single
 * summary at shutdown tells you every asset the port is missing and which
 * subsystem asked for it, instead of scrolling through possibly hundreds
 * of repeated fopen-failed lines across a whole session. */
#define MISSING_ASSET_TABLE_SIZE 128

typedef struct {
    char     path[128];
    uint32_t first_lr;
    uint32_t count;
    uint32_t first_scene;
    uint32_t first_frame;
} missing_asset_t;

static missing_asset_t g_missing_assets[MISSING_ASSET_TABLE_SIZE];
static int g_missing_asset_count = 0;

static bool record_missing_asset(const char *path, uint32_t lr) {
    if (!s_libc_debug_mode) return false;
    for (int i = 0; i < g_missing_asset_count; ++i) {
        if (strcmp(g_missing_assets[i].path, path) == 0) {
            g_missing_assets[i].count++;
            return false;
        }
    }
    if (g_missing_asset_count < MISSING_ASSET_TABLE_SIZE) {
        missing_asset_t *m = &g_missing_assets[g_missing_asset_count++];
        strncpy(m->path, path, sizeof(m->path) - 1);
        m->path[sizeof(m->path) - 1] = '\0';
        m->first_lr = lr;
        m->count = 1;
        m->first_scene = g_ctx_scene;
        m->first_frame = g_ctx_frame;
        return true;
    }
    return false;
}

void bridge_libc_dump_missing_assets(void) {
    if (!s_libc_debug_mode) return;
    if (g_missing_asset_count == 0) {
        fprintf(stderr, "[ASSETS] No missing-asset fopen() failures this run.\n");
        return;
    }
    fprintf(stderr, "[ASSETS] %d distinct missing file(s) requested this run:\n", g_missing_asset_count);
    for (int i = 0; i < g_missing_asset_count; ++i) {
        fprintf(stderr, "  '%s'  (requested %u time(s), first from lr=0x%08X, frame=%u scene=0x%08X)\n",
                g_missing_assets[i].path, g_missing_assets[i].count, g_missing_assets[i].first_lr,
                g_missing_assets[i].first_frame, g_missing_assets[i].first_scene);
    }
}

#define NUM_HEAP_BINS    28
#define HEAP_BLOCK_MAGIC 0x5333444Du  /* 'S3DM' - allocated */
#define HEAP_FREE_MAGIC  0x46524545u  /* 'FREE' - on free list  */
#define HEAP_MIN_SIZE    16u

typedef struct {
    uint32_t prev_size; /* payload capacity of physically preceding block (0 if at base) */
    uint32_t size;      /* payload capacity of this block (always multiple of 16) */
    uint32_t magic;     /* HEAP_BLOCK_MAGIC or HEAP_FREE_MAGIC */
    uint32_t owner_lr;  /* guest lr that allocated this block */
} guest_block_t;

/* When a block is free (magic == HEAP_FREE_MAGIC), its payload (at blk_addr + 16)
 * holds doubly-linked free list pointers and a boundary tag footer:
 *   [blk_addr + 16 + 0]: uint32_t next;
 *   [blk_addr + 16 + 4]: uint32_t prev;
 *   ...
 *   [blk_addr + 16 + size - 4]: uint32_t footer_size (matches blk->size)
 */
typedef struct {
    uint32_t next;
    uint32_t prev;
} guest_free_node_t;

static uint32_t g_free_bins[NUM_HEAP_BINS];

static int get_bin_index(uint32_t size) {
    if (size <= 16) return 0;
    int idx = 0;
    uint32_t s = (size - 1) >> 4;
    while (s > 0 && idx < NUM_HEAP_BINS - 1) { s >>= 1; idx++; }
    return idx;
}

static void heap_insert(uint32_t blk_addr, elf32_image_t *img) {
    guest_block_t *blk = (guest_block_t*)G_PTR(blk_addr);
    if (!blk) return;
    blk->magic = HEAP_FREE_MAGIC;

    /* Write boundary tag footer at the tail of free payload */
    *(uint32_t*)G_PTR(blk_addr + 16 + blk->size - 4) = blk->size;

    int b = get_bin_index(blk->size);
    guest_free_node_t *node = (guest_free_node_t*)G_PTR(blk_addr + 16);
    if (!node) return;
    node->next = g_free_bins[b];
    node->prev = 0;

    if (g_free_bins[b]) {
        guest_free_node_t *head = (guest_free_node_t*)G_PTR(g_free_bins[b] + 16);
        if (head) head->prev = blk_addr;
    }
    g_free_bins[b] = blk_addr;
}

static void heap_unlink(uint32_t blk_addr, elf32_image_t *img) {
    guest_block_t *blk = (guest_block_t*)G_PTR(blk_addr);
    if (!blk) return;
    int b = get_bin_index(blk->size);
    guest_free_node_t *node = (guest_free_node_t*)G_PTR(blk_addr + 16);
    if (!node) return;

    if (node->prev) {
        guest_free_node_t *prev_node = (guest_free_node_t*)G_PTR(node->prev + 16);
        if (prev_node) prev_node->next = node->next;
    } else if (g_free_bins[b] == blk_addr) {
        g_free_bins[b] = node->next;
    }

    if (node->next) {
        guest_free_node_t *next_node = (guest_free_node_t*)G_PTR(node->next + 16);
        if (next_node) next_node->prev = node->prev;
    }

    node->next = 0;
    node->prev = 0;
}

static uint32_t guest_malloc(elf32_image_t *img, uint32_t size, uint32_t caller_lr) {
    if (size == 0) size = 1;
    if (!caller_lr) caller_lr = g_guest_lr;
    uint32_t needed = (size + 15u) & ~15u;
    int bin = get_bin_index(needed);

    /* Search free bins for a matching segregated block */
    for (int b = bin; b < NUM_HEAP_BINS; ++b) {
        uint32_t blk_addr = g_free_bins[b];
        int limit = 5000;
        while (blk_addr && limit-- > 0) {
            guest_block_t *blk = (guest_block_t*)G_PTR(blk_addr);
            guest_free_node_t *node = (guest_free_node_t*)G_PTR(blk_addr + 16);
            if (!blk || blk->magic != HEAP_FREE_MAGIC || !node) {
                blk_addr = node ? node->next : 0;
                continue;
            }

            uint32_t next_addr = node->next;

            if (blk->size >= needed) {
                heap_unlink(blk_addr, img);

                /* Split if remainder >= 32 bytes (16-byte header + 16-byte min payload) */
                if (blk->size >= needed + 32) {
                    uint32_t rem_addr = blk_addr + 16 + needed;
                    uint32_t rem_size = blk->size - needed - 16;
                    guest_block_t *rem = (guest_block_t*)G_PTR(rem_addr);
                    if (rem) {
                        rem->prev_size = needed;
                        rem->size      = rem_size;
                        rem->magic     = HEAP_FREE_MAGIC;
                        rem->owner_lr  = 0;

                        uint32_t after_rem = rem_addr + 16 + rem_size;
                        if (after_rem < g_heap_cur) {
                            guest_block_t *after_blk = (guest_block_t*)G_PTR(after_rem);
                            if (after_blk) after_blk->prev_size = rem_size;
                        }

                        heap_insert(rem_addr, img);
                        blk->size = needed;
                    }
                } else {
                    /* Not split: update following block's prev_size */
                    uint32_t after_blk_addr = blk_addr + 16 + blk->size;
                    if (after_blk_addr < g_heap_cur) {
                        guest_block_t *after_blk = (guest_block_t*)G_PTR(after_blk_addr);
                        if (after_blk) after_blk->prev_size = blk->size;
                    }
                }

                blk->magic = HEAP_BLOCK_MAGIC;
                blk->owner_lr = caller_lr;
                lr_track_alloc(caller_lr, blk->size);
                return blk_addr + 16;
            }

            blk_addr = next_addr;
        }
    }

    /* Allocate fresh block from the bump pointer arena */
    uint32_t total = 16 + needed;
    if (g_heap_cur + total > g_heap_max) {
        static time_t s_last_oom_print = 0;
        time_t now = time(NULL);
        if (now != s_last_oom_print) {
            s_last_oom_print = now;
            uint32_t used_mb = (g_heap_cur - 0x02000000u) >> 20;
            uint32_t free_mb = (g_heap_max - g_heap_cur) >> 20;
            uint32_t free_total = 0;
            for (int b2 = 0; b2 < NUM_HEAP_BINS; ++b2) {
                uint32_t fa = g_free_bins[b2];
                int limit2 = 2000;
                while (fa && limit2-- > 0) {
                    guest_block_t *fb = (guest_block_t*)G_PTR(fa);
                    guest_free_node_t *fn = (guest_free_node_t*)G_PTR(fa + 16);
                    if (!fb || fb->magic != HEAP_FREE_MAGIC || !fn) break;
                    free_total += fb->size;
                    fa = fn->next;
                }
            }
            fprintf(stderr, "[-] guest heap OOM: req=%u lr=0x%08X bump_used=%uMB bump_free=%uMB freelist_bytes=%u"
                            "  ctx=(frame=%u scene=0x%08X game=0x%08X)\n",
                    size, caller_lr, used_mb, free_mb, free_total,
                    g_ctx_frame, g_ctx_scene, g_ctx_game);
            bridge_libc_dump_leak_report(20);
            bridge_libc_dump_churn_report(20);
        }
        return 0;
    }

    uint32_t blk_addr = g_heap_cur;
    g_heap_cur += total;

    guest_block_t *blk = (guest_block_t*)G_PTR(blk_addr);
    /* blk->prev_size was initialized by preceding block (or 0 at base) */
    blk->size     = needed;
    blk->magic    = HEAP_BLOCK_MAGIC;
    blk->owner_lr = caller_lr;

    /* Initialize the next block's prev_size at new g_heap_cur */
    if (g_heap_cur + sizeof(guest_block_t) <= g_heap_max) {
        guest_block_t *next_blk = (guest_block_t*)G_PTR(g_heap_cur);
        if (next_blk) next_blk->prev_size = needed;
    }

    lr_track_alloc(caller_lr, blk->size);
    return blk_addr + 16;
}

static uint32_t guest_free(elf32_image_t *img, uint32_t addr) {
    if (!addr || addr < 0x02000010u || addr >= g_heap_max) return 0;
    uint32_t blk_addr = addr - 16;
    guest_block_t *blk = (guest_block_t*)G_PTR(blk_addr);
    if (!blk || blk->magic != HEAP_BLOCK_MAGIC) return 0;

    uint32_t freed_size = blk->size;
    lr_track_free(blk->owner_lr, freed_size);
    g_acc_free_bytes += freed_size;
    blk->magic = HEAP_FREE_MAGIC;

    /* Forward-coalesce with contiguous free physical neighbors ahead */
    uint32_t next_addr;
    while ((next_addr = blk_addr + 16 + blk->size) < g_heap_cur) {
        guest_block_t *nx = (guest_block_t*)G_PTR(next_addr);
        if (!nx || nx->magic != HEAP_FREE_MAGIC) break;
        heap_unlink(next_addr, img);
        blk->size += 16 + nx->size;
        memset(nx, 0, sizeof(guest_block_t));
    }

    /* Backward-coalesce with contiguous free physical neighbor behind */
    if (blk->prev_size >= HEAP_MIN_SIZE) {
        uint32_t prev_addr = blk_addr - 16 - blk->prev_size;
        if (prev_addr >= 0x02000000u) {
            guest_block_t *pr = (guest_block_t*)G_PTR(prev_addr);
            if (pr && pr->magic == HEAP_FREE_MAGIC && pr->size == blk->prev_size) {
                heap_unlink(prev_addr, img);
                pr->size += 16 + blk->size;
                memset(blk, 0, sizeof(guest_block_t));
                blk_addr = prev_addr;
                blk = pr;
            }
        }
    }

    /* Reclaim trailing space if coalesced block reaches top of heap */
    if (blk_addr + 16 + blk->size >= g_heap_cur) {
        /* Shrink g_heap_cur directly - memory returned to bump pool */
        g_heap_cur = blk_addr;
        memset(blk, 0, sizeof(guest_block_t));
    } else {
        /* Not at top of heap: update following block's prev_size tag and insert into free bin */
        uint32_t after_addr = blk_addr + 16 + blk->size;
        guest_block_t *after_blk = (guest_block_t*)G_PTR(after_addr);
        if (after_blk) after_blk->prev_size = blk->size;

        heap_insert(blk_addr, img);
    }

    return freed_size;
}

/* Resolve real caller return-address for leak-attribution and profiling.
 * In libS3DClient.so, 0x014EB594 is the return address inside the engine's
 * inlined memory allocation wrapper (Pandora::EngineCore::Memory::Alloc / operator new).
 * When allocations are funneled through this wrapper, we reach up one stack frame
 * to [sp + 4] to attribute the allocation to the actual calling game subsystem. */
static inline uint32_t resolve_caller_lr(elf32_image_t *img, uint32_t sp) {
    uint32_t lr = g_guest_lr;
    if (lr == 0x014EB594u && sp && sp + 4 < img->mem_size) {
        uint32_t parent = *(uint32_t*)(img->mem + sp + 4);
        if (parent >= 0x01000000u && parent < 0x01000000u + img->mem_size) {
            lr = parent;
        }
    }
    return lr;
}

static uint32_t guest_realloc(elf32_image_t *img, uint32_t addr, uint32_t new_size, uint32_t caller_lr) {
    if (!caller_lr) caller_lr = g_guest_lr;
    if (addr == 0) return guest_malloc(img, new_size, caller_lr);
    if (new_size == 0) {
        guest_free(img, addr);
        return 0;
    }
    uint32_t blk_addr = addr - 16;
    guest_block_t *blk = (guest_block_t*)G_PTR(blk_addr);
    if (!blk || blk->magic != HEAP_BLOCK_MAGIC) {
        return guest_malloc(img, new_size, caller_lr);
    }
    uint32_t needed = (new_size + 15u) & ~15u;
    if (needed <= blk->size) {
        return addr; /* Already large enough */
    }

    /* Case A: block is at the top of the heap - expand g_heap_cur in-place */
    if (blk_addr + 16 + blk->size == g_heap_cur) {
        uint32_t extra = needed - blk->size;
        if (g_heap_cur + extra <= g_heap_max) {
            g_heap_cur += extra;
            lr_track_free(blk->owner_lr, blk->size);
            blk->size = needed;
            blk->owner_lr = caller_lr;
            lr_track_alloc(caller_lr, blk->size);
            if (g_heap_cur + sizeof(guest_block_t) <= g_heap_max) {
                guest_block_t *next_blk = (guest_block_t*)G_PTR(g_heap_cur);
                if (next_blk) next_blk->prev_size = needed;
            }
            return addr;
        }
    }

    /* Case B: block is followed by a contiguous free block large enough */
    uint32_t next_addr = blk_addr + 16 + blk->size;
    if (next_addr < g_heap_cur) {
        guest_block_t *nx = (guest_block_t*)G_PTR(next_addr);
        if (nx && nx->magic == HEAP_FREE_MAGIC && (blk->size + 16 + nx->size >= needed)) {
            heap_unlink(next_addr, img);
            uint32_t total_avail = blk->size + 16 + nx->size;
            lr_track_free(blk->owner_lr, blk->size);

            if (total_avail >= needed + 32) {
                blk->size = needed;
                uint32_t rem_addr = blk_addr + 16 + needed;
                uint32_t rem_size = total_avail - needed - 16;
                guest_block_t *rem = (guest_block_t*)G_PTR(rem_addr);
                rem->prev_size = needed;
                rem->size      = rem_size;
                rem->magic     = HEAP_FREE_MAGIC;
                rem->owner_lr  = 0;

                uint32_t after_rem = rem_addr + 16 + rem_size;
                if (after_rem < g_heap_cur) {
                    guest_block_t *af = (guest_block_t*)G_PTR(after_rem);
                    if (af) af->prev_size = rem_size;
                }
                heap_insert(rem_addr, img);
            } else {
                blk->size = total_avail;
                uint32_t after_blk = blk_addr + 16 + blk->size;
                if (after_blk < g_heap_cur) {
                    guest_block_t *af = (guest_block_t*)G_PTR(after_blk);
                    if (af) af->prev_size = blk->size;
                }
            }

            blk->owner_lr = caller_lr;
            lr_track_alloc(caller_lr, blk->size);
            return addr;
        }
    }

    /* Case C: general realloc */
    uint32_t new_addr = guest_malloc(img, new_size, caller_lr);
    void *dst = G_PTR(new_addr);
    void *src = G_PTR(addr);
    if (dst && src) {
        memcpy(dst, src, blk->size);
        guest_free(img, addr);
    }
    return new_addr;
}

static uint32_t guest_calloc(elf32_image_t *img, uint32_t num, uint32_t size, uint32_t caller_lr) {
    if (size && num > UINT32_MAX / size) return 0;
    uint32_t total = num * size;
    uint32_t addr = guest_malloc(img, total, caller_lr);
    void *p = G_PTR(addr);
    if (p) {
        memset(p, 0, total);
    }
    return addr;
}

static uint32_t guest_memalign(elf32_image_t *img, uint32_t alignment, uint32_t size, uint32_t caller_lr) {
    if (!caller_lr) caller_lr = g_guest_lr;
    if (size == 0) return 0;
    if (alignment <= 16) {
        return guest_malloc(img, size, caller_lr);
    }
    // Round alignment up to next power of 2 if needed
    if ((alignment & (alignment - 1)) != 0) {
        uint32_t a = 16;
        while (a < alignment) a <<= 1;
        alignment = a;
    }

    uint32_t needed = (size + 15u) & ~15u;
    if (needed < HEAP_MIN_SIZE) needed = HEAP_MIN_SIZE;

    // Over-allocate to ensure an aligned payload pointer with preceding 16-byte block header
    uint32_t alloc_extra = alignment + 32;
    uint32_t raw_addr = guest_malloc(img, needed + alloc_extra, caller_lr);
    if (!raw_addr) return 0;

    uint32_t raw_payload = raw_addr;
    uint32_t aligned_payload = (raw_payload + alignment - 1) & ~(alignment - 1);

    if (aligned_payload == raw_payload) {
        return aligned_payload;
    }

    uint32_t gap = aligned_payload - raw_payload;
    if (gap < 32) {
        aligned_payload += alignment;
        gap = aligned_payload - raw_payload;
    }

    uint32_t raw_blk_addr = raw_payload - 16;
    guest_block_t *raw_blk = (guest_block_t*)G_PTR(raw_blk_addr);
    uint32_t new_blk_addr = aligned_payload - 16;

    guest_block_t *lead_blk = raw_blk;
    uint32_t lead_size = gap - 16;
    lead_blk->size = lead_size;
    lead_blk->magic = HEAP_FREE_MAGIC;
    lead_blk->owner_lr = 0;
    lr_track_free(caller_lr, gap);

    guest_block_t *new_blk = (guest_block_t*)G_PTR(new_blk_addr);
    new_blk->prev_size = lead_size;
    new_blk->size = raw_blk->size - gap;
    new_blk->magic = HEAP_BLOCK_MAGIC;
    new_blk->owner_lr = caller_lr;

    uint32_t after_new = new_blk_addr + 16 + new_blk->size;
    if (after_new < g_heap_cur) {
        guest_block_t *af = (guest_block_t*)G_PTR(after_new);
        if (af) af->prev_size = new_blk->size;
    }

    heap_insert(raw_blk_addr, img);
    return aligned_payload;
}

static uint32_t wrap_malloc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t caller_lr = resolve_caller_lr(img, sp);
    if (r0 >= 1000000 && s_libc_debug_mode) {
        static uint32_t s_big_alloc_count = 0;
        if (s_big_alloc_count < 10) {
            s_big_alloc_count++;
            const char *fname = (r2 && r2 < img->mem_size && img->mem[r2]) ? (const char*)G_PTR(r2) : "unknown";
            fprintf(stderr, "[MEM_BIG] malloc size=%u caller_lr=0x%08X (file=%s line=%u)%s\n",
                    r0, caller_lr, fname, r3, s_big_alloc_count == 10 ? " [further big alloc logs throttled]" : "");
        }
    }
    uint32_t ptr = guest_malloc(img, r0, caller_lr);
    g_acc_malloc_calls++;
    if (ptr) g_acc_malloc_bytes += r0;
    return ptr;
}

static uint32_t wrap_calloc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t caller_lr = resolve_caller_lr(img, sp);
    uint32_t ptr = guest_calloc(img, r0, r1, caller_lr);
    g_acc_malloc_calls++;
    if (ptr) g_acc_malloc_bytes += (uint64_t)r0 * r1;
    return ptr;
}

static uint32_t wrap_realloc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t caller_lr = resolve_caller_lr(img, sp);
    uint32_t ptr = guest_realloc(img, r0, r1, caller_lr);
    g_acc_malloc_calls++;
    if (ptr) g_acc_malloc_bytes += r1;
    return ptr;
}

static uint32_t wrap_free(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    g_acc_free_calls++;
    guest_free(img, r0);
    return 0;
}

static uint32_t wrap_memalign(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t caller_lr = resolve_caller_lr(img, sp);
    uint32_t ptr = guest_memalign(img, r0, r1, caller_lr);
    g_acc_malloc_calls++;
    if (ptr) g_acc_malloc_bytes += r1;
    return ptr;
}

static uint32_t wrap_dlmalloc_usable_size(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 < 16) return 0;
    guest_block_t *blk = (guest_block_t*)G_PTR(r0 - 16);
    return (blk && blk->magic == HEAP_BLOCK_MAGIC) ? blk->size : 0;
}

/* String & memory operations */
static uint32_t wrap_memcpy(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 && r1 && r2) memcpy(G_PTR(r0), G_PTR(r1), r2);
    return r0;
}

static uint32_t wrap_memmove(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 && r1 && r2) memmove(G_PTR(r0), G_PTR(r1), r2);
    return r0;
}

static uint32_t wrap_memset(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 && r2) memset(G_PTR(r0), r1, r2);
    return r0;
}

static uint32_t wrap_memcmp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return (r0 && r1 && r2) ? (uint32_t)memcmp(G_PTR(r0), G_PTR(r1), r2) : 0;
}

static uint32_t wrap_memchr(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return 0;
    void *p = memchr(G_PTR(r0), r1, r2);
    return p ? (uint32_t)((uint8_t*)p - img->mem) : 0;
}

static uint32_t wrap_memrchr(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r2) return 0;
    const uint8_t *s = (const uint8_t*)G_PTR(r0);
    for (int i = (int)r2 - 1; i >= 0; --i) {
        if (s[i] == (uint8_t)r1) return r0 + i;
    }
    return 0;
}

static uint32_t wrap_memmem(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r2 || r1 < r3) return 0;
    const uint8_t *haystack = (const uint8_t*)G_PTR(r0);
    const uint8_t *needle = (const uint8_t*)G_PTR(r2);
    for (size_t i = 0; i <= r1 - r3; ++i) {
        if (memcmp(haystack + i, needle, r3) == 0) return r0 + (uint32_t)i;
    }
    return 0;
}

static uint32_t wrap_strlen(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s = (const char*)G_PTR(r0);
    return s ? (uint32_t)strlen(s) : 0;
}

static uint32_t wrap_strcmp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s1 = (const char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return (uint32_t)strcmp(s1, s2);
}

static uint32_t wrap_strncmp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s1 = (const char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return (uint32_t)strncmp(s1, s2, r2);
}

static uint32_t wrap_strcpy(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *s1 = (char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (s1 && s2) strcpy(s1, s2);
    return r0;
}

static uint32_t wrap_strncpy(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *s1 = (char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (s1 && s2) strncpy(s1, s2, r2);
    return r0;
}

static uint32_t wrap_strcat(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *s1 = (char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (s1 && s2) strcat(s1, s2);
    return r0;
}

static uint32_t wrap_strncat(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *s1 = (char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (s1 && s2) strncat(s1, s2, r2);
    return r0;
}

static uint32_t wrap_strlcat(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *dst = (char*)G_PTR(r0);
    const char *src = (const char*)G_PTR(r1);
    if (!dst || !src || !r2) return 0;
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);
    if (dlen >= r2) return (uint32_t)(r2 + slen);
    size_t copy = (slen < r2 - dlen - 1) ? slen : (r2 - dlen - 1);
    memcpy(dst + dlen, src, copy);
    dst[dlen + copy] = '\0';
    return (uint32_t)(dlen + slen);
}

static uint32_t wrap_strstr(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *s1 = (char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (!s1 || !s2) return 0;
    char *p = strstr(s1, s2);
    return p ? (uint32_t)((uint8_t*)p - img->mem) : 0;
}

static uint32_t wrap_strchr(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *s = (char*)G_PTR(r0);
    if (!s) return 0;
    char *p = strchr(s, (int)r1);
    return p ? (uint32_t)((uint8_t*)p - img->mem) : 0;
}

static uint32_t wrap_strrchr(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    char *s = (char*)G_PTR(r0);
    if (!s) return 0;
    char *p = strrchr(s, (int)r1);
    return p ? (uint32_t)((uint8_t*)p - img->mem) : 0;
}

static uint32_t wrap_strcspn(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s1 = (const char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    return (s1 && s2) ? (uint32_t)strcspn(s1, s2) : 0;
}

static uint32_t wrap_strdup(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s = (const char*)G_PTR(r0);
    if (!s) return 0;
    size_t len = strlen(s) + 1;
    uint32_t addr = guest_malloc(img, len, g_guest_lr);
    void *dst = G_PTR(addr);
    if (dst) memcpy(dst, s, len);
    return addr;
}

static uint32_t wrap_strcasecmp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s1 = (const char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return (uint32_t)_stricmp(s1, s2);
}

static uint32_t wrap_strncasecmp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s1 = (const char*)G_PTR(r0);
    const char *s2 = (const char*)G_PTR(r1);
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return (uint32_t)_strnicmp(s1, s2, r2);
}

static uint32_t wrap_strtol(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s = (const char*)G_PTR(r0);
    if (!s) return 0;
    char *endptr = NULL;
    long val = strtol(s, &endptr, (int)r2);
    void *dst = G_PTR(r1);
    if (dst && endptr) {
        uint32_t end_addr = r0 + (uint32_t)(endptr - s);
        memcpy(dst, &end_addr, 4);
    }
    return (uint32_t)val;
}

static uint32_t wrap_strtoul(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s = (const char*)G_PTR(r0);
    if (!s) return 0;
    char *endptr = NULL;
    unsigned long val = strtoul(s, &endptr, (int)r2);
    void *dst = G_PTR(r1);
    if (dst && endptr) {
        uint32_t end_addr = r0 + (uint32_t)(endptr - s);
        memcpy(dst, &end_addr, 4);
    }
    return (uint32_t)val;
}

static uint32_t wrap_strtod(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s = (const char*)G_PTR(r0);
    if (!s) return 0;
    char *endptr = NULL;
    double val = strtod(s, &endptr);
    void *dst = G_PTR(r1);
    if (dst && endptr) {
        uint32_t end_addr = r0 + (uint32_t)(endptr - s);
        memcpy(dst, &end_addr, 4);
    }
    uint32_t parts[2];
    memcpy(parts, &val, 8);
    g_guest_ret_r1 = parts[1];
    g_has_guest_ret_r1 = 1;
    return parts[0];
}

static uint32_t wrap_atoi(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s = (const char*)G_PTR(r0);
    return s ? (uint32_t)atoi(s) : 0;
}

static uint32_t wrap_atol(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *s = (const char*)G_PTR(r0);
    return s ? (uint32_t)atol(s) : 0;
}

static uint32_t wrap_basename(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *path = (const char*)G_PTR(r0);
    if (!path) return 0;
    const char *last_slash = strrchr(path, '/');
    const char *last_bslash = strrchr(path, '\\');
    const char *p = path;
    if (last_slash && last_slash >= p) p = last_slash + 1;
    if (last_bslash && last_bslash >= p) p = last_bslash + 1;
    return r0 + (uint32_t)(p - path);
}

static uint32_t wrap_strerror(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *msg = strerror((int)r0);
    if (!msg) msg = "Unknown error";
    uint32_t addr = GUEST_DATA_IMPORT_BASE + 0x700;
    char *dst = (char*)G_PTR(addr);
    if (dst) strncpy(dst, msg, 63);
    return addr;
}

static uint32_t wrap_strerror_r(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r1 && r2) {
        const char *msg = strerror((int)r0);
        strncpy((char*)G_PTR(r1), msg ? msg : "Unknown error", r2);
    }
    return 0;
}

static uint32_t wrap_strtok_r(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r1 || !r2) return 0;
    char *str = r0 ? (char*)G_PTR(r0) : NULL;
    const char *delim = (const char*)G_PTR(r1);
    uint32_t *save_ptr = (uint32_t*)G_PTR(r2);
    char *save = *save_ptr ? (char*)G_PTR(*save_ptr) : NULL;
    char *res = strtok_r(str, delim, &save);
    if (res) {
        *save_ptr = (uint32_t)((uint8_t*)save - img->mem);
        return (uint32_t)((uint8_t*)res - img->mem);
    }
    *save_ptr = 0;
    return 0;
}

/* Ctype */
static uint32_t wrap_tolower(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return (uint32_t)tolower((int)r0); }
static uint32_t wrap_toupper(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return (uint32_t)toupper((int)r0); }
static uint32_t wrap_isalnum(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return isalnum((int)r0) != 0; }
static uint32_t wrap_isalpha(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return isalpha((int)r0) != 0; }
static uint32_t wrap_isdigit(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return isdigit((int)r0) != 0; }
static uint32_t wrap_isspace(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return isspace((int)r0) != 0; }
static uint32_t wrap_isxdigit(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return isxdigit((int)r0) != 0; }
static uint32_t wrap_ispunct(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return ispunct((int)r0) != 0; }
static uint32_t wrap_isupper(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return isupper((int)r0) != 0; }
static uint32_t wrap_islower(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return islower((int)r0) != 0; }
static uint32_t wrap_iscntrl(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return iscntrl((int)r0) != 0; }

/* File I/O */
#define MAX_OPEN_FILES 2048
static FILE *g_file_table[MAX_OPEN_FILES];

/* -----------------------------------------------------------------------
 * Virtual STK handle system
 *
 * The engine opens S3DMain.stk thousands of times (once per asset load)
 * and never calls fclose on those handles. On real Android/Vita the OS
 * recycles file descriptors, so this is fine. On our bridge the flat
 * table fills permanently and the game crashes.
 *
 * Solution: intercept every fopen for a path ending in "S3DMain.stk".
 * We keep ONE real FILE* open persistently (g_stk_file). For each
 * virtual open we allocate a slot in g_stk_slots[] and return a handle
 * in the range [STK_VFD_BASE, STK_VFD_BASE+MAX_STK_SLOTS). Each slot
 * remembers its own seek position. fread/fseek/ftell/fclose on a virtual
 * handle use the shared FILE* with positional seek, never closing it.
 * ----------------------------------------------------------------------- */
#define MAX_STK_SLOTS 65536
#define STK_VFD_BASE  0x10000u

static FILE    *g_stk_file = NULL;
static char     g_stk_path[512] = {0};  /* remembered so we can reopen on demand */
static int64_t  g_stk_pos[MAX_STK_SLOTS];   /* -1 = slot free */
static uint32_t g_stk_vfd_alloc = 0;        /* telemetry: total virtual opens */

/* Ensure g_stk_file is open; reopen from g_stk_path if needed. */
static FILE *stk_ensure_open(void) {
    if (g_stk_file) return g_stk_file;
    if (g_stk_path[0]) {
        g_stk_file = fopen(g_stk_path, "rb");
        if (g_stk_file)
            fprintf(stderr, "[+] stk_ensure_open: reopened %s\n", g_stk_path);
        else
            fprintf(stderr, "[-] stk_ensure_open: FAILED to reopen %s\n", g_stk_path);
    }
    return g_stk_file;
}

static void stk_slots_init(void) {
    for (int i = 0; i < MAX_STK_SLOTS; ++i) g_stk_pos[i] = -1;
}

static int is_stk_vfd(uint32_t fd) {
    return fd >= STK_VFD_BASE && fd < STK_VFD_BASE + MAX_STK_SLOTS;
}

static int stk_slot(uint32_t fd) {
    return (int)(fd - STK_VFD_BASE);
}

/* Open a new virtual STK handle; returns the virtual FD.
 * If all slots are in use, evict the slot after the last-evicted one
 * (round-robin). This is safe because the engine never re-uses a handle
 * after the matching fopen/fread/fclose sequence; any evicted slot will
 * have already finished its read pass. */
static uint32_t stk_vfd_open(void) {
    /* Search for an available inactive slot */
    for (int i = 0; i < MAX_STK_SLOTS; ++i) {
        if (g_stk_pos[i] == -1) {
            g_stk_pos[i] = 0;
            g_stk_vfd_alloc++;
            return STK_VFD_BASE + (uint32_t)i;
        }
    }
    /* Evict next slot in round-robin order when table is fully saturated */
    static int s_evict_next = 0;
    static uint32_t s_stk_evict_count = 0;
    int slot = s_evict_next;
    s_evict_next = (s_evict_next + 1) % MAX_STK_SLOTS;
    s_stk_evict_count++;
    if (s_stk_evict_count <= 10 || (s_stk_evict_count % 100 == 0)) {
        fprintf(stderr, "[!] WARNING: STK virtual-FD table exhausted (MAX_STK_SLOTS=%d)! Evicting slot %d (total evictions=%u)\n",
                MAX_STK_SLOTS, slot, s_stk_evict_count);
    }
    g_stk_pos[slot] = 0;
    g_stk_vfd_alloc++;
    return STK_VFD_BASE + (uint32_t)slot;
}

/* -----------------------------------------------------------------------
 * In-memory Virtual File System (VFS) for temporary files (~temp*.xkv, etc.)
 *
 * During startup and shop initialization (aiStoreAndInv), the game decrypts
 * XML store definitions into "~tempgd!ms!k!gr!c!m!gff!crs.xkv" (offers) and
 * "~tempgd!ms!k!gr!c!m!n!g!q.xkv" (weapon catalog), writes them via fopen,
 * parses them via xml.parseFile, and neglects to delete them from disk.
 *
 * In addition, the engine attempts to create "./.nomedia" (Android media scanner
 * exclusion file, completely useless on Windows).
 *
 * We virtualize both:
 * 1) Any path containing ".nomedia" is directed to the system null device
 *    ("nul" on Windows) so 0 files are written to disk.
 * 2) Any temporary XML file (~temp*.xkv) is captured into an in-memory VFS
 *    buffer in RAM. The XML parser reads it directly from memory without
 *    ever touching the physical hard drive.
 * ----------------------------------------------------------------------- */
#define MEM_VFD_BASE   0x20000u
#define MAX_MEM_VFDS   64
#define MAX_MEM_FILES  16

typedef struct {
    char name[128];
    uint8_t *data;
    size_t size;
    size_t capacity;
} mem_vfs_file_t;

typedef struct {
    int file_idx;    /* index in s_mem_vfs_files, -1 if closed */
    size_t pos;      /* current cursor offset */
    int is_write;    /* 1 if opened for write */
} mem_vfs_slot_t;

static mem_vfs_file_t s_mem_vfs_files[MAX_MEM_FILES];
static mem_vfs_slot_t s_mem_vfs_slots[MAX_MEM_VFDS];

static void mem_vfs_init(void) {
    for (int i = 0; i < MAX_MEM_VFDS; ++i) {
        s_mem_vfs_slots[i].file_idx = -1;
        s_mem_vfs_slots[i].pos = 0;
        s_mem_vfs_slots[i].is_write = 0;
    }
}

void bridge_libc_cleanup_mem_vfs(void) {
    for (int i = 0; i < MAX_MEM_FILES; ++i) {
        if (s_mem_vfs_files[i].data) {
            free(s_mem_vfs_files[i].data);
            s_mem_vfs_files[i].data = NULL;
        }
        s_mem_vfs_files[i].size = 0;
        s_mem_vfs_files[i].capacity = 0;
        s_mem_vfs_files[i].name[0] = '\0';
    }
    mem_vfs_init();
}

static inline int is_mem_vfd(uint32_t fd) {
    return (fd >= MEM_VFD_BASE && fd < MEM_VFD_BASE + MAX_MEM_VFDS);
}

static const char *get_path_basename(const char *path) {
    if (!path) return "";
    const char *p1 = strrchr(path, '/');
    const char *p2 = strrchr(path, '\\');
    const char *base = p1 > p2 ? p1 : p2;
    return base ? base + 1 : path;
}

static inline int is_nomedia_path(const char *path, const char *raw_path) {
    if (path && (strstr(path, ".nomedia") || strstr(path, "nomedia"))) return 1;
    if (raw_path && (strstr(raw_path, ".nomedia") || strstr(raw_path, "nomedia"))) return 1;
    return 0;
}

static inline int is_temp_vfs_path(const char *path, const char *raw_path) {
    if (path && (strstr(path, "~temp") || strstr(path, ".xkv"))) return 1;
    if (raw_path && (strstr(raw_path, "~temp") || strstr(raw_path, ".xkv"))) return 1;
    return 0;
}

static FILE *open_null_device(const char *mode) {
#ifdef _WIN32
    return fopen("nul", mode);
#else
    return fopen("/dev/null", mode);
#endif
}

static uint32_t mem_vfs_open(const char *path, const char *mode) {
    if (!path || !mode) return 0;
    const char *base = get_path_basename(path);
    int is_write = (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL);

    int f_idx = -1;
    for (int i = 0; i < MAX_MEM_FILES; ++i) {
        if (s_mem_vfs_files[i].name[0] && strcmp(s_mem_vfs_files[i].name, base) == 0) {
            f_idx = i;
            break;
        }
    }

    if (is_write) {
        if (f_idx == -1) {
            for (int i = 0; i < MAX_MEM_FILES; ++i) {
                if (!s_mem_vfs_files[i].name[0]) {
                    f_idx = i;
                    strncpy(s_mem_vfs_files[i].name, base, sizeof(s_mem_vfs_files[i].name) - 1);
                    break;
                }
            }
        }
        if (f_idx == -1) {
            f_idx = 0;
            strncpy(s_mem_vfs_files[0].name, base, sizeof(s_mem_vfs_files[0].name) - 1);
        }
        s_mem_vfs_files[f_idx].size = 0;
    } else {
        if (f_idx == -1) return 0; /* File does not exist for reading */
    }

    for (int h = 0; h < MAX_MEM_VFDS; ++h) {
        if (s_mem_vfs_slots[h].file_idx == -1) {
            s_mem_vfs_slots[h].file_idx = f_idx;
            s_mem_vfs_slots[h].pos = 0;
            s_mem_vfs_slots[h].is_write = is_write;
            return MEM_VFD_BASE + (uint32_t)h;
        }
    }
    return 0;
}

static void mem_vfs_close(uint32_t fd) {
    if (!is_mem_vfd(fd)) return;
    int h = (int)(fd - MEM_VFD_BASE);
    s_mem_vfs_slots[h].file_idx = -1;
    s_mem_vfs_slots[h].pos = 0;
}

static size_t mem_vfs_write(uint32_t fd, const void *src, size_t size) {
    if (!is_mem_vfd(fd) || !src || size == 0) return 0;
    int h = (int)(fd - MEM_VFD_BASE);
    int f_idx = s_mem_vfs_slots[h].file_idx;
    if (f_idx < 0 || f_idx >= MAX_MEM_FILES) return 0;

    mem_vfs_file_t *file = &s_mem_vfs_files[f_idx];
    size_t needed = s_mem_vfs_slots[h].pos + size;
    if (needed > file->capacity) {
        size_t new_cap = file->capacity ? file->capacity * 2 : 16384;
        while (new_cap < needed) new_cap *= 2;
        uint8_t *new_data = (uint8_t*)realloc(file->data, new_cap);
        if (!new_data) return 0;
        file->data = new_data;
        file->capacity = new_cap;
    }

    memcpy(file->data + s_mem_vfs_slots[h].pos, src, size);
    s_mem_vfs_slots[h].pos += size;
    if (s_mem_vfs_slots[h].pos > file->size) {
        file->size = s_mem_vfs_slots[h].pos;
    }
    return size;
}

static size_t mem_vfs_read(uint32_t fd, void *dst, size_t size) {
    if (!is_mem_vfd(fd) || !dst || size == 0) return 0;
    int h = (int)(fd - MEM_VFD_BASE);
    int f_idx = s_mem_vfs_slots[h].file_idx;
    if (f_idx < 0 || f_idx >= MAX_MEM_FILES) return 0;

    mem_vfs_file_t *file = &s_mem_vfs_files[f_idx];
    if (s_mem_vfs_slots[h].pos >= file->size) return 0;

    size_t avail = file->size - s_mem_vfs_slots[h].pos;
    if (size > avail) size = avail;
    memcpy(dst, file->data + s_mem_vfs_slots[h].pos, size);
    s_mem_vfs_slots[h].pos += size;
    return size;
}

static int mem_vfs_seek(uint32_t fd, int64_t offset, int whence) {
    if (!is_mem_vfd(fd)) return -1;
    int h = (int)(fd - MEM_VFD_BASE);
    int f_idx = s_mem_vfs_slots[h].file_idx;
    if (f_idx < 0 || f_idx >= MAX_MEM_FILES) return -1;

    mem_vfs_file_t *file = &s_mem_vfs_files[f_idx];
    int64_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)s_mem_vfs_slots[h].pos;
    else if (whence == SEEK_END) base = (int64_t)file->size;
    else return -1;

    int64_t new_pos = base + offset;
    if (new_pos < 0) new_pos = 0;
    if ((size_t)new_pos > file->size) new_pos = file->size;
    s_mem_vfs_slots[h].pos = (size_t)new_pos;
    return 0;
}

static int64_t mem_vfs_tell(uint32_t fd) {
    if (!is_mem_vfd(fd)) return -1;
    int h = (int)(fd - MEM_VFD_BASE);
    return (int64_t)s_mem_vfs_slots[h].pos;
}

static int mem_vfs_getc(uint32_t fd) {
    uint8_t c;
    if (mem_vfs_read(fd, &c, 1) == 1) return (int)c;
    return -1;
}

static int mem_vfs_putc(uint32_t fd, int c) {
    uint8_t ch = (uint8_t)c;
    if (mem_vfs_write(fd, &ch, 1) == 1) return (int)ch;
    return -1;
}

static char *mem_vfs_gets(uint32_t fd, char *buf, int max_len) {
    if (max_len <= 1 || !buf) return NULL;
    int i = 0;
    while (i < max_len - 1) {
        int c = mem_vfs_getc(fd);
        if (c < 0) {
            if (i == 0) return NULL;
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

static int mem_vfs_puts(uint32_t fd, const char *str) {
    if (!str) return -1;
    size_t len = strlen(str);
    return (mem_vfs_write(fd, str, len) == len) ? 0 : -1;
}

static size_t mem_vfs_size(uint32_t fd) {
    if (!is_mem_vfd(fd)) return 0;
    int h = (int)(fd - MEM_VFD_BASE);
    int f_idx = s_mem_vfs_slots[h].file_idx;
    if (f_idx >= 0 && f_idx < MAX_MEM_FILES) {
        return s_mem_vfs_files[f_idx].size;
    }
    return 0;
}

static int mem_vfs_stat_path(const char *path, size_t *out_size) {
    if (!path) return 0;
    const char *base = get_path_basename(path);
    for (int i = 0; i < MAX_MEM_FILES; ++i) {
        if (s_mem_vfs_files[i].name[0] && strcmp(s_mem_vfs_files[i].name, base) == 0) {
            if (out_size) *out_size = s_mem_vfs_files[i].size;
            return 1;
        }
    }
    return 0;
}

static void mem_vfs_delete(const char *path) {
    if (!path) return;
    const char *base = get_path_basename(path);
    for (int i = 0; i < MAX_MEM_FILES; ++i) {
        if (s_mem_vfs_files[i].name[0] && strcmp(s_mem_vfs_files[i].name, base) == 0) {
            if (s_mem_vfs_files[i].data) {
                free(s_mem_vfs_files[i].data);
                s_mem_vfs_files[i].data = NULL;
            }
            s_mem_vfs_files[i].size = 0;
            s_mem_vfs_files[i].capacity = 0;
            s_mem_vfs_files[i].name[0] = '\0';
            break;
        }
    }
}

/* --------------------------
 * Real FD table helpers
 * -------------------------- */
static uint32_t alloc_fd(FILE *f) {
    for (uint32_t i = 3; i < MAX_OPEN_FILES; ++i) {
        if (!g_file_table[i]) {
            g_file_table[i] = f;
            return i;
        }
    }
    fprintf(stderr, "[-] alloc_fd: file table exhausted! (MAX_OPEN_FILES=%d)\n", MAX_OPEN_FILES);
    return 0;
}

static FILE *get_fd(elf32_image_t *img, uint32_t fd) {
    if (fd == 0) return stdin;
    if (fd == 1) return stdout;
    if (fd == 2) return stderr;
    if (fd >= 3 && fd < MAX_OPEN_FILES) return g_file_table[fd];

    if (img && fd < img->mem_size) {
        for (uint32_t i = 0; i < img->data_import_count; ++i) {
            if (strcmp(img->data_import_names[i], "__sF") == 0) {
                uint32_t sf_addr = img->data_import_addrs[i];
                if (fd >= sf_addr && fd < sf_addr + 3 * 96) {
                    uint32_t s = (fd - sf_addr) / 96;
                    if (s == 0) return stdin;
                    if (s == 1) return stdout;
                    if (s == 2) return stderr;
                }
            } else if (strcmp(img->data_import_names[i], "stderr") == 0 && fd == img->data_import_addrs[i]) {
                return stderr;
            } else if (strcmp(img->data_import_names[i], "stdout") == 0 && fd == img->data_import_addrs[i]) {
                return stdout;
            } else if (strcmp(img->data_import_names[i], "stdin") == 0 && fd == img->data_import_addrs[i]) {
                return stdin;
            }
        }
    }
    return NULL;
}

static void free_fd(uint32_t fd) {
    if (fd >= 3 && fd < MAX_OPEN_FILES) g_file_table[fd] = NULL;
}

static void sanitize_path(char *dst, const char *src, size_t max_len) {
    if (!src) { dst[0] = '\0'; return; }
    const char *p = src;
    if (strncmp(p, "file://", 7) == 0) p += 7;
    else if (strncmp(p, "file:", 5) == 0) p += 5;

    if (strstr(p, "pop2/")) p = strstr(p, "pop2/") + 5;
    else if (strstr(p, "pop2\\")) p = strstr(p, "pop2\\") + 5;
    else if (strstr(p, "files/")) p = strstr(p, "files/") + 6;

    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) {
        p += 2;
    }
    while (*p == '/' || *p == '\\') p++;

    snprintf(dst, max_len, "%s/%s", g_root_dir, p);
    for (char *c = dst; *c; ++c) {
        if (*c == '/') *c = '\\';
    }
}

static void fill_stat32(void *dst, int64_t size, uint32_t mode) {
    memset(dst, 0, 128);
    uint32_t size32 = (uint32_t)size;
    uint32_t blksize = 4096;
    uint32_t blocks = (uint32_t)((size + 511) / 512);

    memcpy((uint8_t*)dst + 16, &mode, 4);       /* st_mode */
    memcpy((uint8_t*)dst + 44, &size32, 4);     /* st_size (packed 32-bit offset 44 / 0x2C) */
    memcpy((uint8_t*)dst + 48, &size, 8);       /* st_size (AAPCS 64-bit aligned offset 48 / 0x30) */
    memcpy((uint8_t*)dst + 56, &blksize, 4);    /* st_blksize */
    memcpy((uint8_t*)dst + 60, &blocks, 4);     /* st_blocks */
}

/* Helper: check if a path refers to S3DMain.stk */
static int path_is_stk(const char *path) {
    if (!path) return 0;
    /* Use case-insensitive substring search - handles any prefix/separator */
    const char *p = path;
    while (*p) {
        if ((p[0]=='S'||p[0]=='s') &&
            (p[1]=='3') &&
            (p[2]=='D'||p[2]=='d') &&
            (p[3]=='M'||p[3]=='m') &&
            (p[4]=='a'||p[4]=='A') &&
            (p[5]=='i'||p[5]=='I') &&
            (p[6]=='n'||p[6]=='N') &&
            (p[7]=='.') &&
            (p[8]=='s'||p[8]=='S') &&
            (p[9]=='t'||p[9]=='T') &&
            (p[10]=='k'||p[10]=='K') &&
            (p[11]=='\0'||p[11]=='/'||p[11]=='\\'||p[11]=='?'))
            return 1;
        p++;
    }
    return 0;
}

static FILE *try_open_candidate(const char *candidate, const char *mode) {
    if (!candidate || !candidate[0]) return NULL;
    return fopen(candidate, mode);
}

/* Check if a loose file exists directly on disk to serve as an override.
 * Checks sanitized path first, then raw path if distinct. */
static FILE *fopen_loose_override(const char *path, const char *raw_path, const char *mode) {
    FILE *f = try_open_candidate(path, mode);
    if (!f && raw_path && strcmp(path, raw_path) != 0) {
        f = try_open_candidate(raw_path, mode);
    }
    return f;
}

/* Helper: check if a path refers to an optional configuration file that
 * the engine attempts to load but defaults gracefully if absent. */
static bool is_optional_config(const char *path, const char *raw) {
    if (!path && !raw) return false;
    const char *check_paths[2] = { path, raw };
    for (int i = 0; i < 2; ++i) {
        const char *p = check_paths[i];
        if (!p || !p[0]) continue;
        if (strstr(p, "S3DClient.cfg") || strstr(p, "s3dclient.cfg")) return true;
    }
    return false;
}

/* Helper: check if a path refers to a game asset whose primary home is S3DMain.stk.
 * In ShiVa3D, S3DMain.stk is the primary container for all models, textures,
 * animations, audio, scenes, and shaders. When the engine probes for a loose
 * asset override and none exists on disk, returning NULL immediately instructs
 * the engine to load the asset from S3DMain.stk without error. */
static bool is_asset_path(const char *path, const char *raw) {
    if (!path && !raw) return false;
    const char *check_paths[2] = { path, raw };
    for (int i = 0; i < 2; ++i) {
        const char *p = check_paths[i];
        if (!p || !p[0]) continue;

        /* Check known asset / cache directories */
        if (strstr(p, "Resources") || strstr(p, "resources") ||
            strstr(p, "Textures")  || strstr(p, "textures")  ||
            strstr(p, "Models")    || strstr(p, "models")    ||
            strstr(p, "Sounds")    || strstr(p, "sounds")    ||
            strstr(p, "Scenes")    || strstr(p, "scenes")    ||
            strstr(p, "Shaders")   || strstr(p, "shaders")   ||
            strstr(p, "Musics")    || strstr(p, "musics")    ||
            strstr(p, "Fonts")     || strstr(p, "fonts")     ||
            strstr(p, "Particles") || strstr(p, "particles") ||
            strstr(p, "Materials") || strstr(p, "materials") ||
            strstr(p, "Animations")|| strstr(p, "animations")||
            strstr(p, "Cache")     || strstr(p, "cache")) {
            return true;
        }

        /* Check known asset extensions (stripping query string if present) */
        const char *dot = strrchr(p, '.');
        if (dot) {
            char ext[16];
            size_t elen = 0;
            while (dot[elen] && dot[elen] != '?' && dot[elen] != '/' && dot[elen] != '\\' && elen < sizeof(ext) - 1) {
                ext[elen] = dot[elen];
                elen++;
            }
            ext[elen] = '\0';
#ifdef _WIN32
            #define EXT_MATCH(s) (_stricmp(ext, s) == 0)
#else
            #define EXT_MATCH(s) (strcasecmp(ext, s) == 0)
#endif
            if (EXT_MATCH(".etc") || EXT_MATCH(".ddz") || EXT_MATCH(".dds") ||
                EXT_MATCH(".pvr") || EXT_MATCH(".png") || EXT_MATCH(".tga") ||
                EXT_MATCH(".jpg") || EXT_MATCH(".jpeg")|| EXT_MATCH(".bmp") ||
                EXT_MATCH(".mdo") || EXT_MATCH(".ani") || EXT_MATCH(".wav") ||
                EXT_MATCH(".mp3") || EXT_MATCH(".ogg") || EXT_MATCH(".sho") ||
                EXT_MATCH(".fxo") || EXT_MATCH(".scn") || EXT_MATCH(".stk") ||
                EXT_MATCH(".s01") || EXT_MATCH(".s02") || EXT_MATCH(".s03")) {
#undef EXT_MATCH
                return true;
            }
#undef EXT_MATCH
        }
    }
    return false;
}

static uint32_t wrap_fopen(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r1) return 0;
    const char *raw_path = (const char*)G_PTR(r0);
    const char *mode = (const char*)G_PTR(r1);
    char path[PATH_MAX];
    sanitize_path(path, raw_path, sizeof(path));

    /* --- Android .nomedia exclusion file: redirect to null device --- */
    if (is_nomedia_path(path, raw_path)) {
        FILE *nf = open_null_device(mode);
        if (nf) {
            uint32_t fd = alloc_fd(nf);
            if (fd) return fd;
            fclose(nf);
        }
        return 0;
    }

    /* --- Virtual In-Memory Temporary Files (~temp*.xkv) --- */
    if (is_temp_vfs_path(path, raw_path)) {
        uint32_t vfd = mem_vfs_open(path, mode);
        if (!vfd && raw_path && strcmp(path, raw_path) != 0) {
            vfd = mem_vfs_open(raw_path, mode);
        }
        return vfd;
    }

    /* --- Primary Asset Archive: intercept opens of S3DMain.stk --- */
    if (path_is_stk(path) || path_is_stk(raw_path)) {
        /* Open the real FILE* once and reuse it forever */
        if (!g_stk_file) {
            g_stk_file = fopen(path, "rb");
            if (!g_stk_file) g_stk_file = fopen(raw_path, "rb");
            if (g_stk_file && !g_stk_path[0]) {
                /* Remember whichever path worked for potential reopen */
                strncpy(g_stk_path, path, sizeof(g_stk_path) - 1);
            }
            if (!g_stk_file) {
                fprintf(stderr, "[-] fopen STK failed: '%s' (sanitized: '%s')\n", raw_path, path);
                return 0;
            }
        }
        uint32_t vfd = stk_vfd_open();
        return vfd; /* always valid now - eviction fallback ensures non-zero */
    }

    /* --- Loose File Override: check if file exists directly on disk --- */
    FILE *f = fopen_loose_override(path, raw_path, mode);
    if (f) {
        uint32_t fd = alloc_fd(f);
        if (!fd) { fclose(f); return 0; }
        return fd;
    }

    /* --- S3DMain.stk Primary VFS Fallthrough ---
     * In ShiVa3D, S3DMain.stk is the primary container for all assets. The engine's
     * asset loader probes for loose files first as an override; returning 0 immediately
     * directs the engine to read the asset from its internal STK archive without error. */
    if (is_asset_path(path, raw_path)) {
        static int s_verbose_assets = -1;
        if (s_verbose_assets == -1) {
            s_verbose_assets = (getenv("SNF_VERBOSE_ASSETS") != NULL) ? 1 : 0;
        }
        if (s_verbose_assets) {
            fprintf(stderr, "[ASSETS] Loose asset not found, delegating to S3DMain.stk: '%s'\n", raw_path);
        }
        return 0;
    }

    /* Optional engine configs that default gracefully if absent */
    if (is_optional_config(path, raw_path)) {
        return 0;
    }

    /* Genuinely unexpected missing file */
    if (record_missing_asset(path, g_guest_lr)) {
        fprintf(stderr, "[-] fopen failed: '%s' (sanitized: '%s', mode: '%s')\n", raw_path, path, mode);
    }
    return 0;
}

static uint32_t wrap_fclose(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0)) {
        mem_vfs_close(r0);
        return 0;
    }
    /* Virtual STK handle: just free the slot, don't close the shared FILE* */
    if (is_stk_vfd(r0)) {
        int s = stk_slot(r0);
        g_stk_pos[s] = -1;
        return 0;
    }
    FILE *f = get_fd(img, r0);
    if (f && r0 >= 3) {
        fclose(f);
        free_fd(r0);
    }
    return 0;
}

/* Seek the shared STK FILE* to the slot's saved position, perform I/O,
 * then save the new position back into the slot. Returns the new pos or -1.
 * Caller must hold the FILE* exclusively (single-threaded engine). */
static int64_t stk_seek_to(int slot) {
    FILE *f = stk_ensure_open();
    if (!f || g_stk_pos[slot] < 0) return -1;
#ifdef _WIN32
    return (_fseeki64(f, g_stk_pos[slot], SEEK_SET) == 0) ? g_stk_pos[slot] : -1;
#else
    return (fseek(f, (long)g_stk_pos[slot], SEEK_SET) == 0) ? g_stk_pos[slot] : -1;
#endif
}

static uint32_t wrap_fread(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r3)) {
        if (!r0 || r1 == 0 || r2 == 0) return 0;
        size_t total_bytes = (size_t)r1 * (size_t)r2;
        size_t bytes_read = mem_vfs_read(r3, G_PTR(r0), total_bytes);
        return (uint32_t)(bytes_read / r1);
    }
    if (is_stk_vfd(r3)) {
        int s = stk_slot(r3);
        FILE *sf = stk_ensure_open();
        if (!r0 || !sf || stk_seek_to(s) < 0) return 0;
        size_t n = fread(G_PTR(r0), r1, r2, sf);
#ifdef _WIN32
        g_stk_pos[s] = _ftelli64(sf);
#else
        g_stk_pos[s] = ftell(sf);
#endif
        return (uint32_t)n;
    }
    FILE *f = get_fd(img, r3);
    return (f && r0) ? (uint32_t)fread(G_PTR(r0), r1, r2, f) : 0;
}

static uint32_t wrap_fwrite(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r3)) {
        if (!r0 || r1 == 0 || r2 == 0) return 0;
        size_t total_bytes = (size_t)r1 * (size_t)r2;
        size_t bytes_written = mem_vfs_write(r3, G_PTR(r0), total_bytes);
        return (uint32_t)(bytes_written / r1);
    }
    /* STK is read-only; fall through to normal path (will be NULL) */
    FILE *f = get_fd(img, r3);
    return (f && r0) ? (uint32_t)fwrite(G_PTR(r0), r1, r2, f) : 0;
}

static uint32_t wrap_fseek(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0)) {
        return (uint32_t)mem_vfs_seek(r0, (int64_t)(int32_t)r1, (int)r2);
    }
    if (is_stk_vfd(r0)) {
        int s = stk_slot(r0);
        FILE *sf = stk_ensure_open();
        if (!sf || g_stk_pos[s] < 0) return (uint32_t)-1;
        /* Compute new position without actually seeking (save for later) */
        int64_t base = 0;
        int whence = (int)r2;
        if (whence == SEEK_SET) {
            base = 0;
        } else if (whence == SEEK_CUR) {
            base = g_stk_pos[s];
        } else if (whence == SEEK_END) {
            /* Need real file size */
#ifdef _WIN32
            _fseeki64(sf, 0, SEEK_END);
            base = _ftelli64(sf);
#else
            fseek(sf, 0, SEEK_END);
            base = ftell(sf);
#endif
        }
        int64_t offset = (int64_t)(int32_t)r1;
        int64_t newpos = base + offset;
        if (newpos < 0) newpos = 0;
        g_stk_pos[s] = newpos;
        return 0;
    }
    FILE *f = get_fd(img, r0);
#ifdef _WIN32
    return f ? (uint32_t)_fseeki64(f, (int64_t)(int32_t)r1, (int)r2) : (uint32_t)-1;
#else
    return f ? (uint32_t)fseek(f, (long)r1, (int)r2) : (uint32_t)-1;
#endif
}

static uint32_t wrap_ftell(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0)) {
        return (uint32_t)mem_vfs_tell(r0);
    }
    if (is_stk_vfd(r0)) {
        int s = stk_slot(r0);
        return (g_stk_pos[s] >= 0) ? (uint32_t)g_stk_pos[s] : (uint32_t)-1;
    }
    FILE *f = get_fd(img, r0);
#ifdef _WIN32
    return f ? (uint32_t)_ftelli64(f) : (uint32_t)-1;
#else
    return f ? (uint32_t)ftell(f) : (uint32_t)-1;
#endif
}

static uint32_t wrap_fflush(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0) || is_stk_vfd(r0)) return 0;
    FILE *f = get_fd(img, r0);
    return f ? (uint32_t)fflush(f) : 0;
}

static uint32_t wrap_fgetc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0)) {
        return (uint32_t)mem_vfs_getc(r0);
    }
    if (is_stk_vfd(r0)) {
        int s = stk_slot(r0);
        FILE *sf = stk_ensure_open();
        if (!sf || stk_seek_to(s) < 0) return (uint32_t)-1;
        int c = fgetc(sf);
#ifdef _WIN32
        g_stk_pos[s] = _ftelli64(sf);
#else
        g_stk_pos[s] = ftell(sf);
#endif
        return (uint32_t)c;
    }
    FILE *f = get_fd(img, r0);
    return f ? (uint32_t)fgetc(f) : (uint32_t)-1;
}

static uint32_t wrap_fputc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r1)) {
        return (uint32_t)mem_vfs_putc(r1, (int)r0);
    }
    FILE *f = get_fd(img, r1);
    return f ? (uint32_t)fputc((int)r0, f) : (uint32_t)-1;
}

static uint32_t wrap_fgets(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r2)) {
        return mem_vfs_gets(r2, (char*)G_PTR(r0), (int)r1) ? r0 : 0;
    }
    FILE *f = get_fd(img, r2);
    if (f && r0) {
        char *res = fgets((char*)G_PTR(r0), (int)r1, f);
        return res ? r0 : 0;
    }
    return 0;
}

static uint32_t wrap_fputs(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r1)) {
        return (uint32_t)mem_vfs_puts(r1, (const char*)G_PTR(r0));
    }
    FILE *f = get_fd(img, r1);
    const char *str = (const char*)G_PTR(r0);
    return (f && str) ? (uint32_t)fputs(str, f) : (uint32_t)-1;
}

static uint32_t wrap_ungetc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    FILE *f = get_fd(img, r1);
    return f ? (uint32_t)ungetc((int)r0, f) : (uint32_t)-1;
}

static uint32_t wrap_ferror(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0) || is_stk_vfd(r0)) return 0;
    FILE *f = get_fd(img, r0);
    return f ? (uint32_t)ferror(f) : 0;
}

static uint32_t wrap_setvbuf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return 0;
}

/* POSIX File descriptor operations */
static uint32_t wrap_open(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return (uint32_t)-1;
    const char *raw_path = (const char*)G_PTR(r0);
    char path[PATH_MAX];
    sanitize_path(path, raw_path, sizeof(path));

    int flags = r1 & 3;
    const char *mode = "rb";
    if (flags == 1) mode = "wb";
    else if (flags == 2) mode = "r+b";
    if (r1 & 0x200) mode = "wb";

    /* --- Android .nomedia exclusion file: redirect to null device --- */
    if (is_nomedia_path(path, raw_path)) {
        FILE *nf = open_null_device(mode);
        if (nf) {
            uint32_t fd = alloc_fd(nf);
            if (fd) return fd;
            fclose(nf);
        }
        return (uint32_t)-1;
    }

    /* --- Virtual In-Memory Temporary Files (~temp*.xkv) --- */
    if (is_temp_vfs_path(path, raw_path)) {
        uint32_t vfd = mem_vfs_open(path, mode);
        if (!vfd && raw_path && strcmp(path, raw_path) != 0) {
            vfd = mem_vfs_open(raw_path, mode);
        }
        return vfd ? vfd : (uint32_t)-1;
    }

    /* Also intercept STK opens via POSIX open() */
    if (path_is_stk(path) || path_is_stk(raw_path)) {
        if (!g_stk_file) {
            g_stk_file = fopen(path, "rb");
            if (!g_stk_file) g_stk_file = fopen(raw_path, "rb");
            if (!g_stk_file) return (uint32_t)-1;
        }
        uint32_t vfd = stk_vfd_open();
        return vfd ? vfd : (uint32_t)-1;
    }

    FILE *f = fopen_loose_override(path, raw_path, mode);
    if (f) {
        uint32_t fd = alloc_fd(f);
        if (!fd) { fclose(f); return (uint32_t)-1; }
        return fd;
    }
    if (!is_asset_path(path, raw_path) && !is_optional_config(path, raw_path)) {
        fprintf(stderr, "[-] open failed: '%s' (sanitized: '%s')\n", raw_path, path);
    }
    return (uint32_t)-1;
}

static uint32_t wrap_close(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return wrap_fclose(img, r0, r1, r2, r3, sp);
}

static uint32_t wrap_read(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0)) {
        if (!r1) return (uint32_t)-1;
        return (uint32_t)mem_vfs_read(r0, G_PTR(r1), r2);
    }
    if (is_stk_vfd(r0)) {
        int s = stk_slot(r0);
        if (!r1 || !g_stk_file || stk_seek_to(s) < 0) return (uint32_t)-1;
        size_t n = fread(G_PTR(r1), 1, r2, g_stk_file);
#ifdef _WIN32
        g_stk_pos[s] = _ftelli64(g_stk_file);
#else
        g_stk_pos[s] = ftell(g_stk_file);
#endif
        return (uint32_t)n;
    }
    FILE *f = get_fd(img, r0);
    if (f && r1) return (uint32_t)fread(G_PTR(r1), 1, r2, f);
    return (uint32_t)-1;
}

static uint32_t wrap_write(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0)) {
        if (!r1) return (uint32_t)-1;
        return (uint32_t)mem_vfs_write(r0, G_PTR(r1), r2);
    }
    FILE *f = get_fd(img, r0);
    if (f && r1) return (uint32_t)fwrite(G_PTR(r1), 1, r2, f);
    return (uint32_t)-1;
}

static uint32_t wrap_lseek(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (is_mem_vfd(r0)) {
        if (mem_vfs_seek(r0, (int64_t)(int32_t)r1, (int)r2) == 0) {
            return (uint32_t)mem_vfs_tell(r0);
        }
        return (uint32_t)-1;
    }
    if (is_stk_vfd(r0)) {
        /* Reuse fseek logic then return new position */
        wrap_fseek(img, r0, r1, r2, r3, sp);
        return (uint32_t)g_stk_pos[stk_slot(r0)];
    }
    FILE *f = get_fd(img, r0);
    if (f) {
#ifdef _WIN32
        if (_fseeki64(f, (int64_t)(int32_t)r1, (int)r2) == 0) {
            return (uint32_t)_ftelli64(f);
        }
#else
        if (fseek(f, (long)r1, (int)r2) == 0) {
            return (uint32_t)ftell(f);
        }
#endif
    }
    return (uint32_t)-1;
}

static uint32_t wrap_stat(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r1) return (uint32_t)-1;
    const char *raw_path = (const char*)G_PTR(r0);
    char path[PATH_MAX];
    sanitize_path(path, raw_path, sizeof(path));

    if (is_nomedia_path(path, raw_path)) {
        return (uint32_t)-1;
    }
    if (is_temp_vfs_path(path, raw_path)) {
        size_t fsize = 0;
        if (mem_vfs_stat_path(path, &fsize) || mem_vfs_stat_path(raw_path, &fsize)) {
            fill_stat32(G_PTR(r1), (int64_t)fsize, 0100666);
            return 0;
        }
    }

    struct __stat64 st;
    if (_stat64(path, &st) == 0 || _stat64(raw_path, &st) == 0) {
        fill_stat32(G_PTR(r1), st.st_size, (uint32_t)st.st_mode);
        return 0;
    }
    if (path_is_stk(path) || path_is_stk(raw_path)) {
        FILE *sf = stk_ensure_open();
        if (sf) {
            int fd = _fileno(sf);
            if (_fstat64(fd, &st) == 0) {
                fill_stat32(G_PTR(r1), st.st_size, (uint32_t)st.st_mode);
                return 0;
            }
        }
    }
    if (!is_asset_path(path, raw_path) && !is_optional_config(path, raw_path)) {
        static int s_stat_fail_cnt = 0;
        if (s_stat_fail_cnt++ < 30) {
            fprintf(stderr, "[TRANSLATOR FILE_NOT_FOUND] stat failed: '%s' (sanitized: '%s')\n", raw_path, path);
        }
    }
    return (uint32_t)-1;
}

static uint32_t wrap_fstat(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r1) return (uint32_t)-1;
    if (is_mem_vfd(r0)) {
        size_t fsize = mem_vfs_size(r0);
        fill_stat32(G_PTR(r1), (int64_t)fsize, 0100666);
        return 0;
    }
    if (is_stk_vfd(r0)) {
        FILE *sf = stk_ensure_open();
        if (sf) {
            int fd = _fileno(sf);
            struct __stat64 st;
            if (_fstat64(fd, &st) == 0) {
                fill_stat32(G_PTR(r1), st.st_size, (uint32_t)st.st_mode);
                return 0;
            }
        }
        return (uint32_t)-1;
    }
    FILE *f = get_fd(img, r0);
    if (f) {
        int fd = _fileno(f);
        struct __stat64 st;
        if (_fstat64(fd, &st) == 0) {
            fill_stat32(G_PTR(r1), st.st_size, (uint32_t)st.st_mode);
            return 0;
        }
    }
    return (uint32_t)-1;
}

static uint32_t wrap_access(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return (uint32_t)-1;
    const char *raw_path = (const char*)G_PTR(r0);
    char path[PATH_MAX];
    sanitize_path(path, raw_path, sizeof(path));

    if (is_nomedia_path(path, raw_path)) {
        return (uint32_t)-1;
    }
    if (is_temp_vfs_path(path, raw_path)) {
        size_t fsize = 0;
        if (mem_vfs_stat_path(path, &fsize) || mem_vfs_stat_path(raw_path, &fsize)) {
            return 0;
        }
    }

    int res = _access(path, r1);
    if (res != 0) {
        res = _access(raw_path, r1);
    }
    if (res != 0) {
        if (!is_asset_path(path, raw_path) && !is_optional_config(path, raw_path)) {
            static int s_acc_fail_cnt = 0;
            if (s_acc_fail_cnt++ < 30) {
                fprintf(stderr, "[TRANSLATOR FILE_NOT_FOUND] access failed: '%s' (sanitized: '%s')\n", raw_path, path);
            }
        }
    }
    return res;
}

static uint32_t wrap_unlink(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return (uint32_t)-1;
    const char *raw_path = (const char*)G_PTR(r0);
    char path[PATH_MAX];
    sanitize_path(path, raw_path, sizeof(path));

    if (is_nomedia_path(path, raw_path)) {
        return 0;
    }
    if (is_temp_vfs_path(path, raw_path)) {
        mem_vfs_delete(path);
        mem_vfs_delete(raw_path);
        return 0;
    }
    return remove(path);
}

static uint32_t wrap_mkdir(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return (uint32_t)-1;
    char path[PATH_MAX];
    sanitize_path(path, (const char*)G_PTR(r0), sizeof(path));
    return _mkdir(path);
}

static uint32_t wrap_rmdir(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return (uint32_t)-1;
    char path[PATH_MAX];
    sanitize_path(path, (const char*)G_PTR(r0), sizeof(path));
    return _rmdir(path);
}

static uint32_t wrap_getcwd(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0) {
        strncpy((char*)G_PTR(r0), g_root_dir, r1);
        return r0;
    }
    return 0;
}

/* Logging & Printing */
static void format_guest_args(elf32_image_t *img, char *out, size_t max_out, const char *fmt, const uint32_t *args, int max_args) {
    if (!out || max_out == 0) return;
    if (!fmt) { out[0] = '\0'; return; }

    char *dst = out;
    char *dst_end = out + max_out - 1;
    int arg_idx = 0;

    for (const char *p = fmt; *p && dst < dst_end; ++p) {
        if (*p == '%' && *(p + 1)) {
            p++;
            if (*p == '%') {
                *dst++ = '%';
                continue;
            }
            char spec[32];
            spec[0] = '%';
            int sidx = 1;
            while (*p && !isalpha((unsigned char)*p) && sidx < 28) {
                spec[sidx++] = *p++;
            }
            spec[sidx++] = *p;
            spec[sidx] = '\0';

            char formatted[1024];
            formatted[0] = '\0';

            if (*p == 's') {
                uint32_t str_addr = (args && arg_idx < max_args) ? args[arg_idx++] : 0;
                const char *s = str_addr ? (const char*)G_PTR(str_addr) : NULL;
                snprintf(formatted, sizeof(formatted), spec, s ? s : "(null)");
            } else if (*p == 'f' || *p == 'g' || *p == 'e' || *p == 'G' || *p == 'E') {
                if (arg_idx & 1) arg_idx++;
                double dval = 0.0;
                if (args && arg_idx + 1 < max_args) {
                    memcpy(&dval, &args[arg_idx], 8);
                    arg_idx += 2;
                }
                snprintf(formatted, sizeof(formatted), spec, dval);
            } else if (*p == 'l' || *p == 'd' || *p == 'i' || *p == 'u' || *p == 'x' || *p == 'X' || *p == 'c' || *p == 'p') {
                if (strstr(spec, "ll") || strstr(spec, "I64")) {
                    if (arg_idx & 1) arg_idx++;
                    uint64_t val64 = 0;
                    if (args && arg_idx + 1 < max_args) {
                        memcpy(&val64, &args[arg_idx], 8);
                        arg_idx += 2;
                    }
                    snprintf(formatted, sizeof(formatted), spec, val64);
                } else {
                    uint32_t val = (args && arg_idx < max_args) ? args[arg_idx++] : 0;
                    snprintf(formatted, sizeof(formatted), spec, val);
                }
            } else {
                uint32_t val = (args && arg_idx < max_args) ? args[arg_idx++] : 0;
                snprintf(formatted, sizeof(formatted), spec, val);
            }

            size_t flen = strlen(formatted);
            size_t space = dst_end - dst;
            if (flen > space) flen = space;
            memcpy(dst, formatted, flen);
            dst += flen;
        } else {
            *dst++ = *p;
        }
    }
    *dst = '\0';
}

static uint32_t wrap_android_log_print(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!s_libc_debug_mode) return 1;
    const char *tag = (const char*)G_PTR(r1);
    const char *fmt = (const char*)G_PTR(r2);
    if (!tag) tag = "Android";
    if (!fmt) fmt = "";

    uint32_t args[32];
    args[0] = r3;
    for (int i = 0; i < 31; ++i) args[1 + i] = READ_STACK(i);

    char buf[2048];
    format_guest_args(img, buf, sizeof(buf), fmt, args, 32);
    printf("[Log:%s] %s\n", tag, buf);
    return 1;
}

static uint32_t wrap_android_log_vprint(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!s_libc_debug_mode) return 1;
    const char *tag = (const char*)G_PTR(r1);
    const char *fmt = (const char*)G_PTR(r2);
    const uint32_t *ap = (const uint32_t*)G_PTR(r3);
    if (!tag) tag = "Android";
    if (!fmt) fmt = "";

    char buf[2048];
    format_guest_args(img, buf, sizeof(buf), fmt, ap, ap ? 32 : 0);
    printf("[Log:%s] %s\n", tag, buf);
    return 1;
}

static uint32_t wrap_puts(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!s_libc_debug_mode) return 0;
    const char *s = (const char*)G_PTR(r0);
    return s ? (uint32_t)puts(s) : 0;
}

static uint32_t wrap_printf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!s_libc_debug_mode) return 0;
    const char *fmt = (const char*)G_PTR(r0);
    if (!fmt) return 0;

    uint32_t args[32];
    args[0] = r1;
    args[1] = r2;
    args[2] = r3;
    for (int i = 0; i < 29; ++i) args[3 + i] = READ_STACK(i);

    char buf[4096];
    format_guest_args(img, buf, sizeof(buf), fmt, args, 32);
    fputs(buf, stdout);
    fflush(stdout);
    return (uint32_t)strlen(buf);
}

static uint32_t wrap_fprintf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    FILE *f = get_fd(img, r0);
    const char *fmt = (const char*)G_PTR(r1);
    if (!fmt) return 0;

    uint32_t args[32];
    args[0] = r2;
    args[1] = r3;
    for (int i = 0; i < 30; ++i) args[2 + i] = READ_STACK(i);

    char buf[4096];
    format_guest_args(img, buf, sizeof(buf), fmt, args, 32);
    if (f) {
        fputs(buf, f);
        fflush(f);
    } else {
        printf("%s", buf);
        fflush(stdout);
    }
    return (uint32_t)strlen(buf);
}

static uint32_t wrap_vfprintf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    FILE *f = get_fd(img, r0);
    const char *fmt = (const char*)G_PTR(r1);
    const uint32_t *ap = (const uint32_t*)G_PTR(r2);
    if (!fmt) return 0;

    char buf[4096];
    format_guest_args(img, buf, sizeof(buf), fmt, ap, ap ? 32 : 0);
    if (f) {
        fputs(buf, f);
        fflush(f);
    } else {
        printf("%s", buf);
        fflush(stdout);
    }
    return (uint32_t)strlen(buf);
}

static uint32_t wrap_sprintf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r1) return 0;
    char *dst = (char*)G_PTR(r0);
    const char *fmt = (const char*)G_PTR(r1);
    if (!dst || !fmt) return 0;

    uint32_t args[32];
    args[0] = r2;
    args[1] = r3;
    for (int i = 0; i < 30; ++i) args[2 + i] = READ_STACK(i);

    char buf[4096];
    format_guest_args(img, buf, sizeof(buf), fmt, args, 32);
    strcpy(dst, buf);
    return (uint32_t)strlen(buf);
}

static uint32_t wrap_snprintf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r2) return 0;
    char *dst = (char*)G_PTR(r0);
    size_t max_len = r1;
    const char *fmt = (const char*)G_PTR(r2);
    if (!dst || !fmt) return 0;

    uint32_t args[32];
    args[0] = r3;
    for (int i = 0; i < 31; ++i) args[1 + i] = READ_STACK(i);

    char buf[4096];
    format_guest_args(img, buf, sizeof(buf), fmt, args, 32);
    if (max_len > 0) {
        strncpy(dst, buf, max_len - 1);
        dst[max_len - 1] = '\0';
    }
    return (uint32_t)strlen(buf);
}

static uint32_t wrap_vsprintf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r1) return 0;
    char *dst = (char*)G_PTR(r0);
    const char *fmt = (const char*)G_PTR(r1);
    const uint32_t *ap = (const uint32_t*)G_PTR(r2);
    if (!dst || !fmt) return 0;

    char buf[4096];
    format_guest_args(img, buf, sizeof(buf), fmt, ap, ap ? 32 : 0);
    strcpy(dst, buf);
    return (uint32_t)strlen(buf);
}

static uint32_t wrap_vsnprintf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r2) return 0;
    char *dst = (char*)G_PTR(r0);
    size_t max_len = r1;
    const char *fmt = (const char*)G_PTR(r2);
    const uint32_t *ap = (const uint32_t*)G_PTR(r3);
    if (!dst || !fmt) return 0;

    char buf[4096];
    format_guest_args(img, buf, sizeof(buf), fmt, ap, ap ? 32 : 0);
    if (max_len > 0) {
        strncpy(dst, buf, max_len - 1);
        dst[max_len - 1] = '\0';
    }
    return (uint32_t)strlen(buf);
}

static uint32_t wrap_sscanf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *str = (const char*)G_PTR(r0);
    const char *fmt = (const char*)G_PTR(r1);
    if (!str || !fmt) return 0;

    uint32_t arg_ptrs[16];
    arg_ptrs[0] = r2;
    arg_ptrs[1] = r3;
    for (int i = 0; i < 14; ++i) arg_ptrs[2 + i] = READ_STACK(i);

    void *host_ptrs[16];
    for (int i = 0; i < 16; ++i) {
        host_ptrs[i] = arg_ptrs[i] ? G_PTR(arg_ptrs[i]) : NULL;
    }
    return (uint32_t)sscanf(str, fmt, host_ptrs[0], host_ptrs[1], host_ptrs[2], host_ptrs[3],
                            host_ptrs[4], host_ptrs[5], host_ptrs[6], host_ptrs[7]);
}

static uint32_t wrap_vfscanf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return 0;
}

static uint32_t wrap_fdopen(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return r0;
}

/* ARM aeabi math */
static uint32_t wrap_aeabi_idiv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    int32_t a = (int32_t)r0;
    int32_t b = (int32_t)r1;
    return (b != 0) ? (uint32_t)(a / b) : 0;
}

static uint32_t wrap_aeabi_idivmod(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    int32_t a = (int32_t)r0;
    int32_t b = (int32_t)r1;
    if (b != 0) {
        g_guest_ret_r1 = (uint32_t)(a % b);
        g_has_guest_ret_r1 = 1;
        return (uint32_t)(a / b);
    }
    return 0;
}

static uint32_t wrap_aeabi_uidiv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return (r1 != 0) ? (r0 / r1) : 0;
}

static uint32_t wrap_aeabi_uidivmod(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r1 != 0) {
        g_guest_ret_r1 = r0 % r1;
        g_has_guest_ret_r1 = 1;
        return r0 / r1;
    }
    return 0;
}

static uint32_t wrap_aeabi_ldivmod(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    int64_t a = ((int64_t)r1 << 32) | r0;
    int64_t b = ((int64_t)r3 << 32) | r2;
    if (b != 0) {
        int64_t q = a / b;
        g_guest_ret_r1 = (uint32_t)(q >> 32);
        g_has_guest_ret_r1 = 1;
        return (uint32_t)q;
    }
    return 0;
}

static uint32_t wrap_aeabi_uldivmod(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint64_t a = ((uint64_t)r1 << 32) | r0;
    uint64_t b = ((uint64_t)r3 << 32) | r2;
    if (b != 0) {
        uint64_t q = a / b;
        g_guest_ret_r1 = (uint32_t)(q >> 32);
        g_has_guest_ret_r1 = 1;
        return (uint32_t)q;
    }
    return 0;
}

/* Math functions */
static uint32_t wrap_sinf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = sinf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_cosf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = cosf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_tanf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = tanf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_asinf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = asinf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_acosf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = acosf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_atanf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = atanf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_atan2f(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float y, x; memcpy(&y, &r0, 4); memcpy(&x, &r1, 4); float res = atan2f(y, x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_sqrtf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = sqrtf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_powf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x, y; memcpy(&x, &r0, 4); memcpy(&y, &r1, 4); float res = powf(x, y); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_expf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = expf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_logf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = logf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_log10f(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = log10f(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_ceilf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = ceilf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_floorf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x; memcpy(&x, &r0, 4); float res = floorf(x); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_fmodf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x, y; memcpy(&x, &r0, 4); memcpy(&y, &r1, 4); float res = fmodf(x, y); uint32_t out; memcpy(&out, &res, 4); return out;
}
static uint32_t wrap_floor_double(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    double val; uint32_t parts[2] = { r0, r1 }; memcpy(&val, parts, 8); double res = floor(val); memcpy(parts, &res, 8);
    g_guest_ret_r1 = parts[1]; g_has_guest_ret_r1 = 1; return parts[0];
}
static uint32_t wrap_sqrt_double(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    double val; uint32_t parts[2] = { r0, r1 }; memcpy(&val, parts, 8); double res = sqrt(val); memcpy(parts, &res, 8);
    g_guest_ret_r1 = parts[1]; g_has_guest_ret_r1 = 1; return parts[0];
}

static uint32_t wrap_fmaf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x, y, z;
    memcpy(&x, &r0, 4);
    memcpy(&y, &r1, 4);
    memcpy(&z, &r2, 4);
    float res = fmaf(x, y, z);
    uint32_t out;
    memcpy(&out, &res, 4);
    return out;
}

static uint32_t wrap_fmaxf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x, y;
    memcpy(&x, &r0, 4);
    memcpy(&y, &r1, 4);
    float res = fmaxf(x, y);
    uint32_t out;
    memcpy(&out, &res, 4);
    return out;
}

static uint32_t wrap_fminf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x, y;
    memcpy(&x, &r0, 4);
    memcpy(&y, &r1, 4);
    float res = fminf(x, y);
    uint32_t out;
    memcpy(&out, &res, 4);
    return out;
}

static uint32_t wrap_nextafterf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float x, y;
    memcpy(&x, &r0, 4);
    memcpy(&y, &r1, 4);
    float res = nextafterf(x, y);
    uint32_t out;
    memcpy(&out, &res, 4);
    return out;
}

static uint32_t wrap_frexp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t in_words[2] = { r0, r1 };
    double x;
    memcpy(&x, in_words, 8);
    int exp_val = 0;
    double res = frexp(x, &exp_val);
    void *dst = G_PTR(r2);
    if (dst) {
        memcpy(dst, &exp_val, 4);
    }
    uint32_t out_words[2];
    memcpy(out_words, &res, 8);
    g_guest_ret_r1 = out_words[1];
    g_has_guest_ret_r1 = 1;
    return out_words[0];
}

static uint32_t wrap_ldexp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t in_words[2] = { r0, r1 };
    double x;
    memcpy(&x, in_words, 8);
    double res = ldexp(x, (int)r2);
    uint32_t out_words[2];
    memcpy(out_words, &res, 8);
    g_guest_ret_r1 = out_words[1];
    g_has_guest_ret_r1 = 1;
    return out_words[0];
}

/* Directory operations */
#define MAX_OPEN_DIRS 32
static DIR *g_dir_table[MAX_OPEN_DIRS];

static uint32_t wrap_opendir(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return 0;
    const char *raw_path = (const char*)G_PTR(r0);
    char path[PATH_MAX];
    sanitize_path(path, raw_path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) d = opendir(raw_path);
    if (!d) return 0;
    for (uint32_t i = 1; i < MAX_OPEN_DIRS; ++i) {
        if (!g_dir_table[i]) {
            g_dir_table[i] = d;
            return i;
        }
    }
    closedir(d);
    return 0;
}

static uint32_t wrap_closedir(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 > 0 && r0 < MAX_OPEN_DIRS && g_dir_table[r0]) {
        closedir(g_dir_table[r0]);
        g_dir_table[r0] = NULL;
    }
    return 0;
}

static uint32_t wrap_readdir(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 == 0 || r0 >= MAX_OPEN_DIRS || !g_dir_table[r0]) return 0;
    struct dirent *de = readdir(g_dir_table[r0]);
    if (!de) return 0;
    uint32_t addr = GUEST_DATA_IMPORT_BASE + 0x800;
    uint8_t *slot = img->mem + addr;
    memset(slot, 0, 280);
    *(uint16_t*)(slot + 16) = 280;
    *(uint8_t*)(slot + 18) = 0; /* DT_UNKNOWN */
    strncpy((char*)(slot + 19), de->d_name, 255);
    return addr;
}

static uint32_t wrap_rewinddir(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 > 0 && r0 < MAX_OPEN_DIRS && g_dir_table[r0]) {
        rewinddir(g_dir_table[r0]);
    }
    return 0;
}

static uint8_t *g_img_mem_scandir = NULL;

static int scandir_alphasort_comparator(const void *a, const void *b) {
    uint32_t addr_a = *(const uint32_t *)a;
    uint32_t addr_b = *(const uint32_t *)b;
    if (!g_img_mem_scandir || !addr_a || !addr_b) return 0;
    const char *na = (const char *)(g_img_mem_scandir + addr_a + 19);
    const char *nb = (const char *)(g_img_mem_scandir + addr_b + 19);
    return strcoll(na, nb);
}

static uint32_t wrap_alphasort(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r1) return 0;
    uint32_t addr_a = *(uint32_t*)G_PTR(r0);
    uint32_t addr_b = *(uint32_t*)G_PTR(r1);
    if (!addr_a || !addr_b) return 0;
    const char *na = (const char*)(img->mem + addr_a + 19);
    const char *nb = (const char*)(img->mem + addr_b + 19);
    return (uint32_t)strcoll(na, nb);
}

static uint32_t wrap_scandir(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) {
        if (r1) *(uint32_t*)G_PTR(r1) = 0;
        return (uint32_t)-1;
    }
    const char *raw_path = (const char*)G_PTR(r0);
    char path[PATH_MAX];
    sanitize_path(path, raw_path, sizeof(path));

    DIR *d = opendir(path);
    if (!d) d = opendir(raw_path);
    if (!d) {
        if (s_libc_debug_mode) {
            fprintf(stderr, "[TRANSLATOR FILE_NOT_FOUND] scandir could not open '%s' (sanitized: '%s')\n", raw_path, path);
        }
        if (r1) *(uint32_t*)G_PTR(r1) = 0;
        return (uint32_t)-1;
    }

    if (s_libc_debug_mode) {
        fprintf(stderr, "[TRANSLATOR LIBC] scandir scanning '%s' (sanitized: '%s')\n", raw_path, path);
    }

    uint32_t cap = 64;
    uint32_t count = 0;
    uint32_t *entries = (uint32_t*)malloc(cap * sizeof(uint32_t));

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!de->d_name[0]) continue;

        // Allocate guest struct dirent (280 bytes)
        uint32_t entry_addr = guest_malloc(img, 280, g_guest_lr);
        if (!entry_addr) break;

        uint8_t *slot = img->mem + entry_addr;
        memset(slot, 0, 280);
        *(uint64_t*)(slot + 0) = 1;     // d_ino
        *(int64_t*)(slot + 8) = 0;      // d_off
        *(uint16_t*)(slot + 16) = 280;  // d_reclen

        // Determine file type
        char full_entry_path[1024];
        snprintf(full_entry_path, sizeof(full_entry_path), "%s/%s", path, de->d_name);
        struct __stat64 st;
        uint8_t dtype = 0; // DT_UNKNOWN
        if (_stat64(full_entry_path, &st) == 0) {
            if (st.st_mode & _S_IFDIR) dtype = 4; // DT_DIR
            else if (st.st_mode & _S_IFREG) dtype = 8; // DT_REG
        }
        *(uint8_t*)(slot + 18) = dtype;
        strncpy((char*)(slot + 19), de->d_name, 255);

        if (count >= cap) {
            cap *= 2;
            entries = (uint32_t*)realloc(entries, cap * sizeof(uint32_t));
        }
        entries[count++] = entry_addr;
    }
    closedir(d);

    // Sort if comparator was requested
    if (count > 0 && r3 != 0) {
        g_img_mem_scandir = img->mem;
        qsort(entries, count, sizeof(uint32_t), scandir_alphasort_comparator);
    }

    // Allocate namelist array in guest memory
    uint32_t list_size = (count > 0 ? count : 1) * sizeof(uint32_t);
    uint32_t list_addr = guest_malloc(img, list_size, g_guest_lr);
    if (list_addr && count > 0) {
        memcpy(img->mem + list_addr, entries, count * sizeof(uint32_t));
    }
    free(entries);

    if (r1) {
        *(uint32_t*)G_PTR(r1) = list_addr;
    }

    if (s_libc_debug_mode) {
        fprintf(stderr, "[TRANSLATOR LIBC] scandir completed '%s': found %u entries\n", path, count);
    }
    return count;
}

/* POSIX memory and process operations */
static uint32_t wrap_mmap(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t len = r1;
    if (len == 0) return (uint32_t)-1;
    uint32_t addr = guest_malloc(img, len, g_guest_lr);
    return addr ? addr : (uint32_t)-1;
}

static uint32_t wrap_munmap(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0) guest_free(img, r0);
    return 0;
}

static uint32_t wrap_gmtime_r(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || !r1) return 0;
    const time_t *tp = (const time_t*)G_PTR(r0);
    struct tm *res = (struct tm*)G_PTR(r1);
    if (tp && res) {
        struct tm *gm = gmtime(tp);
        if (gm) {
            *res = *gm;
            return r1;
        }
    }
    return 0;
}


static uint32_t wrap_dup(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return r0;
}

static elf32_image_t *s_qsort_img = NULL;

static int hud_zorder_compare(const void *a, const void *b) {
    uint32_t elemA = *(const uint32_t*)a;
    uint32_t elemB = *(const uint32_t*)b;
    if (!elemA || !elemB || elemA == elemB) return 0;
    if (!s_qsort_img || !s_qsort_img->mem) return 0;

    uint8_t *mem = s_qsort_img->mem;
    if (elemA + 0xa3 >= s_qsort_img->mem_size || elemB + 0xa3 >= s_qsort_img->mem_size) return 0;

    uint8_t zA = mem[elemA + 0xa3];
    uint8_t zB = mem[elemB + 0xa3];
    if (zA != zB) {
        return (zA < zB) ? -1 : 1;
    }

    // Tie-breaker: original engine compares index ascending (smaller index drawn first, larger index drawn on top)
    uint32_t idxA = *(uint32_t*)(mem + elemA);
    uint32_t idxB = *(uint32_t*)(mem + elemB);
    if (idxA != idxB) {
        return (idxA < idxB) ? -1 : 1;
    }
    return (elemA < elemB) ? -1 : 1;
}

static int render_cmp_643228(const void *a, const void *b) {
    const uint8_t *p0 = (const uint8_t*)a;
    const uint8_t *p1 = (const uint8_t*)b;
    uint16_t v2_0 = *(const uint16_t*)(p0 + 2);
    uint16_t v2_1 = *(const uint16_t*)(p1 + 2);
    if (v2_0 < v2_1) return 1;
    if (v2_0 > v2_1) return -1;

    uint32_t v8_0 = *(const uint32_t*)(p0 + 8);
    uint32_t v8_1 = *(const uint32_t*)(p1 + 8);
    if (v8_0 < v8_1) return -1;
    if (v8_0 > v8_1) return 1;

    float vf_0 = *(const float*)(p0 + 0x14);
    float vf_1 = *(const float*)(p1 + 0x14);
    if (vf_0 < vf_1) return -1;
    if (vf_0 > vf_1) return 1;

    uint32_t v4_0 = *(const uint32_t*)(p0 + 4);
    uint32_t v4_1 = *(const uint32_t*)(p1 + 4);
    if (v4_0 < v4_1) return -1;
    if (v4_0 > v4_1) return 1;

    uint32_t v10_0 = *(const uint32_t*)(p0 + 0x10);
    uint32_t v10_1 = *(const uint32_t*)(p1 + 0x10);
    if (v10_0 < v10_1) return -1;
    if (v10_0 == v10_1) return 0;
    return 1;
}

static int render_cmp_6432b0(const void *a, const void *b) {
    const uint8_t *p0 = (const uint8_t*)a;
    const uint8_t *p1 = (const uint8_t*)b;
    uint16_t v2_0 = *(const uint16_t*)(p0 + 2);
    uint16_t v2_1 = *(const uint16_t*)(p1 + 2);
    if (v2_0 < v2_1) return 1;
    if (v2_0 > v2_1) return -1;

    float vf_0 = *(const float*)(p0 + 0x14);
    float vf_1 = *(const float*)(p1 + 0x14);
    if (vf_0 < vf_1) return -1;
    if (vf_0 > vf_1) return 1;

    uint32_t v4_0 = *(const uint32_t*)(p0 + 4);
    uint32_t v4_1 = *(const uint32_t*)(p1 + 4);
    if (v4_0 < v4_1) return -1;
    if (v4_0 > v4_1) return 1;

    uint32_t v10_0 = *(const uint32_t*)(p0 + 0x10);
    uint32_t v10_1 = *(const uint32_t*)(p1 + 0x10);
    if (v10_0 < v10_1) return -1;
    if (v10_0 == v10_1) return 0;
    return 1;
}

static int render_cmp_643324(const void *a, const void *b) {
    const uint8_t *p0 = (const uint8_t*)a;
    const uint8_t *p1 = (const uint8_t*)b;
    uint16_t v2_0 = *(const uint16_t*)(p0 + 2);
    uint16_t v2_1 = *(const uint16_t*)(p1 + 2);
    if (v2_0 < v2_1) return 1;
    if (v2_0 > v2_1) return -1;

    float vf_0 = *(const float*)(p0 + 0x14);
    float vf_1 = *(const float*)(p1 + 0x14);
    if (vf_0 > vf_1) return -1;
    if (vf_0 < vf_1) return 1;

    uint32_t v4_0 = *(const uint32_t*)(p0 + 4);
    uint32_t v4_1 = *(const uint32_t*)(p1 + 4);
    if (v4_0 < v4_1) return -1;
    if (v4_0 > v4_1) return 1;

    uint32_t v10_0 = *(const uint32_t*)(p0 + 0x10);
    uint32_t v10_1 = *(const uint32_t*)(p1 + 0x10);
    if (v10_0 < v10_1) return -1;
    if (v10_0 == v10_1) return 0;
    return 1;
}

static int render_cmp_643398(const void *a, const void *b) {
    const uint8_t *p0 = (const uint8_t*)a;
    const uint8_t *p1 = (const uint8_t*)b;
    uint16_t v2_0 = *(const uint16_t*)(p0 + 2);
    uint16_t v2_1 = *(const uint16_t*)(p1 + 2);
    if (v2_0 < v2_1) return 1;
    if (v2_0 > v2_1) return -1;

    float vf_0 = *(const float*)(p0 + 0x14);
    float vf_1 = *(const float*)(p1 + 0x14);
    if (vf_0 < vf_1) return 1;
    if (vf_0 > vf_1) return -1;

    uint32_t v4_0 = *(const uint32_t*)(p0 + 4);
    uint32_t v4_1 = *(const uint32_t*)(p1 + 4);
    if (v4_0 < v4_1) return -1;
    if (v4_0 == v4_1) return 0;
    return 1;
}

static int render_cmp_6433f8(const void *a, const void *b) {
    const uint8_t *p0 = (const uint8_t*)a;
    const uint8_t *p1 = (const uint8_t*)b;
    uint16_t v2_0 = *(const uint16_t*)(p0 + 2);
    uint16_t v2_1 = *(const uint16_t*)(p1 + 2);
    if (v2_0 < v2_1) return 1;
    if (v2_0 > v2_1) return -1;

    uint32_t v4_0 = *(const uint32_t*)(p0 + 4);
    uint32_t v4_1 = *(const uint32_t*)(p1 + 4);
    if (v4_0 < v4_1) return -1;
    if (v4_0 == v4_1) return 0;
    return 1;
}

static int render_cmp_643438(const void *a, const void *b) {
    const uint8_t *p0 = (const uint8_t*)a;
    const uint8_t *p1 = (const uint8_t*)b;
    float vf_0 = *(const float*)p0;
    float vf_1 = *(const float*)p1;
    if (vf_0 > vf_1) return 1;
    if (vf_0 < vf_1) return -1;

    uint32_t v4_0 = *(const uint32_t*)(p0 + 4);
    uint32_t v4_1 = *(const uint32_t*)(p1 + 4);
    if (v4_0 < v4_1) return -1;
    if (v4_0 == v4_1) return 0;
    return 1;
}

static int render_cmp_66a45c(const void *a, const void *b) {
    if (!s_qsort_img || !s_qsort_img->mem) return 0;
    uint8_t *mem = s_qsort_img->mem;
    uint32_t ptrA = *(const uint32_t*)a;
    uint32_t ptrB = *(const uint32_t*)b;
    if (!ptrA || !ptrB || ptrA == ptrB) return 0;
    if (ptrA + 0x164 >= s_qsort_img->mem_size || ptrB + 0x164 >= s_qsort_img->mem_size) return 0;

    float s15_A = *(const float*)(mem + ptrA + 0x15c);
    float s14_A = *(const float*)(mem + ptrA + 0x160);
    float diff_A = s14_A - s15_A;

    float s15_B = *(const float*)(mem + ptrB + 0x15c);
    float s13_B = *(const float*)(mem + ptrB + 0x160);
    float diff_B = s13_B - s15_B;

    if (diff_A < diff_B) return -1;
    return 1;
}

static int render_cmp_65f200(const void *a, const void *b) {
    if (!s_qsort_img || !s_qsort_img->mem) return 0;
    uint8_t *mem = s_qsort_img->mem;
    uint32_t ptrA = *(const uint32_t*)a;
    uint32_t ptrB = *(const uint32_t*)b;
    if (!ptrA || !ptrB || ptrA == ptrB) return 0;
    if (ptrA + 0x180 >= s_qsort_img->mem_size || ptrB + 0x180 >= s_qsort_img->mem_size) return 0;

    uint32_t r2 = *(const uint32_t*)(mem + ptrA + 0x178);
    uint32_t r3 = *(const uint32_t*)(mem + ptrB + 0x178);
    if (!r2 || !r3 || r2 + 0x1c >= s_qsort_img->mem_size || r3 + 0x1c >= s_qsort_img->mem_size) return 0;

    uint32_t r2_c = *(const uint32_t*)(mem + r2 + 0xc);
    uint32_t r3_c = *(const uint32_t*)(mem + r3 + 0xc);
    if (!r2_c || !r3_c || r2_c + 0x1c >= s_qsort_img->mem_size || r3_c + 0x1c >= s_qsort_img->mem_size) return 0;

    uint32_t r1 = *(const uint32_t*)(mem + r2_c + 8);
    uint32_t r0 = *(const uint32_t*)(mem + r2_c + 0x18);
    uint32_t r3_8 = *(const uint32_t*)(mem + r3_c + 8);
    uint32_t r2_18 = *(const uint32_t*)(mem + r3_c + 0x18);

    if (r1 & 1) {
        if (!(r3_8 & 1)) return -1;
    } else {
        if (r3_8 & 1) return 1;
    }

    if (r0 < r2_18) return 1;
    if (r0 == r2_18) return 0;
    return -1;
}

static int render_cmp_5e8a1c(const void *a, const void *b) {
    if (!s_qsort_img || !s_qsort_img->mem) return 0;
    uint8_t *mem = s_qsort_img->mem;
    uint32_t r3 = *(const uint32_t*)a;
    uint32_t r2 = *(const uint32_t*)b;
    if (!r3 || !r2 || r3 + 0x90 >= s_qsort_img->mem_size || r2 + 0x90 >= s_qsort_img->mem_size) return 0;

    uint8_t p0_8e = mem[r3 + 0x8e];
    uint8_t p1_8e = mem[r2 + 0x8e];
    if (p0_8e < p1_8e) return -1;
    if (p0_8e > p1_8e) return 1;

    uint8_t p0_8c = mem[r3 + 0x8c];
    uint8_t p1_8c = mem[r2 + 0x8c];
    if (p0_8c < p1_8c) return 1;
    if (p0_8c > p1_8c) return -1;

    uint32_t p0_0c = *(const uint32_t*)(mem + r3 + 0xc);
    uint32_t p1_0c = *(const uint32_t*)(mem + r2 + 0xc);
    if (p0_0c < p1_0c) return -1;
    if (p0_0c > p1_0c) return 1;
    return 0;
}

static int render_cmp_797d38(const void *a, const void *b) {
    const uint8_t *p0 = (const uint8_t*)a;
    const uint8_t *p1 = (const uint8_t*)b;
    float dist_a = *(const float*)(p0 + 4);
    float dist_b = *(const float*)(p1 + 4);
    if (dist_a > dist_b) return 1;
    if (dist_a < dist_b) return -1;
    return 0;
}

static int generic_default_compare(const void *a, const void *b) {
    uint32_t valA = *(const uint32_t*)a;
    uint32_t valB = *(const uint32_t*)b;
    if (valA < valB) return -1;
    if (valA > valB) return 1;
    return 0;
}

/* -----------------------------------------------------------------------
 * qsort dispatch: native implementation of engine comparators.
 *
 * NOTE ON HARDCODED COMPARATOR ADDRESSES:
 * The Android libS3DClient.so binary invokes qsort() with internal static
 * comparator functions that lack exported symbols. Running these comparators
 * via Dynarmic ARM execution would incur severe context-switching overhead
 * (hundreds of JIT roundtrips per sort). Therefore, the comparator logic
 * was reverse-engineered and re-implemented in native C:
 *   - 0x610ba8 / HUDTree::SortElementsByZOrderFunc: HUD element z-ordering
 *   - 0x66a45c, 0x65f200, 0x5e8a1c, 0x797d38: Render/particle layer ordering
 *   - 0x643228 .. 0x643438: 32-byte spatial node sorting
 * If an unrecognized comparator is encountered, an unhandled warning is logged
 * with element hex dump, and generic_default_compare is used as a fallback.
 * ----------------------------------------------------------------------- */
static uint32_t wrap_qsort(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0 || r1 <= 1 || r2 == 0) return 0;
    if (r0 + r1 * r2 > img->mem_size) return 0;

    static uint32_t s_sym_hud_sort = 0;
    if (!s_sym_hud_sort) {
        s_sym_hud_sort = elf32_lookup_symbol(img, "_ZN7Pandora10EngineCore7HUDTree24SortElementsByZOrderFuncEPKvS3_");
    }

    uint32_t target_r3 = (r3 & ~1u);
    if (target_r3 >= 0x01000000u) {
        target_r3 -= 0x01000000u;
    }

    if (r2 == 4) {
        if (target_r3 == 0x610ba8 || (s_sym_hud_sort && (r3 & ~1u) == (s_sym_hud_sort & ~1u))) {
            s_qsort_img = img;
            qsort(img->mem + r0, r1, r2, hud_zorder_compare);
            return 0;
        }
        if (target_r3 == 0x66a45c) {
            s_qsort_img = img;
            qsort(img->mem + r0, r1, r2, render_cmp_66a45c);
            return 0;
        }
        if (target_r3 == 0x65f200) {
            s_qsort_img = img;
            qsort(img->mem + r0, r1, r2, render_cmp_65f200);
            return 0;
        }
        if (target_r3 == 0x5e8a1c) {
            s_qsort_img = img;
            qsort(img->mem + r0, r1, r2, render_cmp_5e8a1c);
            return 0;
        }
    }

    if (r2 == 8) {
        if (target_r3 == 0x797d38) {
            qsort(img->mem + r0, r1, r2, render_cmp_797d38);
            return 0;
        }
    }

    if (r2 == 32) {
        if (target_r3 == 0x643228) { qsort(img->mem + r0, r1, r2, render_cmp_643228); return 0; }
        if (target_r3 == 0x6432b0) { qsort(img->mem + r0, r1, r2, render_cmp_6432b0); return 0; }
        if (target_r3 == 0x643324) { qsort(img->mem + r0, r1, r2, render_cmp_643324); return 0; }
        if (target_r3 == 0x643398) { qsort(img->mem + r0, r1, r2, render_cmp_643398); return 0; }
        if (target_r3 == 0x6433f8) { qsort(img->mem + r0, r1, r2, render_cmp_6433f8); return 0; }
        if (target_r3 == 0x643438) { qsort(img->mem + r0, r1, r2, render_cmp_643438); return 0; }
    }

    if (s_libc_debug_mode) {
        static int s_unhandled_cmp_cnt = 0;
        if (s_unhandled_cmp_cnt++ < 20) {
            fprintf(stderr, "[TRANSLATOR WARNING] qsort unhandled comparator r3=0x%08X (target=0x%06X, count=%u, size=%u) lr=0x%08X - sorting with generic fallback\n",
                    r3, target_r3, r1, r2, g_guest_lr);
            uint32_t dump_len = (r2 < 32) ? r2 : 32;
            if (r0 + dump_len <= img->mem_size) {
                fprintf(stderr, "  first element bytes:");
                for (uint32_t i = 0; i < dump_len; ++i) {
                    fprintf(stderr, " %02X", img->mem[r0 + i]);
                }
                fprintf(stderr, "\n");
            }
        }
    }
    if (r2 >= 4) {
        qsort(img->mem + r0, r1, r2, generic_default_compare);
    }
    return 0;
}

/* Time functions */
static uint32_t wrap_time(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    time_t t = time(NULL);
    void *p = G_PTR(r0);
    if (p) memcpy(p, &t, 4);
    return (uint32_t)t;
}

static uint32_t wrap_gettimeofday(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    void *p0 = G_PTR(r0);
    void *p4 = G_PTR(r0 + 4);
    if (p0 && p4) {
        static LARGE_INTEGER freq, start;
        static int init = 0;
        if (!init) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); init = 1; }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        uint64_t us = (now.QuadPart - start.QuadPart) * 1000000ull / freq.QuadPart;
        uint32_t tv_sec = (uint32_t)(us / 1000000ull);
        uint32_t tv_usec = (uint32_t)(us % 1000000ull);
        memcpy(p0, &tv_sec, 4);
        memcpy(p4, &tv_usec, 4);
    }
    return 0;
}

static uint32_t wrap_clock_gettime(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    void *p0 = G_PTR(r1);
    void *p4 = G_PTR(r1 + 4);
    if (p0 && p4) {
        static LARGE_INTEGER freq, start;
        static int init = 0;
        if (!init) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); init = 1; }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        uint64_t ns = (now.QuadPart - start.QuadPart) * 1000000000ull / freq.QuadPart;
        uint32_t tv_sec = (uint32_t)(ns / 1000000000ull);
        uint32_t tv_nsec = (uint32_t)(ns % 1000000000ull);
        memcpy(p0, &tv_sec, 4);
        memcpy(p4, &tv_nsec, 4);
    }
    return 0;
}

static uint32_t wrap_usleep(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    uint32_t ms = r0 / 1000;
    if (ms > 50) ms = 50;
    Sleep(ms);
    return 0;
}

static uint32_t wrap_sleep(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    Sleep(r0 * 1000);
    return 0;
}

static uint32_t wrap_localtime(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    void *src = G_PTR(r0);
    if (!src) return 0;
    time_t t; memcpy(&t, src, sizeof(time_t));
    struct tm *tm_val = localtime(&t);
    if (!tm_val) return 0;
    uint32_t addr = GUEST_DATA_IMPORT_BASE + 0x600;
    void *dst = G_PTR(addr);
    if (dst) memcpy(dst, tm_val, sizeof(struct tm));
    return addr;
}

/* Random & Misc */
static uint32_t wrap_rand(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return (uint32_t)rand(); }
static uint32_t wrap_srand(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { srand(r0); return 0; }
static uint32_t wrap_errno(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return GUEST_DATA_IMPORT_BASE + 0x800; }
static uint32_t wrap_getenv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (s_libc_debug_mode) {
        const char *name = r0 ? (const char*)G_PTR(r0) : "<null>";
        fprintf(stderr, "[TRANSLATOR LIBC] getenv('%s') -> NULL\n", name);
    }
    return 0;
}
static uint32_t wrap_getpid(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { return 1000; }

#define DEFINE_STUB_RET0(name) \
    static uint32_t wrap_stub_##name(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { \
        if (s_libc_debug_mode) { \
            static int s_logged = 0; \
            if (s_logged++ < 5) { \
                fprintf(stderr, "[TRANSLATOR UNHANDLED LIBC] " #name "(r0=0x%x, r1=0x%x, r2=0x%x, r3=0x%x, lr=0x%08X) -> ret 0\n", r0, r1, r2, r3, g_guest_lr); \
            } \
        } \
        return 0; \
    }

#define DEFINE_STUB_RET_NEG1(name) \
    static uint32_t wrap_stub_##name(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) { \
        if (s_libc_debug_mode) { \
            static int s_logged = 0; \
            if (s_logged++ < 5) { \
                fprintf(stderr, "[TRANSLATOR UNHANDLED LIBC] " #name "(r0=0x%x, r1=0x%x, r2=0x%x, r3=0x%x, lr=0x%08X) -> ret -1\n", r0, r1, r2, r3, g_guest_lr); \
            } \
        } \
        return (uint32_t)-1; \
    }

DEFINE_STUB_RET0(chmod)
DEFINE_STUB_RET0(fchmod)
DEFINE_STUB_RET0(fchown)
DEFINE_STUB_RET0(fcntl)
DEFINE_STUB_RET0(fsync)
DEFINE_STUB_RET0(ftruncate)
DEFINE_STUB_RET0(ioctl)
DEFINE_STUB_RET0(getpwuid)
DEFINE_STUB_RET0(utimes)
DEFINE_STUB_RET0(alarm)
DEFINE_STUB_RET0(bsd_signal)
DEFINE_STUB_RET0(sigaction)
DEFINE_STUB_RET0(setjmp)
DEFINE_STUB_RET0(longjmp)
DEFINE_STUB_RET0(sigsetjmp)
DEFINE_STUB_RET0(siglongjmp)
DEFINE_STUB_RET0(aeabi_unwind_cpp_pr0)
DEFINE_STUB_RET0(aeabi_unwind_cpp_pr1)
DEFINE_STUB_RET0(getuid)
DEFINE_STUB_RET0(geteuid)
DEFINE_STUB_RET0(getgid)
DEFINE_STUB_RET0(getegid)
DEFINE_STUB_RET0(pthread_mutex_init)
DEFINE_STUB_RET0(pthread_mutex_lock)
DEFINE_STUB_RET0(pthread_mutex_trylock)
DEFINE_STUB_RET0(pthread_mutex_unlock)
DEFINE_STUB_RET0(pthread_mutex_destroy)
DEFINE_STUB_RET0(pthread_mutexattr_init)
DEFINE_STUB_RET0(pthread_mutexattr_destroy)
DEFINE_STUB_RET0(pthread_mutexattr_settype)
DEFINE_STUB_RET0(pthread_cond_init)
DEFINE_STUB_RET0(pthread_cond_destroy)
DEFINE_STUB_RET0(pthread_cond_signal)
DEFINE_STUB_RET0(pthread_cond_broadcast)
DEFINE_STUB_RET0(pthread_cond_wait)
DEFINE_STUB_RET0(pthread_key_create)
DEFINE_STUB_RET0(pthread_key_delete)
DEFINE_STUB_RET0(pthread_getspecific)
DEFINE_STUB_RET0(pthread_setspecific)
DEFINE_STUB_RET0(pthread_attr_init)
DEFINE_STUB_RET0(pthread_attr_destroy)
DEFINE_STUB_RET0(pthread_attr_setstacksize)
DEFINE_STUB_RET0(pthread_attr_setdetachstate)
DEFINE_STUB_RET0(pthread_create)
DEFINE_STUB_RET0(pthread_join)
DEFINE_STUB_RET0(pthread_exit)
DEFINE_STUB_RET0(pthread_once)
DEFINE_STUB_RET0(pthread_self)
DEFINE_STUB_RET0(pthread_setschedparam)
DEFINE_STUB_RET0(cxa_atexit)
DEFINE_STUB_RET0(cxa_finalize)
DEFINE_STUB_RET0(aeabi_atexit)
DEFINE_STUB_RET0(dlopen)
DEFINE_STUB_RET0(dlsym)
DEFINE_STUB_RET0(dlclose)
DEFINE_STUB_RET0(dlerror)
DEFINE_STUB_RET0(dladdr)
DEFINE_STUB_RET0(Unwind_Resume)
DEFINE_STUB_RET0(Unwind_RaiseException)
DEFINE_STUB_RET0(Unwind_DeleteException)
DEFINE_STUB_RET0(Unwind_GetLanguageSpecificData)
DEFINE_STUB_RET0(Unwind_GetRegionStart)
DEFINE_STUB_RET0(Unwind_VRS_Get)
DEFINE_STUB_RET0(Unwind_VRS_Set)
DEFINE_STUB_RET0(gnu_unwind_frame)
DEFINE_STUB_RET0(gethostbyname)
DEFINE_STUB_RET0(gethostname)
DEFINE_STUB_RET0(freeaddrinfo)
DEFINE_STUB_RET0(inet_ntoa)
DEFINE_STUB_RET0(inet_ntop)

DEFINE_STUB_RET_NEG1(socket)
DEFINE_STUB_RET_NEG1(connect)
DEFINE_STUB_RET_NEG1(bind)
DEFINE_STUB_RET_NEG1(listen)
DEFINE_STUB_RET_NEG1(accept)
DEFINE_STUB_RET_NEG1(send)
DEFINE_STUB_RET_NEG1(recv)
DEFINE_STUB_RET_NEG1(shutdown)
DEFINE_STUB_RET_NEG1(poll)
DEFINE_STUB_RET_NEG1(select)
DEFINE_STUB_RET_NEG1(setsockopt)
DEFINE_STUB_RET_NEG1(getsockopt)
DEFINE_STUB_RET_NEG1(getsockname)
DEFINE_STUB_RET_NEG1(getpeername)
DEFINE_STUB_RET_NEG1(getaddrinfo)
DEFINE_STUB_RET_NEG1(inet_addr)
DEFINE_STUB_RET_NEG1(inet_pton)

static uint32_t wrap_assert2(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *file = r0 ? (const char*)G_PTR(r0) : "unknown";
    const char *func = r2 ? (const char*)G_PTR(r2) : "unknown";
    const char *expr = r3 ? (const char*)G_PTR(r3) : "unknown";
    fprintf(stderr, "[-] ASSERTION FAILED: %s:%d in %s(): %s\n", file, r1, func, expr);
    return 0;
}

static uint32_t wrap_uname(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (!r0) return (uint32_t)-1;
    uint8_t *p = img->mem + r0;
    memset(p, 0, 65 * 6);
    strncpy((char*)(p + 0),   "Linux", 64);
    strncpy((char*)(p + 65),  "localhost", 64);
    strncpy((char*)(p + 130), "3.4.0-pop2-pc", 64);
    strncpy((char*)(p + 195), "#1 SMP PREEMPT 2026", 64);
    strncpy((char*)(p + 260), "armv7l", 64);
    strncpy((char*)(p + 325), "localdomain", 64);
    return 0;
}

static volatile int g_exit_requested = 0;
static int g_exit_code = 0;

void bridge_request_exit(int exit_code) {
    g_exit_requested = 1;
    g_exit_code = exit_code;
}

int bridge_is_exit_requested(void) {
    return g_exit_requested;
}

int bridge_get_exit_code(void) {
    return g_exit_code;
}

static uint32_t wrap_abort(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    fprintf(stderr, "[-] Guest called abort()! lr=0x%08X pc=0x%08X sp=0x%08X\n", g_guest_lr, g_guest_pc, sp);
    bridge_request_exit(1);
    return 0;
}

static uint32_t wrap_stack_chk_fail(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    fprintf(stderr, "[-] Guest called __stack_chk_fail! lr=0x%08X pc=0x%08X sp=0x%08X\n", g_guest_lr, g_guest_pc, sp);
    return 0;
}

static uint32_t wrap_exit(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (s_libc_debug_mode) {
        fprintf(stderr, "[-] Guest called exit(%d)! sp=0x%08X\n", r0, sp);
    }
    bridge_request_exit((int)r0);
    return 0;
}

/* Dispatch table */
struct LibcDispatch {
    const char *name;
    svc_handler_fn fn;
};

static const struct LibcDispatch s_libc_table[] = {
    {"malloc", wrap_malloc},
    {"free", wrap_free},
    {"calloc", wrap_calloc},
    {"realloc", wrap_realloc},
    {"memalign", wrap_memalign},
    {"dlmalloc_usable_size", wrap_dlmalloc_usable_size},
    {"memcpy", wrap_memcpy},
    {"memmove", wrap_memmove},
    {"memset", wrap_memset},
    {"memcmp", wrap_memcmp},
    {"memchr", wrap_memchr},
    {"memrchr", wrap_memrchr},
    {"memmem", wrap_memmem},
    {"strlen", wrap_strlen},
    {"strcmp", wrap_strcmp},
    {"strncmp", wrap_strncmp},
    {"strcpy", wrap_strcpy},
    {"strncpy", wrap_strncpy},
    {"strcat", wrap_strcat},
    {"strncat", wrap_strncat},
    {"strlcat", wrap_strlcat},
    {"strstr", wrap_strstr},
    {"strchr", wrap_strchr},
    {"strrchr", wrap_strrchr},
    {"strcspn", wrap_strcspn},
    {"strdup", wrap_strdup},
    {"strcasecmp", wrap_strcasecmp},
    {"strncasecmp", wrap_strncasecmp},
    {"strerror", wrap_strerror},
    {"strerror_r", wrap_strerror_r},
    {"strtok_r", wrap_strtok_r},
    {"atoi", wrap_atoi},
    {"atol", wrap_atol},
    {"strtol", wrap_strtol},
    {"strtoul", wrap_strtoul},
    {"strtod", wrap_strtod},
    {"tolower", wrap_tolower},
    {"toupper", wrap_toupper},
    {"isalnum", wrap_isalnum},
    {"isalpha", wrap_isalpha},
    {"isdigit", wrap_isdigit},
    {"isspace", wrap_isspace},
    {"isxdigit", wrap_isxdigit},
    {"ispunct", wrap_ispunct},
    {"isupper", wrap_isupper},
    {"islower", wrap_islower},
    {"iscntrl", wrap_iscntrl},
    {"fopen", wrap_fopen},
    {"fclose", wrap_fclose},
    {"fread", wrap_fread},
    {"fwrite", wrap_fwrite},
    {"fseek", wrap_fseek},
    {"ftell", wrap_ftell},
    {"fflush", wrap_fflush},
    {"fgetc", wrap_fgetc},
    {"fputc", wrap_fputc},
    {"fgets", wrap_fgets},
    {"fputs", wrap_fputs},
    {"ungetc", wrap_ungetc},
    {"ferror", wrap_ferror},
    {"setvbuf", wrap_setvbuf},
    {"open", wrap_open},
    {"close", wrap_close},
    {"read", wrap_read},
    {"write", wrap_write},
    {"lseek", wrap_lseek},
    {"stat", wrap_stat},
    {"lstat", wrap_stat},
    {"fstat", wrap_fstat},
    {"access", wrap_access},
    {"unlink", wrap_unlink},
    {"remove", wrap_unlink},
    {"mkdir", wrap_mkdir},
    {"rmdir", wrap_rmdir},
    {"getcwd", wrap_getcwd},
    {"sprintf", wrap_sprintf},
    {"snprintf", wrap_snprintf},
    {"vsprintf", wrap_vsprintf},
    {"vsnprintf", wrap_vsnprintf},
    {"printf", wrap_printf},
    {"fprintf", wrap_fprintf},
    {"vfprintf", wrap_vfprintf},
    {"sscanf", wrap_sscanf},
    {"vfscanf", wrap_vfscanf},
    {"fdopen", wrap_fdopen},
    {"puts", wrap_puts},
    {"__android_log_print", wrap_android_log_print},
    {"__android_log_vprint", wrap_android_log_vprint},
    {"basename", wrap_basename},
    {"dup", wrap_dup},
    {"mmap", wrap_mmap},
    {"munmap", wrap_munmap},
    {"opendir", wrap_opendir},
    {"closedir", wrap_closedir},
    {"readdir", wrap_readdir},
    {"rewinddir", wrap_rewinddir},
    {"scandir", wrap_scandir},
    {"alphasort", wrap_alphasort},
    {"qsort", wrap_qsort},
    {"uname", wrap_uname},
    {"gmtime_r", wrap_gmtime_r},
    {"chmod", wrap_stub_chmod},
    {"fchmod", wrap_stub_fchmod},
    {"fchown", wrap_stub_fchown},
    {"fcntl", wrap_stub_fcntl},
    {"fsync", wrap_stub_fsync},
    {"ftruncate", wrap_stub_ftruncate},
    {"ioctl", wrap_stub_ioctl},
    {"getpwuid", wrap_stub_getpwuid},
    {"utimes", wrap_stub_utimes},
    {"alarm", wrap_stub_alarm},
    {"bsd_signal", wrap_stub_bsd_signal},
    {"sigaction", wrap_stub_sigaction},
    {"setjmp", wrap_stub_setjmp},
    {"longjmp", wrap_stub_longjmp},
    {"sigsetjmp", wrap_stub_sigsetjmp},
    {"siglongjmp", wrap_stub_siglongjmp},
    {"__aeabi_unwind_cpp_pr0", wrap_stub_aeabi_unwind_cpp_pr0},
    {"__aeabi_unwind_cpp_pr1", wrap_stub_aeabi_unwind_cpp_pr1},
    {"__aeabi_idiv", wrap_aeabi_idiv},
    {"__aeabi_idivmod", wrap_aeabi_idivmod},
    {"__aeabi_uidiv", wrap_aeabi_uidiv},
    {"__aeabi_uidivmod", wrap_aeabi_uidivmod},
    {"__aeabi_ldivmod", wrap_aeabi_ldivmod},
    {"__aeabi_uldivmod", wrap_aeabi_uldivmod},
    {"sinf", wrap_sinf},
    {"cosf", wrap_cosf},
    {"tanf", wrap_tanf},
    {"asinf", wrap_asinf},
    {"acosf", wrap_acosf},
    {"atanf", wrap_atanf},
    {"atan2f", wrap_atan2f},
    {"sqrtf", wrap_sqrtf},
    {"powf", wrap_powf},
    {"expf", wrap_expf},
    {"logf", wrap_logf},
    {"log10f", wrap_log10f},
    {"ceilf", wrap_ceilf},
    {"floorf", wrap_floorf},
    {"floor", wrap_floor_double},
    {"sqrt", wrap_sqrt_double},
    {"fmodf", wrap_fmodf},
    {"fmaf", wrap_fmaf},
    {"fmaxf", wrap_fmaxf},
    {"fminf", wrap_fminf},
    {"nextafterf", wrap_nextafterf},
    {"frexp", wrap_frexp},
    {"ldexp", wrap_ldexp},
    {"time", wrap_time},
    {"gettimeofday", wrap_gettimeofday},
    {"clock_gettime", wrap_clock_gettime},
    {"usleep", wrap_usleep},
    {"sleep", wrap_sleep},
    {"localtime", wrap_localtime},
    {"rand", wrap_rand},
    {"lrand48", wrap_rand},
    {"srand", wrap_srand},
    {"srand48", wrap_srand},
    {"__errno", wrap_errno},
    {"getenv", wrap_getenv},
    {"getpid", wrap_getpid},
    {"getuid", wrap_stub_getuid},
    {"geteuid", wrap_stub_geteuid},
    {"getgid", wrap_stub_getgid},
    {"getegid", wrap_stub_getegid},
    {"pthread_mutex_init", wrap_stub_pthread_mutex_init},
    {"pthread_mutex_lock", wrap_stub_pthread_mutex_lock},
    {"pthread_mutex_trylock", wrap_stub_pthread_mutex_trylock},
    {"pthread_mutex_unlock", wrap_stub_pthread_mutex_unlock},
    {"pthread_mutex_destroy", wrap_stub_pthread_mutex_destroy},
    {"pthread_mutexattr_init", wrap_stub_pthread_mutexattr_init},
    {"pthread_mutexattr_destroy", wrap_stub_pthread_mutexattr_destroy},
    {"pthread_mutexattr_settype", wrap_stub_pthread_mutexattr_settype},
    {"pthread_cond_init", wrap_stub_pthread_cond_init},
    {"pthread_cond_destroy", wrap_stub_pthread_cond_destroy},
    {"pthread_cond_signal", wrap_stub_pthread_cond_signal},
    {"pthread_cond_broadcast", wrap_stub_pthread_cond_broadcast},
    {"pthread_cond_wait", wrap_stub_pthread_cond_wait},
    {"pthread_key_create", wrap_stub_pthread_key_create},
    {"pthread_key_delete", wrap_stub_pthread_key_delete},
    {"pthread_getspecific", wrap_stub_pthread_getspecific},
    {"pthread_setspecific", wrap_stub_pthread_setspecific},
    {"pthread_attr_init", wrap_stub_pthread_attr_init},
    {"pthread_attr_destroy", wrap_stub_pthread_attr_destroy},
    {"pthread_attr_setstacksize", wrap_stub_pthread_attr_setstacksize},
    {"pthread_attr_setdetachstate", wrap_stub_pthread_attr_setdetachstate},
    {"pthread_create", wrap_stub_pthread_create},
    {"pthread_join", wrap_stub_pthread_join},
    {"pthread_exit", wrap_stub_pthread_exit},
    {"pthread_once", wrap_stub_pthread_once},
    {"pthread_self", wrap_stub_pthread_self},
    {"pthread_setschedparam", wrap_stub_pthread_setschedparam},
    {"__cxa_atexit", wrap_stub_cxa_atexit},
    {"__cxa_finalize", wrap_stub_cxa_finalize},
    {"__aeabi_atexit", wrap_stub_aeabi_atexit},
    {"__assert2", wrap_assert2},
    {"__stack_chk_fail", wrap_stack_chk_fail},
    {"abort", wrap_abort},
    {"exit", wrap_exit},
    {"dlopen", wrap_stub_dlopen},
    {"dlsym", wrap_stub_dlsym},
    {"dlclose", wrap_stub_dlclose},
    {"dlerror", wrap_stub_dlerror},
    {"dladdr", wrap_stub_dladdr},
    {"_Unwind_Resume", wrap_stub_Unwind_Resume},
    {"_Unwind_RaiseException", wrap_stub_Unwind_RaiseException},
    {"_Unwind_DeleteException", wrap_stub_Unwind_DeleteException},
    {"_Unwind_GetLanguageSpecificData", wrap_stub_Unwind_GetLanguageSpecificData},
    {"_Unwind_GetRegionStart", wrap_stub_Unwind_GetRegionStart},
    {"_Unwind_VRS_Get", wrap_stub_Unwind_VRS_Get},
    {"_Unwind_VRS_Set", wrap_stub_Unwind_VRS_Set},
    {"__gnu_unwind_frame", wrap_stub_gnu_unwind_frame},
    {"socket", wrap_stub_socket},
    {"connect", wrap_stub_connect},
    {"bind", wrap_stub_bind},
    {"listen", wrap_stub_listen},
    {"accept", wrap_stub_accept},
    {"send", wrap_stub_send},
    {"recv", wrap_stub_recv},
    {"shutdown", wrap_stub_shutdown},
    {"poll", wrap_stub_poll},
    {"select", wrap_stub_select},
    {"setsockopt", wrap_stub_setsockopt},
    {"getsockopt", wrap_stub_getsockopt},
    {"getsockname", wrap_stub_getsockname},
    {"getpeername", wrap_stub_getpeername},
    {"gethostbyname", wrap_stub_gethostbyname},
    {"gethostname", wrap_stub_gethostname},
    {"getaddrinfo", wrap_stub_getaddrinfo},
    {"freeaddrinfo", wrap_stub_freeaddrinfo},
    {"inet_addr", wrap_stub_inet_addr},
    {"inet_ntoa", wrap_stub_inet_ntoa},
    {"inet_ntop", wrap_stub_inet_ntop},
    {"inet_pton", wrap_stub_inet_pton},
    {NULL, NULL}
};

svc_handler_fn bridge_libc_lookup(const char *name) {
    for (int i = 0; s_libc_table[i].name != NULL; ++i) {
        if (strcmp(s_libc_table[i].name, name) == 0) {
            return s_libc_table[i].fn;
        }
    }
    return NULL;
}

uint32_t bridge_libc_get_heap_used_mb(void) {
    return (g_heap_cur - 0x02000000u) >> 20;
}

uint64_t bridge_libc_get_malloc_count(void) {
    return g_acc_malloc_calls;
}

uint64_t bridge_libc_get_free_count(void) {
    return g_acc_free_calls;
}

uint32_t bridge_libc_get_stk_vfd_count(void) {
    return g_stk_vfd_alloc;
}
