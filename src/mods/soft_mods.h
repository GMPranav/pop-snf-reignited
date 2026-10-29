#ifndef SOFT_MODS_H
#define SOFT_MODS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "elf32_loader.h"

typedef struct {
    bool clean_main_menu;      // Hide "Get More Coins" & "Get More Games" mobile promo UI
    bool clean_shop;           // Hide "+" coin button & suppress freemium popups in shop
    bool clean_level_complete; // Hide Facebook & social share buttons from level end screen
    bool clean_touch_hud;      // Hide on-screen touch buttons (Potion flask & Stealth Walk silhouette)
} soft_mods_config_t;

// Initialize the soft-mods subsystem with the active configuration
void soft_mods_init(const soft_mods_config_t *cfg);

// Update runtime configuration if modified dynamically
void soft_mods_set_config(const soft_mods_config_t *cfg);

// Retrieve active soft-mods configuration
const soft_mods_config_t* soft_mods_get_config(void);

// Apply guest memory patches once after ELF loading
void soft_mods_apply_patches(elf32_image_t *img);

// Apply active HUD soft-mods on each frame when p0 is available
void soft_mods_apply_hud(elf32_image_t *img, uint32_t p0, uint32_t p0_hud, uint32_t p0_scene);

// Update or evaluate active gameplay state using authoritative MainAI state machine
void soft_mods_update_gameplay_state(elf32_image_t *img, uint32_t p0, uint32_t p0_scene);

// Patch compressed texture in-place if Clean Shop mod is enabled and texture matches UI_mainmenu_store
void soft_mods_patch_compressed_texture(int level, int w, int h, int imageSize, void *data);

// Returns true if the game is currently in active gameplay (in-game level with controls active and unpaused)
bool soft_mods_is_in_gameplay(void);

#ifdef __cplusplus
}
#endif

#endif // SOFT_MODS_H
