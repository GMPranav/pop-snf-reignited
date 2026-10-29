#include <windows.h>
#include <commdlg.h>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GL/gl.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <sys/stat.h>
#include <ctime>

#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/backends/imgui_impl_sdl2.h"
#include "../../third_party/imgui/backends/imgui_impl_opengl2.h"

#include "../config.h"
#include "../gpu_detect.h"
#include "extractor.h"
#include "embedded_assets.h"

namespace fs = std::filesystem;

static std::string s_app_dir = ".";
static void init_app_dir() {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH)) {
        char *last_slash = strrchr(exe_path, '\\');
        if (!last_slash) last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            s_app_dir = exe_path;
            SetCurrentDirectoryA(exe_path);
            return;
        }
    }
#endif
    s_app_dir = ".";
}

enum ViewState {
    VIEW_MAIN,
    VIEW_SETTINGS,
    VIEW_EXTRACTING
};

static ViewState s_view = VIEW_MAIN;
static int s_settings_tab = 0; // Active settings tab index (0=Graphics, 1=Controls, 2=Save Data, 3=Enhancements)
static bool s_open_reset_confirm = false;
static uint32_t s_reset_toast_ticks = 0;

// Textures
static GLuint s_tex_hero = 0;
static GLuint s_tex_logo = 0;

// Extraction State
static std::atomic<float> s_extract_progress(0.0f);
static std::string s_extract_status = "Ready";
static std::mutex s_extract_mutex;
static std::atomic<bool> s_extract_done(false);
static std::atomic<bool> s_extract_success(false);
static std::thread s_extract_thread;

// Control Binding State
static int s_rebinding_action = -1; // -1 = none
static bool s_rebinding_gamepad = false;
static bool s_rebinding_is_sec = false;
static uint32_t s_rebinding_start_ticks = 0;

// Dolphin-style Escape Handling & Rebind Timing
// True while the Controls tab was shown last frame (undo/redo only tracks key binds)
static bool s_on_controls_tab = false;
static bool s_on_controls_tab_next = false;
static bool s_escape_held = false;
static uint32_t s_escape_hold_start = 0;
static uint32_t s_last_escape_tap = 0;
static const uint32_t ESCAPE_HOLD_DURATION_MS = 750;
static const uint32_t ESCAPE_DOUBLE_TAP_MS = 400;

// Screen bounding boxes for on-screen prompt buttons
static ImVec2 s_clear_btn_min = {0, 0};
static ImVec2 s_clear_btn_max = {0, 0};
static ImVec2 s_cancel_btn_min = {0, 0};
static ImVec2 s_cancel_btn_max = {0, 0};

static void cancel_rebinding() {
    s_rebinding_action = -1;
    s_escape_held = false;
    s_escape_hold_start = 0;
    s_last_escape_tap = 0;
}

// Undo / Redo History
static std::vector<GameConfig> s_undo_stack;
static std::vector<GameConfig> s_redo_stack;
static const size_t MAX_UNDO_LEVELS = 50;

static void push_undo_state(const GameConfig &cfg) {
    s_undo_stack.push_back(cfg);
    if (s_undo_stack.size() > MAX_UNDO_LEVELS) {
        s_undo_stack.erase(s_undo_stack.begin());
    }
    s_redo_stack.clear();
}

static bool can_undo() {
    return !s_undo_stack.empty();
}

static bool can_redo() {
    return !s_redo_stack.empty();
}

static void perform_undo(GameConfig &cfg) {
    if (s_undo_stack.empty()) return;
    s_redo_stack.push_back(cfg);
    cfg = s_undo_stack.back();
    s_undo_stack.pop_back();
}

static void perform_redo(GameConfig &cfg) {
    if (s_redo_stack.empty()) return;
    s_undo_stack.push_back(cfg);
    cfg = s_redo_stack.back();
    s_redo_stack.pop_back();
}

struct ActionDef {
    const char *name;
    int *primary_key;
    int *sec_key;
    int *pad_btn;
};

static std::vector<ActionDef> get_action_defs(GameConfig &cfg) {
    return {
        { "Move Up",                     &cfg.key_up,     &cfg.key_sec_up,     &cfg.pad_up },
        { "Move Down",                   &cfg.key_down,   &cfg.key_sec_down,   &cfg.pad_down },
        { "Move Left",                   &cfg.key_left,   &cfg.key_sec_left,   &cfg.pad_left },
        { "Move Right",                  &cfg.key_right,  &cfg.key_sec_right,  &cfg.pad_right },
        { "Attack",                      &cfg.key_attack, &cfg.key_sec_attack, &cfg.pad_attack },
        { "Jump / Forward Slash",        &cfg.key_jump,   &cfg.key_sec_jump,   &cfg.pad_jump },
        { "Roll / Interact / Up Slash",  &cfg.key_roll,   &cfg.key_sec_roll,   &cfg.pad_roll },
        { "Stealth Walk / Block",        &cfg.key_block,  &cfg.key_sec_block,  &cfg.pad_block },
        { "Use Healing Potion",          &cfg.key_potion, &cfg.key_sec_potion, &cfg.pad_potion },
        { "Back / Cancel",               &cfg.key_back,   &cfg.key_sec_back,   &cfg.pad_back },
        { "Pause Game",                  &cfg.key_pause,  &cfg.key_sec_pause,  &cfg.pad_pause },
    };
}

static void apply_bind_with_swap(GameConfig &cfg, int action_idx, bool is_sec, int new_bind, bool is_gamepad) {
    auto actions = get_action_defs(cfg);
    if (action_idx < 0 || action_idx >= (int)actions.size()) return;

    if (is_gamepad) {
        int *target = actions[action_idx].pad_btn;
        if (!target) return;
        int old_val = *target;
        if (old_val == new_bind) return;

        push_undo_state(cfg);

        if (new_bind >= 0) {
            for (size_t i = 0; i < actions.size(); ++i) {
                if ((int)i == action_idx) continue;
                if (actions[i].pad_btn && *actions[i].pad_btn == new_bind) {
                    *actions[i].pad_btn = old_val;
                    old_val = -1;
                }
            }
        }
        *target = new_bind;
    } else {
        int *target = is_sec ? actions[action_idx].sec_key : actions[action_idx].primary_key;
        if (!target) return;
        int old_val = *target;
        if (old_val == new_bind) return;

        push_undo_state(cfg);

        if (new_bind != 0) {
            for (size_t i = 0; i < actions.size(); ++i) {
                // Check primary slot
                if (!((int)i == action_idx && !is_sec)) {
                    if (actions[i].primary_key && *actions[i].primary_key == new_bind) {
                        *actions[i].primary_key = old_val;
                        old_val = 0;
                    }
                }
                // Check secondary slot
                if (!((int)i == action_idx && is_sec)) {
                    if (actions[i].sec_key && *actions[i].sec_key == new_bind) {
                        *actions[i].sec_key = old_val;
                        old_val = 0;
                    }
                }
            }
        }
        *target = new_bind;
    }
}

static std::string find_asset_file(const char *filename) {
    const char *candidates[] = {
        filename,
        "ref_assets/Logos_Wallpapers_Hero/%s",
        "../ref_assets/Logos_Wallpapers_Hero/%s",
        "assets/%s",
        "../assets/%s",
        NULL
    };
    char buf[512];
    for (int i = 0; candidates[i]; ++i) {
        snprintf(buf, sizeof(buf), candidates[i], filename);
        if (fs::exists(buf)) {
            return std::string(buf);
        }
    }
    return "";
}

static GLuint load_gl_texture_from_mem(const unsigned char *data_buf, size_t data_len) {
    if (!data_buf || data_len == 0) return 0;
    int w, h, comp;
    unsigned char *data = stbi_load_from_memory(data_buf, (int)data_len, &w, &h, &comp, 4);
    if (!data) return 0;

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(data);
    return tex_id;
}

static GLuint load_gl_texture(const char *filename) {
    // Official Reignited branding logo is always sourced from embedded assets
    if (strstr(filename, "logo")) {
        return load_gl_texture_from_mem(s_embedded_logo_png, s_embedded_logo_png_len);
    }

    std::string path = find_asset_file(filename);
    if (!path.empty()) {
        int w, h, comp;
        unsigned char *data = stbi_load(path.c_str(), &w, &h, &comp, 4);
        if (data) {
            GLuint tex_id = 0;
            glGenTextures(1, &tex_id);
            glBindTexture(GL_TEXTURE_2D, tex_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(data);
            return tex_id;
        }
    }

    // Fallback to embedded standalone assets
    if (strstr(filename, "hero")) {
        return load_gl_texture_from_mem(s_embedded_hero_jpg, s_embedded_hero_jpg_len);
    }
    if (strstr(filename, "logo")) {
        return load_gl_texture_from_mem(s_embedded_logo_png, s_embedded_logo_png_len);
    }

    return 0;
}

static void set_app_icon(SDL_Window *window) {
    int w = 0, h = 0, comp = 0;
    unsigned char *data = nullptr;
    std::string path = find_asset_file("app_icon.png");
    if (!path.empty()) {
        data = stbi_load(path.c_str(), &w, &h, &comp, 4);
    }
    if (!data) {
        data = stbi_load_from_memory(s_embedded_logo_png, (int)s_embedded_logo_png_len, &w, &h, &comp, 4);
    }
    if (!data) return;

    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
        data, w, h, 32, 4 * w,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
    );
    if (surface) {
        SDL_SetWindowIcon(window, surface);
        SDL_FreeSurface(surface);
    }
    stbi_image_free(data);
}

static std::string open_apk_dialog() {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "Prince of Persia APK (*.apk)\0*.apk;*.zip\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Select Prince of Persia: Shadow and the Flame APK";
    if (GetOpenFileNameA(&ofn)) {
        if (!s_app_dir.empty()) SetCurrentDirectoryA(s_app_dir.c_str());
        return std::string(filename);
    }
    if (!s_app_dir.empty()) SetCurrentDirectoryA(s_app_dir.c_str());
    return "";
}

static void start_extraction(const std::string &apk_path) {
    s_extract_progress = 0.01f;
    {
        std::lock_guard<std::mutex> lock(s_extract_mutex);
        s_extract_status = "Starting extraction...";
    }
    s_extract_done = false;
    s_extract_success = false;
    s_view = VIEW_EXTRACTING;

    if (s_extract_thread.joinable()) {
        s_extract_thread.join();
    }

    std::string target = s_app_dir;
    s_extract_thread = std::thread([apk_path, target]() {
        bool ok = extract_apk_to_dir(apk_path.c_str(), target.c_str(), [](float p, const char *msg) {
            s_extract_progress = p;
            std::lock_guard<std::mutex> lock(s_extract_mutex);
            s_extract_status = msg;
        });
        s_extract_success = ok;
        s_extract_done = true;
    });
}

#ifdef _WIN32
static void ensure_compatibility_flags(const std::string &exe_path) {
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, 
                        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                        0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        char current_val[400] = {0};
        DWORD val_size = sizeof(current_val) - 1;
        DWORD type = 0;
        LONG res = RegQueryValueExA(hKey, exe_path.c_str(), NULL, &type, (LPBYTE)current_val, &val_size);
        if (res != ERROR_SUCCESS || strstr(current_val, "DISABLEDXMAXIMIZEDWINDOWEDMODE") == NULL) {
            // Append to any existing flags instead of clobbering them
            char new_val[512] = {0};
            if (res == ERROR_SUCCESS && current_val[0] != '\0') {
                snprintf(new_val, sizeof(new_val), "%s DISABLEDXMAXIMIZEDWINDOWEDMODE", current_val);
            } else {
                snprintf(new_val, sizeof(new_val), "~ DISABLEDXMAXIMIZEDWINDOWEDMODE");
            }
            RegSetValueExA(hKey, exe_path.c_str(), 0, REG_SZ, (const BYTE*)new_val, (DWORD)strlen(new_val) + 1);
        }
        RegCloseKey(hKey);
    }
}

static void apply_windows_gpu_preference(const std::string &exe_path, int pref) {
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\DirectX\\UserGpuPreferences",
                        0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        if (pref > 0) {
            char val[64];
            snprintf(val, sizeof(val), "GpuPreference=%d;SwapEffectUpgradeEnable=0;", pref);
            RegSetValueExA(hKey, exe_path.c_str(), 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
        } else {
            const char *val = "SwapEffectUpgradeEnable=0;";
            RegSetValueExA(hKey, exe_path.c_str(), 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
        }
        RegCloseKey(hKey);
    }
}
#endif

static bool s_launcher_debug = false;

static std::string get_game_exe_path() {
    return s_app_dir + "\\PoPSnF_Reignited.exe";
}

static std::string get_game_exe_name() {
    return "PoPSnF_Reignited.exe";
}

static void launch_game() {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    std::string full_exe = get_game_exe_path();
#ifdef _WIN32
    ensure_compatibility_flags(full_exe);
    apply_windows_gpu_preference(full_exe, g_config.gpu_preference);
#endif
    std::string cmd = "\"" + full_exe + "\"";
    if (s_launcher_debug) {
        cmd += " --debug";
    }
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    if (CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, FALSE, 0, NULL, s_app_dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        exit(0);
    } else {
        std::string exe_name = get_game_exe_name();
        std::string fallback_cmd = exe_name;
        if (s_launcher_debug) {
            fallback_cmd += " --debug";
        }
        std::vector<char> fallback_buf(fallback_cmd.begin(), fallback_cmd.end());
        fallback_buf.push_back('\0');
        if (CreateProcessA(NULL, fallback_buf.data(), NULL, NULL, FALSE, 0, NULL, s_app_dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            exit(0);
        }
        std::string err_msg = "Failed to start " + exe_name + ". Make sure it exists in this directory.";
        MessageBoxA(NULL, err_msg.c_str(), "Launch Error", MB_ICONERROR);
    }
}

static void save_gl_screenshot(const char *filename, int w, int h) {
    uint8_t *rgba = (uint8_t*)malloc(w * h * 4);
    if (!rgba) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    FILE *f = fopen(filename, "wb");
    if (f) {
        uint8_t header[54] = {
            'B', 'M', 0,0,0,0, 0,0,0,0, 54,0,0,0,
            40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 32,0,
            0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
        };
        uint32_t fsize = 54 + w * h * 4;
        memcpy(header + 2, &fsize, 4);
        memcpy(header + 18, &w, 4);
        memcpy(header + 22, &h, 4);
        fwrite(header, 1, 54, f);
        for (int i = 0; i < w * h; ++i) {
            uint8_t b = rgba[i * 4 + 2];
            uint8_t g = rgba[i * 4 + 1];
            uint8_t r = rgba[i * 4 + 0];
            uint8_t a = rgba[i * 4 + 3];
            fputc(b, f); fputc(g, f); fputc(r, f); fputc(a, f);
        }
        fclose(f);
        printf("[+] Launcher screenshot saved: %s\n", filename);
    }
    free(rgba);
}

static fs::path get_save_file_path() {
    std::error_code ec;
    fs::path p1 = fs::path(s_app_dir) / "Saves" / "POP2SaveData.sts";
    if (fs::exists(p1, ec)) return p1;
    fs::path p2 = fs::path("Saves") / "POP2SaveData.sts";
    if (fs::exists(p2, ec)) return p2;
    return p1;
}

static bool has_save_file() {
    std::error_code ec;
    fs::path p1 = fs::path(s_app_dir) / "Saves" / "POP2SaveData.sts";
    if (fs::exists(p1, ec)) return true;
    fs::path p2 = fs::path("Saves") / "POP2SaveData.sts";
    if (fs::exists(p2, ec)) return true;

    fs::path dir = fs::path(s_app_dir) / "Saves";
    if (fs::exists(dir, ec) && fs::is_directory(dir, ec)) {
        for (const auto &entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".sts") return true;
        }
    }
    return false;
}

static std::string get_save_file_info() {
    std::error_code ec;
    fs::path p = get_save_file_path();
    if (!fs::exists(p, ec)) {
        return "No save data found (fresh game state)";
    }
    auto sz = fs::file_size(p, ec);
    struct stat st;
    char time_buf[64] = "";
    if (stat(p.string().c_str(), &st) == 0) {
        struct tm *tm_info = localtime(&st.st_mtime);
        if (tm_info) {
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
        }
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "File: %s\nSize: %.1f KB (%llu bytes)\nLast Modified: %s",
             p.filename().string().c_str(), (float)sz / 1024.0f, (unsigned long long)sz, time_buf);
    return std::string(buf);
}

static bool delete_save_data() {
    bool deleted_any = false;
    std::error_code ec;

    auto clean_dir = [&](const fs::path &dir) {
        if (fs::exists(dir, ec) && fs::is_directory(dir, ec)) {
            for (const auto &entry : fs::directory_iterator(dir, ec)) {
                if (fs::is_regular_file(entry.path(), ec)) {
                    std::string fname = entry.path().filename().string();
                    std::string ext = entry.path().extension().string();
                    if (fname == "POP2SaveData.sts" || ext == ".sts") {
                        // Keep a safety backup before deleting primary save
                        if (fname == "POP2SaveData.sts") {
                            fs::path bak = entry.path();
                            bak += ".bak";
                            fs::copy_file(entry.path(), bak, fs::copy_options::overwrite_existing, ec);
                        }
                        if (fs::remove(entry.path(), ec)) {
                            deleted_any = true;
                        }
                    }
                }
            }
        }
    };

    clean_dir(fs::path(s_app_dir) / "Saves");
    if (s_app_dir != ".") {
        clean_dir(fs::path("Saves"));
    }

    return deleted_any;
}

// Draw the nostalgic PoP 2008 segmented progress bar matching Loading.png
static void draw_segmented_progress_bar(float progress, float width, float height) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + width, p0.y + height);

    // Frame border / dark track
    draw->AddRectFilled(p0, p1, IM_COL32(10, 14, 22, 240), 2.0f);
    draw->AddRect(p0, p1, IM_COL32(80, 95, 120, 255), 2.0f, 0, 1.5f);

    const int total_segments = 38;
    float pad = 3.0f;
    float total_seg_w = width - (pad * 2.0f);
    float seg_gap = 2.0f;
    float seg_w = (total_seg_w - (total_segments - 1) * seg_gap) / (float)total_segments;
    float seg_h = height - (pad * 2.0f);

    int active_segments = (int)(progress * (float)total_segments + 0.5f);
    if (active_segments > total_segments) active_segments = total_segments;

    for (int i = 0; i < total_segments; ++i) {
        float x_left = p0.x + pad + i * (seg_w + seg_gap);
        float x_right = x_left + seg_w;
        float y_top = p0.y + pad;
        float y_bot = y_top + seg_h;

        if (i < active_segments) {
            // Glowing cyan/azure segment
            draw->AddRectFilled(ImVec2(x_left, y_top), ImVec2(x_right, y_bot), IM_COL32(56, 189, 248, 255), 1.0f);
            draw->AddRectFilled(ImVec2(x_left, y_top), ImVec2(x_right, y_top + seg_h * 0.4f), IM_COL32(186, 230, 253, 180), 1.0f);
        } else {
            // Empty segment slot
            draw->AddRectFilled(ImVec2(x_left, y_top), ImVec2(x_right, y_bot), IM_COL32(20, 26, 38, 200), 1.0f);
        }
    }

    ImGui::Dummy(ImVec2(width, height));
}

static void apply_pop2008_style() {
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;

    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;

    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.09f, 0.11f, 0.95f);
    colors[ImGuiCol_Border]               = ImVec4(0.35f, 0.40f, 0.48f, 0.80f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.14f, 0.18f, 0.90f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.30f, 0.38f, 1.00f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.16f, 0.19f, 0.24f, 1.00f);
    colors[ImGuiCol_Button]               = ImVec4(0.16f, 0.18f, 0.22f, 0.95f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.34f, 0.44f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.14f, 0.45f, 0.70f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.22f, 0.26f, 0.34f, 1.00f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.30f, 0.36f, 0.46f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.18f, 0.48f, 0.75f, 1.00f);
    colors[ImGuiCol_CheckMark]            = ImVec4(0.35f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.35f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.55f, 0.85f, 1.00f, 1.00f);
    colors[ImGuiCol_Tab]                  = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.28f, 0.34f, 0.44f, 1.00f);
    colors[ImGuiCol_TabActive]            = ImVec4(0.22f, 0.26f, 0.34f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]  = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.94f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
}

int main(int argc, char *argv[]) {
    init_app_dir();

    const char *screenshot_path = nullptr;
    bool screenshot_settings = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--extract") == 0 && i + 1 < argc) {
            const char *apk_path = argv[++i];
            printf("[Launcher CLI] Extracting APK '%s' into %s...\n", apk_path, s_app_dir.c_str());
            bool ok = extract_apk_to_dir(apk_path, s_app_dir.c_str(), [](float p, const char *msg) {
                printf("[Extraction %3.0f%%] %s\n", p * 100.0f, msg);
            });
            if (ok) {
                printf("[Launcher CLI] Extraction successful! All game assets populated.\n");
                return 0;
            } else {
                fprintf(stderr, "[-] Extraction failed!\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--settings-screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
            screenshot_settings = true;
        } else if (strcmp(argv[i], "--controls-screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
            screenshot_settings = true;
            s_settings_tab = 1;
        } else if (strcmp(argv[i], "--controls-rebind-screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
            screenshot_settings = true;
            s_settings_tab = 1;
            s_rebinding_action = 4; // Attack
        } else if (strcmp(argv[i], "--savedata-screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
            screenshot_settings = true;
            s_settings_tab = 2;
        } else if ((strcmp(argv[i], "--enhancements-screenshot") == 0 || strcmp(argv[i], "--mods-screenshot") == 0) && i + 1 < argc) {
            screenshot_path = argv[++i];
            screenshot_settings = true;
            s_settings_tab = 3;
        } else if (strcmp(argv[i], "--screenshot-reset-confirm") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
            s_open_reset_confirm = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            s_launcher_debug = true;
        }
    }

    // Prevent DPI blur on Windows
    SetProcessDPIAware();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "[-] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Open first controller for gamepad binding in launcher
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            SDL_GameControllerOpen(i);
            break;
        }
    }

    const int WIN_W = 850;
    const int WIN_H = 540;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *window = SDL_CreateWindow(
        "Prince of Persia: Shadow and the Flame - Reignited - Launcher & Configurator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );

    if (!window) {
        fprintf(stderr, "[-] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    set_app_icon(window);

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    apply_pop2008_style();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL2_Init();

    s_tex_hero = load_gl_texture("hero.jpg");
    s_tex_logo = load_gl_texture("logo.png");

    g_config.load("config.ini");
    GameConfig temp_cfg = g_config;

    if (screenshot_settings) {
        s_view = VIEW_SETTINGS;
    }

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                running = false;
            }

            // Key and Mouse capture for rebinding or shortcuts
            if (event.type == SDL_KEYDOWN) {
                SDL_Scancode sc = event.key.keysym.scancode;
                if (s_rebinding_action >= 0) {
                    if (SDL_GetTicks() - s_rebinding_start_ticks > 150) {
                        if (sc == SDL_SCANCODE_ESCAPE) {
                            if (s_rebinding_gamepad) {
                                cancel_rebinding();
                            } else if (!event.key.repeat) {
                                uint32_t now = SDL_GetTicks();
                                if (s_last_escape_tap > 0 && (now - s_last_escape_tap < ESCAPE_DOUBLE_TAP_MS)) {
                                    // Double-tap Escape: bind Escape immediately
                                    apply_bind_with_swap(temp_cfg, s_rebinding_action, s_rebinding_is_sec, (int)SDL_SCANCODE_ESCAPE, false);
                                    cancel_rebinding();
                                } else {
                                    // Begin holding Escape
                                    s_escape_held = true;
                                    s_escape_hold_start = now;
                                }
                            }
                        } else if (!s_rebinding_gamepad) {
                            apply_bind_with_swap(temp_cfg, s_rebinding_action, s_rebinding_is_sec, (int)sc, false);
                            cancel_rebinding();
                        }
                    }
                } else {
                    // Global shortcuts when not actively rebinding
                    SDL_Keymod mod = SDL_GetModState();
                    bool ctrl = (mod & KMOD_CTRL) != 0;
                    bool shift = (mod & KMOD_SHIFT) != 0;
                    if (!s_on_controls_tab || s_view != VIEW_SETTINGS) {
                        // Undo history only covers key binds; ignore elsewhere
                    } else if (ctrl && !shift && sc == SDL_SCANCODE_Z) {
                        perform_undo(temp_cfg);
                    } else if ((ctrl && sc == SDL_SCANCODE_Y) || (ctrl && shift && sc == SDL_SCANCODE_Z)) {
                        perform_redo(temp_cfg);
                    }
                }
            } else if (event.type == SDL_KEYUP) {
                if (s_rebinding_action >= 0 && !s_rebinding_gamepad) {
                    if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        if (s_escape_held) {
                            uint32_t held = SDL_GetTicks() - s_escape_hold_start;
                            s_escape_held = false;
                            if (held < ESCAPE_HOLD_DURATION_MS) {
                                s_last_escape_tap = SDL_GetTicks();
                            }
                        }
                    }
                }
            } else if (event.type == SDL_MOUSEWHEEL) {
                if (s_rebinding_action >= 0 && !s_rebinding_gamepad) {
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

                    if (wheel_bind != 0) {
                        apply_bind_with_swap(temp_cfg, s_rebinding_action, s_rebinding_is_sec, wheel_bind, false);
                        cancel_rebinding();
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (s_rebinding_action >= 0) {
                    if (SDL_GetTicks() - s_rebinding_start_ticks > 150) {
                        float mx = (float)event.button.x;
                        float my = (float)event.button.y;
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            if (mx >= s_clear_btn_min.x && mx <= s_clear_btn_max.x &&
                                my >= s_clear_btn_min.y && my <= s_clear_btn_max.y) {
                                apply_bind_with_swap(temp_cfg, s_rebinding_action, s_rebinding_is_sec, s_rebinding_gamepad ? -1 : 0, s_rebinding_gamepad);
                                cancel_rebinding();
                            } else if (mx >= s_cancel_btn_min.x && mx <= s_cancel_btn_max.x &&
                                       my >= s_cancel_btn_min.y && my <= s_cancel_btn_max.y) {
                                cancel_rebinding();
                            } else if (!s_rebinding_gamepad) {
                                apply_bind_with_swap(temp_cfg, s_rebinding_action, s_rebinding_is_sec, MOUSE_BIND_LMB, false);
                                cancel_rebinding();
                            }
                        } else if (!s_rebinding_gamepad) {
                            int mouse_bind = mouse_to_bind(event.button.button);
                            apply_bind_with_swap(temp_cfg, s_rebinding_action, s_rebinding_is_sec, mouse_bind, false);
                            cancel_rebinding();
                        }
                    }
                }
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                if (s_rebinding_action >= 0 && s_rebinding_gamepad) {
                    apply_bind_with_swap(temp_cfg, s_rebinding_action, false, (int)event.cbutton.button, true);
                    cancel_rebinding();
                }
            } else if (event.type == SDL_CONTROLLERAXISMOTION) {
                if (s_rebinding_action >= 0 && s_rebinding_gamepad) {
                    if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && event.caxis.value > 16000) {
                        apply_bind_with_swap(temp_cfg, s_rebinding_action, false, PAD_BIND_LT, true);
                        cancel_rebinding();
                    } else if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && event.caxis.value > 16000) {
                        apply_bind_with_swap(temp_cfg, s_rebinding_action, false, PAD_BIND_RT, true);
                        cancel_rebinding();
                    }
                }
            } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                SDL_GameControllerOpen(event.cdevice.which);
            }
        }

        // Process Escape hold duration and single tap timeout
        if (s_rebinding_action >= 0) {
            uint32_t now = SDL_GetTicks();
            if (s_escape_held) {
                if (now - s_escape_hold_start >= ESCAPE_HOLD_DURATION_MS) {
                    apply_bind_with_swap(temp_cfg, s_rebinding_action, s_rebinding_is_sec, (int)SDL_SCANCODE_ESCAPE, false);
                    cancel_rebinding();
                }
            } else if (s_last_escape_tap > 0) {
                if (now - s_last_escape_tap >= ESCAPE_DOUBLE_TAP_MS) {
                    cancel_rebinding();
                }
            }
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)WIN_W, (float)WIN_H));
        ImGui::Begin("LauncherRoot", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus
        );

        ImDrawList *draw_list = ImGui::GetWindowDrawList();

        // Draw hero background graphic
        if (s_tex_hero) {
            draw_list->AddImage(
                (ImTextureID)(intptr_t)s_tex_hero,
                ImVec2(0, 0),
                ImVec2((float)WIN_W, (float)WIN_H),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, (s_view == VIEW_SETTINGS) ? 120 : 220)
            );
        } else {
            draw_list->AddRectFilled(ImVec2(0, 0), ImVec2((float)WIN_W, (float)WIN_H), IM_COL32(20, 24, 32, 255));
        }

        // Vignette / Shadow Overlay
        draw_list->AddRectFilledMultiColor(
            ImVec2(0, 0), ImVec2((float)WIN_W, (float)WIN_H),
            IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 40),
            IM_COL32(0, 0, 0, 220), IM_COL32(0, 0, 0, 240)
        );

        bool installed = is_game_installed(s_app_dir.c_str());

        // --- VIEW: MAIN LAUNCHER ---
        if (s_view == VIEW_MAIN) {
            // Draw PoP Logo: centered horizontally directly above the buttons
            if (s_tex_logo) {
                float logo_w = 460.0f;
                float logo_h = 230.0f;
                float logo_x = ((float)WIN_W - logo_w) * 0.5f;
                float logo_y = 165.0f;
                draw_list->AddImage(
                    (ImTextureID)(intptr_t)s_tex_logo,
                    ImVec2(logo_x, logo_y),
                    ImVec2(logo_x + logo_w, logo_y + logo_h),
                    ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, 255)
                );
            }

            // Installation status text: centered horizontally right below logo
            const char *status_str = installed ? "STATUS: GAME ASSETS INSTALLED & READY" : "STATUS: ASSETS MISSING - PLEASE EXTRACT APK";
            ImVec2 text_sz = ImGui::CalcTextSize(status_str);
            float text_x = ((float)WIN_W - text_sz.x) * 0.5f;
            ImGui::SetCursorPos(ImVec2(text_x, 405.0f));
            if (installed) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.85f, 0.50f, 1.0f));
                ImGui::Text("%s", status_str);
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                ImGui::Text("%s", status_str);
                ImGui::PopStyleColor();
            }

            // Launcher action button bar: horizontally centered across the window
            ImGui::SetCursorPos(ImVec2(99.0f, 445.0f));

            // [ Launch the game! ]
            if (ImGui::Button("Launch the game!", ImVec2(180, 48))) {
                if (installed) {
                    launch_game();
                } else {
                    std::string apk = open_apk_dialog();
                    if (!apk.empty()) {
                        start_extraction(apk);
                    }
                }
            }

            ImGui::SameLine(0, 14);
            // [ Settings ]
            if (ImGui::Button("Settings", ImVec2(130, 48))) {
                temp_cfg = g_config;
                s_undo_stack.clear();
                s_redo_stack.clear();
                s_view = VIEW_SETTINGS;
            }

            ImGui::SameLine(0, 14);
            // [ Extract Game Assets ]
            if (ImGui::Button("Extract Game Assets", ImVec2(190, 48))) {
                std::string apk = open_apk_dialog();
                if (!apk.empty()) {
                    start_extraction(apk);
                }
            }

            ImGui::SameLine(0, 14);
            // [ Quit ]
            if (ImGui::Button("Quit", ImVec2(110, 48))) {
                running = false;
            }

            // Bottom bar: save file indicator and Reset Progress option
            bool save_exists = has_save_file();
            ImGui::SetCursorPos(ImVec2(24.0f, 506.0f));
            if (s_reset_toast_ticks > 0 && (SDL_GetTicks() - s_reset_toast_ticks < 5000)) {
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.50f, 1.0f), "[+] Save file deleted. Progress has been reset.");
            } else if (save_exists) {
                ImGui::TextDisabled("Save: POP2SaveData.sts");
            }

            ImGui::SetCursorPos(ImVec2((float)WIN_W - 150.0f, 502.0f));
            ImGui::BeginDisabled(!save_exists);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.16f, 0.16f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.22f, 0.22f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.12f, 0.12f, 1.00f));
            if (ImGui::Button("Reset progress", ImVec2(130, 26))) {
                s_open_reset_confirm = true;
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (save_exists) {
                    ImGui::SetTooltip("Permanently delete the save file to start the game over from Chapter 1.");
                } else {
                    ImGui::SetTooltip("No active save file exists to delete.");
                }
            }
        }

        // --- VIEW: SETTINGS (PoP 2008 Tabbed Dialog) ---
        else if (s_view == VIEW_SETTINGS) {
            // Header
            ImGui::SetCursorPos(ImVec2(24, 20));
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
            ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.45f, 1.0f), "PRINCE OF PERSIA: CONFIGURATION");
            ImGui::PopFont();

            // Right-side command buttons (Default, OK, Cancel)
            ImGui::SetCursorPos(ImVec2(710, 50));
            if (ImGui::Button("Default", ImVec2(115, 34))) {
                push_undo_state(temp_cfg);
                temp_cfg.reset_to_defaults();
            }
            ImGui::SetCursorPos(ImVec2(710, 95));
            if (ImGui::Button("OK", ImVec2(115, 34))) {
                g_config = temp_cfg;
                g_config.save("config.ini");
#ifdef _WIN32
                std::string full_exe = get_game_exe_path();
                ensure_compatibility_flags(full_exe);
                apply_windows_gpu_preference(full_exe, g_config.gpu_preference);
#endif
                cancel_rebinding();
                s_view = VIEW_MAIN;
            }
            ImGui::SetCursorPos(ImVec2(710, 140));
            if (ImGui::Button("Cancel", ImVec2(115, 34))) {
                cancel_rebinding();
                s_view = VIEW_MAIN;
            }

            // Tab bar at left
            ImGui::SetCursorPos(ImVec2(24, 60));
            if (ImGui::BeginTabBar("SettingsTabs")) {
                // --- TAB 1: GRAPHICS ---
                if (ImGui::BeginTabItem("Graphics Options")) {
                    ImGui::Spacing();
                    ImGui::BeginChild("GraphicsChild", ImVec2(660, 420), true);

                    // Screen Resolution Dropdown
                    ImGui::Text("Screen Resolution:");
                    const char *resolutions[] = {
                        "1280 x 720  (16:9 720p - Native)",
                        "1600 x 900  (16:9)",
                        "1920 x 1080 (16:9 1080p - Full HD)",
                        "2560 x 1440 (16:9 1440p - 2K QHD)",
                        "3840 x 2160 (16:9 4K UHD)"
                    };
                    const int res_w[] = { 1280, 1600, 1920, 2560, 3840 };
                    const int res_h[] = { 720,  900,  1080, 1440, 2160 };
                    int cur_res_idx = 0;
                    for (int i = 0; i < 5; ++i) {
                        if (temp_cfg.width == res_w[i] && temp_cfg.height == res_h[i]) {
                            cur_res_idx = i;
                            break;
                        }
                    }
                    ImGui::SetNextItemWidth(340);
                    if (ImGui::Combo("##Resolution", &cur_res_idx, resolutions, 5)) {
                        temp_cfg.width = res_w[cur_res_idx];
                        temp_cfg.height = res_h[cur_res_idx];
                    }

                    ImGui::Spacing();
                    // Fullscreen
                    ImGui::Checkbox("Fullscreen Mode (Borderless Desktop)", &temp_cfg.fullscreen);

                    ImGui::Spacing();
                    // Graphics Adapter (GPU)
                    const SystemGpuTopology &topo = get_system_gpu_topology();
                    if (topo.adapters.size() > 1) {
                        ImGui::Text("Graphics Adapter (GPU):");
                        std::vector<std::string> gpu_labels;
                        std::vector<const char*> gpu_opts;
                        std::vector<int> gpu_vals;

                        gpu_labels.push_back("Windows Default / Auto-Detect");
                        gpu_vals.push_back(0);

                        if (topo.integrated_index != -1) {
                            gpu_labels.push_back(topo.adapters[topo.integrated_index].name + " (Display-Attached / Tear-Free VSync) [Recommended]");
                            gpu_vals.push_back(1);
                        }

                        if (topo.discrete_index != -1) {
                            gpu_labels.push_back(topo.adapters[topo.discrete_index].name + " (Discrete High Performance)");
                            gpu_vals.push_back(2);
                        }

                        for (size_t i = 0; i < gpu_labels.size(); ++i) {
                            gpu_opts.push_back(gpu_labels[i].c_str());
                        }

                        int effective_pref = (temp_cfg.gpu_preference == -1) ? topo.recommended_preference : temp_cfg.gpu_preference;
                        int cur_gpu_idx = 0;
                        for (size_t i = 0; i < gpu_vals.size(); ++i) {
                            if (effective_pref == gpu_vals[i]) {
                                cur_gpu_idx = (int)i;
                                break;
                            }
                        }

                        ImGui::SetNextItemWidth(340);
                        if (ImGui::Combo("##GPUAdapter", &cur_gpu_idx, gpu_opts.data(), (int)gpu_opts.size())) {
                            temp_cfg.gpu_preference = gpu_vals[cur_gpu_idx];
                        }
                        if (ImGui::IsItemHovered()) {
                            if (topo.integrated_index != -1) {
                                std::string tip = "Selects which GPU runs the game on multi-GPU systems.\n" +
                                                  topo.adapters[topo.integrated_index].name +
                                                  " is directly wired to your display panel, guaranteeing tear-free VSync.";
                                ImGui::SetTooltip("%s", tip.c_str());
                            } else {
                                ImGui::SetTooltip("Selects which GPU runs the game on multi-GPU systems.");
                            }
                        }
                    } else if (topo.adapters.size() == 1) {
                        ImGui::Text("Graphics Adapter (GPU):");
                        ImGui::TextDisabled("%s (System Default)", topo.adapters[0].name.c_str());
                    }

                    ImGui::Spacing();
                    // VSync
                    ImGui::Checkbox("Vertical Sync (VSync - Match Refresh Rate)", &temp_cfg.vsync);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Synchronizes frame rate with monitor refresh to eliminate screen tearing.");
                    }

                    ImGui::Spacing();
                    // Frame Rate Limit
                    ImGui::BeginDisabled(temp_cfg.vsync);
                    ImGui::Text("Frame Rate Limit (FPS):%s", temp_cfg.vsync ? " (Locked by VSync)" : "");
                    const char *fps_opts[] = {
                        "30 FPS",
                        "60 FPS (Default / Recommended)",
                        "120 FPS",
                        "144 FPS",
                        "165 FPS",
                        "240 FPS",
                        "Unlimited / Off"
                    };
                    const int fps_vals[] = { 30, 60, 120, 144, 165, 240, 0 };
                    int cur_fps_idx = 1; // default to 60 FPS
                    bool found_fps = false;
                    for (int i = 0; i < 7; ++i) {
                        if (temp_cfg.fps_limit == fps_vals[i]) {
                            cur_fps_idx = i;
                            found_fps = true;
                            break;
                        }
                    }
                    char custom_fps_buf[64];
                    if (!found_fps) {
                        snprintf(custom_fps_buf, sizeof(custom_fps_buf), "Custom (%d FPS)", temp_cfg.fps_limit);
                    }
                    ImGui::SetNextItemWidth(340);
                    if (ImGui::BeginCombo("##FPSLimit", found_fps ? fps_opts[cur_fps_idx] : custom_fps_buf)) {
                        for (int i = 0; i < 7; ++i) {
                            const bool is_selected = (cur_fps_idx == i && found_fps);
                            if (ImGui::Selectable(fps_opts[i], is_selected)) {
                                temp_cfg.fps_limit = fps_vals[i];
                            }
                            if (is_selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        if (temp_cfg.vsync) {
                            ImGui::SetTooltip("Frame rate limit is inactive because VSync is enabled.\nThe game presentation is synchronized directly with your display's refresh rate.");
                        } else {
                            ImGui::SetTooltip("Sets maximum engine frame rate. 60 FPS is recommended for standard gameplay pacing.");
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Antialiasing (MSAA)
                    ImGui::Text("Antialiasing (MSAA):");
                    const char *msaa_opts[] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA (High Quality)" };
                    const int msaa_vals[] = { 0, 2, 4, 8 };
                    int cur_msaa_idx = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (temp_cfg.msaa == msaa_vals[i]) cur_msaa_idx = i;
                    }
                    ImGui::SetNextItemWidth(340);
                    if (ImGui::Combo("##MSAA", &cur_msaa_idx, msaa_opts, 4)) {
                        temp_cfg.msaa = msaa_vals[cur_msaa_idx];
                    }

                    ImGui::Spacing();
                    // Anisotropic Filtering
                    ImGui::Text("Anisotropic Filtering (AF):");
                    const char *af_opts[] = { "Off (1x)", "2x AF", "4x AF", "8x AF", "16x AF (Crisp Textures)" };
                    const int af_vals[] = { 1, 2, 4, 8, 16 };
                    int cur_af_idx = 4;
                    for (int i = 0; i < 5; ++i) {
                        if (temp_cfg.anisotropic == af_vals[i]) cur_af_idx = i;
                    }
                    ImGui::SetNextItemWidth(340);
                    if (ImGui::Combo("##AF", &cur_af_idx, af_opts, 5)) {
                        temp_cfg.anisotropic = af_vals[cur_af_idx];
                    }

                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                // --- TAB 2: CONTROLS ---
                ImGuiTabItemFlags ctrl_flags = (s_settings_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("Controls Options", nullptr, ctrl_flags)) {
                    s_settings_tab = -1;
                    s_on_controls_tab_next = true;
                    ImGui::Spacing();
                    ImGui::BeginChild("ControlsChild", ImVec2(660, 420), true);

                    if (s_rebinding_action >= 0) {
                        auto actions = get_action_defs(temp_cfg);
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.22f, 0.35f, 0.95f));
                        ImGui::BeginChild("RebindPrompt", ImVec2(0, 56), true);

                        uint32_t now = SDL_GetTicks();
                        if (s_escape_held) {
                            float prog = (float)(now - s_escape_hold_start) / (float)ESCAPE_HOLD_DURATION_MS;
                            if (prog > 1.0f) prog = 1.0f;
                            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Holding ESC to bind Escape... (Release to cancel)");
                            ImGui::ProgressBar(prog, ImVec2(340, 18), "Binding Escape...");
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f),
                                "Press input for [%s%s]...",
                                actions[s_rebinding_action].name,
                                s_rebinding_is_sec ? " (Secondary)" : ""
                            );
                            ImGui::TextColored(ImVec4(0.7f, 0.75f, 0.85f, 1.0f),
                                s_rebinding_gamepad ? "Pull trigger, press button, or press ESC / Cancel to abort." : "Hold ESC or double-tap ESC to bind. Tap ESC to cancel."
                            );
                        }

                        ImGui::SameLine(465);
                        ImGui::SetCursorPosY(12);
                        ImGui::Button("Clear", ImVec2(65, 30));
                        s_clear_btn_min = ImGui::GetItemRectMin();
                        s_clear_btn_max = ImGui::GetItemRectMax();

                        ImGui::SameLine(540);
                        ImGui::SetCursorPosY(12);
                        ImGui::Button("Cancel", ImVec2(75, 30));
                        s_cancel_btn_min = ImGui::GetItemRectMin();
                        s_cancel_btn_max = ImGui::GetItemRectMax();

                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                        ImGui::Spacing();
                    } else {
                        s_clear_btn_min = s_clear_btn_max = ImVec2(0, 0);
                        s_cancel_btn_min = s_cancel_btn_max = ImVec2(0, 0);
                    }

                    ImGui::Text("Click any button below to change its key binding:");
                    ImGui::SameLine(410);
                    ImGui::BeginDisabled(!can_undo());
                    if (ImGui::Button("Undo (Ctrl+Z)", ImVec2(105, 24))) {
                        perform_undo(temp_cfg);
                    }
                    ImGui::EndDisabled();

                    ImGui::SameLine(525);
                    ImGui::BeginDisabled(!can_redo());
                    if (ImGui::Button("Redo (Ctrl+Y)", ImVec2(105, 24))) {
                        perform_redo(temp_cfg);
                    }
                    ImGui::EndDisabled();

                    ImGui::Spacing();

                    if (ImGui::BeginTable("KeybindTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 195);
                        ImGui::TableSetupColumn("Primary Key", ImGuiTableColumnFlags_WidthFixed, 130);
                        ImGui::TableSetupColumn("Secondary Key", ImGuiTableColumnFlags_WidthFixed, 130);
                        ImGui::TableSetupColumn("Gamepad Button", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        auto actions = get_action_defs(temp_cfg);
                        for (size_t i = 0; i < actions.size(); ++i) {
                            ImGui::TableNextRow();

                            // Action Name
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%s", actions[i].name);

                            // Primary Key
                            ImGui::TableSetColumnIndex(1);
                            char btn_label[64];
                            snprintf(btn_label, sizeof(btn_label), "%s##pri%zu",
                                get_bind_name(*actions[i].primary_key), i);
                            if (ImGui::Button(btn_label, ImVec2(130, 0))) {
                                s_rebinding_action = (int)i;
                                s_rebinding_gamepad = false;
                                s_rebinding_is_sec = false;
                                s_rebinding_start_ticks = SDL_GetTicks();
                                s_escape_held = false;
                                s_escape_hold_start = 0;
                                s_last_escape_tap = 0;
                            }

                            // Secondary Key
                            ImGui::TableSetColumnIndex(2);
                            const char *sec_name = actions[i].sec_key && *actions[i].sec_key > 0
                                ? get_bind_name(*actions[i].sec_key)
                                : "None";
                            snprintf(btn_label, sizeof(btn_label), "%s##sec%zu", sec_name, i);
                            if (ImGui::Button(btn_label, ImVec2(130, 0))) {
                                s_rebinding_action = (int)i;
                                s_rebinding_gamepad = false;
                                s_rebinding_is_sec = true;
                                s_rebinding_start_ticks = SDL_GetTicks();
                                s_escape_held = false;
                                s_escape_hold_start = 0;
                                s_last_escape_tap = 0;
                            }

                            // Gamepad
                            ImGui::TableSetColumnIndex(3);
                            if (actions[i].pad_btn) {
                                const char *pad_name = get_pad_name(*actions[i].pad_btn);
                                snprintf(btn_label, sizeof(btn_label), "%s##pad%zu", pad_name, i);
                                if (ImGui::Button(btn_label, ImVec2(140, 0))) {
                                    s_rebinding_action = (int)i;
                                    s_rebinding_gamepad = true;
                                    s_rebinding_is_sec = false;
                                    s_rebinding_start_ticks = SDL_GetTicks();
                                    s_escape_held = false;
                                    s_escape_hold_start = 0;
                                    s_last_escape_tap = 0;
                                }
                            } else {
                                ImGui::TextDisabled("N/A");
                            }
                        }
                        ImGui::EndTable();
                    }

                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                // --- TAB 3: SAVE DATA / PROGRESS ---
                ImGuiTabItemFlags save_flags = (s_settings_tab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("Save Data", nullptr, save_flags)) {
                    s_settings_tab = -1;
                    ImGui::Spacing();
                    ImGui::BeginChild("SaveDataChild", ImVec2(660, 420), true);

                    ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.45f, 1.0f), "GAME PROGRESS & SAVE DATA MANAGEMENT");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::Spacing();

                    bool save_exists = has_save_file();
                    if (save_exists) {
                        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.50f, 1.0f), "Status: Active save file detected");
                        ImGui::Spacing();
                        std::string info = get_save_file_info();
                        ImGui::TextWrapped("%s", info.c_str());
                        ImGui::Spacing();
                        ImGui::TextDisabled("Location: %s", get_save_file_path().string().c_str());
                        ImGui::Spacing();
                        ImGui::Spacing();

                        ImGui::TextWrapped("Resetting progress will permanently erase your current campaign save file. The game will create a fresh new save on next launch and start over from Chapter 1.");
                        ImGui::Spacing();
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 0.95f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.24f, 0.24f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.14f, 0.14f, 1.00f));
                        if (ImGui::Button("Reset progress", ImVec2(150, 36))) {
                            s_open_reset_confirm = true;
                        }
                        ImGui::PopStyleColor(3);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Permanently delete your current save file (POP2SaveData.sts) to restart the game from Chapter 1.");
                        }
                    } else {
                        ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.70f, 1.0f), "Status: No save data found (fresh game state)");
                        ImGui::Spacing();
                        ImGui::TextDisabled("When you play the game, save files will be stored in:\n%s",
                                            (fs::path(s_app_dir) / "Saves").string().c_str());
                        ImGui::Spacing();
                        ImGui::Spacing();

                        ImGui::BeginDisabled(true);
                        ImGui::Button("Reset progress", ImVec2(150, 36));
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("No active save file exists to delete.");
                        }
                    }

                    if (s_reset_toast_ticks > 0 && (SDL_GetTicks() - s_reset_toast_ticks < 5000)) {
                        ImGui::Spacing();
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.50f, 1.0f), "[+] Save file deleted. Progress has been reset successfully.");
                    }

                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                // --- TAB 4: ENHANCEMENTS & SOFT-MODS ---
                ImGuiTabItemFlags enh_flags = (s_settings_tab == 3) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("Enhancements", nullptr, enh_flags)) {
                    s_settings_tab = -1;
                    ImGui::Spacing();
                    ImGui::BeginChild("EnhancementsChild", ImVec2(660, 420), true);

                    ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.45f, 1.0f), "PC VISUAL ENHANCEMENTS & SOFT-MODS");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextWrapped("Optional non-destructive enhancements designed to tailor the presentation and UI for modern PC displays while eliminating mobile artifacts.");
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Section: Main Menu & UI
                    ImGui::TextColored(ImVec4(0.40f, 0.80f, 1.0f, 1.0f), "User Interface Enhancements");
                    ImGui::Spacing();

                    ImGui::Checkbox("Clean Main Menu (Hide mobile promo buttons)", &temp_cfg.mod_clean_main_menu);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Removes mobile freemium buttons (\"Get More Coins\" and \"Get More Games\") from the bottom of the main menu for a clean PC title experience.");
                    }
                    ImGui::Spacing();
                    ImGui::Indent(28.0f);
                    ImGui::TextDisabled("Removes \"Get More Coins\" and \"Get More Games\" buttons from the main menu.");
                    ImGui::Unindent(28.0f);

                    ImGui::Spacing();

                    ImGui::Checkbox("Clean Shop (Remove \"+\" coin button & freemium popups)", &temp_cfg.mod_clean_shop);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Removes the \"+\" button beside the coin counter and suppresses \"Need more coins\" and offerwall promo popups for an unobtrusive store experience.");
                    }
                    ImGui::Spacing();
                    ImGui::Indent(28.0f);
                    ImGui::TextDisabled("Hides \"+\" coin button, suppresses \"Need more coins\" popup, and blocks ads.");
                    ImGui::Unindent(28.0f);

                    ImGui::Spacing();

                    ImGui::Checkbox("Clean Social & Level Complete (Remove Facebook & share UI)", &temp_cfg.mod_clean_level_complete);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Removes Facebook social buttons, level end share cards, the Friends menu in Extras, and Achievement share icons for an authentic PC experience.");
                    }
                    ImGui::Spacing();
                    ImGui::Indent(28.0f);
                    ImGui::TextDisabled("Hides Facebook links, Friends menu, and social share buttons across all menus.");
                    ImGui::Unindent(28.0f);

                    ImGui::Spacing();

                    ImGui::Checkbox("Clean Touch Controls (Hide on-screen touch buttons)", &temp_cfg.mod_clean_touch_hud);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Removes all floating on-screen mobile touch controls during gameplay for an uncluttered PC display. Keyboard and gamepad controls remain fully functional.");
                    }
                    ImGui::Spacing();
                    ImGui::Indent(28.0f);
                    ImGui::TextDisabled("Hides all on-screen touch buttons (joystick, action, potion, stealth walk) during gameplay.");
                    ImGui::Unindent(28.0f);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.70f, 1.0f), "More soft-mod enhancements will be introduced in future updates.");

                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }

        // --- VIEW: ASSET EXTRACTION (Modeled after Loading.png) ---
        else if (s_view == VIEW_EXTRACTING) {
            float progress = s_extract_progress.load();
            std::string status_msg;
            {
                std::lock_guard<std::mutex> lock(s_extract_mutex);
                status_msg = s_extract_status;
            }

            ImGui::SetCursorPos(ImVec2(100, 160));
            ImGui::BeginChild("ExtractModal", ImVec2(650, 240), true);

            ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.45f, 1.0f), "PRINCE OF PERSIA: ASSET EXTRACTION");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Extracting game assets from APK package...");
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", status_msg.c_str());
            ImGui::Spacing();

            // Segmented Progress Bar
            draw_segmented_progress_bar(progress, 620, 24);

            ImGui::Spacing();
            ImGui::Text("Progress: %3.0f%%", progress * 100.0f);

            if (s_extract_done.load()) {
                ImGui::Spacing();
                if (s_extract_success.load()) {
                    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.50f, 1.0f), "Success! All assets unpacked and configured.");
                    if (ImGui::Button("Continue to Launcher", ImVec2(180, 36))) {
                        s_view = VIEW_MAIN;
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Extraction encountered an error!");
                    if (ImGui::Button("Back", ImVec2(120, 36))) {
                        s_view = VIEW_MAIN;
                    }
                }
            }

            ImGui::EndChild();
        }

        // --- MODAL POPUP: RESET PROGRESS CONFIRMATION ---
        if (s_open_reset_confirm) {
            ImGui::OpenPopup("Reset Progress?");
            s_open_reset_confirm = false;
        }

        ImVec2 center = ImVec2((float)WIN_W * 0.5f, (float)WIN_H * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Reset Progress?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.40f, 0.40f, 1.0f));
            ImGui::Text("WARNING: This action cannot be undone!");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Text("Are you sure you want to reset all game progress?\n\nThis will permanently delete 'POP2SaveData.sts'.\nAll story progression and unlocked chapters will restart from Chapter 1.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button("Yes, Delete Save File", ImVec2(175, 34))) {
                delete_save_data();
                s_reset_toast_ticks = SDL_GetTicks();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0, 15);
            if (ImGui::Button("Cancel", ImVec2(100, 34))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, WIN_W, WIN_H);
        glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);

        static int s_rendered_frames = 0;
        if (screenshot_path && ++s_rendered_frames >= 10) {
            save_gl_screenshot(screenshot_path, WIN_W, WIN_H);
            running = false;
        }

        s_on_controls_tab = s_on_controls_tab_next;
        s_on_controls_tab_next = false;

        SDL_Delay(16);
    }

    if (s_extract_thread.joinable()) {
        s_extract_thread.join();
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (s_tex_hero) glDeleteTextures(1, &s_tex_hero);
    if (s_tex_logo) glDeleteTextures(1, &s_tex_logo);

    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}
#endif
