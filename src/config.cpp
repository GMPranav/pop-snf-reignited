#include "config.h"
#include "gpu_detect.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

GameConfig g_config;

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

void GameConfig::reset_to_defaults() {
    width = 1280;
    height = 720;
    fullscreen = false;
    vsync = true;
    vsync_mode = 0;
    present_delay_ms = 0.0f;
    perf_log = false;
    perf_profile = false;
    fps_limit = 60;
    msaa = 8;
    anisotropic = 16;
    gpu_preference = get_system_gpu_topology().recommended_preference;
    mod_clean_main_menu = true;
    mod_clean_shop = true;
    mod_clean_level_complete = true;
    mod_clean_touch_hud = true;

    // Movement / Navigation (WASD primary, Arrows secondary)
    key_up = SDL_SCANCODE_W;
    key_down = SDL_SCANCODE_S;
    key_left = SDL_SCANCODE_A;
    key_right = SDL_SCANCODE_D;

    key_sec_up = SDL_SCANCODE_UP;
    key_sec_down = SDL_SCANCODE_DOWN;
    key_sec_left = SDL_SCANCODE_LEFT;
    key_sec_right = SDL_SCANCODE_RIGHT;

    // Core Actions
    key_attack = SDL_SCANCODE_L;        // L: Attack
    key_sec_attack = 0;

    key_jump = SDL_SCANCODE_J;          // J: Jump / Stand Jump / Forward Slash
    key_sec_jump = SDL_SCANCODE_SPACE;  // Space: Secondary Jump

    key_roll = SDL_SCANCODE_RETURN;     // Enter: Roll / Menu Interaction / Up Slash
    key_sec_roll = SDL_SCANCODE_LSHIFT; // Left Shift: Secondary Roll

    key_block = SDL_SCANCODE_Q;         // Q: Stealth Walk / Block in Combat
    key_sec_block = 0;

    key_potion = SDL_SCANCODE_E;        // E: Use Healing Potion
    key_sec_potion = 0;

    key_back = SDL_SCANCODE_ESCAPE;     // Esc: Back / Cancel
    key_sec_back = 0;

    key_pause = SDL_SCANCODE_P;         // P: Pause Game
    key_sec_pause = 0;

    pad_up = SDL_CONTROLLER_BUTTON_DPAD_UP;
    pad_down = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    pad_left = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    pad_right = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    pad_attack = SDL_CONTROLLER_BUTTON_X;               // X / Square: Attack
    pad_jump = SDL_CONTROLLER_BUTTON_A;                 // A / Cross: Jump
    pad_roll = SDL_CONTROLLER_BUTTON_B;                 // B / Circle: Roll / Interact
    pad_block = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;     // LB / L1: Block / Stealth
    pad_potion = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;   // RB / R1: Use Healing Potion
    pad_back = SDL_CONTROLLER_BUTTON_BACK;              // Back / Select
    pad_pause = SDL_CONTROLLER_BUTTON_START;            // Start / Menu
}

static int parse_bind_value(const char *val) {
    if (!val || val[0] == '\0') return 0;
    char *end = nullptr;
    long num = strtol(val, &end, 10);
    if (end && *end == '\0') {
        return (int)num;
    }
    if (_stricmp(val, "Mouse Left") == 0 || _stricmp(val, "Mouse 1") == 0 || _stricmp(val, "LMB") == 0) return MOUSE_BIND_LMB;
    if (_stricmp(val, "Mouse Middle") == 0 || _stricmp(val, "Mouse 2") == 0 || _stricmp(val, "MMB") == 0) return MOUSE_BIND_MMB;
    if (_stricmp(val, "Mouse Right") == 0 || _stricmp(val, "Mouse 3") == 0 || _stricmp(val, "RMB") == 0) return MOUSE_BIND_RMB;
    if (_stricmp(val, "Mouse 4") == 0 || _stricmp(val, "Mouse X1") == 0) return MOUSE_BIND_X1;
    if (_stricmp(val, "Mouse 5") == 0 || _stricmp(val, "Mouse X2") == 0) return MOUSE_BIND_X2;
    if (_stricmp(val, "Wheel Up") == 0 || _stricmp(val, "Mouse Wheel Up") == 0 || _stricmp(val, "WheelUp") == 0) return MOUSE_BIND_WHEEL_UP;
    if (_stricmp(val, "Wheel Down") == 0 || _stricmp(val, "Mouse Wheel Down") == 0 || _stricmp(val, "WheelDown") == 0) return MOUSE_BIND_WHEEL_DOWN;
    if (_stricmp(val, "Wheel Left") == 0 || _stricmp(val, "Mouse Wheel Left") == 0 || _stricmp(val, "WheelLeft") == 0) return MOUSE_BIND_WHEEL_LEFT;
    if (_stricmp(val, "Wheel Right") == 0 || _stricmp(val, "Mouse Wheel Right") == 0 || _stricmp(val, "WheelRight") == 0) return MOUSE_BIND_WHEEL_RIGHT;

    SDL_Scancode sc = SDL_GetScancodeFromName(val);
    if (sc != SDL_SCANCODE_UNKNOWN) return (int)sc;

    return 0;
}

static int parse_pad_value(const char *val) {
    if (!val || val[0] == '\0') return -1;
    char *end = nullptr;
    long num = strtol(val, &end, 10);
    if (end && *end == '\0') {
        return (int)num;
    }
    if (_stricmp(val, "LT") == 0 || _stricmp(val, "L2") == 0 ||
        _stricmp(val, "Left Trigger") == 0 || _stricmp(val, "LT / L2") == 0) return PAD_BIND_LT;
    if (_stricmp(val, "RT") == 0 || _stricmp(val, "R2") == 0 ||
        _stricmp(val, "Right Trigger") == 0 || _stricmp(val, "RT / R2") == 0) return PAD_BIND_RT;
    if (_stricmp(val, "LB") == 0 || _stricmp(val, "L1") == 0 || _stricmp(val, "LB / L1") == 0) return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    if (_stricmp(val, "RB") == 0 || _stricmp(val, "R1") == 0 || _stricmp(val, "RB / R1") == 0) return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    if (_stricmp(val, "None") == 0) return -1;

    SDL_GameControllerButton btn = SDL_GameControllerGetButtonFromString(val);
    if (btn != SDL_CONTROLLER_BUTTON_INVALID) return (int)btn;

    return -1;
}

bool GameConfig::load(const char *path) {
    reset_to_defaults();

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[Config] No %s found, using default configuration.\n", path);
        return false;
    }

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        // Strip comment
        char *comment = strpbrk(line, ";#");
        if (comment) *comment = '\0';

        trim(line);
        if (line[0] == '\0') continue;

        if (line[0] == '[' && line[strlen(line) - 1] == ']') {
            line[strlen(line) - 1] = '\0';
            strncpy(section, line + 1, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            trim(section);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcmp(section, "Graphics") == 0) {
            if (strcmp(key, "Width") == 0) width = atoi(val);
            else if (strcmp(key, "Height") == 0) height = atoi(val);
            else if (strcmp(key, "Fullscreen") == 0) fullscreen = (atoi(val) != 0);
            else if (strcmp(key, "VSync") == 0) vsync = (atoi(val) != 0);
            else if (strcmp(key, "VSyncMode") == 0) vsync_mode = atoi(val);
            else if (strcmp(key, "PresentDelayMs") == 0) present_delay_ms = (float)atof(val);
            else if (strcmp(key, "PerfLog") == 0) perf_log = (atoi(val) != 0);
            else if (strcmp(key, "PerfProfile") == 0) perf_profile = (atoi(val) != 0);
            else if (strcmp(key, "FPSLimit") == 0 || strcmp(key, "FpsLimit") == 0 || strcmp(key, "FPS_Limit") == 0) fps_limit = atoi(val);
            else if (strcmp(key, "MSAA") == 0) msaa = atoi(val);
            else if (strcmp(key, "Anisotropic") == 0) anisotropic = atoi(val);
            else if (strcmp(key, "GPUPreference") == 0 || strcmp(key, "GpuPreference") == 0) gpu_preference = atoi(val);
        } else if (strcmp(section, "Keyboard") == 0) {
            if (strcmp(key, "KeyUp") == 0) key_up = parse_bind_value(val);
            else if (strcmp(key, "KeyDown") == 0) key_down = parse_bind_value(val);
            else if (strcmp(key, "KeyLeft") == 0) key_left = parse_bind_value(val);
            else if (strcmp(key, "KeyRight") == 0) key_right = parse_bind_value(val);
            else if (strcmp(key, "KeyAttack") == 0) key_attack = parse_bind_value(val);
            else if (strcmp(key, "KeyJump") == 0) key_jump = parse_bind_value(val);
            else if (strcmp(key, "KeyRoll") == 0) key_roll = parse_bind_value(val);
            else if (strcmp(key, "KeyBlock") == 0) key_block = parse_bind_value(val);
            else if (strcmp(key, "KeyPotion") == 0) key_potion = parse_bind_value(val);
            else if (strcmp(key, "KeyConfirm") == 0) key_roll = parse_bind_value(val); // Legacy alias
            else if (strcmp(key, "KeyBack") == 0) key_back = parse_bind_value(val);
            else if (strcmp(key, "KeyPause") == 0) key_pause = parse_bind_value(val);

            else if (strcmp(key, "KeySecUp") == 0) key_sec_up = parse_bind_value(val);
            else if (strcmp(key, "KeySecDown") == 0) key_sec_down = parse_bind_value(val);
            else if (strcmp(key, "KeySecLeft") == 0) key_sec_left = parse_bind_value(val);
            else if (strcmp(key, "KeySecRight") == 0) key_sec_right = parse_bind_value(val);
            else if (strcmp(key, "KeySecAttack") == 0) key_sec_attack = parse_bind_value(val);
            else if (strcmp(key, "KeySecJump") == 0) key_sec_jump = parse_bind_value(val);
            else if (strcmp(key, "KeySecRoll") == 0) key_sec_roll = parse_bind_value(val);
            else if (strcmp(key, "KeySecBlock") == 0) key_sec_block = parse_bind_value(val);
            else if (strcmp(key, "KeySecPotion") == 0) key_sec_potion = parse_bind_value(val);
            else if (strcmp(key, "KeySecBack") == 0) key_sec_back = parse_bind_value(val);
            else if (strcmp(key, "KeySecPause") == 0) key_sec_pause = parse_bind_value(val);
        } else if (strcmp(section, "Gamepad") == 0) {
            if (strcmp(key, "PadUp") == 0) pad_up = parse_pad_value(val);
            else if (strcmp(key, "PadDown") == 0) pad_down = parse_pad_value(val);
            else if (strcmp(key, "PadLeft") == 0) pad_left = parse_pad_value(val);
            else if (strcmp(key, "PadRight") == 0) pad_right = parse_pad_value(val);
            else if (strcmp(key, "PadAttack") == 0) pad_attack = parse_pad_value(val);
            else if (strcmp(key, "PadJump") == 0) pad_jump = parse_pad_value(val);
            else if (strcmp(key, "PadRoll") == 0) pad_roll = parse_pad_value(val);
            else if (strcmp(key, "PadBlock") == 0) pad_block = parse_pad_value(val);
            else if (strcmp(key, "PadPotion") == 0) pad_potion = parse_pad_value(val);
            else if (strcmp(key, "PadBack") == 0) pad_back = parse_pad_value(val);
            else if (strcmp(key, "PadPause") == 0) pad_pause = parse_pad_value(val);
        } else if (strcmp(section, "Mods") == 0 || strcmp(section, "Enhancements") == 0) {
            if (strcmp(key, "CleanMainMenu") == 0) mod_clean_main_menu = (atoi(val) != 0);
            else if (strcmp(key, "CleanShop") == 0) mod_clean_shop = (atoi(val) != 0);
            else if (strcmp(key, "CleanLevelComplete") == 0 || strcmp(key, "CleanLevelEnd") == 0) mod_clean_level_complete = (atoi(val) != 0);
            else if (strcmp(key, "CleanTouchControls") == 0 || strcmp(key, "CleanTouchHUD") == 0 || strcmp(key, "HideTouchButtons") == 0 || strcmp(key, "HideTouchControls") == 0) mod_clean_touch_hud = (atoi(val) != 0);
        }
    }

    fclose(f);

    // Sanity clamping
    if (width < 640) width = 1280;
    if (height < 360) height = 720;
    if (vsync_mode < 0 || vsync_mode > 2) vsync_mode = 0;
    if (!(present_delay_ms >= 0.0f)) present_delay_ms = 0.0f;
    if (present_delay_ms > 12.0f) present_delay_ms = 12.0f;
    if (fps_limit < 0) fps_limit = 60;
    if (fps_limit > 360) fps_limit = 360;
    if (msaa < 0) msaa = 0;
    if (msaa > 16) msaa = 16;
    if (anisotropic < 1) anisotropic = 1;
    if (anisotropic > 16) anisotropic = 16;
    if (gpu_preference == -1) gpu_preference = get_system_gpu_topology().recommended_preference;

    printf("[Config] Loaded %s (%dx%d, Fullscreen=%d, VSync=%d, FPSLimit=%d, MSAA=%dx, AF=%dx)\n",
           path, width, height, (int)fullscreen, (int)vsync, fps_limit, msaa, anisotropic);
    return true;
}

bool GameConfig::save(const char *path) const {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[-] Failed to open %s for writing\n", path);
        return false;
    }

    fprintf(f, "; Prince of Persia: Shadow and the Flame - Reignited Configuration\n\n");

    fprintf(f, "[Graphics]\n");
    fprintf(f, "Width = %d\n", width);
    fprintf(f, "Height = %d\n", height);
    fprintf(f, "Fullscreen = %d\n", fullscreen ? 1 : 0);
    fprintf(f, "VSync = %d\n", vsync ? 1 : 0);
    fprintf(f, "VSyncMode = %d\n", vsync_mode);
    fprintf(f, "PresentDelayMs = %.1f\n", present_delay_ms);
    fprintf(f, "PerfLog = %d\n", perf_log ? 1 : 0);
    fprintf(f, "PerfProfile = %d\n", perf_profile ? 1 : 0);
    fprintf(f, "FPSLimit = %d\n", fps_limit);
    fprintf(f, "MSAA = %d\n", msaa);
    fprintf(f, "Anisotropic = %d\n", anisotropic);
    fprintf(f, "GPUPreference = %d\n\n", gpu_preference);

    fprintf(f, "[Keyboard]\n");
    fprintf(f, "KeyUp = %d\n", key_up);
    fprintf(f, "KeyDown = %d\n", key_down);
    fprintf(f, "KeyLeft = %d\n", key_left);
    fprintf(f, "KeyRight = %d\n", key_right);
    fprintf(f, "KeyAttack = %d\n", key_attack);
    fprintf(f, "KeyJump = %d\n", key_jump);
    fprintf(f, "KeyRoll = %d\n", key_roll);
    fprintf(f, "KeyBlock = %d\n", key_block);
    fprintf(f, "KeyPotion = %d\n", key_potion);
    fprintf(f, "KeyBack = %d\n", key_back);
    fprintf(f, "KeyPause = %d\n", key_pause);

    fprintf(f, "KeySecUp = %d\n", key_sec_up);
    fprintf(f, "KeySecDown = %d\n", key_sec_down);
    fprintf(f, "KeySecLeft = %d\n", key_sec_left);
    fprintf(f, "KeySecRight = %d\n", key_sec_right);
    fprintf(f, "KeySecAttack = %d\n", key_sec_attack);
    fprintf(f, "KeySecJump = %d\n", key_sec_jump);
    fprintf(f, "KeySecRoll = %d\n", key_sec_roll);
    fprintf(f, "KeySecBlock = %d\n", key_sec_block);
    fprintf(f, "KeySecPotion = %d\n", key_sec_potion);
    fprintf(f, "KeySecBack = %d\n", key_sec_back);
    fprintf(f, "KeySecPause = %d\n\n", key_sec_pause);

    fprintf(f, "[Gamepad]\n");
    fprintf(f, "PadUp = %d\n", pad_up);
    fprintf(f, "PadDown = %d\n", pad_down);
    fprintf(f, "PadLeft = %d\n", pad_left);
    fprintf(f, "PadRight = %d\n", pad_right);
    fprintf(f, "PadAttack = %d\n", pad_attack);
    fprintf(f, "PadJump = %d\n", pad_jump);
    fprintf(f, "PadRoll = %d\n", pad_roll);
    fprintf(f, "PadBlock = %d\n", pad_block);
    fprintf(f, "PadPotion = %d\n", pad_potion);
    fprintf(f, "PadBack = %d\n", pad_back);
    fprintf(f, "PadPause = %d\n\n", pad_pause);

    fprintf(f, "[Mods]\n");
    fprintf(f, "CleanMainMenu = %d\n", mod_clean_main_menu ? 1 : 0);
    fprintf(f, "CleanShop = %d\n", mod_clean_shop ? 1 : 0);
    fprintf(f, "CleanLevelComplete = %d\n", mod_clean_level_complete ? 1 : 0);
    fprintf(f, "CleanTouchControls = %d\n", mod_clean_touch_hud ? 1 : 0);

    fclose(f);
    printf("[Config] Configuration successfully saved to %s\n", path);
    return true;
}

const char* get_bind_name(int bind_code) {
    if (bind_code == 0) return "None";
    if (bind_code == MOUSE_BIND_LMB) return "Mouse Left";
    if (bind_code == MOUSE_BIND_MMB) return "Mouse Middle";
    if (bind_code == MOUSE_BIND_RMB) return "Mouse Right";
    if (bind_code == MOUSE_BIND_X1) return "Mouse 4";
    if (bind_code == MOUSE_BIND_X2) return "Mouse 5";
    if (bind_code == MOUSE_BIND_WHEEL_UP) return "Wheel Up";
    if (bind_code == MOUSE_BIND_WHEEL_DOWN) return "Wheel Down";
    if (bind_code == MOUSE_BIND_WHEEL_LEFT) return "Wheel Left";
    if (bind_code == MOUSE_BIND_WHEEL_RIGHT) return "Wheel Right";
    if (bind_code > MOUSE_BIND_BASE && bind_code <= MOUSE_BIND_BASE + 10) {
        static char mbuf[32];
        snprintf(mbuf, sizeof(mbuf), "Mouse %d", bind_code - MOUSE_BIND_BASE);
        return mbuf;
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)bind_code);
    if (name && name[0] != '\0') return name;
    static char buf[32];
    snprintf(buf, sizeof(buf), "Key %d", bind_code);
    return buf;
}

const char* get_pad_name(int pad_btn) {
    if (pad_btn == PAD_BIND_LT) return "LT / L2";
    if (pad_btn == PAD_BIND_RT) return "RT / R2";

    switch ((SDL_GameControllerButton)pad_btn) {
        case SDL_CONTROLLER_BUTTON_A: return "A / Cross";
        case SDL_CONTROLLER_BUTTON_B: return "B / Circle";
        case SDL_CONTROLLER_BUTTON_X: return "X / Square";
        case SDL_CONTROLLER_BUTTON_Y: return "Y / Triangle";
        case SDL_CONTROLLER_BUTTON_BACK: return "Back / Select";
        case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
        case SDL_CONTROLLER_BUTTON_START: return "Start / Menu";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Left Stick Click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right Stick Click";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB / L1";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB / R1";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Pad Right";
        default: return "None";
    }
}
