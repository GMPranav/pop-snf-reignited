#ifndef CONFIG_H
#define CONFIG_H

#include <SDL2/SDL_scancode.h>
#include <SDL2/SDL_gamecontroller.h>
#include <SDL2/SDL_mouse.h>
#include <SDL2/SDL_keyboard.h>

#define MOUSE_BIND_BASE 1000
#define MOUSE_BIND_LMB  1001 /* Primary mouse button (SDL_BUTTON_LEFT) */
#define MOUSE_BIND_MMB  1002 /* Middle mouse button (SDL_BUTTON_MIDDLE) */
#define MOUSE_BIND_RMB  1003 /* Secondary mouse button (SDL_BUTTON_RIGHT) */
#define MOUSE_BIND_X1   1004 /* Extra side button 1 (SDL_BUTTON_X1) */
#define MOUSE_BIND_X2   1005 /* Extra side button 2 (SDL_BUTTON_X2) */
#define MOUSE_BIND_WHEEL_UP    1006
#define MOUSE_BIND_WHEEL_DOWN  1007
#define MOUSE_BIND_WHEEL_LEFT  1008
#define MOUSE_BIND_WHEEL_RIGHT 1009

inline bool is_mouse_bind(int code) { return code >= 1001 && code <= 1010; }
inline int mouse_to_bind(int sdl_btn) { return MOUSE_BIND_BASE + sdl_btn; }
inline int bind_to_mouse(int code) { return code - MOUSE_BIND_BASE; }

const char* get_bind_name(int bind_code);
const char* get_pad_name(int pad_btn);

#define PAD_BIND_LT 25 /* Left analog trigger (SDL_CONTROLLER_AXIS_TRIGGERLEFT) */
#define PAD_BIND_RT 26 /* Right analog trigger (SDL_CONTROLLER_AXIS_TRIGGERRIGHT) */

struct GameConfig {
    // Graphics Settings
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    bool vsync = true;
    bool perf_profile = false;  // with PerfLog: time every engine->host call and attribute slow frames (small overhead)
    bool perf_log = false;     // write logs/perf_<date>.log (frame times, spikes, causes) every run
    float present_delay_ms = 0.0f; // wait this long after the compositor sync before presenting (config.ini only, 0-12)
    int vsync_mode = 0;        // 0 = auto, 1 = force driver swap interval, 2 = force DWM flush pacing (config.ini only)
    int fps_limit = 60;         // 0 (unlimited), 30, 60, 120, 144, 165, 240
    int msaa = 8;               // 0 (off), 2, 4, 8
    int anisotropic = 16;       // 1 (off), 2, 4, 8, 16
    int gpu_preference = -1;    // -1 (Auto-detect from hardware), 0 (System Default), 1 (Integrated/Power Saving), 2 (Discrete/High Performance)

    // Soft-Mods / Visual Enhancements
    bool mod_clean_main_menu = true;      // Clean Main Menu: remove "Get More Coins" & "Get More Games" buttons
    bool mod_clean_shop = true;           // Clean Shop: remove "+" button & freemium popups in shop
    bool mod_clean_level_complete = true; // Clean Level Complete: remove Facebook & social share buttons from level end screen
    bool mod_clean_touch_hud = true;      // Clean Touch Controls: remove floating on-screen buttons for potion and stealth walk

    // Keyboard Primary Controls (SDL_Scancode)
    int key_up = SDL_SCANCODE_W;        // WASD: Movement / Menu Navigation
    int key_down = SDL_SCANCODE_S;
    int key_left = SDL_SCANCODE_A;
    int key_right = SDL_SCANCODE_D;
    int key_attack = SDL_SCANCODE_L;    // L: Attack
    int key_jump = SDL_SCANCODE_J;      // J: Jump / Stand Jump / Forward Slash
    int key_roll = SDL_SCANCODE_RETURN; // Enter: Roll / Menu Interaction / Up Slash
    int key_block = SDL_SCANCODE_Q;     // Q: Stealth Walk / Block in Combat
    int key_potion = SDL_SCANCODE_E;    // E: Use Healing Potion
    int key_back = SDL_SCANCODE_ESCAPE; // Esc: Back / Cancel
    int key_pause = SDL_SCANCODE_P;     // P: Pause Game

    // Keyboard Secondary Controls (SDL_Scancode)
    int key_sec_up = SDL_SCANCODE_UP;   // Arrow Keys: Secondary Navigation
    int key_sec_down = SDL_SCANCODE_DOWN;
    int key_sec_left = SDL_SCANCODE_LEFT;
    int key_sec_right = SDL_SCANCODE_RIGHT;
    int key_sec_jump = SDL_SCANCODE_SPACE; // Space: Secondary Jump
    int key_sec_roll = SDL_SCANCODE_LSHIFT; // Left Shift: Secondary Roll
    int key_sec_attack = 0;
    int key_sec_block = 0;
    int key_sec_potion = 0;
    int key_sec_back = 0;
    int key_sec_pause = 0;

    // Gamepad Controls (SDL_GameControllerButton)
    int pad_up = SDL_CONTROLLER_BUTTON_DPAD_UP;
    int pad_down = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    int pad_left = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    int pad_right = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    int pad_attack = SDL_CONTROLLER_BUTTON_X;               // X / Square: Attack
    int pad_jump = SDL_CONTROLLER_BUTTON_A;                 // A / Cross: Jump
    int pad_roll = SDL_CONTROLLER_BUTTON_B;                 // B / Circle: Roll / Interact
    int pad_block = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;     // LB / L1: Block / Stealth
    int pad_potion = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;   // RB / R1: Use Healing Potion
    int pad_back = SDL_CONTROLLER_BUTTON_BACK;              // Back / Select
    int pad_pause = SDL_CONTROLLER_BUTTON_START;            // Start / Menu

    void reset_to_defaults();
    bool load(const char *path = "config.ini");
    bool save(const char *path = "config.ini") const;
};

extern GameConfig g_config;

#endif // CONFIG_H
