#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>

#if defined(SNF_DEBUG) && SNF_DEBUG
bool g_debug_mode = true;
#else
bool g_debug_mode = false;
#endif

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GL/gl.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>   /* timeBeginPeriod / timeEndPeriod */
#include <immintrin.h>  /* _mm_pause() */
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <time.h>

/* Allow Windows UserGpuPreferences and user settings to dynamically control GPU routing */
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000000;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0;
    __declspec(dllexport) uint8_t *g_guest_memory_base = nullptr;
}

#include <dwmapi.h>
typedef HRESULT (WINAPI *PFN_DwmFlush)(void);
static PFN_DwmFlush s_pfn_DwmFlush = nullptr;
typedef HRESULT (WINAPI *PFN_DwmGetCompositionTimingInfo)(HWND hwnd, DWM_TIMING_INFO *pTimingInfo);
static PFN_DwmGetCompositionTimingInfo s_pfn_DwmGetCompositionTimingInfo = nullptr;
static bool s_dwm_queried = false;
static int s_swap_control = 0;
static bool s_dwm_composition_enforced = false;
#else
static int s_swap_control = 0;
#endif

/* How frames are paced: see "Resolve how presentation is paced" in main(). */
enum VsyncMode { VSYNC_OFF = 0, VSYNC_DRIVER, VSYNC_DWM, VSYNC_SOFTWARE };
static VsyncMode s_vsync_mode = VSYNC_OFF;
static bool s_offscreen = false;   /* guest renders into an offscreen FBO that is blitted to the window */
#ifdef _WIN32

static void ensure_compatibility_flags(void) {
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH)) {
        HKEY hKey;
        if (RegCreateKeyExA(HKEY_CURRENT_USER,
                            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                            0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            char current_val[512] = {0};
            DWORD val_size = sizeof(current_val);
            DWORD type = 0;
            LONG res = RegQueryValueExA(hKey, exe_path, NULL, &type, (LPBYTE)current_val, &val_size);
            if (res != ERROR_SUCCESS || strstr(current_val, "DISABLEDXMAXIMIZEDWINDOWEDMODE") == NULL) {
                char new_val[512] = {0};
                if (res == ERROR_SUCCESS && current_val[0] != '\0') {
                    /* Append our flag to whatever is already there.
                     * AppCompatFlags/Layers is a space-separated list of tokens. */
                    snprintf(new_val, sizeof(new_val), "%s DISABLEDXMAXIMIZEDWINDOWEDMODE", current_val);
                } else {
                    /* No existing entry — write a fresh value with the ~ prefix
                     * (applies to all user accounts in the compatibility shim). */
                    snprintf(new_val, sizeof(new_val), "~ DISABLEDXMAXIMIZEDWINDOWEDMODE");
                }
                RegSetValueExA(hKey, exe_path, 0, REG_SZ, (const BYTE*)new_val, (DWORD)strlen(new_val) + 1);
                if (g_debug_mode) {
                    printf("[+] Applied DWM windowed composition compatibility flag: \"%s\"\n", new_val);
                }
            }
            s_dwm_composition_enforced = true;
            RegCloseKey(hKey);
        }
    }
}

static void apply_windows_gpu_preference(int pref) {
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH)) {
        HKEY hKey;
        if (RegCreateKeyExA(HKEY_CURRENT_USER,
                            "Software\\Microsoft\\DirectX\\UserGpuPreferences",
                            0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            char current_val[256] = {0};
            DWORD val_size = sizeof(current_val);
            DWORD type = 0;
            LONG res = RegQueryValueExA(hKey, exe_path, NULL, &type, (LPBYTE)current_val, &val_size);

            if (pref > 0) {
                char desired_val[64];
                snprintf(desired_val, sizeof(desired_val), "GpuPreference=%d;SwapEffectUpgradeEnable=0;", pref);
                if (res != ERROR_SUCCESS || strstr(current_val, desired_val) == NULL) {
                    RegSetValueExA(hKey, exe_path, 0, REG_SZ, (const BYTE*)desired_val, (DWORD)strlen(desired_val) + 1);
                }
            } else if (res == ERROR_SUCCESS) {
                const char *desired_val = "SwapEffectUpgradeEnable=0;";
                RegSetValueExA(hKey, exe_path, 0, REG_SZ, (const BYTE*)desired_val, (DWORD)strlen(desired_val) + 1);
            }
            RegCloseKey(hKey);
        }
    }
}

static void ensure_dwm_init(void) {
    if (!s_dwm_queried) {
        s_dwm_queried = true;
        HMODULE hDwm = LoadLibraryA("dwmapi.dll");
        if (hDwm) {
            s_pfn_DwmFlush = (PFN_DwmFlush)(void*)GetProcAddress(hDwm, "DwmFlush");
            s_pfn_DwmGetCompositionTimingInfo = (PFN_DwmGetCompositionTimingInfo)(void*)GetProcAddress(hDwm, "DwmGetCompositionTimingInfo");
            if (s_pfn_DwmFlush && g_debug_mode) {
                printf("[+] DwmFlush compositor synchronization loaded from dwmapi.dll\n");
            }
            if (s_pfn_DwmGetCompositionTimingInfo && g_debug_mode) {
                printf("[+] DwmGetCompositionTimingInfo timing diagnostics loaded from dwmapi.dll\n");
            }
        }
    }
}
#else
#include <unistd.h>
#define _chdir chdir
#define _access access
#define _mkdir(d) mkdir(d, 0777)
#endif

#include "elf32/elf32_loader.h"
#include "runtime/dynarmic_host.h"
#include "bridge/bridge_libc.h"
#include "bridge/bridge_gles.h"
#include "bridge/bridge_openal.h"
#include "bridge/bridge_jni.h"
#include "build_version.h"
#include "config.h"
#include "gpu_detect.h"
#include "mods/soft_mods.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

static int g_render_width = 1280;
static int g_render_height = 720;
static int g_window_width = 1280;
static int g_window_height = 720;
static int s_display_hz = 60;
static const char *s_gl_vendor = "Unknown";
static const char *s_gl_renderer = "Unknown";

// Viewport scaling & letterboxing state (maps render resolution to window)
static int   s_dst_x = 0;
static int   s_dst_y = 0;
static int   s_dst_w = 1280;
static int   s_dst_h = 720;
static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;
static bool  s_scaling_active = false;

static void update_viewport_scaling(int win_w, int win_h) {
    if (win_w <= 0 || win_h <= 0) return;
    g_window_width = win_w;
    g_window_height = win_h;

    if (win_w == g_render_width && win_h == g_render_height) {
        s_scaling_active = false;
        s_dst_x = 0;
        s_dst_y = 0;
        s_dst_w = win_w;
        s_dst_h = win_h;
        s_scale_x = 1.0f;
        s_scale_y = 1.0f;
        bridge_gles_set_viewport_scaling(0, g_render_width, g_render_height, 0, 0, win_w, win_h);
        return;
    }

    s_scaling_active = true;
    float render_aspect = (float)g_render_width / (float)g_render_height;
    float win_aspect = (float)win_w / (float)win_h;

    if (win_aspect > render_aspect + 0.001f) {
        // Window is wider than target aspect ratio: pillarbox (black bars on left/right)
        s_dst_w = (int)(win_h * render_aspect + 0.5f);
        s_dst_h = win_h;
        s_dst_x = (win_w - s_dst_w) / 2;
        s_dst_y = 0;
    } else if (win_aspect < render_aspect - 0.001f) {
        // Window is taller than target aspect ratio: letterbox (black bars on top/bottom)
        s_dst_w = win_w;
        s_dst_h = (int)(win_w / render_aspect + 0.5f);
        s_dst_x = 0;
        s_dst_y = (win_h - s_dst_h) / 2;
    } else {
        // Aspect ratio matches exactly
        s_dst_x = 0;
        s_dst_y = 0;
        s_dst_w = win_w;
        s_dst_h = win_h;
    }

    s_scale_x = (float)s_dst_w / (float)g_render_width;
    s_scale_y = (float)s_dst_h / (float)g_render_height;

    bridge_gles_set_viewport_scaling(1, g_render_width, g_render_height,
                                     s_dst_x, s_dst_y, s_dst_w, s_dst_h);
    if (g_debug_mode) {
        printf("[+] Viewport scaling enabled: render=%dx%d -> window=%dx%d (dest: %d,%d %dx%d, scale: %.3fx%.3f)\n",
               g_render_width, g_render_height, win_w, win_h, s_dst_x, s_dst_y, s_dst_w, s_dst_h, s_scale_x, s_scale_y);
    }
}

/* The Android activity displays app_splash.png while ShiVa3D initializes.
 * Present the same image from the native host until the guest engine is
 * ready to submit its first frame. */
static void show_startup_splash(SDL_Window *window) {
    int image_w = 0, image_h = 0, components = 0;
    unsigned char *pixels = stbi_load("app_splash.png", &image_w, &image_h,
                                     &components, 4);
    if (!pixels) {
        fprintf(stderr, "[!] Startup splash unavailable (app_splash.png): %s\n",
                stbi_failure_reason());
        return;
    }

    int drawable_w = 0, drawable_h = 0;
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    if (drawable_w <= 0 || drawable_h <= 0 || image_w <= 0 || image_h <= 0) {
        stbi_image_free(pixels);
        return;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_w, image_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    const float image_aspect = (float)image_w / (float)image_h;
    const float drawable_aspect = (float)drawable_w / (float)drawable_h;
    float half_w = 1.0f;
    float half_h = 1.0f;
    if (drawable_aspect > image_aspect) {
        half_w = image_aspect / drawable_aspect;
    } else {
        half_h = drawable_aspect / image_aspect;
    }

    glViewport(0, 0, drawable_w, drawable_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-half_w, -half_h);
        glTexCoord2f(1.0f, 1.0f); glVertex2f( half_w, -half_h);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-half_w,  half_h);
        glTexCoord2f(1.0f, 0.0f); glVertex2f( half_w,  half_h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
    SDL_GL_SwapWindow(window);
    if (g_debug_mode) {
        printf("[+] Displaying Ubisoft startup splash while the engine loads.\n");
    }
}

/* =========================================================================
 * Per-frame Performance Profiler
 * =========================================================================
 * Tracks three phases each frame:
 *   ENGINE  - dynarmic_call(engineRunOneFrame) + all per-frame bridge work
 *   SWAP    - SDL_GL_SwapWindow (blocks until VSync if vsync=on)
 *   SLEEP   - SDL_Delay used for the 60fps cap
 *
 * Every 60 frames (≈1 s) a summary line is written to stdout and perf.log:
 *   [PERF] frame=N  fps=60.0  avg=16.7ms  p50=16.6  p95=17.2  p99=18.1  max=22.4
 *          engine=15.8ms  swap=0.7ms  sleep=0.2ms  jitter=0  heap_delta=+0MB
 *
 * Any frame whose TOTAL exceeds JITTER_THRESHOLD_US gets an immediate line:
 *   [JITTER f=N] total=24.3ms  engine=23.1ms  swap=1.1ms  sleep=0.0ms
 *                heap_delta=+4MB  (asset load spike?)
 * ========================================================================= */
#define PERF_WINDOW   300            /* sliding window size (frames) */
#define JITTER_US     20000          /* 20 ms - anything over this is a jitter (unpaced mode) */
#define PERF_REPORT_INTERVAL 60      /* print summary every N frames */

struct FrameTimings {
    int64_t total_us;
    int64_t engine_us;
    int64_t swap_us;       /* whole presentation phase: dwm + present + swap call + everything between */
    int64_t sleep_us;
    int64_t heap_delta_allocs;  /* net allocation count (mallocs - frees) this frame */
    int64_t dwm_us;        /* waiting for the compositor refresh */
    int64_t present_us;    /* offscreen resolve + blit to the window (stalls here when the GPU is behind) */
    int64_t swapcall_us;   /* SDL_GL_SwapWindow */
};

static FrameTimings  s_perf_window[PERF_WINDOW];
static int           s_perf_head   = 0;
static int           s_perf_filled = 0;

static FILE         *s_perf_log    = nullptr;
static int64_t       s_perf_qpf    = 1;   /* QueryPerformanceFrequency */
static int64_t       s_frame_jitter_total = 0;  /* jitter count this second */

/* Cumulative heap counters snapped at frame start */
static uint64_t s_perf_malloc_prev = 0;
static uint64_t s_perf_free_prev   = 0;

/* Presentation-path phase timings of the current frame (filled in by the swap code in the main loop) */
static int64_t s_ph_dwm_us = 0, s_ph_present_us = 0, s_ph_swapcall_us = 0;

/* Run-wide state for the performance log (enabled by --debug or PerfLog = 1 in config.ini) */
static bool     s_perf_enabled     = false;
static int64_t  s_perf_start_us    = 0;
static uint64_t s_run_frames       = 0;
static uint64_t s_run_over_budget  = 0;   /* frames longer than one refresh + 4 ms */
static uint64_t s_run_missed_vsync = 0;   /* frames longer than 1.5 refreshes (a visibly repeated frame) */
static int64_t  s_run_max_total_us = 0;
static int64_t  s_run_sum_total_us = 0;
static int64_t  s_sec_over = 0, s_sec_missed = 0;
static uint32_t s_perf_scene = 0, s_perf_ce_state = 0;   /* current scene / engine state, set by the main loop */
static uint32_t s_perf_last_scene = 0;
static int      s_spike_lines_sec = 0;   /* spike lines written this second (capped) */
/* Host-call profile (PerfProfile = 1): engine time split into bridge calls and guest code, aggregated over slow
 * frames between scene changes so each level load gets one breakdown line */
static bool     s_prof_active = false;
static int64_t  s_load_engine_us = 0;
static uint32_t s_load_frames = 0;
static int64_t  s_sec_host_us = 0;

static const char *perf_stamp(void);

static void perf_write_loadprof(const char *label) {
    if (!s_perf_log || !s_prof_active || s_load_frames == 0) return;
    char top[512];
    int64_t host_us = 0;
    uint32_t calls = 0;
    host_prof_aggregate_top(8, top, sizeof(top), &host_us, &calls);
    char hot[900];
    host_prof_sample_top(8, hot, sizeof(hot));
    fprintf(s_perf_log, "[LOADPROF %s t=%s] slow_frames=%u engine=%.0fms = host_calls %.0fms (%u calls) + guest_code %.0fms | top: %s\n",
            label, perf_stamp(), s_load_frames, s_load_engine_us / 1000.0, host_us / 1000.0, calls,
            (s_load_engine_us - host_us) / 1000.0, top);
    fprintf(s_perf_log, "[LOADPROF guest hot] %s\n", hot);
    fflush(s_perf_log);
    host_prof_aggregate_reset();
    s_load_engine_us = 0;
    s_load_frames = 0;
}

static inline int64_t perf_now_us(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (t.QuadPart * 1000000LL) / s_perf_qpf;
}

/* Always initialize the QPC frequency (the frame limiter depends on it in every build) */
static void perf_init(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_perf_qpf = f.QuadPart;
}

/* "HH:MM:SS.mmm" since the log was opened - lets spikes be matched to a run recording */
static const char *perf_stamp(void) {
    static char buf[32];
    int64_t ms = (perf_now_us() - s_perf_start_us) / 1000;
    if (ms < 0) ms = 0;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", (int)(ms / 3600000), (int)((ms / 60000) % 60),
             (int)((ms / 1000) % 60), (int)(ms % 1000));
    return buf;
}

struct PerfMemCounters {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
};

static size_t perf_working_set_mb(void) {
    typedef BOOL (WINAPI *PFN_K32GetProcessMemoryInfo)(HANDLE, PerfMemCounters*, DWORD);
    static PFN_K32GetProcessMemoryInfo fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k) fn = (PFN_K32GetProcessMemoryInfo)(void*)GetProcAddress(k, "K32GetProcessMemoryInfo");
    }
    PerfMemCounters pmc = {};
    pmc.cb = sizeof(pmc);
    if (fn && fn(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize / (1024 * 1024);
    return 0;
}

/* Opens logs/perf_<date>_<time>.log (one file per run) and writes a header describing the setup.
 * Call after the config is loaded and the GL context / presentation mode are known. */
static void perf_open_log(void) {
    s_perf_enabled = g_debug_mode || g_config.perf_log;
    if (!s_perf_enabled) return;
    s_prof_active = g_config.perf_profile;
    host_prof_enable(s_prof_active ? 1 : 0);

    _mkdir("logs");
    char path[128];
    time_t now = time(nullptr);
    struct tm *lt = localtime(&now);
    if (lt) {
        snprintf(path, sizeof(path), "logs/perf_%04d%02d%02d_%02d%02d%02d.log",
                 lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
    } else {
        snprintf(path, sizeof(path), "logs/perf_latest.log");
    }
    s_perf_log = fopen(path, "w");
    s_perf_start_us = perf_now_us();
    if (!s_perf_log) return;

    static const char *mode_names[] = { "off (FPS limiter)", "driver swap interval", "DWM compositor flush",
                                        "software limiter at refresh rate" };
    fprintf(s_perf_log, "# Prince of Persia: Shadow and the Flame - Reignited performance log | build=%s\n", BUILD_TAG);
    fprintf(s_perf_log, "# gpu=\"%s\" | gpu_preference=%d | display=%d Hz\n",
            s_gl_renderer ? s_gl_renderer : "?", g_config.gpu_preference, s_display_hz);
    fprintf(s_perf_log, "# window=%dx%d render=%dx%d fullscreen=%d | offscreen=%d msaa_requested=%d | af=%d\n",
            g_window_width, g_window_height, g_render_width, g_render_height, (int)g_config.fullscreen,
            (int)s_offscreen, g_config.msaa, g_config.anisotropic);
    fprintf(s_perf_log, "# vsync=%d mode=%s | fps_limit=%d | vsync_mode_cfg=%d present_delay_ms=%.1f\n",
            (int)g_config.vsync, mode_names[s_vsync_mode], g_config.fps_limit, g_config.vsync_mode,
            g_config.present_delay_ms);
    fprintf(s_perf_log, "# [PERF]: fps avg p50 p95 p99 max (ms/frame) | engine, dwm (refresh wait), present (blit), "
                        "swap, sleep (avg ms) | over = frames > refresh+4ms, missed = frames > 1.5 refreshes | "
                        "ws = working set MB | dwm_missed/dropped = compositor counters\n");
    fprintf(s_perf_log, "# [SPIKE]: single slow frame with its phase breakdown and a likely cause\n");
    fprintf(s_perf_log, "# profile=%d: spikes show host (bridge calls: file reads, GL uploads, audio...) vs guest (game code + JIT); [LOADPROF] sums slow frames per level load\n", (int)s_prof_active);
    fflush(s_perf_log);
}

/* Call at the start of every frame to snapshot heap counters */
static void perf_frame_begin(void) {
    s_perf_malloc_prev = bridge_libc_get_malloc_count();
    s_perf_free_prev   = bridge_libc_get_free_count();
    s_ph_dwm_us = s_ph_present_us = s_ph_swapcall_us = 0;
    if (s_prof_active) host_prof_frame_reset();
}

/* Record timing and write spike / per-second lines */
static void perf_frame_end(int frame_no,
                            int64_t engine_us, int64_t swap_us, int64_t sleep_us) {
    int64_t total_us = engine_us + swap_us + sleep_us;

    uint64_t mallocs_this_frame = bridge_libc_get_malloc_count() - s_perf_malloc_prev;
    uint64_t frees_this_frame   = bridge_libc_get_free_count()   - s_perf_free_prev;
    /* Net allocation count this frame (positive = more mallocs than frees).
     * Stored as a count, not bytes - we cannot cheaply measure per-frame byte deltas. */
    int64_t heap_delta_allocs = (int64_t)mallocs_this_frame - (int64_t)frees_this_frame;

    FrameTimings ft = { total_us, engine_us, swap_us, sleep_us, heap_delta_allocs,
                        s_ph_dwm_us, s_ph_present_us, s_ph_swapcall_us };
    s_perf_window[s_perf_head] = ft;
    s_perf_head = (s_perf_head + 1) % PERF_WINDOW;
    if (s_perf_filled < PERF_WINDOW) s_perf_filled++;

    /* Frame budget: one refresh when presentation is vsynced, otherwise the FPS limit */
    bool paced_by_limit = (s_vsync_mode == VSYNC_OFF) ? (g_config.fps_limit > 0) : (s_display_hz > 0);
    int64_t expected_us = (s_vsync_mode == VSYNC_OFF)
        ? ((g_config.fps_limit > 0) ? (1000000LL / g_config.fps_limit) : 16667)
        : (1000000LL / (s_display_hz > 0 ? s_display_hz : 60));
    int64_t jitter_thresh = paced_by_limit ? (expected_us + 4000) : JITTER_US;

    s_run_frames++;
    s_run_sum_total_us += total_us;
    if (total_us > s_run_max_total_us) s_run_max_total_us = total_us;
    bool is_missed = paced_by_limit && total_us > (expected_us * 3) / 2;
    if (total_us >= jitter_thresh) { s_run_over_budget++; s_sec_over++; }
    if (is_missed) { s_run_missed_vsync++; s_sec_missed++; }

    /* Profile bookkeeping: slow engine frames feed the per-level-load breakdown */
    int64_t host_frame_us = s_prof_active ? host_prof_frame_total_us() : 0;
    s_sec_host_us += host_frame_us;
    if (s_prof_active && engine_us > 30000) {
        host_prof_aggregate_frame();
        s_load_engine_us += engine_us;
        s_load_frames++;
    }

    /* Scene transitions, so spikes can be matched to level loads */
    if (s_perf_scene != s_perf_last_scene) {
        perf_write_loadprof("before scene change");
        if (s_perf_log) {
            fprintf(s_perf_log, "[SCENE t=%s f=%d] scene 0x%08X -> 0x%08X ce_state=%u play=%d\n", perf_stamp(), frame_no,
                    s_perf_last_scene, s_perf_scene, s_perf_ce_state, (int)soft_mods_is_in_gameplay());
            fflush(s_perf_log);
        }
        s_perf_last_scene = s_perf_scene;
    }

    /* Immediate spike line */
    if (total_us >= jitter_thresh) {
        s_frame_jitter_total++;
        const char *cause = "other";
        if (engine_us > 8000)                           cause = "engine/CPU (asset load or heavy logic)";
        else if (heap_delta_allocs > 512)               cause = "allocation burst (asset load)";
        else if (s_ph_present_us > 4000)                cause = "GPU behind (present blit stalled)";
        else if (s_ph_dwm_us > expected_us + 2000)      cause = "compositor wait overran (missed refresh)";
        else if (s_ph_swapcall_us > 4000)               cause = "swap call stalled";
        else if (sleep_us > expected_us)                cause = "limiter";
        if (g_debug_mode) {
            printf("[SPIKE f=%d] total=%.2fms engine=%.2f dwm=%.2f present=%.2f swap=%.2f sleep=%.2f heap=%+ld -> %s\n",
                   frame_no, total_us / 1000.0, engine_us / 1000.0, s_ph_dwm_us / 1000.0, s_ph_present_us / 1000.0,
                   s_ph_swapcall_us / 1000.0, sleep_us / 1000.0, (long)heap_delta_allocs, cause);
        }
        /* Cap spike lines per second so a sustained slowdown cannot bloat the log (the PERF line still counts them) */
        if (s_perf_log && s_spike_lines_sec++ < 10) {
            char prof[600] = "";
            if (s_prof_active && engine_us > 8000) {
                char top[400];
                host_prof_frame_top(5, top, sizeof(top));
                snprintf(prof, sizeof(prof), " | host=%.2fms/%u guest=%.2fms top: %s", host_frame_us / 1000.0,
                         host_prof_frame_calls(), (engine_us - host_frame_us) / 1000.0, top);
            }
            fprintf(s_perf_log,
                    "[SPIKE t=%s f=%d] total=%.2fms engine=%.2f dwm=%.2f present=%.2f swap=%.2f sleep=%.2f"
                    " heap=%+ld scene=0x%08X ce=%u play=%d -> %s%s\n",
                    perf_stamp(), frame_no, total_us / 1000.0, engine_us / 1000.0, s_ph_dwm_us / 1000.0,
                    s_ph_present_us / 1000.0, s_ph_swapcall_us / 1000.0, sleep_us / 1000.0, (long)heap_delta_allocs,
                    s_perf_scene, s_perf_ce_state, (int)soft_mods_is_in_gameplay(), cause, prof);
            fflush(s_perf_log);
        }
    }

    /* Per-second summary */
    if (frame_no > 0 && frame_no % PERF_REPORT_INTERVAL == 0 && s_perf_filled > 0) {
        /* Sort a temp copy for percentiles */
        static int64_t tmp[PERF_WINDOW];
        int n = s_perf_filled;
        for (int i = 0; i < n; ++i) tmp[i] = s_perf_window[i].total_us;
        /* Insertion sort (n <= 300, cheap) */
        for (int i = 1; i < n; ++i) {
            int64_t key = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > key) { tmp[j+1] = tmp[j]; --j; }
            tmp[j+1] = key;
        }
        int64_t p50  = tmp[(int)(n * 0.50)];
        int64_t p95  = tmp[(int)(n * 0.95)];
        int64_t p99  = tmp[(int)(n * 0.99)];
        int64_t tmax = tmp[n - 1];

        /* Averages */
        int64_t sum_total = 0, sum_eng = 0, sum_swap = 0, sum_slp = 0, sum_hdelta = 0;
        int64_t sum_dwm = 0, sum_pres = 0, sum_swc = 0;
        for (int i = 0; i < n; ++i) {
            sum_total  += s_perf_window[i].total_us;
            sum_eng    += s_perf_window[i].engine_us;
            sum_swap   += s_perf_window[i].swap_us;
            sum_slp    += s_perf_window[i].sleep_us;
            sum_hdelta += s_perf_window[i].heap_delta_allocs;
            sum_dwm    += s_perf_window[i].dwm_us;
            sum_pres   += s_perf_window[i].present_us;
            sum_swc    += s_perf_window[i].swapcall_us;
        }
        double avg_ms  = (sum_total  / (double)n) / 1000.0;
        double eng_ms  = (sum_eng    / (double)n) / 1000.0;
        double swap_ms = (sum_swap   / (double)n) / 1000.0;
        double slp_ms  = (sum_slp    / (double)n) / 1000.0;
        double dwm_ms  = (sum_dwm    / (double)n) / 1000.0;
        double pres_ms = (sum_pres   / (double)n) / 1000.0;
        double swc_ms  = (sum_swc    / (double)n) / 1000.0;
        double fps     = (avg_ms > 0.0) ? (1000.0 / avg_ms) : 0.0;
        int64_t avg_hdelta = sum_hdelta / n;

        /* Compositor counters (deltas since the previous summary) */
        static uint64_t s_last_missed = 0, s_last_dropped = 0;
        long dwm_missed = 0, dwm_dropped = 0;
        if (s_pfn_DwmGetCompositionTimingInfo) {
            DWM_TIMING_INFO timing = {};
            timing.cbSize = sizeof(timing);
            if (SUCCEEDED(s_pfn_DwmGetCompositionTimingInfo(NULL, &timing))) {
                dwm_missed  = (long)(timing.cFramesMissed  - s_last_missed);
                dwm_dropped = (long)(timing.cFramesDropped - s_last_dropped);
                s_last_missed  = timing.cFramesMissed;
                s_last_dropped = timing.cFramesDropped;
            }
        }

        if (g_debug_mode) {
            printf("[PERF f=%d] fps=%.1f  avg=%.2fms  p50=%.2f  p95=%.2f  p99=%.2f  max=%.2f"
                   "  engine=%.2fms  swap=%.2fms  sleep=%.2fms  jitter=%lld  heap_delta=%+ld allocs/f\n",
                   frame_no, fps, avg_ms, p50/1000.0, p95/1000.0, p99/1000.0, tmax/1000.0,
                   eng_ms, swap_ms, slp_ms, (long long)s_frame_jitter_total, (long)avg_hdelta);
        }
        if (s_perf_log) {
            fprintf(s_perf_log,
                    "[PERF t=%s f=%d] fps=%.1f avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f | engine=%.2f dwm=%.2f"
                    " present=%.2f swap=%.2f sleep=%.2f | over=%lld missed=%lld | heap=%+ld/f ws=%zuMB"
                    " dwm_missed=%+ld dropped=%+ld | host=%.2f | play=%d\n",
                    perf_stamp(), frame_no, fps, avg_ms, p50/1000.0, p95/1000.0, p99/1000.0, tmax/1000.0,
                    eng_ms, dwm_ms, pres_ms, swc_ms, slp_ms, (long long)s_sec_over, (long long)s_sec_missed,
                    (long)avg_hdelta, perf_working_set_mb(), dwm_missed, dwm_dropped,
                    s_prof_active ? (s_sec_host_us / (double)PERF_REPORT_INTERVAL) / 1000.0 : 0.0,
                    (int)soft_mods_is_in_gameplay());
            fflush(s_perf_log);
        }

        s_frame_jitter_total = 0;
        s_sec_over = 0;
        s_sec_missed = 0;
        s_spike_lines_sec = 0;
        s_sec_host_us = 0;
    }
}

/* Final run summary, written on a clean exit */
static void perf_write_summary(void) {
    if (!s_perf_log || s_run_frames == 0) return;
    perf_write_loadprof("final");
    fprintf(s_perf_log,
            "[SUMMARY t=%s] frames=%llu avg=%.2fms (%.1f fps) max=%.2fms over_budget=%llu (%.2f%%) missed_refresh=%llu (%.3f%%)\n",
            perf_stamp(), (unsigned long long)s_run_frames,
            (s_run_sum_total_us / (double)s_run_frames) / 1000.0,
            1000000.0 / (s_run_sum_total_us / (double)s_run_frames), s_run_max_total_us / 1000.0,
            (unsigned long long)s_run_over_budget, 100.0 * s_run_over_budget / s_run_frames,
            (unsigned long long)s_run_missed_vsync, 100.0 * s_run_missed_vsync / s_run_frames);
    fflush(s_perf_log);
}

/* Android keycodes (standard Android KeyEvent values) */
enum AndroidKeyCode {
    AKEYCODE_BACK          = 4,    // Back / Cancel / Escape
    AKEYCODE_DPAD_UP       = 19,
    AKEYCODE_DPAD_DOWN     = 20,
    AKEYCODE_DPAD_LEFT     = 21,
    AKEYCODE_DPAD_RIGHT    = 22,
    AKEYCODE_DPAD_CENTER   = 23,   // Menu confirm / DPAD_CENTER
    AKEYCODE_BUTTON_A      = 96,   // Jump / Action / Tutorial dismiss
    AKEYCODE_BUTTON_B      = 97,   // Gamepad B / Action 2
    AKEYCODE_BUTTON_C      = 98,
    AKEYCODE_BUTTON_X      = 99,   // Attack / Sword
    AKEYCODE_BUTTON_Y      = 100,  // Block
    AKEYCODE_BUTTON_L1     = 102,  // Potion / Tab prev
    AKEYCODE_BUTTON_R1     = 103,  // Roll / Tab next
    AKEYCODE_BUTTON_START  = 108,  // Pause
    AKEYCODE_BUTTON_SELECT = 109,
};

static uint32_t s_sym_JNI_OnLoad = 0;
static uint32_t s_sym_engineInitialize = 0;
static uint32_t s_sym_engineRunOneFrame = 0;
static uint32_t s_sym_engineDidPassFirstFrame = 0;
static uint32_t s_sym_engineSetSystemVersion = 0;
static uint32_t s_sym_engineSetDirectories = 0;
static uint32_t s_sym_enginePause = 0;
static uint32_t s_sym_engineSurfaceCreated = 0;
static uint32_t s_sym_engineSurfaceChanged = 0;
static uint32_t s_sym_engineOnMouseMove = 0;
static uint32_t s_sym_S3DClient_iPhone_OnMouseMoved = 0;
static uint32_t s_sym_engineOnMouseButtonDown = 0;
static uint32_t s_sym_engineOnMouseButtonUp = 0;
static uint32_t s_sym_engineOnKeyboardKeyDown = 0;
static uint32_t s_sym_engineOnKeyboardKeyUp = 0;
static uint32_t s_sym_engineOnTouchesChange = 0;
static uint32_t s_sym_setBuildType = 0;
static uint32_t s_sym_setVersionCode = 0;
static uint32_t s_sym_initMobileSDK = 0;
static uint32_t s_sym_windowFocusChange = 0;
static uint32_t s_sym_engineGetOverlayMovieHasChanged = 0;
static uint32_t s_sym_engineOnOverlayMovieStopped = 0;
static uint32_t s_sym_engineGetOverlayMovie = 0;
static uint32_t s_sym_engineGetWantSwapBuffers = 0;
static uint32_t s_sym_sendEventToCurrentUser = 0;
static uint32_t s_sym_resetStringPool = 0;
static uint32_t s_sym_kernel_getInstance = 0;
static uint32_t s_sym_aistack_clearTempHandles = 0;
static std::atomic<uint32_t> g_engine_frame_cnt{0};


static uint32_t write_guest_str(elf32_image_t *img, uint32_t addr, const char *s) {
    strcpy((char*)(img->mem + addr), s);
    return addr;
}

static void send_ai_event(dynarmic_host_t *host, elf32_image_t *img, uint32_t sym_send, const char *ai_name, const char *event_name) {
    if (!sym_send) return;
    uint32_t a_ai = write_guest_str(img, 0x00007000u, ai_name);
    uint32_t a_evt = write_guest_str(img, 0x00007100u, event_name);
    if (g_debug_mode) {
        printf("[+] Dispatching AI event '%s' -> '%s'...\n", ai_name, event_name);
    }
    dynarmic_call(host, sym_send, a_ai, a_evt, 0, 0);
}

static void reset_ai_temporary_handles(dynarmic_host_t *host, elf32_image_t *img) {
    if (!s_sym_kernel_getInstance || !s_sym_aistack_clearTempHandles) return;
    static uint32_t s_cached_ai_stack = 0;
    if (!s_cached_ai_stack) {
        uint32_t kernel_ptr = dynarmic_call(host, s_sym_kernel_getInstance, 0, 0, 0, 0);
        if (kernel_ptr && kernel_ptr + 0x84 + 4 <= img->mem_size) {
            uint32_t ai_engine = *(uint32_t*)(img->mem + kernel_ptr + 0x84);
            if (ai_engine && ai_engine + 0x18 + 4 <= img->mem_size) {
                s_cached_ai_stack = *(uint32_t*)(img->mem + ai_engine + 0x18);
                if (s_cached_ai_stack && g_debug_mode) {
                    printf("[+] AIStack temporary handles auto-cleanup active (AIStack=0x%08X)\n", s_cached_ai_stack);
                }
            }
        }
    }
    if (s_cached_ai_stack && s_cached_ai_stack + 0x18 + 4 <= img->mem_size) {
        uint32_t count = *(uint32_t*)(img->mem + s_cached_ai_stack + 0x18);
        if (count > 0) {
            dynarmic_call(host, s_sym_aistack_clearTempHandles, s_cached_ai_stack, 0, 0, 0);
        }
    }
}

/* Safely reinterpret a float's bit-pattern as a uint32_t.
 * Using *(uint32_t*)&f is undefined behavior in C++ (strict aliasing violation).
 * memcpy is guaranteed to produce the correct bit-pattern on any conforming compiler. */
static inline uint32_t float_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static void send_key(dynarmic_host_t *host, int android_code, int is_down) {
    if (is_down) {
        if (s_sym_engineOnKeyboardKeyDown) {
            // Pass unicode=0 so engine evaluates the keycode via its jump table.
            // A non-zero unicode value bypasses the keycode table and routes to Space (0x24).
            dynarmic_call(host, s_sym_engineOnKeyboardKeyDown, FAKE_ENV_ADDR, 0, android_code, 0);
        }
    } else {
        if (s_sym_engineOnKeyboardKeyUp) {
            dynarmic_call(host, s_sym_engineOnKeyboardKeyUp, FAKE_ENV_ADDR, 0, android_code, 0);
        }
    }
}

static void dispatch_pad_btn(dynarmic_host_t *host, int btn, int down) {
    if (btn < 0) return;
    if (btn == g_config.pad_up) send_key(host, AKEYCODE_DPAD_UP, down);
    else if (btn == g_config.pad_down) send_key(host, AKEYCODE_DPAD_DOWN, down);
    else if (btn == g_config.pad_left) send_key(host, AKEYCODE_DPAD_LEFT, down);
    else if (btn == g_config.pad_right) send_key(host, AKEYCODE_DPAD_RIGHT, down);
    else if (btn == g_config.pad_attack) send_key(host, AKEYCODE_BUTTON_Y, down);
    else if (btn == g_config.pad_jump) send_key(host, AKEYCODE_BUTTON_X, down);
    else if (btn == g_config.pad_roll) send_key(host, AKEYCODE_DPAD_CENTER, down);
    else if (btn == g_config.pad_block) send_key(host, AKEYCODE_BUTTON_L1, down);
    else if (btn == g_config.pad_potion) send_key(host, AKEYCODE_BUTTON_R1, down);
    else if (btn == g_config.pad_back) send_key(host, AKEYCODE_BACK, down);
    else if (btn == g_config.pad_pause) send_key(host, AKEYCODE_BUTTON_START, down);
}

// Touch phase constants (Android MotionEvent ACTION values)
#define TOUCH_PHASE_DOWN  0
#define TOUCH_PHASE_MOVE  1
#define TOUCH_PHASE_UP    2

// Guest memory scratch region for touch buffer: 0x00006000 (safe 256-byte area)
#define TOUCH_BUFFER_ADDR 0x00006000u

// send_touch: dispatches a single-finger engineOnTouchesChange event to the engine.
// The ShiVa3D touch buffer is a flat float array: [x, y, phase, x, y, phase, ...]
// Arguments: (JNIEnv*, jobject, jobject buffer, numTouches, screenW, screenH)
// Since engineOnTouchesChange has 6 args, the last 2 (screenW, screenH) go on the stack.
static void send_touch(dynarmic_host_t *host, elf32_image_t *img, float x, float y, int phase) {
    if (!s_sym_engineOnTouchesChange) return;

    // Write the touch data into guest memory (3 floats per touch point)
    float *buf = (float*)(img->mem + TOUCH_BUFFER_ADDR);
    buf[0] = x;
    buf[1] = y;
    buf[2] = (float)phase;

    // Stack args: screenW, screenH (guest engine operates in render resolution space)
    uint32_t stack_args[2] = { (uint32_t)g_render_width, (uint32_t)g_render_height };

    // Call: engineOnTouchesChange(env, obj, bufferAddr, numTouches=1, screenW, screenH)
    // r0=env, r1=obj(0), r2=bufferGuestAddr, r3=numTouches
    // Stack: screenW, screenH
    dynarmic_call_stack(host, s_sym_engineOnTouchesChange,
                        FAKE_ENV_ADDR,       // r0: JNIEnv*
                        0,                   // r1: jobject (this)
                        TOUCH_BUFFER_ADDR,   // r2: ByteBuffer jobject (= guest data address)
                        1,                   // r3: numTouches
                        stack_args, 2);
}

static const char* get_screenshot_dir(void) {
    static char s_prefix[64] = "";
    static int s_inited = 0;
    if (!s_inited) {
        s_inited = 1;
        if (_access("screenshots", 0) == 0) {
            strcpy(s_prefix, "screenshots/");
        } else if (_access("../screenshots", 0) == 0) {
            strcpy(s_prefix, "../screenshots/");
        } else {
            _mkdir("screenshots");
            strcpy(s_prefix, "screenshots/");
        }
    }
    return s_prefix;
}

#ifdef _WIN32
static int s_orig_stdout = -1;
static int s_orig_stderr = -1;
static FILE *s_log_file = NULL;
static FILE *s_archive_log_file = NULL;
static HANDLE s_pipe_read = NULL;
static HANDLE s_pipe_write = NULL;

static unsigned __stdcall log_tee_thread(void *arg) {
    char buf[4096];
    DWORD bytes = 0;
    while (ReadFile(s_pipe_read, buf, sizeof(buf) - 1, &bytes, NULL) && bytes > 0) {
        buf[bytes] = '\0';
        if (s_orig_stdout >= 0) {
            _write(s_orig_stdout, buf, bytes);
        }
        if (s_log_file) {
            fwrite(buf, 1, bytes, s_log_file);
            fflush(s_log_file);
        }
        if (s_archive_log_file) {
            fwrite(buf, 1, bytes, s_archive_log_file);
            fflush(s_archive_log_file);
        }
    }
    return 0;
}

static void setup_dual_logging() {
    _mkdir("logs");
    _mkdir("game/logs");

    s_log_file = fopen("pop_pc.log", "wb");
    if (!s_log_file) s_log_file = fopen("game/pop_pc.log", "wb");

    time_t now = time(NULL);
    struct tm tm_buf;
    memset(&tm_buf, 0, sizeof(tm_buf));
    struct tm *t = (localtime_s(&tm_buf, &now) == 0) ? &tm_buf : NULL;
    char archive_path[256];
    if (t) {
        snprintf(archive_path, sizeof(archive_path), "logs/pop_pc_%04d%02d%02d_%02d%02d%02d.log",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        snprintf(archive_path, sizeof(archive_path), "logs/pop_pc_latest.log");
    }
    s_archive_log_file = fopen(archive_path, "wb");
    if (!s_archive_log_file) {
        snprintf(archive_path, sizeof(archive_path), "game/%s", archive_path);
        s_archive_log_file = fopen(archive_path, "wb");
    }

    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (CreatePipe(&hRead, &hWrite, &sa, 0)) {
        s_pipe_read = hRead;
        s_pipe_write = hWrite;

        s_orig_stdout = _dup(_fileno(stdout));
        s_orig_stderr = _dup(_fileno(stderr));

        int write_fd = _open_osfhandle((intptr_t)hWrite, _O_WRONLY | _O_TEXT);
        if (write_fd >= 0) {
            _dup2(write_fd, _fileno(stdout));
            _dup2(write_fd, _fileno(stderr));
            /* write_fd is now redundant — stdout/stderr own the underlying handle.
             * Close it to avoid leaking a file descriptor. */
            _close(write_fd);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);

            uintptr_t th = _beginthreadex(NULL, 0, log_tee_thread, NULL, 0, NULL);
            if (th) CloseHandle((HANDLE)th);
        }
    }

}
#endif

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug") == 0) {
            g_debug_mode = true;
        } else if (strcmp(argv[i], "--no-debug") == 0) {
            g_debug_mode = false;
        }
    }

    bridge_libc_set_debug_mode(g_debug_mode ? 1 : 0);
    bridge_jni_set_debug_mode(g_debug_mode ? 1 : 0);
    bridge_gles_set_debug_mode(g_debug_mode ? 1 : 0);

#ifdef _WIN32
    if (g_debug_mode) {
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        setup_dual_logging();
    }
    /* Enable 1ms Windows timer resolution so SDL_Delay/Sleep() wakes
     * within ~1ms instead of the default 15.6ms. This is the primary
     * cause of frame-time jitter when software-capping at 60fps.
     * Balanced by timeEndPeriod(1) at shutdown. */
    timeBeginPeriod(1);
    /* Always initialize the QPC frequency regardless of build mode.
     * perf_now_us() divides by s_perf_qpf — if it stays at its default
     * value of 1, every timing value is ~10,000,000x too large and the
     * software FPS limiter never fires in release builds. */
    perf_init();
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    // Lock working directory strictly to the executable's own directory
#ifdef _WIN32
    char exe_dir[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_dir, MAX_PATH)) {
        char *last_slash = strrchr(exe_dir, '\\');
        if (!last_slash) last_slash = strrchr(exe_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            SetCurrentDirectoryA(exe_dir);
        }
    }
#endif

    int max_frames = 0;
    int tap_frame = 0;
    int click2_frame = 0;
    float click2_x = 0;
    float click2_y = 0;
    const char *click2_name = NULL;

    int click3_frame = 0;
    float click3_x = 0;
    float click3_y = 0;
    const char *click3_name = NULL;
    int key_action_frame = 0;
    int key_action_code = 0;
    const char *key_action_name = NULL;
    int key_action2_frame = 0;
    int key_action2_code = 0;
    const char *key_action2_name = NULL;
    bool is_level1_play = false;

    int auto_screenshot_interval = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot-interval") == 0 && i + 1 < argc) {
            auto_screenshot_interval = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--tap-frame") == 0 || strcmp(argv[i], "--tap") == 0) && i + 1 < argc) {
            tap_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--menu") == 0 && i + 1 < argc) {
            const char *mname = argv[++i];
            tap_frame = 70;
            if (max_frames == 0) max_frames = 300;
            click2_frame = 200;
            if (strcmp(mname, "world_map") == 0 || strcmp(mname, "worldmap") == 0) {
                click2_name = "world_map";
                click2_x = 180.0f; click2_y = 300.0f;
            } else if (strcmp(mname, "extras") == 0) {
                click2_name = "extras";
                click2_x = 300.0f; click2_y = 615.0f;
            } else if (strcmp(mname, "settings") == 0) {
                click2_name = "settings";
                click2_x = 50.0f; click2_y = 570.0f;
            } else if (strcmp(mname, "coins") == 0 || strcmp(mname, "store") == 0) {
                click2_name = "coins";
                click2_x = 1195.0f; click2_y = 540.0f;
            } else if (strcmp(mname, "new_game") == 0 || strcmp(mname, "newgame") == 0) {
                click2_name = "new_game";
                click2_x = 180.0f; click2_y = 195.0f;
                is_level1_play = true;
                if (max_frames < 1750) max_frames = 1750;
            } else {
                fprintf(stderr, "[-] Unknown menu name: %s\n", mname);
            }
        } else if (strcmp(argv[i], "--action") == 0 && i + 1 < argc) {
            const char *aname = argv[++i];
            if (max_frames < 320) max_frames = 320;
            if (strcmp(aname, "quit") == 0) {
                key_action_name = "key_back";
                key_action_frame = 200;
                key_action_code = AKEYCODE_BACK;
                click3_name = "quit_yes";
                click3_frame = 225;
                click3_x = 820.0f; click3_y = 355.0f;
                if (max_frames == 0) max_frames = 280;
            } else if (strcmp(aname, "back") == 0) {
                click3_name = "back";
                click3_frame = 200;
                click3_x = 55.0f; click3_y = 45.0f;
            } else if (strcmp(aname, "esc") == 0 || strcmp(aname, "key_back") == 0) {
                key_action_name = "key_back";
                key_action_frame = 200;
                key_action_code = AKEYCODE_BACK;
            } else if (strcmp(aname, "inventory") == 0) {
                click3_name = "inventory";
                click3_frame = 200;
                click3_x = 430.0f; click3_y = 45.0f;
            } else if (strcmp(aname, "store_tab") == 0) {
                click3_name = "store_tab";
                click3_frame = 200;
                click3_x = 230.0f; click3_y = 45.0f;
            } else if (strcmp(aname, "level1") == 0) {
                click3_name = "level1";
                click3_frame = 200;
                click3_x = 600.0f; click3_y = 180.0f;
                if (max_frames < 360) max_frames = 360;
            } else if (strcmp(aname, "friends") == 0) {
                click3_name = "friends";
                click3_frame = 200;
                click3_x = 150.0f; click3_y = 450.0f;
            } else if (strcmp(aname, "r1") == 0 || strcmp(aname, "tab_next") == 0) {
                key_action_name = "tab_next";
                key_action_frame = 200;
                key_action_code = AKEYCODE_BUTTON_R1;
            } else if (strcmp(aname, "l1") == 0 || strcmp(aname, "tab_prev") == 0) {
                key_action_name = "tab_prev";
                key_action_frame = 200;
                key_action_code = AKEYCODE_BUTTON_L1;
            } else if (strcmp(aname, "confirm") == 0 || strcmp(aname, "a") == 0) {
                key_action_name = "confirm";
                key_action_frame = 200;
                key_action_code = AKEYCODE_BUTTON_A;
            } else if (strcmp(aname, "level1_play") == 0) {
                is_level1_play = true;
                key_action_name = "confirm";
                key_action_frame = 200;
                key_action_code = AKEYCODE_BUTTON_A;
                key_action2_name = "play";
                key_action2_frame = 360;
                key_action2_code = AKEYCODE_BUTTON_A;
                if (max_frames < 1200) max_frames = 1200;
            // Store / Inventory subtab shortcuts - coordinates measured from phone_coins_store_live.png
            // Phone screenshot is 1456x816; game renders at 1280x720. Scale: x*1280/1456, y*720/816.
            // Icons sit at phone y≈815 (very bottom strip), phone x≈230/315/400/480.
            } else if (strcmp(aname, "weapons") == 0) {
                click3_name = "weapons";
                click3_frame = 250;
                click3_x = 280.0f; click3_y = 960.0f;
                if (max_frames < 320) max_frames = 320;
            } else if (strcmp(aname, "potions") == 0) {
                click3_name = "potions";
                click3_frame = 250;
                click3_x = 712.0f; click3_y = 892.0f;
                if (max_frames < 320) max_frames = 320;
            } else if (strcmp(aname, "combos") == 0) {
                click3_name = "combos";
                click3_frame = 250;
                click3_x = 560.0f; click3_y = 960.0f;
                if (max_frames < 320) max_frames = 320;
            } else if (strcmp(aname, "bundles") == 0) {
                click3_name = "bundles";
                click3_frame = 250;
                click3_x = 700.0f; click3_y = 960.0f;
                if (max_frames < 320) max_frames = 320;
            } else if (strcmp(aname, "buy") == 0 || strcmp(aname, "buy_sword") == 0) {
                click3_name = "buy_sword";
                click3_frame = 230;
                click3_x = 1090.0f; click3_y = 641.0f;
                if (max_frames < 300) max_frames = 300;
            } else if (strcmp(aname, "achievements") == 0) {
                click3_name = "achievements";
                click3_frame = 250;
                click3_x = 300.0f; click3_y = 265.0f;
                if (max_frames < 350) max_frames = 350;
            } else {
                fprintf(stderr, "[-] Unknown action: %s\n", aname);
            }
        }
    }

    printf("=============================================================================\n");
    printf(" Prince of Persia: Shadow and the Flame - Reignited [%s]\n", BUILD_TAG);
    printf(" Mode: %s\n", g_debug_mode ? "DEBUG (telemetry, logging, and profiling enabled)" : "PRODUCTION (optimized)");
    printf("=============================================================================\n");
    if (max_frames > 0) {
        printf("[+] Running with frame limit: %d frames\n", max_frames);
    }
    if (tap_frame > 0) {
        printf("[+] Scheduled simulated tap at frame: %d\n", tap_frame);
    }
    if (click2_name) {
        printf("[+] Scheduled simulated menu click on '%s' at frame %d (%0.1f, %0.1f)\n", click2_name, click2_frame, click2_x, click2_y);
    }

    /* Tell Windows this process handles DPI itself (per-monitor v2).
     * Without this, at e.g. 125% DPI a 1920x1080 window would be created as
     * 1920x1080 *logical* pixels = 2400x1350 *physical* pixels, overflowing
     * the monitor. With per-monitor DPI awareness the values we pass to
     * SDL_CreateWindow are always physical pixels, matching the config exactly.
     * SDL_HINT_WINDOWS_DPI_AWARENESS requires SDL >= 2.24.0; on older builds
     * it silently does nothing but won't break anything. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[-] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Open first available game controller
    SDL_GameController *controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            if (controller) {
                if (g_debug_mode) {
                    printf("[+] Game controller connected: %s\n", SDL_GameControllerName(controller));
                }
                break;
            }
        }
    }

    g_config.load("config.ini");
    soft_mods_config_t sm_cfg = {};
    sm_cfg.clean_main_menu = g_config.mod_clean_main_menu;
    sm_cfg.clean_shop = g_config.mod_clean_shop;
    sm_cfg.clean_level_complete = g_config.mod_clean_level_complete;
    sm_cfg.clean_touch_hud = g_config.mod_clean_touch_hud;
    soft_mods_init(&sm_cfg);
#ifdef _WIN32
    ensure_compatibility_flags();
    apply_windows_gpu_preference(g_config.gpu_preference);
#endif
    g_render_width = g_config.width;
    g_render_height = g_config.height;
    g_window_width = g_config.width;
    g_window_height = g_config.height;
    g_max_anisotropy = (float)g_config.anisotropic;
    bridge_jni_set_screen_size(g_render_width, g_render_height);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    if (g_config.msaa > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_config.msaa);
    } else {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }

    Uint32 win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (g_config.fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    const char *win_title = g_debug_mode
        ? "Prince of Persia: Shadow and the Flame - Reignited [" BUILD_TAG "]"
        : "Prince of Persia: Shadow and the Flame - Reignited";

    SDL_Window *window = SDL_CreateWindow(
        win_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_window_width, g_window_height,
        win_flags
    );

    // Requested MSAA level may be unsupported by the GPU/driver: step down until a pixel format works
    for (int samples = (g_config.msaa > 2) ? g_config.msaa / 2 : 0; !window && g_config.msaa > 0; samples = (samples > 2) ? samples / 2 : 0) {
        fprintf(stderr, "[!] %dx MSAA unsupported, retrying with %dx\n", g_config.msaa, samples);
        g_config.msaa = samples;
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, samples > 0 ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, samples);
        window = SDL_CreateWindow(
            win_title,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            g_window_width, g_window_height,
            win_flags
        );
    }

    if (!window) {
        fprintf(stderr, "[-] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "[-] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }

    // Query physical backbuffer dimensions and configure resolution scaling
    int draw_w = 0, draw_h = 0;
    SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);
    update_viewport_scaling(draw_w, draw_h);

    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.refresh_rate > 0) {
        s_display_hz = dm.refresh_rate;
    }

    s_gl_vendor   = (const char*)glGetString(GL_VENDOR);
    s_gl_renderer = (const char*)glGetString(GL_RENDERER);
    const char *gl_version  = (const char*)glGetString(GL_VERSION);
    if (g_debug_mode) {
        printf("[+] Host OpenGL: %s | %s | %s\n",
               s_gl_vendor ? s_gl_vendor : "Unknown",
               s_gl_renderer ? s_gl_renderer : "Unknown",
               gl_version ? gl_version : "Unknown");
    }

    bool is_igpu = is_active_gpu_integrated(s_gl_vendor, s_gl_renderer);

#ifdef _WIN32
    ensure_dwm_init();
    const SystemGpuTopology &topo = get_system_gpu_topology();

    // On Optimus/Enduro hybrid laptops the user may have forced the discrete GPU via
    // GPUPreference=2 in config.ini (takes effect on the NEXT launch because the
    // NvOptimusEnablement/AmdPowerXpressRequestHighPerformance exports on this binary
    // already instructed the driver to use the iGPU for this launch).
    // If the registry preference took effect and the active context is on the dGPU,
    // driver VSync via wglSwapIntervalEXT(1) is unreliable on Optimus: the dGPU
    // renders but must copy frames over PCIe to the iGPU for display; VSync can
    // stall at 30 FPS (double-buffer queue stall) or not block at all (tearing).
    // Fall back to swap_interval=0 + DWM + software hybrid-sleep limiter instead.
    bool is_optimus_dgpu = topo.is_hybrid_system && !is_igpu;
    if (is_optimus_dgpu && g_debug_mode) {
        printf("[+] Optimus/hybrid dGPU active (frames are copied to the display-attached iGPU).\n");
    }
    // Warn about one-launch delay: GPUPreference=2 is set, but NvOptimusEnablement=0
    // on this binary told the driver to use the iGPU for *this* launch. The registry
    // key written by apply_windows_gpu_preference() takes effect on the next launch.
    if (topo.is_hybrid_system && is_igpu && g_config.gpu_preference == 2) {
        printf("[!] GPUPreference=2 (High Performance GPU) is set but the integrated GPU is active. "
               "If the exe was started directly (not via the launcher), the registry preference applies on the NEXT launch.\n");
    }
#else
    bool is_optimus_dgpu = false;
#endif

    // Frame pacing strategy:
    //   iGPU:         swap=0 — wglSwapIntervalEXT(1) causes a 30fps stall under DWM;
    //                 DWM composition + software hybrid-sleep limiter gives tear-free 60fps.
    //   Optimus dGPU: same as iGPU — driver VSync unreliable over PCIe cross-adapter path.
    //   Desktop dGPU: swap=vsync ? 1 : 0 — driver VSync works correctly on a single GPU
    //                 directly wired to the display.
    // VSyncMode in config.ini: 0 = auto, 1 = force driver swap interval, 2 = force DWM flush pacing.
    // Auto uses the driver swap interval on discrete GPUs (including the dGPU of a hybrid laptop, where
    // the DWM-only path was observed to tear) and DWM pacing on integrated GPUs.
    int swap_interval = 0;
    if (g_config.vsync) {
        if (g_config.vsync_mode == 1) swap_interval = 1;
        else if (g_config.vsync_mode == 2) swap_interval = 0;
        else swap_interval = is_igpu ? 0 : 1;
    }
    int vsync_res = SDL_GL_SetSwapInterval(swap_interval);
    if (vsync_res < 0) {
        fprintf(stderr, "[-] Warning: SDL_GL_SetSwapInterval(%d) failed: %s\n", swap_interval, SDL_GetError());
    } else if (g_debug_mode) {
        printf("[+] VSync %s successfully (driver swap interval = %d, display = %d Hz)\n",
               g_config.vsync ? "enabled" : "disabled", swap_interval, s_display_hz);
    }

    int db = 0, ms_buf = 0, ms_samp = 0;
    SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &db);
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &ms_buf);
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &ms_samp);
    int swap_ctrl = SDL_GL_GetSwapInterval();
    s_swap_control = swap_ctrl;

    // Resolve how presentation is paced. VSync on means the display refresh paces the game and the
    // FPS limit is ignored; VSync off means the FPS limit paces it.
    if (g_config.vsync) {
        if (swap_ctrl > 0) {
            s_vsync_mode = VSYNC_DRIVER;
        }
#ifdef _WIN32
        else if (s_pfn_DwmFlush) {
            s_vsync_mode = VSYNC_DWM;
        }
#endif
        else {
            s_vsync_mode = VSYNC_SOFTWARE;
        }
    } else {
        s_vsync_mode = VSYNC_OFF;
    }
    if (g_debug_mode) {
        static const char *mode_names[] = { "off (FPS limiter)", "driver swap interval", "DWM compositor flush", "software limiter at refresh rate" };
        printf("[+] Frame pacing: %s\n", mode_names[s_vsync_mode]);
    }
    if (g_debug_mode) {
        printf("[+] GL Attributes: DoubleBuffer=%d, MultiSampleBuffers=%d, Samples=%d, SwapControl=%d\n",
               db, ms_buf, ms_samp, swap_ctrl);
    }

#ifdef _WIN32
    {
        const char *scanout_status;
        if (is_optimus_dgpu) {
            scanout_status = "Optimus Cross-Adapter dGPU (Software Frame Limiter / DWM Tear-Free)";
        } else if (is_igpu) {
            scanout_status = "Display-Attached Integrated GPU (Direct Hardware Scanout / Tear-Free VSync)";
        } else {
            scanout_status = "Display-Attached Discrete GPU (Direct Hardware Scanout / Driver VSync)";
        }
        if (g_debug_mode) {
            printf("[+] Display Topology: %s\n", scanout_status);
        }
    }
#endif

    show_startup_splash(window);

    bridge_gles_init();

    // Hybrid-laptop dGPUs hand out a window framebuffer with no MSAA and an ignored swap interval. Render into our
    // own (multisampled) framebuffer instead and present it with one short, vblank-aligned blit.
    if (is_optimus_dgpu || (g_config.msaa > 0 && ms_buf == 0)) {
        int off_samples = bridge_gles_offscreen_init(g_render_width, g_render_height, g_config.msaa);
        if (off_samples >= 0) {
            s_offscreen = true;
            printf("[+] Offscreen framebuffer active (%dx%d, %dx MSAA)\n", g_render_width, g_render_height, off_samples);
        } else {
            fprintf(stderr, "[-] Offscreen framebuffer unavailable; using the window framebuffer directly.\n");
        }
    }
    perf_open_log();
    bridge_openal_init();
    bridge_libc_set_root_dir(".");

    // Clean up any stray temporary files left on disk by earlier unpatched builds
    remove(".nomedia");
    remove("./.nomedia");
    remove("~tempgd!ms!k!gr!c!m!gff!crs.xkv");
    remove("~tempgd!ms!k!gr!c!m!n!g!q.xkv");

    // Guest address space size: 512 MB provides 448 MB for the segregated free-list heap
    // (active gameplay stabilizes at ~45-70 MB) and 32 MB for the ARM stack, reducing
    // process memory commitment from the initial 3 GB debug allocation down to a lean 512 MB.
    const uint32_t guest_mem_size = 512u * 1024u * 1024u;
    if (g_debug_mode) {
        printf("[+] Creating guest address space (%u MB)...\n", guest_mem_size >> 20);
    }
    elf32_image_t *img = elf32_image_create(guest_mem_size);
    if (!img) {
        fprintf(stderr, "[-] Failed to allocate guest image!\n");
        return 1;
    }
    g_guest_memory_base = img->mem;
    bridge_libc_init_heap(img);

    // Set up Android JNI and JavaVM tables in guest memory
    bridge_jni_setup(img);

    // Ensure S3DMain.stk is available in the game directory
    FILE *f_stk = fopen("S3DMain.stk", "rb");
    if (!f_stk) {
        fprintf(stderr, "[-] S3DMain.stk not found in game directory!\n");
#ifdef _WIN32
        MessageBoxA(NULL,
            "S3DMain.stk is missing from the game directory!\n\n"
            "Please run PoPSnF_Launcher.exe and click 'Extract Game Assets' to unpack your APK first.",
            "Assets Missing", MB_ICONERROR | MB_OK);
#endif
        return 1;
    }
    fclose(f_stk);

    // Strictly check and load libS3DClient.so from local game directory
    const char *so_path = "libS3DClient.so";
    FILE *f_check = fopen(so_path, "rb");
    if (!f_check) {
        fprintf(stderr, "[-] libS3DClient.so not found in game directory!\n");
#ifdef _WIN32
        MessageBoxA(NULL,
            "libS3DClient.so is missing from the game directory!\n\n"
            "Please run PoPSnF_Launcher.exe and click 'Extract Game Assets' to unpack your APK first.",
            "Library Missing", MB_ICONERROR | MB_OK);
#endif
        return 1;
    }
    fclose(f_check);

    if (g_debug_mode) {
        printf("[+] Loading %s into guest address space...\n", so_path);
    }
    elf32_module_t *main_mod_init = elf32_load_module(img, so_path, 0x01000000);
    if (!main_mod_init) {
        fprintf(stderr, "[-] Failed to load %s!\n", so_path);
        return 1;
    }
    const uint32_t main_mod_idx = img->module_count - 1;
    const uint32_t mod_base = main_mod_init->base;

    // Apply soft-mods patches (if enabled)
    soft_mods_apply_patches(img);

    // Resolve JNI symbols
    s_sym_JNI_OnLoad                = elf32_lookup_symbol(img, "JNI_OnLoad");
    s_sym_engineInitialize          = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineInitialize");
    s_sym_engineRunOneFrame         = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineRunOneFrame");
    s_sym_engineDidPassFirstFrame    = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineDidPassFirstFrame");
    s_sym_engineSetSystemVersion    = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineSetSystemVersion");
    s_sym_engineSetDirectories      = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineSetDirectories");
    s_sym_enginePause               = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_enginePause");
    s_sym_engineSurfaceCreated      = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnSurfaceCreated");
    s_sym_engineSurfaceChanged      = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnSurfaceChanged");
    s_sym_engineOnMouseMove         = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnMouseMove");
    s_sym_S3DClient_iPhone_OnMouseMoved = elf32_lookup_symbol(img, "S3DClient_iPhone_OnMouseMoved");
    s_sym_engineOnMouseButtonDown   = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnMouseButtonDown");
    s_sym_engineOnMouseButtonUp     = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnMouseButtonUp");
    s_sym_engineOnKeyboardKeyDown   = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnKeyboardKeyDown");
    s_sym_engineOnKeyboardKeyUp     = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnKeyboardKeyUp");
    s_sym_engineOnTouchesChange     = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnTouchesChange");
    s_sym_setBuildType              = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_POP2_SetBuildType");
    s_sym_setVersionCode            = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_POP2_SetVersionCode");
    s_sym_initMobileSDK             = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_POP2_InitMobileSDK");
    s_sym_windowFocusChange         = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_POP2_windowFocusChange");
    s_sym_engineGetOverlayMovieHasChanged = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineGetOverlayMovieHasChanged");
    s_sym_engineOnOverlayMovieStopped     = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineOnOverlayMovieStopped");
    s_sym_engineGetOverlayMovie     = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineGetOverlayMovie");
    s_sym_engineGetWantSwapBuffers  = elf32_lookup_symbol(img, "Java_com_ubisoft_pop2_S3DRenderer_engineGetWantSwapBuffers");
    s_sym_sendEventToCurrentUser    = elf32_lookup_symbol(img, "S3DClient_SendEventToCurrentUser");
    s_sym_resetStringPool           = elf32_lookup_symbol(img, "_ZN4S3DX10AIVariable15ResetStringPoolEj");
    s_sym_kernel_getInstance       = elf32_lookup_symbol(img, "_ZN7Pandora10EngineCore6Kernel11GetInstanceEv");
    s_sym_aistack_clearTempHandles = elf32_lookup_symbol(img, "_ZN7Pandora10EngineCore7AIStack21ClearTemporaryHandlesEv");


    // Engine binary patches:
    // Control tutorial popups: NOP out the 5 tutorial popups that re-trigger settings
    uint32_t nop = 0xE1A00000u;
    uint32_t patch_offsets[] = { 0x1B1CE0, 0x2019E8, 0x201D0C, 0x201EBC, 0x201F94 };
    for (uint32_t off : patch_offsets) {
        memcpy(img->mem + mod_base + off, &nop, 4);
    }
    if (g_debug_mode) {
        printf("[+] Applied control tutorial patches (5 NOPs)\n");
    }

    // User event hook: Patch S3DClient_InstallCurrentUserEventHook to return immediately (BX LR)
    uint32_t sym_hook = elf32_lookup_symbol(img, "S3DClient_InstallCurrentUserEventHook");
    if (sym_hook) {
        uint32_t bx_lr = 0xE12FFF1Eu;
        memcpy(img->mem + (sym_hook & ~1u), &bx_lr, 4);
        if (g_debug_mode) {
            printf("[+] Patched S3DClient_InstallCurrentUserEventHook (BX LR)\n");
        }
    }

    // Display info query: Patch GFXDevice_Window_Android_GetDefaultDisplayInfo to report configured resolution
    // ARM MOVW encoding: 0xE3000000 | (imm4 << 16) | (Rd << 12) | imm12
    // where imm16 = imm4:imm12. R3=scratch, R0/R1/R2 = out ptrs.
    {
        uint32_t w = (uint32_t)g_render_width;
        uint32_t h = (uint32_t)g_render_height;
        // movw r3, #width   (bits[19:16]=imm4, bits[11:0]=imm12)
        uint32_t movw_w = 0xE3003000u | ((w & 0xF000u) << 4) | (w & 0x0FFFu);
        // movw r3, #height
        uint32_t movw_h = 0xE3003000u | ((h & 0xF000u) << 4) | (h & 0x0FFFu);
        uint32_t patch_disp[] = {
            movw_w,       // movw r3, #width
            0xE1C030B0u, // strh r3, [r0]
            movw_h,       // movw r3, #height
            0xE1C130B0u, // strh r3, [r1]
            0xE3A030A0u, // mov  r3, #160   (dpi)
            0xE1C230B0u, // strh r3, [r2]
            0xE3A00001u, // mov  r0, #1     (return 1 = success)
            0xE12FFF1Eu  // bx   lr
        };
        memcpy(img->mem + mod_base + 0x7FDA90, patch_disp, sizeof(patch_disp));
        if (g_debug_mode) {
            printf("[+] Patched GFXDevice_Window_Android_GetDefaultDisplayInfo (%dx%d@160dpi)\n",
                   g_render_width, g_render_height);
        }
    }

    // Worker threads: Patch Thread::CreateThreadEv to return 0 (disables background threads, enables synchronous loaders)
    uint32_t sym_create_thread = elf32_lookup_symbol(img, "_ZN7Pandora10EngineCore6Thread12CreateThreadEv");
    if (!sym_create_thread) sym_create_thread = mod_base + 0x004F4580;
    if (sym_create_thread) {
        uint32_t patch_nothread[] = {
            0xE3A01000u, // mov r1, #0
            0xE5C01004u, // strb r1, [r0, #4] (IsRunning = false)
            0xE3A00000u, // mov r0, #0        (return 0)
            0xE12FFF1Eu  // bx lr
        };
        memcpy(img->mem + (sym_create_thread & ~1u), patch_nothread, sizeof(patch_nothread));
        if (g_debug_mode) {
            printf("[+] Patched Thread::CreateThreadEv (disable threads, enable synchronous loaders)\n");
        }
    }

    // Particle system updater: Patch GFXParticleSystemUpdater::PushParticleSystem (0x7A5C4C)
    // When background threads are disabled, PushParticleSystem returns 0 without updating particles,
    // causing particle effects (sword elemental glows, torches, swirls, dust) to stay frozen on frame 0.
    // By tail-calling GFXParticleSystemInstance::UpdateParticles (0x7A33F4) directly,
    // all particle systems update and animate synchronously on the render thread every frame!
    static const uint32_t patch_particle_updater[] = {
        0xe92d4010u, // 0x7a5c4c: push {r4, lr}
        0xe1a00001u, // 0x7a5c50: mov  r0, r1        (r0 = instance)
        0xe1a01002u, // 0x7a5c54: mov  r1, r2        (r1 = dt)
        0xe3a02000u, // 0x7a5c58: mov  r2, #0        (r2 = 0)
        0xebfff5e4u, // 0x7a5c5c: bl   0x7a33f4      (UpdateParticles)
        0xe3a00001u, // 0x7a5c60: mov  r0, #1        (return 1)
        0xe8bd8010u  // 0x7a5c64: pop  {r4, pc}
    };
    memcpy(img->mem + mod_base + 0x7a5c4c, patch_particle_updater, sizeof(patch_particle_updater));
    if (g_debug_mode) {
        printf("[+] Patched GFXParticleSystemUpdater::PushParticleSystem (synchronous particle animation)\n");
    }

    // HUD Z-order tie-breaker: HUDTree::SortElementsByZOrderFunc (0x610ba8):
    // Preserved original engine logic: ascending m_zOrder, with ascending m_index tie-breaker
    // (background strips created first at lower index, foreground icons/text created later at higher index).
    // Native bridge wrap_qsort handles this via hud_zorder_compare in bridge_libc.c.

    // System version and path metadata buffers
    strcpy((char*)(img->mem + STR_VER_ADDR), "v.1.0-pc");
    strcpy((char*)(img->mem + STR_DIR_ADDR), ".");

    // Create Dynarmic ARMv7 JIT
    if (g_debug_mode) {
        printf("[+] Initializing Dynarmic ARMv7 JIT...\n");
    }
    dynarmic_host_t *host = dynarmic_host_create(img);
    if (!host) {
        fprintf(stderr, "[-] Failed to initialize Dynarmic host!\n");
        return 1;
    }

    // Freeze watchdog: samples the guest PC/LR every couple of seconds on a
    // background thread. If the main thread is stuck inside a single
    // jit->Run() call (e.g. a retry-forever loop after a failed allocation,
    // or a busy-wait on a flag that a now-disabled worker thread would have
    // set), the main thread can't print anything itself - this is a
    // best-effort way to still get a PC/LR reading of where it's stuck.
    // g_guest_pc/g_guest_lr are read here without synchronization; that's a
    // benign data race for a diagnostic-only 32-bit read and not worth the
    // complexity of atomics for this purpose.
    std::atomic<bool> watchdog_running{true};
    std::unique_ptr<std::thread> watchdog_thread;
    if (g_debug_mode) {
        watchdog_thread = std::make_unique<std::thread>([&watchdog_running]() {
            uint32_t last_pc = 0;
            uint32_t last_frame = 0;
            int stuck_ticks = 0;
            const int kSampleMs = 2000;
            const int kPollMs = 200;
            while (watchdog_running.load(std::memory_order_relaxed)) {
                // Sleep in short slices so shutdown isn't delayed by up to a
                // full sample interval waiting for this thread to notice.
                for (int waited = 0; waited < kSampleMs; waited += kPollMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                    if (!watchdog_running.load(std::memory_order_relaxed)) return;
                }
                uint32_t cur_frame = g_engine_frame_cnt.load(std::memory_order_relaxed);
                uint32_t pc = g_guest_pc;
                uint32_t lr = g_guest_lr;
                // Only report stuck if NO frames have advanced AND the guest PC hasn't changed
                if (cur_frame == last_frame && pc != 0 && pc == last_pc) {
                    stuck_ticks++;
                    fprintf(stderr, "[WATCHDOG] guest PC hasn't advanced in ~%ds (stuck at frame %u): pc=0x%08X lr=0x%08X\n",
                            stuck_ticks * (kSampleMs / 1000), cur_frame, pc, lr);
                } else {
                    stuck_ticks = 0;
                }
                last_pc = pc;
                last_frame = cur_frame;
            }
        });
    }

    // Execute .init_array constructors
    elf32_module_t *main_mod = elf32_get_module(img, main_mod_idx);
    if (main_mod && main_mod->init_array && main_mod->init_array_count > 0) {
        if (g_debug_mode) {
            printf("[+] Executing %u .init_array constructors...\n", main_mod->init_array_count);
        }
        for (uint32_t i = 0; i < main_mod->init_array_count; ++i) {
            uint32_t fn_addr = main_mod->init_array[i];
            if (fn_addr != 0 && fn_addr != 0xFFFFFFFFu) {
                dynarmic_call(host, fn_addr, 0, 0, 0, 0);
            }
        }
        if (g_debug_mode) {
            printf("[+] Static constructors initialized!\n");
        }
    }

    // Call JNI_OnLoad
    if (s_sym_JNI_OnLoad) {
        if (g_debug_mode) {
            printf("[+] Calling JNI_OnLoad...\n");
        }
        dynarmic_call(host, s_sym_JNI_OnLoad, FAKE_VM_ADDR, 0, 0, 0);
    }

    // Call POP2 startup configurations
    if (s_sym_setBuildType) {
        if (g_debug_mode) {
            printf("[+] Setting build type to 1 (Google Play)...\n");
        }
        dynarmic_call(host, s_sym_setBuildType, FAKE_ENV_ADDR, 0, 1, 0);
    }
    if (s_sym_setVersionCode) {
        if (g_debug_mode) {
            printf("[+] Setting version code...\n");
        }
        dynarmic_call(host, s_sym_setVersionCode, FAKE_ENV_ADDR, 0, STR_VER_ADDR, 0);
    }

    // Call engineSetSystemVersion
    if (s_sym_engineSetSystemVersion) {
        if (g_debug_mode) {
            printf("[+] Setting system version...\n");
        }
        dynarmic_call(host, s_sym_engineSetSystemVersion, FAKE_ENV_ADDR, 0, STR_VER_ADDR, 0);
    }

    // Call engineSetDirectories (3 path args: cache, home, pack)
    if (s_sym_engineSetDirectories) {
        if (g_debug_mode) {
            printf("[+] Setting engine directories...\n");
        }
        uint32_t stack_args[] = { STR_DIR_ADDR };
        dynarmic_call_stack(host, s_sym_engineSetDirectories, FAKE_ENV_ADDR, 0, STR_DIR_ADDR, STR_DIR_ADDR, stack_args, 1);
    }

    // Call surface created & changed
    if (s_sym_engineSurfaceCreated) {
        if (g_debug_mode) {
            printf("[+] Creating engine surface...\n");
        }
        dynarmic_call(host, s_sym_engineSurfaceCreated, 0, 0, 0, 0);
    }
    if (s_sym_engineSurfaceChanged) {
        if (g_debug_mode) {
            printf("[+] Setting surface dimensions (%dx%d)...\n", g_render_width, g_render_height);
        }
        dynarmic_call(host, s_sym_engineSurfaceChanged, FAKE_ENV_ADDR, 0, g_render_width, g_render_height);
    }

    // Initialize engine
    if (s_sym_engineInitialize) {
        if (g_debug_mode) {
            printf("[+] Calling engineInitialize...\n");
        }
        dynarmic_call(host, s_sym_engineInitialize, FAKE_ENV_ADDR, 0, 0, 0);
    }

    // Inform engine of window focus
    if (s_sym_windowFocusChange) {
        if (g_debug_mode) {
            printf("[+] Informing engine of Window Focus (true)...\n");
        }
        dynarmic_call(host, s_sym_windowFocusChange, FAKE_ENV_ADDR, 0, 1, 0);
    }

    if (s_sym_enginePause) {
        dynarmic_call(host, s_sym_enginePause, FAKE_ENV_ADDR, 0, 0, 0);
    }

    // Expand S3DX::AIVariable StringPool from default 1MB to 16MB
    if (s_sym_resetStringPool) {
        if (g_debug_mode) {
            printf("[+] Expanding S3DX::AIVariable::StringPool to 16 MB...\n");
        }
        dynarmic_call(host, s_sym_resetStringPool, 16 * 1024 * 1024, 0, 0, 0);
    }

    if (g_debug_mode) {
        printf("[+] Entering game loop!\n");
    }

    bool running = true;
    SDL_Event event;

    int cur_w = g_window_width;
    int cur_h = g_window_height;
    bool stick_up = false, stick_down = false, stick_left = false, stick_right = false;
    bool trigger_lt = false, trigger_rt = false;

    struct PendingWheelPulse {
        int keycode;
        int frames_remaining;
    };
    std::vector<PendingWheelPulse> pending_wheel_pulses;

    while (running) {
        if (bridge_is_exit_requested()) {
            if (g_debug_mode) {
                printf("[+] Bridge exit requested (code=%d). Terminating main loop...\n", bridge_get_exit_code());
            }
            running = false;
            break;
        }

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        int draw_w = 0, draw_h = 0;
                        SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);
                        update_viewport_scaling(draw_w, draw_h);
                        cur_w = g_window_width;
                        cur_h = g_window_height;
                    }
                    break;

                /* Keyboard Controls */
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    // Ignore OS auto-repeat: SDL fires a continuous stream of
                    // SDL_KEYDOWN events (event.key.repeat != 0) for as long as
                    // a key is held; SDL_KEYUP is never a repeat. Forwarding
                    // every repeat as its own engineOnKeyboardKeyDown() call
                    // was found to leak guest heap - the original touchscreen
                    // build only ever sees one discrete event per tap, never a
                    // rapid-fire repeat, so simply holding a movement key here
                    // could exhaust the heap in well under a minute.
                    if (event.key.repeat) break;
                    int down = (event.type == SDL_KEYDOWN);
                    SDL_Scancode sc = event.key.keysym.scancode;

                    if (sc == g_config.key_up || (g_config.key_sec_up && sc == g_config.key_sec_up))
                        send_key(host, AKEYCODE_DPAD_UP, down);
                    else if (sc == g_config.key_down || (g_config.key_sec_down && sc == g_config.key_sec_down))
                        send_key(host, AKEYCODE_DPAD_DOWN, down);
                    else if (sc == g_config.key_left || (g_config.key_sec_left && sc == g_config.key_sec_left))
                        send_key(host, AKEYCODE_DPAD_LEFT, down);
                    else if (sc == g_config.key_right || (g_config.key_sec_right && sc == g_config.key_sec_right))
                        send_key(host, AKEYCODE_DPAD_RIGHT, down);
                    else if (sc == g_config.key_attack || (g_config.key_sec_attack && sc == g_config.key_sec_attack))
                        send_key(host, AKEYCODE_BUTTON_Y, down);
                    else if (sc == g_config.key_jump || (g_config.key_sec_jump && sc == g_config.key_sec_jump))
                        send_key(host, AKEYCODE_BUTTON_X, down);
                    else if (sc == g_config.key_roll || (g_config.key_sec_roll && sc == g_config.key_sec_roll))
                        send_key(host, AKEYCODE_DPAD_CENTER, down);
                    else if (sc == g_config.key_block || (g_config.key_sec_block && sc == g_config.key_sec_block))
                        send_key(host, AKEYCODE_BUTTON_L1, down);
                    else if (sc == g_config.key_potion || (g_config.key_sec_potion && sc == g_config.key_sec_potion))
                        send_key(host, AKEYCODE_BUTTON_R1, down);
                    else if (sc == g_config.key_back || (g_config.key_sec_back && sc == g_config.key_sec_back))
                        send_key(host, AKEYCODE_BACK, down);
                    else if (sc == g_config.key_pause || (g_config.key_sec_pause && sc == g_config.key_sec_pause))
                        send_key(host, AKEYCODE_BUTTON_START, down);
                    else if (sc == SDL_SCANCODE_F12 || sc == SDL_SCANCODE_PRINTSCREEN) {
                        if (down) {
                            char sname[128];
                            static int s_manual_ss = 0;
                            snprintf(sname, sizeof(sname), "%s%s_manual_%03d.bmp", get_screenshot_dir(), BUILD_TAG, ++s_manual_ss);
                            bridge_gles_save_screenshot(sname, cur_w, cur_h);
                            printf("[+] Manual screenshot captured: %s\n", sname);
                        }
                    }
                    break;
                }

                /* Gamepad Controller Buttons */
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    int down = (event.type == SDL_CONTROLLERBUTTONDOWN);
                    dispatch_pad_btn(host, (int)event.cbutton.button, down);
                    break;
                }

                /* Gamepad Analog Stick & Triggers */
                case SDL_CONTROLLERAXISMOTION: {
                    if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                        float val = event.caxis.value / 32767.0f;
                        if (val < -0.5f && !stick_left)  { send_key(host, AKEYCODE_DPAD_LEFT, 1); stick_left = true; }
                        if (val > -0.2f && stick_left)   { send_key(host, AKEYCODE_DPAD_LEFT, 0); stick_left = false; }
                        if (val > 0.5f && !stick_right)  { send_key(host, AKEYCODE_DPAD_RIGHT, 1); stick_right = true; }
                        if (val < 0.2f && stick_right)   { send_key(host, AKEYCODE_DPAD_RIGHT, 0); stick_right = false; }
                    } else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                        float val = event.caxis.value / 32767.0f;
                        if (val < -0.5f && !stick_up)    { send_key(host, AKEYCODE_DPAD_UP, 1); stick_up = true; }
                        if (val > -0.2f && stick_up)     { send_key(host, AKEYCODE_DPAD_UP, 0); stick_up = false; }
                        if (val > 0.5f && !stick_down)   { send_key(host, AKEYCODE_DPAD_DOWN, 1); stick_down = true; }
                        if (val < 0.2f && stick_down)    { send_key(host, AKEYCODE_DPAD_DOWN, 0); stick_down = false; }
                    } else if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
                        if (event.caxis.value > 16000 && !trigger_lt) {
                            dispatch_pad_btn(host, PAD_BIND_LT, 1);
                            trigger_lt = true;
                        } else if (event.caxis.value < 8000 && trigger_lt) {
                            dispatch_pad_btn(host, PAD_BIND_LT, 0);
                            trigger_lt = false;
                        }
                    } else if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                        if (event.caxis.value > 16000 && !trigger_rt) {
                            dispatch_pad_btn(host, PAD_BIND_RT, 1);
                            trigger_rt = true;
                        } else if (event.caxis.value < 8000 && trigger_rt) {
                            dispatch_pad_btn(host, PAD_BIND_RT, 0);
                            trigger_rt = false;
                        }
                    }
                    break;
                }

                case SDL_CONTROLLERDEVICEADDED: {
                    if (!controller) {
                        controller = SDL_GameControllerOpen(event.cdevice.which);
                        if (controller && g_debug_mode) {
                            printf("[+] Game controller connected: %s\n", SDL_GameControllerName(controller));
                        }
                    }
                    break;
                }

                case SDL_CONTROLLERDEVICEREMOVED: {
                    if (controller) {
                        SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller);
                        if (joy && SDL_JoystickInstanceID(joy) == event.cdevice.which) {
                            if (g_debug_mode) {
                                printf("[-] Game controller disconnected: %s\n", SDL_GameControllerName(controller));
                            }
                            SDL_GameControllerClose(controller);
                            controller = nullptr;
                        }
                    }
                    break;
                }

                /* Mouse Scroll Wheel */
                case SDL_MOUSEWHEEL: {
                    int dy = event.wheel.y;
                    int dx = event.wheel.x;
#if SDL_VERSION_ATLEAST(2, 0, 4)
                    if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                        dy = -dy;
                        dx = -dx;
                    }
#endif
                    int wheel_bind = 0;
                    if (dy > 0) wheel_bind = MOUSE_BIND_WHEEL_UP;
                    else if (dy < 0) wheel_bind = MOUSE_BIND_WHEEL_DOWN;
                    else if (dx > 0) wheel_bind = MOUSE_BIND_WHEEL_RIGHT;
                    else if (dx < 0) wheel_bind = MOUSE_BIND_WHEEL_LEFT;

                    if (wheel_bind != 0 && soft_mods_is_in_gameplay()) {
                        int target_key = 0;
                        if (wheel_bind == g_config.key_up || (g_config.key_sec_up && wheel_bind == g_config.key_sec_up))
                            target_key = AKEYCODE_DPAD_UP;
                        else if (wheel_bind == g_config.key_down || (g_config.key_sec_down && wheel_bind == g_config.key_sec_down))
                            target_key = AKEYCODE_DPAD_DOWN;
                        else if (wheel_bind == g_config.key_left || (g_config.key_sec_left && wheel_bind == g_config.key_sec_left))
                            target_key = AKEYCODE_DPAD_LEFT;
                        else if (wheel_bind == g_config.key_right || (g_config.key_sec_right && wheel_bind == g_config.key_sec_right))
                            target_key = AKEYCODE_DPAD_RIGHT;
                        else if (wheel_bind == g_config.key_attack || (g_config.key_sec_attack && wheel_bind == g_config.key_sec_attack))
                            target_key = AKEYCODE_BUTTON_Y;
                        else if (wheel_bind == g_config.key_jump || (g_config.key_sec_jump && wheel_bind == g_config.key_sec_jump))
                            target_key = AKEYCODE_BUTTON_X;
                        else if (wheel_bind == g_config.key_roll || (g_config.key_sec_roll && wheel_bind == g_config.key_sec_roll))
                            target_key = AKEYCODE_DPAD_CENTER;
                        else if (wheel_bind == g_config.key_block || (g_config.key_sec_block && wheel_bind == g_config.key_sec_block))
                            target_key = AKEYCODE_BUTTON_L1;
                        else if (wheel_bind == g_config.key_potion || (g_config.key_sec_potion && wheel_bind == g_config.key_sec_potion))
                            target_key = AKEYCODE_BUTTON_R1;
                        else if (wheel_bind == g_config.key_back || (g_config.key_sec_back && wheel_bind == g_config.key_sec_back))
                            target_key = AKEYCODE_BACK;
                        else if (wheel_bind == g_config.key_pause || (g_config.key_sec_pause && wheel_bind == g_config.key_sec_pause))
                            target_key = AKEYCODE_BUTTON_START;

                        if (target_key != 0) {
                            bool found = false;
                            for (auto &p : pending_wheel_pulses) {
                                if (p.keycode == target_key) {
                                    p.frames_remaining = 3;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                send_key(host, target_key, 1);
                                pending_wheel_pulses.push_back({ target_key, 3 });
                            }
                        }
                    }
                    break;
                }

                /* Mouse Clicks & Movements */
                case SDL_MOUSEMOTION: {
                    // Update mouse position directly in the engine without triggering Android's touch-down assumption.
                    // S3DClient_iPhone_OnMouseMoved updates INPDevice mouse coords in normalized screen space [-1, 1]
                    // without calling OnMouseButtonPressed or toggling is_mouse_down. This restores PC hover states
                    // and allows single clicks on buttons instead of requiring double-clicks.
                    float mx = (float)event.motion.x;
                    float my = (float)event.motion.y;
                    if (s_scaling_active) {
                        mx = (mx - (float)s_dst_x) / s_scale_x;
                        my = (my - (float)s_dst_y) / s_scale_y;
                    }
                    if (s_sym_S3DClient_iPhone_OnMouseMoved) {
                        float nx = (2.0f * mx / (float)g_render_width) - 1.0f;
                        float ny = (2.0f * (float)(g_render_height - my) / (float)g_render_height) - 1.0f;
                        dynarmic_call(host, s_sym_S3DClient_iPhone_OnMouseMoved, float_bits(nx), float_bits(ny), 0, 0);
                    }
                    break;
                }

                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    int down = (event.type == SDL_MOUSEBUTTONDOWN);
                    int mouse_bind = mouse_to_bind(event.button.button);

                    // Track active mouse-down key so release is always paired
                    static int s_active_mouse_key = 0;

                    // Action keys from mouse inputs are state-gated: only active in gameplay.
                    // In menus/dialogs, mouse buttons act strictly as UI pointer clicks to prevent
                    // controller shortcuts (e.g. Y=Store, Return=Confirm) from preempting clicks.
                    {
                        const bool in_gameplay = soft_mods_is_in_gameplay();
                        int target_key = 0;
                        if (mouse_bind == g_config.key_up || (g_config.key_sec_up && mouse_bind == g_config.key_sec_up))
                            target_key = AKEYCODE_DPAD_UP;
                        else if (mouse_bind == g_config.key_down || (g_config.key_sec_down && mouse_bind == g_config.key_sec_down))
                            target_key = AKEYCODE_DPAD_DOWN;
                        else if (mouse_bind == g_config.key_left || (g_config.key_sec_left && mouse_bind == g_config.key_sec_left))
                            target_key = AKEYCODE_DPAD_LEFT;
                        else if (mouse_bind == g_config.key_right || (g_config.key_sec_right && mouse_bind == g_config.key_sec_right))
                            target_key = AKEYCODE_DPAD_RIGHT;
                        else if (mouse_bind == g_config.key_attack || (g_config.key_sec_attack && mouse_bind == g_config.key_sec_attack))
                            target_key = AKEYCODE_BUTTON_Y;
                        else if (mouse_bind == g_config.key_jump || (g_config.key_sec_jump && mouse_bind == g_config.key_sec_jump))
                            target_key = AKEYCODE_BUTTON_X;
                        else if (mouse_bind == g_config.key_roll || (g_config.key_sec_roll && mouse_bind == g_config.key_sec_roll))
                            target_key = AKEYCODE_DPAD_CENTER;
                        else if (mouse_bind == g_config.key_block || (g_config.key_sec_block && mouse_bind == g_config.key_sec_block))
                            target_key = AKEYCODE_BUTTON_L1;
                        else if (mouse_bind == g_config.key_potion || (g_config.key_sec_potion && mouse_bind == g_config.key_sec_potion))
                            target_key = AKEYCODE_BUTTON_R1;
                        else if (mouse_bind == g_config.key_back || (g_config.key_sec_back && mouse_bind == g_config.key_sec_back))
                            target_key = AKEYCODE_BACK;
                        else if (mouse_bind == g_config.key_pause || (g_config.key_sec_pause && mouse_bind == g_config.key_sec_pause))
                            target_key = AKEYCODE_BUTTON_START;

                        // Back / Pause stay usable from the mouse in menus (e.g. to unpause)
                        const bool always_allowed = (target_key == AKEYCODE_BACK || target_key == AKEYCODE_BUTTON_START);
                        if (target_key != 0 && down && (in_gameplay || always_allowed)) {
                            send_key(host, target_key, 1);
                            s_active_mouse_key = target_key;
                        } else if (target_key != 0 && !down && s_active_mouse_key == target_key) {
                            // Release is always paired with its press, even if gameplay state changed meanwhile
                            send_key(host, target_key, 0);
                            s_active_mouse_key = 0;
                        }
                    }

                    // Left mouse button also delivers UI click/touch events for menus & UI
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        float mx = (float)event.button.x;
                        float my = (float)event.button.y;
                        if (s_scaling_active) {
                            mx = (mx - (float)s_dst_x) / s_scale_x;
                            my = (my - (float)s_dst_y) / s_scale_y;
                        }
                        if (down) {
                            if (s_sym_engineOnMouseButtonDown)
                                dynarmic_call(host, s_sym_engineOnMouseButtonDown, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                            send_touch(host, img, mx, my, TOUCH_PHASE_DOWN);
                        } else {
                            if (s_sym_engineOnMouseButtonUp)
                                dynarmic_call(host, s_sym_engineOnMouseButtonUp, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                            send_touch(host, img, mx, my, TOUCH_PHASE_UP);
                        }
                    }
                    break;
                }
            }
        }

        // Update pending wheel pulse keys (hold for 3 frames then release)
        for (size_t i = 0; i < pending_wheel_pulses.size(); ) {
            pending_wheel_pulses[i].frames_remaining--;
            if (pending_wheel_pulses[i].frames_remaining <= 0) {
                send_key(host, pending_wheel_pulses[i].keycode, 0);
                pending_wheel_pulses.erase(pending_wheel_pulses.begin() + i);
            } else {
                ++i;
            }
        }

        int64_t t_frame_start     = perf_now_us();
        int64_t t_swap_start      = t_frame_start; /* hoisted; set inside engine block */
        int64_t t_swap_end        = t_frame_start;
        int64_t t_housekeeping_end = t_frame_start; /* hoisted; set after resetStringPool */
        if (s_perf_enabled) {
            perf_frame_begin();
        }


        // Run one frame of engine
        if (s_sym_engineRunOneFrame) {
            static int s_frame_cnt = 0;
            static uint32_t s_last_ce_state = 0xFFFFFFFF;
            static bool s_first_passed = false;
            s_frame_cnt++;
            g_engine_frame_cnt.store((uint32_t)s_frame_cnt, std::memory_order_relaxed);
            bridge_gles_set_frame_number(s_frame_cnt);
            if (max_frames > 0 && s_frame_cnt > max_frames) {
                if (g_debug_mode) {
                    printf("[+] Reached max frames limit (%d). Exiting...\n", max_frames);
                }
                running = false;
                break;
            }

            // Check if overlay movie is waiting / active
            if (s_sym_engineGetOverlayMovieHasChanged) {
                uint32_t changed = dynarmic_call(host, s_sym_engineGetOverlayMovieHasChanged, FAKE_ENV_ADDR, 0, 0, 0);
                if (changed) {
                    const char *path = "";
                    if (s_sym_engineGetOverlayMovie) {
                        uint32_t mstr = dynarmic_call(host, s_sym_engineGetOverlayMovie, FAKE_ENV_ADDR, 0, 0, 0);
                        path = mstr ? (const char*)(img->mem + mstr) : "";
                        if (path && path[0]) {
                            printf("[+] Overlay movie requested: '%s'\n", path);
                        }
                    }
                    if (path && path[0] && s_sym_engineOnOverlayMovieStopped) {
                        printf("[+] Signalling overlay movie stopped to proceed...\n");
                        dynarmic_call(host, s_sym_engineOnOverlayMovieStopped, FAKE_ENV_ADDR, 0, 0, 0);
                    }
                }
            }

            uint32_t ret = dynarmic_call(host, s_sym_engineRunOneFrame, FAKE_ENV_ADDR, 0, 0, 0);
            uint32_t passed = 0;

            if (tap_frame > 0 && s_frame_cnt == tap_frame) {
                printf("[+] Frame %d: Simulating Tap/Click on screen (640, 650)...\n", s_frame_cnt);
                float mx = 640.0f;
                float my = 650.0f;
                if (s_sym_engineOnMouseButtonDown) {
                    dynarmic_call(host, s_sym_engineOnMouseButtonDown, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                if (s_sym_engineOnMouseButtonUp) {
                    dynarmic_call(host, s_sym_engineOnMouseButtonUp, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                send_key(host, AKEYCODE_BUTTON_A, 1);
                send_key(host, AKEYCODE_BUTTON_A, 0);
            }

            if (click2_frame > 0 && s_frame_cnt == click2_frame) {
                printf("[+] Frame %d: Simulating Mouse Down on '%s' (%0.1f, %0.1f)...\n", s_frame_cnt, click2_name, click2_x, click2_y);
                float mx = click2_x;
                float my = click2_y;
                if (s_sym_engineOnMouseMove) {
                    dynarmic_call(host, s_sym_engineOnMouseMove, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                if (s_sym_engineOnMouseButtonDown) {
                    dynarmic_call(host, s_sym_engineOnMouseButtonDown, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                send_touch(host, img, mx, my, TOUCH_PHASE_DOWN);
                if (s_sym_sendEventToCurrentUser) {
                    if (strcmp(click2_name, "world_map") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onWorldMap");
                    } else if (strcmp(click2_name, "extras") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onShowExtras");
                    } else if (strcmp(click2_name, "settings") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onSettingsScreen");
                    } else if (strcmp(click2_name, "coins") == 0 || strcmp(click2_name, "store") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "aiStoreManager", "onShowStore");
                    } else if (strcmp(click2_name, "new_game") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onNewGame");
                    }
                }
            }
            if (click2_frame > 0 && s_frame_cnt == click2_frame + 2) {
                printf("[+] Frame %d: Simulating Mouse Up on '%s' (%0.1f, %0.1f)...\n", s_frame_cnt, click2_name, click2_x, click2_y);
                float mx = click2_x;
                float my = click2_y;
                if (s_sym_engineOnMouseButtonUp) {
                    dynarmic_call(host, s_sym_engineOnMouseButtonUp, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                send_touch(host, img, mx, my, TOUCH_PHASE_UP);
            }

            if (click3_frame > 0 && s_frame_cnt == click3_frame) {
                printf("[+] Frame %d: Simulating Action Click Down on '%s' (%0.1f, %0.1f)...\n", s_frame_cnt, click3_name, click3_x, click3_y);
                float mx = click3_x;
                float my = click3_y;
                if (s_sym_engineOnMouseMove) {
                    dynarmic_call(host, s_sym_engineOnMouseMove, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                if (s_sym_engineOnMouseButtonDown) {
                    dynarmic_call(host, s_sym_engineOnMouseButtonDown, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                send_touch(host, img, mx, my, TOUCH_PHASE_DOWN);
                if (s_sym_sendEventToCurrentUser) {
                    if (strcmp(click3_name, "quit_yes") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onExitConfirmYes");
                    } else if (strcmp(click3_name, "buy_sword") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "aiStoreAndInv", "onOKButtonPress");
                    } else if (strcmp(click3_name, "achievements") == 0) {
                        send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onShowAchievements");
                    }
                }
            }
            if (click3_frame > 0 && s_frame_cnt == click3_frame + 2) {
                printf("[+] Frame %d: Simulating Action Click Up on '%s' (%0.1f, %0.1f)...\n", s_frame_cnt, click3_name, click3_x, click3_y);
                float mx = click3_x;
                float my = click3_y;
                if (s_sym_engineOnMouseButtonUp) {
                    dynarmic_call(host, s_sym_engineOnMouseButtonUp, FAKE_ENV_ADDR, 0, float_bits(mx), float_bits(my));
                }
                send_touch(host, img, mx, my, TOUCH_PHASE_UP);
            }

            if (key_action_frame > 0 && s_frame_cnt == key_action_frame) {
                printf("[+] Frame %d: Simulating Key Action '%s' (code %d) Down...\n", s_frame_cnt, key_action_name, key_action_code);
                send_key(host, key_action_code, 1);
            }
            if (key_action_frame > 0 && s_frame_cnt == key_action_frame + 2) {
                printf("[+] Frame %d: Simulating Key Action '%s' (code %d) Up...\n", s_frame_cnt, key_action_name, key_action_code);
                send_key(host, key_action_code, 0);
            }

            if (key_action2_frame > 0 && s_frame_cnt == key_action2_frame) {
                printf("[+] Frame %d: Simulating Key Action 2 '%s' (code %d) Down...\n", s_frame_cnt, key_action2_name, key_action2_code);
                send_key(host, key_action2_code, 1);
            }
            if (key_action2_frame > 0 && s_frame_cnt == key_action2_frame + 2) {
                printf("[+] Frame %d: Simulating Key Action 2 '%s' (code %d) Up...\n", s_frame_cnt, key_action2_name, key_action2_code);
                send_key(host, key_action2_code, 0);
            }

            if (is_level1_play) {
                // Allow storybook scroll and intro window-shattering cinematic to play out naturally.
                // Cutscene ends around frame 1250-1300 when Prince lands in courtyard.
                // Test in-game controls after Prince lands in courtyard (frame 1320+):
                // Move Right from frame 1320 to 1400
                if (s_frame_cnt == 1320) {
                    if (g_debug_mode) {
                        printf("[+] Frame %d: Testing in-game MOVE RIGHT (D/Right)...\n", s_frame_cnt);
                    }
                    send_key(host, AKEYCODE_DPAD_RIGHT, 1);
                }
                if (s_frame_cnt == 1400) {
                    if (g_debug_mode) {
                        printf("[+] Frame %d: Stopping in-game MOVE RIGHT...\n", s_frame_cnt);
                    }
                    send_key(host, AKEYCODE_DPAD_RIGHT, 0);
                }
                // Jump at frame 1430
                if (s_frame_cnt == 1430) {
                    if (g_debug_mode) {
                        printf("[+] Frame %d: Testing in-game JUMP (Space/A)...\n", s_frame_cnt);
                    }
                    send_key(host, AKEYCODE_BUTTON_A, 1);
                }
                if (s_frame_cnt == 1434) {
                    send_key(host, AKEYCODE_BUTTON_A, 0);
                }
                // Attack at frame 1480
                if (s_frame_cnt == 1480) {
                    if (g_debug_mode) {
                        printf("[+] Frame %d: Testing in-game ATTACK (J/X)...\n", s_frame_cnt);
                    }
                    send_key(host, AKEYCODE_BUTTON_X, 1);
                }
                if (s_frame_cnt == 1484) {
                    send_key(host, AKEYCODE_BUTTON_X, 0);
                }
                // Block at frame 1540
                if (s_frame_cnt == 1540) {
                    if (g_debug_mode) {
                        printf("[+] Frame %d: Testing in-game BLOCK (L/Y)...\n", s_frame_cnt);
                    }
                    send_key(host, AKEYCODE_BUTTON_Y, 1);
                }
                if (s_frame_cnt == 1544) {
                    send_key(host, AKEYCODE_BUTTON_Y, 0);
                }
                // Roll at frame 1600
                if (s_frame_cnt == 1600) {
                    if (g_debug_mode) {
                        printf("[+] Frame %d: Testing in-game ROLL (LShift/R1)...\n", s_frame_cnt);
                    }
                    send_key(host, AKEYCODE_BUTTON_R1, 1);
                }
                if (s_frame_cnt == 1604) {
                    send_key(host, AKEYCODE_BUTTON_R1, 0);
                }
                if (g_debug_mode && (s_frame_cnt == 500 || s_frame_cnt == 700 || s_frame_cnt == 850 || s_frame_cnt == 1000 || s_frame_cnt == 1100 || s_frame_cnt == 1200 || s_frame_cnt == 1250 || s_frame_cnt == 1300 || s_frame_cnt == 1350 || s_frame_cnt == 1400 || s_frame_cnt == 1450 || s_frame_cnt == 1500 || s_frame_cnt == 1550 || s_frame_cnt == 1600 || s_frame_cnt == 1650 || s_frame_cnt == 1700)) {
                    char sname[128];
                    snprintf(sname, sizeof(sname), "%s%s_level1_frame%d.bmp", get_screenshot_dir(), BUILD_TAG, s_frame_cnt);
                    bridge_gles_save_screenshot(sname, cur_w, cur_h);
                }
            }

            if (g_debug_mode && click2_name && !click3_name && !key_action_name && (s_frame_cnt == 160 || s_frame_cnt == 180 || s_frame_cnt == 200 || s_frame_cnt == 220 || s_frame_cnt == 250)) {
                char sname[128];
                snprintf(sname, sizeof(sname), "%s%s_%s_frame%d.bmp", get_screenshot_dir(), BUILD_TAG, click2_name, s_frame_cnt);
                bridge_gles_save_screenshot(sname, cur_w, cur_h);
            }

            if (g_debug_mode && !is_level1_play && (click3_name || key_action_name) && (s_frame_cnt == 180 || s_frame_cnt == 220 || s_frame_cnt == 250 || s_frame_cnt == 280 || s_frame_cnt == 310 || s_frame_cnt == 340 || s_frame_cnt == 420 || s_frame_cnt == 500 || s_frame_cnt == 600)) {
                char sname[128];
                const char *an = click3_name ? click3_name : key_action_name;
                snprintf(sname, sizeof(sname), "%s%s_%s_%s_frame%d.bmp", get_screenshot_dir(), BUILD_TAG, click2_name ? click2_name : "menu", an, s_frame_cnt);
                bridge_gles_save_screenshot(sname, cur_w, cur_h);
            }

            if (g_debug_mode && auto_screenshot_interval > 0 && (s_frame_cnt == 30 || s_frame_cnt == 60 || s_frame_cnt == 80 || (s_frame_cnt >= 100 && s_frame_cnt % auto_screenshot_interval == 0))) {
                char sname[128];
                snprintf(sname, sizeof(sname), "%s%s_frame%d.bmp", get_screenshot_dir(), BUILD_TAG, s_frame_cnt);
                bridge_gles_save_screenshot(sname, cur_w, cur_h);
                snprintf(sname, sizeof(sname), "%sscreenshot_frame%d.bmp", get_screenshot_dir(), s_frame_cnt);
                bridge_gles_save_screenshot(sname, cur_w, cur_h);
            }

            t_swap_start = perf_now_us();
            if (s_scaling_active && !s_offscreen) {
                bridge_gles_render_letterbox_bars(g_window_width, g_window_height);
            }

            bool do_swap = true;
            if (s_sym_engineDidPassFirstFrame) {
                passed = dynarmic_call(host, s_sym_engineDidPassFirstFrame, FAKE_ENV_ADDR, 0, 0, 0);
                if (passed) {
                    if (!s_first_passed) {
                        s_first_passed = true;
                        if (g_debug_mode) {
                            printf("[!!!] ENGINE PASSED FIRST FRAME at frame %d!\n", s_frame_cnt);
                        }
                    }
                } else {
                    do_swap = false;
                }
            }

            static LARGE_INTEGER s_qpc_swap_end = {};
            if (do_swap) {
                // Submit the frame's rendering. (glFinish would also wait for the previous frame's present to be
                // consumed by the compositor, costing a whole extra refresh interval on hybrid GPUs.)
                if (s_offscreen) glFlush();
                int64_t t_ph0 = perf_now_us();
#ifdef _WIN32
                // With the driver swap interval at 0, wait for the compositor's vblank BEFORE presenting so
                // the swap lands right after a refresh instead of mid-scanout.
                if (s_vsync_mode == VSYNC_DWM) {
                    s_pfn_DwmFlush();
                    // Optional extra wait so the present copy lands clear of the compositor's own pass
                    if (g_config.present_delay_ms > 0.0f) {
                        int64_t t0 = perf_now_us();
                        int64_t delay_us = (int64_t)(g_config.present_delay_ms * 1000.0f);
                        int64_t coarse_ms = (delay_us - 1500) / 1000;
                        if (coarse_ms > 0) SDL_Delay((Uint32)coarse_ms);
                        while (perf_now_us() - t0 < delay_us) _mm_pause();
                    }
                }
#endif
                int64_t t_ph1 = perf_now_us();
                if (s_offscreen) bridge_gles_present(g_window_width, g_window_height);
                int64_t t_ph2 = perf_now_us();
                SDL_GL_SwapWindow(window);
                s_ph_dwm_us      = t_ph1 - t_ph0;
                s_ph_present_us  = t_ph2 - t_ph1;
                s_ph_swapcall_us = perf_now_us() - t_ph2;
#ifdef _WIN32
                QueryPerformanceCounter(&s_qpc_swap_end);
#endif
            }
            if (s_offscreen) bridge_gles_begin_frame();
            t_swap_end = perf_now_us();


            // Detailed diagnostics
            uint32_t ce_ptr = 0x020011E0u;
            uint32_t ce_state = *(uint32_t*)(img->mem + ce_ptr);
            uint8_t ce_p68 = *(uint8_t*)(img->mem + ce_ptr + 0x68);
            uint8_t ce_p69 = *(uint8_t*)(img->mem + ce_ptr + 0x69);
            uint32_t game_ptr = *(uint32_t*)(img->mem + ce_ptr + 0x18);
            uint8_t game_run = game_ptr ? *(uint8_t*)(img->mem + game_ptr + 0x10) : 0xFF;
            uint32_t opt_render = game_ptr ? *(uint32_t*)(img->mem + game_ptr + (0x19 + 0x6a) * 4) : 0;
            uint32_t game_players = game_ptr ? *(uint32_t*)(img->mem + game_ptr + 0x60) : 0;
            uint32_t k_ptr = 0x01A3DC0Cu;
            uint32_t k_renderer = *(uint32_t*)(img->mem + k_ptr + 0x8c);

            uint32_t drv = *(uint32_t*)(img->mem + mod_base + 0xBC0930);
            uint32_t cur_ctx = *(uint32_t*)(img->mem + mod_base + 0xBC0934);
            uint32_t p0 = 0, p0_flags = 0, p0_cam = 0, p0_hud = 0, p0_scene = 0;
            uint8_t p0_8d = 0, p0_8e = 0;
            float p0_vw = 0, p0_vh = 0;
            uint32_t p_arr = game_ptr ? *(uint32_t*)(img->mem + game_ptr + 0x68) : 0;
            if (p_arr) {
                p0 = *(uint32_t*)(img->mem + p_arr);
                if (p0) {
                    p0_flags = *(uint32_t*)(img->mem + p0 + 8);
                    p0_cam   = *(uint32_t*)(img->mem + p0 + 0x24);
                    p0_hud   = *(uint32_t*)(img->mem + p0 + 0x28);
                    p0_scene = *(uint32_t*)(img->mem + p0 + 0x2c);
                    p0_8d    = *(uint8_t*)(img->mem + p0 + 0x8d);
                    p0_8e    = *(uint8_t*)(img->mem + p0 + 0x8e);
                    p0_vw    = *(float*)(img->mem + p0 + 0x84);
                    p0_vh    = *(float*)(img->mem + p0 + 0x88);
                }
            }
            uint32_t hud_mgr = k_renderer ? *(uint32_t*)(img->mem + k_renderer + 0x18) : 0;
            uint32_t hud_cnt = hud_mgr ? *(uint32_t*)(img->mem + hud_mgr + 0x14) : 0;
            uint32_t want_swap = s_sym_engineGetWantSwapBuffers ? dynarmic_call(host, s_sym_engineGetWantSwapBuffers, FAKE_ENV_ADDR, 0, 0, 0) : 0;

            // Keep bridge_libc's leak/asset diagnostics stamped with "where
            // are we in the game" every frame (not just once/sec) so a leak
            // or a missing asset can be reported against the scene it was
            // FIRST seen in, and scene transitions get their own [SCENE]
            // log line the moment they happen.
            bridge_libc_set_scene_context(p0_scene, game_ptr, ce_state, (uint32_t)s_frame_cnt);
            s_perf_scene = p0_scene;
            s_perf_ce_state = ce_state;

            if (p0) {
                soft_mods_apply_hud(img, p0, p0_hud, p0_scene);
            } else {
                soft_mods_apply_hud(img, 0, 0, 0);
            }

            // Track CE state transitions & print telemetry once per real-world second (regardless of framerate)
            if (g_debug_mode) {
                static int64_t s_last_telemetry_us = 0;
                int64_t now_telemetry_us = perf_now_us();
                if (s_frame_cnt <= 10 || (now_telemetry_us - s_last_telemetry_us) >= 1000000LL || ce_state != s_last_ce_state) {
                    s_last_telemetry_us = now_telemetry_us;
                    uint32_t da = 0, de = 0, clr = 0, fbo = 0;
                    bridge_gles_get_frame_stats(&da, &de, &clr, &fbo);
                    uint32_t heap_mb = bridge_libc_get_heap_used_mb();
                    printf("[FRAME %d] ret=%u passed=%u swap=%u drv=%u ctx=0x%08X | ce=(st=%u p68=%u p69=%u) game=(ptr=0x%08X run=%u opt25=%u pl=%u) p0=(ptr=0x%08X flg=0x%X cam=0x%08X hud=0x%08X scn=0x%08X 8d=%u 8e=%u vw=%.1f vh=%.1f) rend=(hud_cnt=%u) | heap=%uMB | draw_el=%u draw_ar=%u clr=%u fbo=%u\n",
                           s_frame_cnt, ret, passed, want_swap, drv, cur_ctx,
                           ce_state, ce_p68, ce_p69,
                           game_ptr, game_run, opt_render, game_players,
                           p0, p0_flags, p0_cam, p0_hud, p0_scene, p0_8d, p0_8e, p0_vw, p0_vh,
                           hud_cnt,
                           heap_mb, de, da, clr, fbo);
                    s_last_ce_state = ce_state;
                    bridge_gles_reset_frame_stats();

                    // Net malloc/free/mmap accounting - cheap, so it rides along
                    // with the existing once-per-second telemetry line above.
                    // If malloc and free bytes track closely, heap exhaustion is
                    // fragmentation, not a leak; if free lags far behind malloc,
                    // it's a real leak and bridge_libc_dump_leak_report() (called
                    // automatically on OOM, or on-demand) will show which lr owns it.
                    bridge_libc_dump_mem_report();

#ifdef _WIN32
                    if (s_pfn_DwmGetCompositionTimingInfo) {
                        DWM_TIMING_INFO timing = {};
                        timing.cbSize = sizeof(timing);
                        if (SUCCEEDED(s_pfn_DwmGetCompositionTimingInfo(NULL, &timing)) && timing.qpcRefreshPeriod > 0) {
                            LARGE_INTEGER qpc_freq;
                            QueryPerformanceFrequency(&qpc_freq);

                            LARGE_INTEGER qpc_now;
                            QueryPerformanceCounter(&qpc_now);

                            LARGE_INTEGER qpc_swap_timestamp = (s_qpc_swap_end.QuadPart > 0) ? s_qpc_swap_end : qpc_now;
                            int64_t phase_qpc = (qpc_swap_timestamp.QuadPart - timing.qpcVBlank) % timing.qpcRefreshPeriod;
                            if (phase_qpc < 0) phase_qpc += timing.qpcRefreshPeriod;
                            float phase_pct = (float)phase_qpc * 100.0f / (float)timing.qpcRefreshPeriod;
                            float phase_ms  = (float)phase_qpc * 1000.0f / (float)qpc_freq.QuadPart;
                            float refresh_ms = (float)timing.qpcRefreshPeriod * 1000.0f / (float)qpc_freq.QuadPart;
                            int est_scanline = (int)(phase_pct * (float)g_window_height / 100.0f);

                            static uint64_t s_last_dwm_dropped = 0;
                            static uint64_t s_last_dwm_missed = 0;
                            uint64_t new_dropped = timing.cFramesDropped - s_last_dwm_dropped;
                            uint64_t new_missed = timing.cFramesMissed - s_last_dwm_missed;
                            s_last_dwm_dropped = timing.cFramesDropped;
                            s_last_dwm_missed = timing.cFramesMissed;

                            bool vsync_active = (s_vsync_mode == VSYNC_DRIVER || s_vsync_mode == VSYNC_DWM);
                            bool is_tearing_risk = !vsync_active;
                            const char *status_str = "TEAR_FREE (Display Scanout Synced)";
                            if (!g_config.vsync) {
                                status_str = "VSYNC_OFF (Unsynchronized Presentation)";
                            } else if (is_tearing_risk) {
                                status_str = "TEARING_VULNERABLE (Direct Flip / Unsynchronized Scanout)";
                            }
                            if (is_tearing_risk && phase_pct >= 10.0f && phase_pct <= 90.0f) {
                                status_str = "ACTIVE_TEAR_ZONE (Swap mid-scanout!)";
                            }

                            printf("[VSYNC DIAG f=%d] gpu=\"%s\" | SwapCtrl=%d | DWM: comp=%s dropped=%llu(+%llu) missed=%llu(+%llu) late=%llu | vblank_phase=%.2fms/%.2fms (%.1f%%, ~line %d/%d) | status=[%s]\n",
                                   s_frame_cnt, s_gl_renderer ? s_gl_renderer : "Unknown",
                                   s_swap_control,
                                   s_dwm_composition_enforced ? "ENFORCED" : "DEFAULT",
                                   (unsigned long long)timing.cFramesDropped, (unsigned long long)new_dropped,
                                   (unsigned long long)timing.cFramesMissed, (unsigned long long)new_missed,
                                   (unsigned long long)timing.cFramesLate,
                                   phase_ms, refresh_ms, phase_pct, est_scanline, g_window_height,
                                   status_str);

                            if (is_tearing_risk && (phase_pct >= 10.0f && phase_pct <= 90.0f)) {
                                printf("[!] SCREEN TEAR WARNING: Swap occurred at scanline ~%d/%d without VBlank lock! Tearing is active.\n",
                                       est_scanline, g_window_height);
                            }
                        }
                    }
#endif
                }
            }

            // Ensure graphics driver mode is GLES2 (3)
            if (drv != 3) {
                if (g_debug_mode) printf("[!] eDeviceDriver is %u (expected 3 for GLES2)! Setting to 3...\n", drv);
                *(uint32_t*)(img->mem + mod_base + 0xBC0930) = 3;
            }

            // Ensure Player 0 rendering is enabled
            if (p0 && (p0_flags & 8) == 0) {
                if (g_debug_mode) printf("[!] Player 0 rendering disabled (flags=0x%X)! Enabling bit 3...\n", p0_flags);
                *(uint32_t*)(img->mem + p0 + 8) |= 8;
            }

            // If Option 0x19 (render enable) is 0, enable it!
            if (game_ptr && opt_render == 0) {
                if (g_debug_mode) printf("[!] Option 0x19 (RenderEnable) is 0! Enabling via Game::SetOption(0x19, 1, 1)...\n");
                uint32_t sym_set_opt = elf32_lookup_symbol(img, "_ZN7Pandora10EngineCore4Game9SetOptionEjjb");
                if (sym_set_opt) {
                    dynarmic_call(host, sym_set_opt, game_ptr, 0x19, 1, 1);
                } else {
                    *(uint32_t*)(img->mem + game_ptr + (0x19 + 0x6a) * 4) = 1;
                }
            }

            // At frame 25, kickstart MainAI events if no draw calls have happened
            if (s_frame_cnt == 25 && s_sym_sendEventToCurrentUser) {
                if (g_debug_mode) printf("[+] Frame 25: Dispatching MainAI startup events...\n");
                send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onEnableRendering");
                send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onHideBlackScreen");
                send_ai_event(host, img, s_sym_sendEventToCurrentUser, "MainAI", "onMainMenu");
            }

            static bool s_game_did_run = false;
            if (game_run == 1) {
                s_game_did_run = true;
            }

            // If game is in state 5 and game has not started yet, kickstart it!
            if (!s_game_did_run && ce_state == 5 && game_ptr && game_run == 0 && s_frame_cnt <= 30) {
                if (g_debug_mode) printf("[!] State 5 active but game_run==0! Kickstarting Game::Run()...\n");
                uint32_t sym_game_run = elf32_lookup_symbol(img, "_ZN7Pandora10EngineCore4Game3RunEv");
                if (sym_game_run) {
                    dynarmic_call(host, sym_game_run, game_ptr, 0, 0, 0);
                } else {
                    *(uint8_t*)(img->mem + game_ptr + 0x10) = 1;
                    *(uint8_t*)(img->mem + game_ptr + 0x11) = 0;
                }
                *(uint8_t*)(img->mem + ce_ptr + 0x69) = 0;
            }

            // If game was running and has now stopped or requested stop, exit cleanly
            if (s_game_did_run && (game_run == 0 || ce_p68 != 0 || ce_p69 != 0)) {
                if (g_debug_mode) {
                    printf("[+] In-game quit detected (game_run=%u, ce_p68=%u, ce_p69=%u). Terminating cleanly...\n",
                           game_run, ce_p68, ce_p69);
                }
                running = false;
                break;
            }

            if (!ret && g_debug_mode) {
                static int s_zero_cnt = 0;
                if (++s_zero_cnt <= 5 || s_zero_cnt % 60 == 0) {
                    printf("[*] engineRunOneFrame yielded (returned 0, count=%d)\n", s_zero_cnt);
                }
            }

            // Reset S3DX::AIVariable StringPool cursor for next frame
            if (s_sym_resetStringPool) {
                dynarmic_call(host, s_sym_resetStringPool, 0, 0, 0, 0);
            }

            // Reset AIStack temporary handles array to prevent multi-megabyte heap leak
            reset_ai_temporary_handles(host, img);

            /* Record housekeeping end so it's attributed to engine cost, not the gap */
            t_housekeeping_end = perf_now_us();
        }


        // Cap at 60 FPS using high-res QPC, and record perf metrics
        {
            /* ENGINE phase = everything from frame start up to (not including) swap.
             * If s_sym_engineRunOneFrame wasn't called this frame (e.g. early out),
             * we still report 0µs for engine. */
            int64_t engine_us = 0;
            int64_t swap_us   = 0;
            if (s_sym_engineRunOneFrame) {
                /* engine_us = full game thread work: engineRunOneFrame + per-frame overhead + housekeeping */
                engine_us = (t_housekeeping_end - t_frame_start) - (t_swap_end - t_swap_start);
                /* swap_us  = GL presentation only (excludes housekeeping) */
                swap_us   = (t_swap_end   - t_swap_start);
                /* Clamp negatives (shouldn't happen but defensive) */
                if (engine_us < 0) engine_us = 0;
                if (swap_us   < 0) swap_us   = 0;
            }

            int64_t elapsed_us = perf_now_us() - t_frame_start;
            // Driver / DWM VSync already block for the display refresh, so the FPS limit only applies
            // when VSync is off; the software fallback paces at the display refresh rate.
            int64_t target_us  = 0;
            if (s_vsync_mode == VSYNC_OFF) {
                target_us = (g_config.fps_limit > 0) ? (1000000LL / g_config.fps_limit) : 0;
            } else if (s_vsync_mode == VSYNC_SOFTWARE) {
                target_us = 1000000LL / (s_display_hz > 0 ? s_display_hz : 60);
            }
            int64_t sleep_us   = 0;
            bool should_software_limit = (target_us > 0);
            if (should_software_limit && elapsed_us < target_us) {
                int64_t t_sleep_start = perf_now_us();
                /* Hybrid sleep: coarse SDL_Delay for the bulk (leave 1.5ms spin margin),
                 * then busy-wait for the final window. With timeBeginPeriod(1) active,
                 * SDL_Delay wakes within ~1ms so a 1.5ms margin is comfortably safe.
                 * This gives sub-millisecond frame precision without burning a full CPU core. */
                int64_t remaining_us = target_us - elapsed_us;
                int64_t delay_ms = (remaining_us - 1500) / 1000;
                if (delay_ms > 0) {
                    SDL_Delay((Uint32)delay_ms);
                }
                /* Spin for the last fraction */
                while (perf_now_us() - t_frame_start < target_us) {
                    _mm_pause();  /* hint to CPU: we're spinning, reduce power/heat */
                }
                sleep_us = perf_now_us() - t_sleep_start;
            }

            if (s_perf_enabled && s_sym_engineRunOneFrame) {
                static int s_perf_frame_no = 0;
                perf_frame_end(++s_perf_frame_no, engine_us, swap_us, sleep_us);
            }
        }


    }

    if (g_debug_mode) {
        printf("[+] Shutting down engine...\n");
    }

    if (watchdog_thread && watchdog_thread->joinable()) {
        watchdog_running.store(false, std::memory_order_relaxed);
        watchdog_thread->join();
    }

    // Final diagnostic dump
    if (g_debug_mode) {
        bridge_libc_dump_missing_assets();
        bridge_libc_dump_mem_report();
        bridge_libc_dump_leak_report(20);
        bridge_libc_dump_churn_report(20);
    }

    dynarmic_host_destroy(host);
    elf32_image_destroy(img);
    bridge_openal_shutdown();
    bridge_libc_cleanup_mem_vfs();

    if (controller) SDL_GameControllerClose(controller);
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

#ifdef _WIN32
    timeEndPeriod(1);
    perf_write_summary();
    if (s_perf_log) { fclose(s_perf_log); s_perf_log = nullptr; }
    if (s_log_file) { fclose(s_log_file); s_log_file = nullptr; }
    if (s_archive_log_file) { fclose(s_archive_log_file); s_archive_log_file = nullptr; }
#endif

    if (g_debug_mode) {
        printf("[+] Goodbye!\n");
    }
    return 0;
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}
#endif
