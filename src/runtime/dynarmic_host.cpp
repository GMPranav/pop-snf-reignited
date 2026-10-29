#include "dynarmic_host.h"
#include "../bridge/bridge_libc.h"
#include "../bridge/bridge_gles.h"
#include "../bridge/bridge_openal.h"
#include "../bridge/bridge_jni.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <array>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <unordered_map>

constexpr uint32_t HALT_SENTINEL_LR = 0x00000800u;
constexpr uint32_t HALT_SVC_ARM     = 0x0000DEADu;
constexpr uint32_t HALT_SVC_THUMB   = 0x000000ADu;

/* INVARIANT: Single-threaded, non-reentrant guest execution model.
 * g_guest_lr and g_guest_pc hold SVC dispatch context for the currently
 * executing host bridge function. Bridge handlers MUST NOT make nested/reentrant
 * calls into guest code (nested jit->Run()), as doing so would overwrite these
 * globals. If reentrant guest invocation is ever needed in the future, these
 * must be converted into a thread-local execution context stack. */
uint32_t g_guest_lr = 0;
uint32_t g_guest_pc = 0;
extern bool g_debug_mode;

/* ---- Host-call profiler (see dynarmic_host.h) ---- */
#ifdef _WIN32
#include <windows.h>
#endif
constexpr uint32_t PROF_MAX = 4096;
static bool       s_prof_on = false;
static int64_t    s_prof_qpf = 1;
static elf32_image_t *s_prof_img = nullptr;
static int64_t    s_prof_us[PROF_MAX];        /* this frame */
static uint32_t   s_prof_cnt[PROF_MAX];
static uint16_t   s_prof_touched[PROF_MAX];
static uint32_t   s_prof_ntouched = 0;
static int64_t    s_prof_frame_us = 0;
static uint32_t   s_prof_frame_calls = 0;
static int64_t    s_agg_us[PROF_MAX];         /* accumulated across frames (cleared by host_prof_aggregate_reset) */
static uint32_t   s_agg_cnt[PROF_MAX];
static int64_t    s_agg_total_us = 0;
static uint32_t   s_agg_calls = 0;

static inline int64_t prof_now(void) {
#ifdef _WIN32
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
#else
    return 0;
#endif
}

/* ---- Guest PC sampler: while profiling, a thread reads the guest program counter about once a millisecond.
 * Samples of slow frames are aggregated and mapped to the nearest exported function, showing where guest time goes. */
static std::atomic<bool>     s_samp_run{false};
static std::atomic<bool>     s_samp_frame_on{false};
static std::atomic<int>      s_samp_in_host{0};
static std::atomic<bool>     s_samp_in_guest{false};   /* inside jit->Run() */
static std::atomic<uint32_t> s_samp_n{0};
constexpr uint32_t SAMP_CAP = 1u << 15;
static uint32_t              s_samp_buf[SAMP_CAP];            /* this frame: guest pc, or 0 = inside a host call */
static std::unordered_map<uint32_t, uint32_t> s_samp_agg;    /* pc>>4 -> count, slow frames since last reset */
static uint32_t              s_samp_agg_total = 0, s_samp_agg_host = 0;
static Dynarmic::A32::Jit   *s_samp_jit = nullptr;
static std::thread           s_samp_thread;

static void sampler_main() {
    while (s_samp_run.load(std::memory_order_relaxed)) {
#ifdef _WIN32
        Sleep(1);
#endif
        if (!s_samp_frame_on.load(std::memory_order_relaxed) || !s_samp_jit || !s_samp_in_guest.load(std::memory_order_relaxed)) continue;
        uint32_t i = s_samp_n.load(std::memory_order_relaxed);
        if (i >= SAMP_CAP) continue;
        s_samp_buf[i] = s_samp_in_host.load(std::memory_order_relaxed) ? 0u : s_samp_jit->Regs()[15];
        s_samp_n.store(i + 1, std::memory_order_release);
    }
}

struct ProfSym { uint32_t value, size; const char *name; };
static std::vector<ProfSym> s_prof_syms;   /* sorted by guest address, built lazily */

static void prof_build_syms() {
    if (!s_prof_syms.empty() || !s_prof_img) return;
    for (uint32_t m = 0; m < s_prof_img->module_count; ++m) {
        const elf32_module_t *mod = &s_prof_img->modules[m];
        if (!mod->dynsym || !mod->dynstr) continue;
        for (uint32_t i = 1; i < mod->dynsym_count; ++i) {
            const Elf32_Sym *sym = &mod->dynsym[i];
            if (sym->st_shndx == SHN_UNDEF || ELF32_ST_TYPE(sym->st_info) != STT_FUNC) continue;
            s_prof_syms.push_back({ (mod->base + sym->st_value) & ~1u, sym->st_size, mod->dynstr + sym->st_name });
        }
    }
    std::sort(s_prof_syms.begin(), s_prof_syms.end(), [](const ProfSym &a, const ProfSym &b) { return a.value < b.value; });
}

static const char *prof_sym_for_pc(uint32_t pc) {
    prof_build_syms();
    auto it = std::upper_bound(s_prof_syms.begin(), s_prof_syms.end(), pc,
                               [](uint32_t v, const ProfSym &s) { return v < s.value; });
    if (it == s_prof_syms.begin()) return "?";
    --it;
    if (it->size && pc >= it->value + it->size) return "?";   /* in a gap between exported functions */
    return it->name;
}

void host_prof_enable(int on) {
    if (on && !s_samp_run.load()) {
        s_samp_run.store(true);
        s_samp_thread = std::thread(sampler_main);
        s_samp_thread.detach();
    } else if (!on) {
        s_samp_run.store(false);
    }
    s_prof_on = on != 0;
#ifdef _WIN32
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_prof_qpf = f.QuadPart ? f.QuadPart : 1;
#endif
}
int host_prof_enabled(void) { return s_prof_on ? 1 : 0; }

void host_prof_frame_reset(void) {
    s_samp_n.store(0, std::memory_order_relaxed);
    s_samp_frame_on.store(s_prof_on, std::memory_order_relaxed);
    for (uint32_t i = 0; i < s_prof_ntouched; ++i) {
        s_prof_us[s_prof_touched[i]] = 0;
        s_prof_cnt[s_prof_touched[i]] = 0;
    }
    s_prof_ntouched = 0;
    s_prof_frame_us = 0;
    s_prof_frame_calls = 0;
}
int64_t host_prof_frame_total_us(void) { return s_prof_frame_us * 1000000LL / s_prof_qpf; }
uint32_t host_prof_frame_calls(void) { return s_prof_frame_calls; }

static void prof_record(uint32_t idx, int64_t ticks) {
    if (idx >= PROF_MAX) return;
    if (s_prof_cnt[idx] == 0) s_prof_touched[s_prof_ntouched++] = (uint16_t)idx;
    s_prof_us[idx] += ticks;
    s_prof_cnt[idx]++;
    s_prof_frame_us += ticks;
    s_prof_frame_calls++;
}

static const char *prof_name(uint32_t idx) {
    return (s_prof_img && idx < s_prof_img->trampoline_count && s_prof_img->trampoline_names[idx])
        ? s_prof_img->trampoline_names[idx] : "?";
}

/* Writes the n most expensive entries of (us[], cnt[]) over the given index list */
static int prof_top(const int64_t *us, const uint32_t *cnt, const uint16_t *list, uint32_t nlist,
                    int n, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) return 0;
    buf[0] = 0;
    uint16_t best[16];
    int nb = 0;
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < nlist; ++i) {
        uint16_t idx = list ? list[i] : (uint16_t)i;
        if (!cnt[idx]) continue;
        int pos = nb;
        while (pos > 0 && us[best[pos - 1]] < us[idx]) --pos;
        if (pos >= n) continue;
        if (nb < n) ++nb;
        for (int j = nb - 1; j > pos; --j) best[j] = best[j - 1];
        best[pos] = idx;
    }
    size_t used = 0;
    for (int i = 0; i < nb; ++i) {
        int w = snprintf(buf + used, bufsz - used, "%s%s=%.2fms/%u", i ? " " : "", prof_name(best[i]),
                         us[best[i]] * 1000.0 / s_prof_qpf, cnt[best[i]]);
        if (w < 0 || (size_t)w >= bufsz - used) break;
        used += (size_t)w;
    }
    return nb;
}

int host_prof_frame_top(int n, char *buf, size_t bufsz) {
    return prof_top(s_prof_us, s_prof_cnt, s_prof_touched, s_prof_ntouched, n, buf, bufsz);
}

void host_prof_aggregate_frame(void) {
    s_samp_frame_on.store(false, std::memory_order_relaxed);
    uint32_t n = std::min<uint32_t>(s_samp_n.load(std::memory_order_acquire), SAMP_CAP);
    for (uint32_t i = 0; i < n; ++i) {
        s_samp_agg_total++;
        if (s_samp_buf[i] == 0) s_samp_agg_host++;
        else s_samp_agg[s_samp_buf[i] >> 4]++;
    }
    for (uint32_t i = 0; i < s_prof_ntouched; ++i) {
        uint16_t idx = s_prof_touched[i];
        s_agg_us[idx] += s_prof_us[idx];
        s_agg_cnt[idx] += s_prof_cnt[idx];
    }
    s_agg_total_us += s_prof_frame_us;
    s_agg_calls += s_prof_frame_calls;
}

int host_prof_aggregate_top(int n, char *buf, size_t bufsz, int64_t *total_us, uint32_t *calls) {
    if (total_us) *total_us = s_agg_total_us * 1000000LL / s_prof_qpf;
    if (calls) *calls = s_agg_calls;
    return prof_top(s_agg_us, s_agg_cnt, nullptr, PROF_MAX, n, buf, bufsz);
}

/* "sym=NN% ..." for the hottest guest functions of the aggregated slow frames, plus how spread out the samples are:
 * a long tail of one-off addresses means first-run JIT translation; a few hot symbols mean real guest work. */
int host_prof_sample_top(int n, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) return 0;
    buf[0] = 0;
    uint32_t guest_total = s_samp_agg_total - s_samp_agg_host;
    if (guest_total == 0) return 0;
    std::unordered_map<const char*, uint32_t> by_sym;
    uint32_t singletons = 0;
    for (auto &kv : s_samp_agg) {
        by_sym[prof_sym_for_pc(kv.first << 4)] += kv.second;
        if (kv.second == 1) singletons++;
    }
    std::vector<std::pair<const char*, uint32_t>> v(by_sym.begin(), by_sym.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    size_t used = (size_t)snprintf(buf, bufsz, "samples=%u (host %u%%) unique16B=%zu once=%u | ", s_samp_agg_total,
                                   s_samp_agg_host * 100u / s_samp_agg_total, s_samp_agg.size(), singletons);
    int w = 0;
    for (int i = 0; i < n && i < (int)v.size() && used < bufsz; ++i) {
        int c = snprintf(buf + used, bufsz - used, "%s%s=%u%%", i ? " " : "", v[i].first, v[i].second * 100u / guest_total);
        if (c < 0 || (size_t)c >= bufsz - used) break;
        used += (size_t)c; ++w;
    }
    return w;
}

void host_prof_aggregate_reset(void) {
    s_samp_agg.clear();
    s_samp_agg_total = 0;
    s_samp_agg_host = 0;
    memset(s_agg_us, 0, sizeof(s_agg_us));
    memset(s_agg_cnt, 0, sizeof(s_agg_cnt));
    s_agg_total_us = 0;
    s_agg_calls = 0;
}

class HostCallbacks : public Dynarmic::A32::UserCallbacks {
public:
    elf32_image_t *img;
    Dynarmic::A32::Jit *jit = nullptr;
    std::vector<svc_handler_fn> handlers;

    HostCallbacks(elf32_image_t *image) : img(image) {}

    void InitHandlers() {
        handlers.resize(img->trampoline_count, nullptr);
        for (uint32_t i = 0; i < img->trampoline_count; ++i) {
            const char *name = img->trampoline_names[i];
            svc_handler_fn fn = bridge_gles_lookup(name);
            if (!fn) fn = bridge_openal_lookup(name);
            if (!fn) fn = bridge_libc_lookup(name);
            if (!fn) fn = bridge_jni_lookup(name);
            handlers[i] = fn;
        }
    }

    std::uint8_t MemoryRead8(std::uint32_t vaddr) override {
        return (vaddr < img->mem_size) ? img->mem[vaddr] : 0;
    }
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override {
        if (vaddr + 1 < img->mem_size) {
            uint16_t val;
            memcpy(&val, img->mem + vaddr, 2);
            return val;
        }
        return 0;
    }
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override {
        if (vaddr + 3 < img->mem_size) {
            uint32_t val;
            memcpy(&val, img->mem + vaddr, 4);
            return val;
        }
        return 0;
    }
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override {
        if (vaddr + 7 < img->mem_size) {
            uint64_t val;
            memcpy(&val, img->mem + vaddr, 8);
            return val;
        }
        return 0;
    }

    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override {
        if (vaddr < img->mem_size) img->mem[vaddr] = value;
    }
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override {
        if (vaddr + 1 < img->mem_size) memcpy(img->mem + vaddr, &value, 2);
    }
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override {
        if (vaddr + 3 < img->mem_size) memcpy(img->mem + vaddr, &value, 4);
    }
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override {
        if (vaddr + 7 < img->mem_size) memcpy(img->mem + vaddr, &value, 8);
    }

    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override {
        fprintf(stderr, "dynarmic: interpreter fallback at pc=0x%08x (instructions=%zu)\n", pc, num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
        uint32_t swi_num = swi & 0x00FFFFFFu;
        if (swi_num == HALT_SVC_ARM || swi_num == HALT_SVC_THUMB) {
            jit->HaltExecution();
            return;
        }

        uint32_t idx = swi_num;
        g_guest_lr = jit->Regs()[14];
        g_guest_pc = jit->Regs()[15];
        if (idx < handlers.size() && handlers[idx]) {
            uint32_t r0 = jit->Regs()[0];
            uint32_t r1 = jit->Regs()[1];
            uint32_t r2 = jit->Regs()[2];
            uint32_t r3 = jit->Regs()[3];
            uint32_t sp = jit->Regs()[13];
            g_has_guest_ret_r1 = 0;
            uint32_t ret;
            if (s_prof_on) {
                int64_t t0 = prof_now();
                s_samp_in_host.store(1, std::memory_order_relaxed);
                ret = handlers[idx](img, r0, r1, r2, r3, sp);
                s_samp_in_host.store(0, std::memory_order_relaxed);
                prof_record(idx, prof_now() - t0);
            } else {
                ret = handlers[idx](img, r0, r1, r2, r3, sp);
            }
            jit->Regs()[0] = ret;
            if (g_has_guest_ret_r1) {
                jit->Regs()[1] = g_guest_ret_r1;
                g_has_guest_ret_r1 = 0;
            }
        } else {
            const char *name = (idx < img->trampoline_count) ? img->trampoline_names[idx] : "unknown";
            fprintf(stderr, "[TRANSLATOR MISSING] Unhandled SVC #%u (%s) r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X lr=0x%08X pc=0x%08X\n",
                    idx, name, jit->Regs()[0], jit->Regs()[1], jit->Regs()[2], jit->Regs()[3], g_guest_lr, g_guest_pc);
            jit->Regs()[0] = 0;
        }
    }

    void AddTicks(std::uint64_t ticks) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000000ull; }

    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override {
        fprintf(stderr, "dynarmic: exception %d at pc=0x%08x\n", (int)exception, pc);
    }
};

struct dynarmic_host_s {
    elf32_image_t *img;
    HostCallbacks callbacks;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> page_table;
    uint32_t stack_top;

    dynarmic_host_s(elf32_image_t *image)
        : img(image),
          callbacks(image),
          stack_top((image && image->mem_size >= 0x01000000u) ? ((image->mem_size - 64u) & ~7u) : 0x01FFFFF0u) {}
};

dynarmic_host_t *dynarmic_host_create(elf32_image_t *img) {
    auto host = new dynarmic_host_s(img);

    // Build absolute offset page table (maps all pages to img->mem)
    size_t num_pages = img->mem_size >> 12;
    for (size_t i = 0; i < Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES; ++i) {
        host->page_table[i] = (i < num_pages) ? img->mem : nullptr;
    }

    // Write HALT sentinel instructions at HALT_SENTINEL_LR (0x00000800)
    uint32_t arm_svc = 0xEF000000u | HALT_SVC_ARM;
    memcpy(img->mem + HALT_SENTINEL_LR, &arm_svc, 4);
    uint16_t thumb_svc = 0xDF00u | (uint16_t)HALT_SVC_THUMB;
    memcpy(img->mem + HALT_SENTINEL_LR + 4, &thumb_svc, 2);

    Dynarmic::A32::UserConfig config;
    config.callbacks = &host->callbacks;
    config.page_table = &host->page_table;
    config.absolute_offset_page_table = true;
    config.processor_id = 0;

    host->jit = std::make_unique<Dynarmic::A32::Jit>(config);
    host->callbacks.jit = host->jit.get();
    host->callbacks.InitHandlers();
    s_prof_img = img;
    s_samp_jit = host->jit.get();

    // Set initial stack pointer
    host->jit->Regs()[13] = host->stack_top;

    if (g_debug_mode) {
        printf("[+] dynarmic_host: initialized ARMv7 JIT with %zu SVC handlers (stack_top=0x%08X)\n",
               host->callbacks.handlers.size(), host->stack_top);
    }
    return host;
}

void dynarmic_host_destroy(dynarmic_host_t *host) {
    delete host;
}

uint32_t dynarmic_call(dynarmic_host_t *host, uint32_t entry_point, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    if (!host || !host->jit || entry_point == 0) return 0;

    auto *jit = host->jit.get();
    bool is_thumb = (entry_point & 1) != 0;
    uint32_t cpsr = jit->Cpsr();
    if (is_thumb) {
        cpsr |= (1u << 5);
    } else {
        cpsr &= ~(1u << 5);
    }
    jit->SetCpsr(cpsr);

    jit->Regs()[13] = host->stack_top;
    jit->Regs()[0] = r0;
    jit->Regs()[1] = r1;
    jit->Regs()[2] = r2;
    jit->Regs()[3] = r3;
    jit->Regs()[14] = HALT_SENTINEL_LR; // LR
    jit->Regs()[15] = entry_point & ~1u; // PC

    jit->ClearHalt();
    s_samp_in_guest.store(true, std::memory_order_relaxed);
    jit->Run();
    s_samp_in_guest.store(false, std::memory_order_relaxed);

    // Reset SP
    jit->Regs()[13] = host->stack_top;
    return jit->Regs()[0];
}

uint32_t dynarmic_call_stack(dynarmic_host_t *host, uint32_t entry_point, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, const uint32_t *stack_args, uint32_t stack_count) {
    if (!host || !host->jit || entry_point == 0) return 0;

    auto *jit = host->jit.get();
    bool is_thumb = (entry_point & 1) != 0;
    uint32_t cpsr = jit->Cpsr();
    if (is_thumb) {
        cpsr |= (1u << 5);
    } else {
        cpsr &= ~(1u << 5);
    }
    jit->SetCpsr(cpsr);

    uint32_t sp = host->stack_top;
    if (stack_count > 0 && stack_args) {
        sp -= stack_count * 4;
        sp &= ~7u; // 8-byte align
        for (uint32_t i = 0; i < stack_count; ++i) {
            memcpy(host->img->mem + sp + i * 4, &stack_args[i], 4);
        }
    }

    jit->Regs()[13] = sp;
    jit->Regs()[0] = r0;
    jit->Regs()[1] = r1;
    jit->Regs()[2] = r2;
    jit->Regs()[3] = r3;
    jit->Regs()[14] = HALT_SENTINEL_LR;
    jit->Regs()[15] = entry_point & ~1u;

    jit->ClearHalt();
    s_samp_in_guest.store(true, std::memory_order_relaxed);
    jit->Run();
    s_samp_in_guest.store(false, std::memory_order_relaxed);

    // Reset SP
    jit->Regs()[13] = host->stack_top;
    return jit->Regs()[0];
}
