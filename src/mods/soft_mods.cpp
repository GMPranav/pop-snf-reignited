#include "soft_mods.h"
#include "elf32_loader.h"
#include "reignited_logo_dxt5.h"
#include <cstdio>
#include <cstring>

static soft_mods_config_t s_soft_mods_cfg = {
    true, // clean_main_menu enabled by default
    true, // clean_shop enabled by default
    true, // clean_level_complete enabled by default
    true  // clean_touch_hud enabled by default
};

void soft_mods_init(const soft_mods_config_t *cfg) {
    if (cfg) {
        s_soft_mods_cfg = *cfg;
    }
}

void soft_mods_set_config(const soft_mods_config_t *cfg) {
    if (cfg) {
        s_soft_mods_cfg = *cfg;
    }
}

const soft_mods_config_t* soft_mods_get_config(void) {
    return &s_soft_mods_cfg;
}

// ---------------------------------------------------------------------------
// Soft-Mod Code Patches (Applied once after ELF image is loaded in memory)
// ---------------------------------------------------------------------------

void soft_mods_apply_patches(elf32_image_t *img) {
    if (!img || !img->mem) return;

    if (s_soft_mods_cfg.clean_shop) {
        // 1. Suppress fnShowShop: suppresses "Need more coins" pack popup
        // when clicking an item with insufficient balance or pressing coin purchase.
        uint32_t sym_fnShowShop = elf32_lookup_symbol(img, "_ZN13aiStoreAndInv10fnShowShopEv");
        if (sym_fnShowShop && sym_fnShowShop + 4 <= img->mem_size) {
            // ARM opcode: bx lr (0xE12FFF1E)
            *(uint32_t*)(img->mem + sym_fnShowShop) = 0xE12FFF1E;
        }

        // 2. Suppress onShowOfferAds in aiShopPrices
        uint32_t sym_onShowOfferAds = elf32_lookup_symbol(img, "_ZN12aiShopPrices14onShowOfferAdsEiPKN4S3DX10AIVariableEPS1_");
        if (sym_onShowOfferAds && sym_onShowOfferAds + 4 <= img->mem_size) {
            // ARM opcode: bx lr (0xE12FFF1E)
            *(uint32_t*)(img->mem + sym_onShowOfferAds) = 0xE12FFF1E;
        }

        // 3. Suppress ad confirmation check in MainAI::onShowConfirmation (+0x90)
        uint32_t sym_onShowConfirmation = elf32_lookup_symbol(img, "_ZN6MainAI18onShowConfirmationEiPKN4S3DX10AIVariableEPS1_");
        if (sym_onShowConfirmation && sym_onShowConfirmation + 0x90 + 4 <= img->mem_size) {
            // Replace conditional bne with unconditional branch b #0x216550 (0xEA00003A)
            *(uint32_t*)(img->mem + sym_onShowConfirmation + 0x90) = 0xEA00003A;
        }

        // 4. Suppress ad prompt when exiting store in aiStoreAndInv::fnLeaveStore (+0x2D0)
        uint32_t sym_fnLeaveStore = elf32_lookup_symbol(img, "_ZN13aiStoreAndInv12fnLeaveStoreEv");
        if (sym_fnLeaveStore && sym_fnLeaveStore + 0x2D0 + 4 <= img->mem_size) {
            // Replace conditional beq with unconditional branch b #0x2d2c94 (0xEA000013)
            *(uint32_t*)(img->mem + sym_fnLeaveStore + 0x2D0) = 0xEA000013;
        }

        // 5. Remove "Coins" freemium purchase item from all shop tabs (Weapons, Supplies, Combos, Bundles):
        // (a) Patch default serialized store string at so_base + 0x0092C284 so new games/resets never contain "Coins:"
        // (b) Patch fnInitCategoryTable to dynamically filter out "Coins:" if present from each category,
        //     ensuring both new games and existing saves start directly with real game items.
        uint32_t sym_fnInitCategoryTable = elf32_lookup_symbol(img, "_ZN13aiStoreAndInv19fnInitCategoryTableEv");
        if (sym_fnInitCategoryTable && sym_fnInitCategoryTable + 0xC00 <= img->mem_size) {
            uint32_t so_base = sym_fnInitCategoryTable - 0x002D109C;
            const char *clean_store_str = "Weapon4:Weapon5:Weapon6-HealthPotion:RevivePotion:LifeUpgrade-ComboLow3:ComboMid3:ComboHigh3-Bundle1:Bundle2:Bundle3";
            size_t clean_len = strlen(clean_store_str);
            uint32_t default_str_addr = so_base + 0x0092C284;
            if (default_str_addr + clean_len + 1 <= img->mem_size) {
                memcpy(img->mem + default_str_addr, clean_store_str, clean_len + 1);
            }

            // 1. Install strip_coins helper at 0x00040000 to sanitize sStoreList in-place
            // in aiStoreAndInv::onInit before it is split into tStoreItemList and generated into 3D meshes.
            static const uint32_t strip_coins_code[] = {
                0xE92D500F, // push {r0-r3, ip, lr}
                0xE3500000, // cmp r0, #0
                0x0A000018, // beq exit_pop (0x70)
                0xE1A01000, // mov r1, r0          ; src = s
                0xE1A02000, // mov r2, r0          ; dst = s
                // loop:
                0xE5D13000, // ldrb r3, [r1]        ; r3 = *src
                0xE3530000, // cmp r3, #0
                0x0A000012, // beq null_term (0x6c)
                0xE591C000, // ldr ip, [r1]
                0xE3063F43, // movw r3, #0x6f43
                0xE3463E69, // movt r3, #0x6e69     ; r3 = 0x6E696F43 ("Coin")
                0xE15C0003, // cmp ip, r3
                0x1A00000A, // bne copy_byte (0x60)
                0xE1D1C0B4, // ldrh ip, [r1, #4]
                0xE3033A73, // movw r3, #0x3a73     ; r3 = 0x3A73 ("s:")
                0xE15C0003, // cmp ip, r3
                0x1A000006, // bne copy_byte (0x60)
                0xE2811006, // add r1, r1, #6       ; skip "Coins:"
                0xE3003100, // movw r3, #0x0100
                0xE3403004, // movt r3, #0x0004     ; r3 = 0x00040100
                0xE593C000, // ldr ip, [r3]
                0xE28CC001, // add ip, ip, #1
                0xE583C000, // str ip, [r3]         ; strip_match_count++
                0xEAFFFFEC, // b loop (0x14)
                // copy_byte:
                0xE4D13001, // ldrb r3, [r1], #1
                0xE4C23001, // strb r3, [r2], #1
                0xEAFFFFE9, // b loop (0x14)
                // null_term:
                0xE5C23000, // strb r3, [r2]        ; *dst = '\0'
                // exit_pop:
                0xE8BD500F, // pop {r0-r3, ip, lr}
                0xE5CD82D0, // strb r8, [sp, #0x2d0] ; execute replaced instruction
                0xE12FFF1E  // bx lr
            };
            uint32_t strip_helper_addr = 0x00040000u;
            if (strip_helper_addr + 0x200 <= img->mem_size) {
                memset(img->mem + strip_helper_addr, 0, 0x200);
                memcpy(img->mem + strip_helper_addr, strip_coins_code, sizeof(strip_coins_code));
            }

            // Hook aiStoreAndInv::onInit at +0x578 (0x002D962C): replace `strb r8, [sp, #0x2d0]` with `bl strip_coins`
            uint32_t sym_onInit = elf32_lookup_symbol(img, "_ZN13aiStoreAndInv6onInitEiPKN4S3DX10AIVariableEPS1_");
            auto encode_bl = [](uint32_t from_pc, uint32_t to_pc) -> uint32_t {
                int32_t disp = (int32_t)to_pc - (int32_t)(from_pc + 8);
                uint32_t imm24 = ((uint32_t)disp >> 2) & 0x00FFFFFFu;
                return 0xEB000000u | imm24;
            };
            auto encode_beq = [](uint32_t from_pc, uint32_t to_pc) -> uint32_t {
                int32_t disp = (int32_t)to_pc - (int32_t)(from_pc + 8);
                uint32_t imm24 = ((uint32_t)disp >> 2) & 0x00FFFFFFu;
                return 0x0A000000u | imm24;
            };

            if (sym_onInit && sym_onInit + 0x580 <= img->mem_size) {
                uint32_t on_init_patch_pc = sym_onInit + 0x578;
                *(uint32_t*)(img->mem + on_init_patch_pc) = encode_bl(on_init_patch_pc, strip_helper_addr);
                printf("[Soft-Mods] Clean Shop: Hooked aiStoreAndInv::onInit to sanitize sStoreList (patch_pc=0x%08X).\n", on_init_patch_pc);
            }

            // 2. Install secondary filter_coins helper at 0x00040200 for fnInitCategoryTable
            static const uint32_t filter_coins_code[] = {
                0xE35A0000, // cmp sl, #0
                0x012FFF1E, // bxeq lr
                0xE59AC000, // ldr ip, [sl]
                0xE3063F43, // movw r3, #0x6f43
                0xE3463E69, // movt r3, #0x6e69  ; r3 = 0x6E696F43 ("Coin")
                0xE15C0003, // cmp ip, r3
                0x112FFF1E, // bxne lr
                0xE1DAC0B4, // ldrh ip, [sl, #4]
                0xE3033A73, // movw r3, #0x3a73  ; r3 = 0x3A73 ("s:")
                0xE15C0003, // cmp ip, r3
                0x112FFF1E, // bxne lr
                0xE28AA006, // add sl, sl, #6
                0xE3003300, // movw r3, #0x0300
                0xE3403004, // movt r3, #0x0004  ; r3 = 0x00040300
                0xE593C000, // ldr ip, [r3]
                0xE28CC001, // add ip, ip, #1
                0xE583C000, // str ip, [r3]      ; match_count++
                0xE12FFF1E  // bx lr
            };
            uint32_t filter_helper_addr = 0x00040200u;
            if (filter_helper_addr + 0x200 <= img->mem_size) {
                memset(img->mem + filter_helper_addr, 0, 0x200);
                memcpy(img->mem + filter_helper_addr, filter_coins_code, sizeof(filter_coins_code));
            }

            // Call sites for Category 0 (Weapons), 1 (Supplies), 2 (Combos), 3 (Bundles)
            struct CatPatch {
                uint32_t call_off;
                uint32_t skip_off;
            } cat_patches[4] = {
                { 0x78C, 0x894 }, // Cat 0 (Weapons)
                { 0x8BC, 0x9C4 }, // Cat 1 (Supplies)
                { 0x9EC, 0xAF4 }, // Cat 2 (Combos)
                { 0xB20, 0x098 }  // Cat 3 (Bundles)
            };

            for (int c = 0; c < 4; ++c) {
                uint32_t call_pc = sym_fnInitCategoryTable + cat_patches[c].call_off;
                uint32_t skip_pc = sym_fnInitCategoryTable + cat_patches[c].skip_off;
                uint32_t *p = (uint32_t*)(img->mem + call_pc);
                p[0] = encode_bl(call_pc, filter_helper_addr); // bl filter_coins
                p[1] = 0xE35A0000;                            // cmp sl, #0
                p[2] = 0x15DA3000;                            // ldrbne r3, [sl]
                p[3] = 0x03A03000;                            // moveq r3, #0
                p[4] = 0xE3530000;                            // cmp r3, #0
                p[5] = encode_beq(call_pc + 0x14, skip_pc);   // beq <skip_target>
                p[6] = 0xE1A00000;                            // nop
                p[7] = 0xE1A00000;                            // nop
            }
            printf("[Soft-Mods] Clean Shop: Removed Coins item from all 4 store category tables (so_base=0x%08X).\n", so_base);

            // 3. Set default shop item to Item 0 instead of Item 1 across all category tabs.
            // Originally, Item 0 was "Coins", so fnResetShopMenu hardcoded default selection to Item 1
            // (e.g. vsub.f32 sX, sX, s16 where s16=1.0f, and vstrgt s16, [sp, #0x84]).
            // With Clean Shop enabled, Item 0 is the first authentic purchasable item (Desert Wind, Health Potion, etc.).
            // In fnResetShopMenu:
            // We zero the initial subtraction (vsub.f32 sX, sX, sX -> 0.0f) across Weapons, Potions, and Combos,
            // and NOP out the branch that overwrites with 1.0f, ensuring Item 0 is cleanly selected by default.

            // In fnResetShopMenu:
            // The game originally computes defaultIndex = (itemCount - 1.0f) using vsub.f32, then if (itemCount - 1 > 0) sets it to 1.0f.
            // To make Item 0 the default, we zero the initial subtraction (vsub.f32 sX, sX, sX -> 0.0f)
            // and NOP out the branch that overwrites with 1.0f.
            struct ResetInitPatch {
                uint32_t offset;
                uint32_t opcode;
            } reset_init_patches[] = {
                { 0x002DD1F4, 0xEE377A47 }, // vsub.f32 s14, s14, s14 (was s14, s14, s16) -> 0.0f (Weapons)
                { 0x002DD51C, 0xEE777AE7 }, // vsub.f32 s15, s15, s15 (was s15, s15, s16) -> 0.0f (Potions)
                { 0x002DD7F4, 0xEE777AE7 }, // vsub.f32 s15, s15, s15 (was s15, s15, s16) -> 0.0f (Combos)
            };
            for (size_t i = 0; i < sizeof(reset_init_patches)/sizeof(reset_init_patches[0]); ++i) {
                uint32_t patch_vsub = so_base + reset_init_patches[i].offset;
                if (patch_vsub + 4 <= img->mem_size) {
                    *(uint32_t*)(img->mem + patch_vsub) = reset_init_patches[i].opcode;
                }
            }

            static const uint32_t reset_menu_offsets[] = { 0x002DD234, 0x002DD558, 0x002DD834 };
            for (size_t i = 0; i < sizeof(reset_menu_offsets)/sizeof(reset_menu_offsets[0]); ++i) {
                uint32_t patch_reset = so_base + reset_menu_offsets[i];
                if (patch_reset + 4 <= img->mem_size) {
                    *(uint32_t*)(img->mem + patch_reset) = 0xE1A00000; // nop (preserves 0.0f default index)
                }
            }
            printf("[Soft-Mods] Clean Shop: Configured Item 0 as default selection across all shop tabs.\n");
        }

        printf("[Soft-Mods] Clean Shop code patches applied successfully.\n");
    }

    if (s_soft_mods_cfg.clean_level_complete) {
        // Neutralize Facebook wall post and social API callbacks in MainAI and aiPlugin with ARM bx lr (0xE12FFF1E)
        static const char *fb_symbols[] = {
            "_ZN6MainAI12onFBWallPostEiPKN4S3DX10AIVariableEPS1_",
            "_ZN8aiPlugin12onFBWallPostEiPKN4S3DX10AIVariableEPS1_",
            "_ZN6MainAI17onInviteFBFriendsEiPKN4S3DX10AIVariableEPS1_",
            "_ZN8aiPlugin10onFBInviteEiPKN4S3DX10AIVariableEPS1_",
            "_ZN6MainAI15onConnectedToFBEiPKN4S3DX10AIVariableEPS1_",
            "_ZN8aiPlugin11onFBConnectEiPKN4S3DX10AIVariableEPS1_",
            "_ZN8aiPlugin12onFBAddScoreEiPKN4S3DX10AIVariableEPS1_",
            "_ZN8aiPlugin14onFBScoresListEiPKN4S3DX10AIVariableEPS1_",
            "_ZN6MainAI13onShowFriendsEiPKN4S3DX10AIVariableEPS1_",
            "_ZN14aiLeaderboards13onShowFriendsEiPKN4S3DX10AIVariableEPS1_",
            "_ZN14aiLeaderboards13onHideFriendsEiPKN4S3DX10AIVariableEPS1_"
        };

        for (size_t i = 0; i < sizeof(fb_symbols) / sizeof(fb_symbols[0]); ++i) {
            uint32_t sym = elf32_lookup_symbol(img, fb_symbols[i]);
            if (sym && sym + 4 <= img->mem_size) {
                *(uint32_t*)(img->mem + sym) = 0xE12FFF1E;
            }
        }

        printf("[Soft-Mods] Clean Level Complete code patches applied successfully.\n");
    }
}

// ---------------------------------------------------------------------------
// Soft-Mod: Clean Main Menu
// Hides mobile-specific freemium buttons ("Get More Coins" & "Get More Games")
// from the main menu, restoring an authentic PC title screen presentation.
// ---------------------------------------------------------------------------

static uint32_t s_cached_hud_mm = 0;
static uint32_t s_cached_cnt_mm = 0;
static uint32_t s_cached_tags_mm = 0;
static uint32_t s_cached_elems_addr_mm = 0;
static uint32_t s_cached_target_elems_mm[32];
static uint32_t s_cached_target_count_mm = 0;

static void mod_clean_main_menu_apply(elf32_image_t *img, uint32_t p0_hud) {
    if (!p0_hud || !img || !img->mem) return;

    // ShiVa3D HUDTree layout:
    // +0x10: TagEntry* tags_array (each entry: uint32_t unk, const char *name)
    // +0x14: uint32_t element_count
    // +0x1C: HUDElement** elements_array
    uint32_t cnt = *(uint32_t*)(img->mem + p0_hud + 0x14);
    uint32_t tags = *(uint32_t*)(img->mem + p0_hud + 0x10);
    uint32_t elems = *(uint32_t*)(img->mem + p0_hud + 0x1c);

    if (!tags || !elems || cnt == 0 || cnt > 2000) return;

    // Rebuild cache when HUD tree structure or address changes
    if (p0_hud != s_cached_hud_mm || cnt != s_cached_cnt_mm || tags != s_cached_tags_mm || elems != s_cached_elems_addr_mm) {
        s_cached_hud_mm = p0_hud;
        s_cached_cnt_mm = cnt;
        s_cached_tags_mm = tags;
        s_cached_elems_addr_mm = elems;
        s_cached_target_count_mm = 0;

        for (uint32_t i = 0; i < cnt && s_cached_target_count_mm < 32; ++i) {
            uint32_t tag_str_ptr = *(uint32_t*)(img->mem + tags + i * 8 + 4);
            if (tag_str_ptr && tag_str_ptr < img->mem_size) {
                const char *tag = (const char*)(img->mem + tag_str_ptr);
                if (strstr(tag, "GetMoreCoins") || strstr(tag, "GetMoreGames") || strstr(tag, "GiftThisApp")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 8 < img->mem_size) {
                        s_cached_target_elems_mm[s_cached_target_count_mm++] = elem;
                    }
                }
            }
        }
    }

    // Clear bit 0 (visible) and bit 1 (active/clickable) on each cached target element
    for (uint32_t i = 0; i < s_cached_target_count_mm; ++i) {
        uint32_t elem = s_cached_target_elems_mm[i];
        if (elem && elem + 8 < img->mem_size) {
            *(uint32_t*)(img->mem + elem + 4) &= ~3;
        }
    }
}

// ---------------------------------------------------------------------------
// Soft-Mod: Clean Shop
// Hides the "+" coin button in coincounter_container and disables its interaction.
// Also neutralizes any residual freemium shop overlay canvases.
// ---------------------------------------------------------------------------

static uint32_t s_cached_hud_shop = 0;
static uint32_t s_cached_cnt_shop = 0;
static uint32_t s_cached_tags_shop = 0;
static uint32_t s_cached_elems_addr_shop = 0;
static uint32_t s_cached_target_elems_shop[32];
static uint32_t s_cached_target_count_shop = 0;
static uint32_t s_cached_nonclick_elems_shop[32];
static uint32_t s_cached_nonclick_count_shop = 0;

static void mod_clean_shop_apply(elf32_image_t *img, uint32_t p0_hud) {
    if (!p0_hud || !img || !img->mem) return;

    uint32_t cnt = *(uint32_t*)(img->mem + p0_hud + 0x14);
    uint32_t tags = *(uint32_t*)(img->mem + p0_hud + 0x10);
    uint32_t elems = *(uint32_t*)(img->mem + p0_hud + 0x1c);

    if (!tags || !elems || cnt == 0 || cnt > 2000) return;

    static uint32_t s_last_strip_matches = 0xFFFFFFFF;
    static uint32_t s_last_cat_matches = 0xFFFFFFFF;
    if (0x00040304 <= img->mem_size) {
        uint32_t strip_m = *(uint32_t*)(img->mem + 0x00040100);
        uint32_t cat_m = *(uint32_t*)(img->mem + 0x00040300);
        if (strip_m != s_last_strip_matches || cat_m != s_last_cat_matches) {
            s_last_strip_matches = strip_m;
            s_last_cat_matches = cat_m;
            printf("[Soft-Mods] Clean Shop: strip_coins in onInit=%u matches, filter_coins in fnInitCategoryTable=%u matches.\n",
                   strip_m, cat_m);
        }
    }

    if (p0_hud != s_cached_hud_shop || cnt != s_cached_cnt_shop || tags != s_cached_tags_shop || elems != s_cached_elems_addr_shop) {
        s_cached_hud_shop = p0_hud;
        s_cached_cnt_shop = cnt;
        s_cached_tags_shop = tags;
        s_cached_elems_addr_shop = elems;
        s_cached_target_count_shop = 0;
        s_cached_nonclick_count_shop = 0;

        for (uint32_t i = 0; i < cnt && s_cached_target_count_shop < 32; ++i) {
            uint32_t tag_str_ptr = *(uint32_t*)(img->mem + tags + i * 8 + 4);
            if (tag_str_ptr && tag_str_ptr < img->mem_size) {
                const char *tag = (const char*)(img->mem + tag_str_ptr);
                if (strstr(tag, "FreeCoins") ||
                    strstr(tag, "freecoin_container") ||
                    strstr(tag, "GetBestCoins") ||
                    strstr(tag, "CoinImage3") ||
                    strstr(tag, "Shop.Canvas") ||
                    strstr(tag, "Shop.InputBlocker") ||
                    strstr(tag, "Shop.DealFreeCoins") ||
                    strstr(tag, "POPUPHUD.PopUpConfirmation")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 8 < img->mem_size) {
                        s_cached_target_elems_shop[s_cached_target_count_shop++] = elem;
                    }
                } else if (strstr(tag, "PlayerCoins")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 8 < img->mem_size && s_cached_nonclick_count_shop < 32) {
                        s_cached_nonclick_elems_shop[s_cached_nonclick_count_shop++] = elem;
                    }
                }
            }
        }
    }

    // Clear bit 0 (visible) and bit 1 (active/clickable) on each target element to hide them completely
    for (uint32_t i = 0; i < s_cached_target_count_shop; ++i) {
        uint32_t elem = s_cached_target_elems_shop[i];
        if (elem && elem + 8 < img->mem_size) {
            *(uint32_t*)(img->mem + elem + 4) &= ~3;
        }
    }

    // Clear bit 1 (active/clickable) on elements that remain visible but non-interactive
    for (uint32_t i = 0; i < s_cached_nonclick_count_shop; ++i) {
        uint32_t elem = s_cached_nonclick_elems_shop[i];
        if (elem && elem + 8 < img->mem_size) {
            *(uint32_t*)(img->mem + elem + 4) &= ~2;
        }
    }
}

// ---------------------------------------------------------------------------
// Soft-Mod: Clean Level Complete
// Hides Facebook social share buttons ("SHARE / GET 25 FREE COINS", Facebook logo,
// and background card) from the level completion screens and other social widgets.
// ---------------------------------------------------------------------------

static uint32_t s_cached_hud_lc = 0;
static uint32_t s_cached_cnt_lc = 0;
static uint32_t s_cached_tags_lc = 0;
static uint32_t s_cached_elems_addr_lc = 0;
static uint32_t s_cached_target_elems_lc[128];
static uint32_t s_cached_target_count_lc = 0;
static uint32_t s_cached_achievements_elem = 0;
static uint32_t s_cached_achievements_text_elem = 0;
static uint32_t s_cached_credits_elem = 0;
static uint32_t s_cached_credits_text_elem = 0;

static void mod_clean_level_complete_apply(elf32_image_t *img, uint32_t p0_hud) {
    if (!p0_hud || !img || !img->mem) return;

    uint32_t cnt = *(uint32_t*)(img->mem + p0_hud + 0x14);
    uint32_t tags = *(uint32_t*)(img->mem + p0_hud + 0x10);
    uint32_t elems = *(uint32_t*)(img->mem + p0_hud + 0x1c);

    if (!tags || !elems || cnt == 0 || cnt > 2000) return;

    if (p0_hud != s_cached_hud_lc || cnt != s_cached_cnt_lc || tags != s_cached_tags_lc || elems != s_cached_elems_addr_lc) {
        if (cnt < 50) return; // Ignore empty / secondary player HUDs

        s_cached_hud_lc = p0_hud;
        s_cached_cnt_lc = cnt;
        s_cached_tags_lc = tags;
        s_cached_elems_addr_lc = elems;
        s_cached_target_count_lc = 0;
        s_cached_achievements_elem = 0;
        s_cached_achievements_text_elem = 0;
        s_cached_credits_elem = 0;
        s_cached_credits_text_elem = 0;

        for (uint32_t i = 0; i < cnt && s_cached_target_count_lc < 128; ++i) {
            uint32_t tag_str_ptr = *(uint32_t*)(img->mem + tags + i * 8 + 4);
            if (tag_str_ptr && tag_str_ptr < img->mem_size) {
                const char *tag = (const char*)(img->mem + tag_str_ptr);
                if (strstr(tag, "Facebook") ||
                    strstr(tag, "facebook") ||
                    strstr(tag, "fb_background") ||
                    strstr(tag, "LCMainMenuFB") ||
                    strstr(tag, "freecoin_container") ||
                    strstr(tag, "FreeCoins") ||
                    strstr(tag, "Friends") ||
                    strstr(tag, "friends") ||
                    strstr(tag, "ShareButton") ||
                    strstr(tag, "ShareAchievement") ||
                    strstr(tag, "GameCenter")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 8 < img->mem_size) {
                        s_cached_target_elems_lc[s_cached_target_count_lc++] = elem;
                    }
                } else if (strstr(tag, "AchievementsText")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 0x18 < img->mem_size) {
                        s_cached_achievements_text_elem = elem;
                    }
                } else if (strstr(tag, "Achievements")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 0x18 < img->mem_size) {
                        float y = *(float*)(img->mem + elem + 0x0C);
                        // Extras menu Achievements button has initial y near 0.1700f
                        if (y > 0.10f && y < 0.25f) {
                            s_cached_achievements_elem = elem;
                        }
                    }
                } else if (strstr(tag, "CreditsButton")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 0x18 < img->mem_size) {
                        s_cached_credits_elem = elem;
                    }
                } else if (strstr(tag, "CreditsText")) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 0x18 < img->mem_size) {
                        s_cached_credits_text_elem = elem;
                    }
                }
            }
        }
    }

    // Clear bit 0 (visible) and bit 1 (active/clickable) on each target element to hide them completely
    for (uint32_t i = 0; i < s_cached_target_count_lc; ++i) {
        uint32_t elem = s_cached_target_elems_lc[i];
        if (elem && elem + 8 < img->mem_size) {
            *(uint32_t*)(img->mem + elem + 4) &= ~3;
        }
    }

    // Shift Achievements and Credits buttons up one slot in Extras menu
    if (s_cached_achievements_elem && s_cached_achievements_elem + 0x10 < img->mem_size) {
        *(float*)(img->mem + s_cached_achievements_elem + 0x0C) = 0.4920f;
    }
    if (s_cached_achievements_text_elem && s_cached_achievements_text_elem + 0x10 < img->mem_size) {
        *(float*)(img->mem + s_cached_achievements_text_elem + 0x0C) = 0.4906f;
    }
    if (s_cached_credits_elem && s_cached_credits_elem + 0x10 < img->mem_size) {
        *(float*)(img->mem + s_cached_credits_elem + 0x0C) = 0.1700f;
    }
    if (s_cached_credits_text_elem && s_cached_credits_text_elem + 0x10 < img->mem_size) {
        *(float*)(img->mem + s_cached_credits_text_elem + 0x0C) = 0.1666f;
    }
}

// ---------------------------------------------------------------------------
// Soft-Mod: Clean Touch Controls (Potion flask & Stealth Walk silhouette)
// Hides floating mobile touch UI buttons during gameplay for an authentic PC display.
// Keyboard (E / Q) and Gamepad (R1 / L1) controls remain fully functional.
// ---------------------------------------------------------------------------

static uint32_t s_cached_hud_touch = 0;
static uint32_t s_cached_cnt_touch = 0;
static uint32_t s_cached_tags_touch = 0;
static uint32_t s_cached_elems_addr_touch = 0;
static uint32_t s_cached_target_elems_touch[32];
static uint32_t s_cached_target_count_touch = 0;

static void mod_clean_touch_hud_apply(elf32_image_t *img, uint32_t p0_hud) {
    if (!p0_hud || !img || !img->mem) return;

    uint32_t cnt = *(uint32_t*)(img->mem + p0_hud + 0x14);
    uint32_t tags = *(uint32_t*)(img->mem + p0_hud + 0x10);
    uint32_t elems = *(uint32_t*)(img->mem + p0_hud + 0x1c);

    if (!tags || !elems || cnt == 0 || cnt > 2000) return;

    // Rescan if HUD address/count changed, OR if we haven't found the controls yet
    // (since HUDGesture is instantiated as a template with "Controls." prefix after level start).
    // The "not found yet" retry is throttled so menus don't pay for a full tag scan every frame.
    static uint32_t s_touch_retry_frames = 0;
    bool structure_changed = (p0_hud != s_cached_hud_touch || cnt != s_cached_cnt_touch ||
                              tags != s_cached_tags_touch || elems != s_cached_elems_addr_touch);
    bool retry_missing = (s_cached_target_count_touch == 0 && (++s_touch_retry_frames % 30) == 0);
    if (structure_changed || retry_missing) {

        s_cached_hud_touch = p0_hud;
        s_cached_cnt_touch = cnt;
        s_cached_tags_touch = tags;
        s_cached_elems_addr_touch = elems;
        s_cached_target_count_touch = 0;

        for (uint32_t i = 0; i < cnt && s_cached_target_count_touch < 32; ++i) {
            uint32_t tag_str_ptr = *(uint32_t*)(img->mem + tags + i * 8 + 4);
            if (tag_str_ptr && tag_str_ptr < img->mem_size) {
                const char *tag = (const char*)(img->mem + tag_str_ptr);
                if (strstr(tag, "Controls.Potion") ||
                    strstr(tag, "Controls.RS_RT") ||
                    strstr(tag, "Controls.Shield") ||
                    strstr(tag, "Controls.WalkButtonBlink") ||
                    strstr(tag, "Controls.Joystick") ||
                    strstr(tag, "Controls.StickThumb") ||
                    strstr(tag, "Controls.ButtonA") ||
                    strstr(tag, "Controls.ButtonB") ||
                    strstr(tag, "RS_RT") ||
                    (cnt < 50 && (strstr(tag, "Potion") || strstr(tag, "potion") || strstr(tag, "Shield") || strstr(tag, "shield")))) {
                    uint32_t elem = *(uint32_t*)(img->mem + elems + i * 4);
                    if (elem && elem + 8 < img->mem_size) {
                        s_cached_target_elems_touch[s_cached_target_count_touch++] = elem;
                    }
                }
            }
        }
        if (s_cached_target_count_touch > 0) {
            printf("[Soft-Mods] Clean Touch Controls: masked %u touch UI elements in HUD 0x%08X (cnt=%u).\n",
                   s_cached_target_count_touch, p0_hud, cnt);
        }
    }

    // Clear bit 0 (visible) and bit 1 (active/clickable) on each cached target element
    for (uint32_t i = 0; i < s_cached_target_count_touch; ++i) {
        uint32_t elem = s_cached_target_elems_touch[i];
        if (elem && elem + 8 < img->mem_size) {
            *(uint32_t*)(img->mem + elem + 4) &= ~3;
        }
    }
}

// ---------------------------------------------------------------------------
// Gameplay State Detection & Input Gating Helper
// Determines whether the player is in active gameplay (MainAI state == stInGame [2]
// and unpaused) to allow mouse buttons to function strictly as UI pointer clicks
// in all menus (Main Menu, World Map, Store, Extras, Pause Menu, Dialogs)
// and combat hotkeys during active gameplay.
// ---------------------------------------------------------------------------

static bool s_is_in_gameplay = false;

bool soft_mods_is_in_gameplay(void) {
    return s_is_in_gameplay;
}

// Walks the engine pointer chain; any broken link means "not in gameplay" rather than keeping stale state
static void soft_mods_update_gameplay_state_impl(elf32_image_t *img, uint32_t p0_scene);

void soft_mods_update_gameplay_state(elf32_image_t *img, uint32_t p0, uint32_t p0_scene) {
    (void)p0;
    s_is_in_gameplay = false;
    if (!img || !img->mem) return;
    soft_mods_update_gameplay_state_impl(img, p0_scene);
}

static void soft_mods_update_gameplay_state_impl(elf32_image_t *img, uint32_t p0_scene) {
    // Traverse the ShiVa ClientEngine -> Game -> AIController -> MainAI instance chain
    // (exact chain verified by PoPSnF_LiveSplit.asl)
    uint32_t ce_ptr = 0x020011E0u;
    if (ce_ptr + 0x1C > img->mem_size) return;

    uint32_t game_ptr = *(uint32_t*)(img->mem + ce_ptr + 0x18);
    if (!game_ptr || game_ptr + 0x54 > img->mem_size) return;

    uint32_t ai_ctrl_table = *(uint32_t*)(img->mem + game_ptr + 0x50);
    if (!ai_ctrl_table || ai_ctrl_table + 4 > img->mem_size) return;

    uint32_t ai_ctrl = *(uint32_t*)(img->mem + ai_ctrl_table);
    if (!ai_ctrl || ai_ctrl + 0x1C > img->mem_size) return;

    uint32_t ai_arr = *(uint32_t*)(img->mem + ai_ctrl + 0x18);
    if (!ai_arr || ai_arr + 4 > img->mem_size) return;

    uint32_t ai_list = *(uint32_t*)(img->mem + ai_arr);
    if (!ai_list || ai_list + 4 > img->mem_size) return;

    uint32_t main_ai_inst = *(uint32_t*)(img->mem + ai_list);
    if (!main_ai_inst || main_ai_inst + 0x18 > img->mem_size) return;

    uint32_t var_array = *(uint32_t*)(img->mem + main_ai_inst + 0x14);
    if (!var_array || var_array + 0x290 > img->mem_size) return;

    // 1. Check bInMainMenu boolean (+0x0E8)
    uint8_t in_main_menu = *(uint8_t*)(img->mem + var_array + 0x0E8);

    // 2. Check sCurrentState string pointer (+0x228 + 8 = +0x230)
    uint32_t state_str_ptr = *(uint32_t*)(img->mem + var_array + 0x230);
    const char *state_str = (state_str_ptr && state_str_ptr < img->mem_size)
        ? (const char*)(img->mem + state_str_ptr) : "";

    // 3. Check sCurrGameState string pointer (+0x27C + 8 = +0x284)
    uint32_t game_state_str_ptr = *(uint32_t*)(img->mem + var_array + 0x284);
    const char *game_state_str = (game_state_str_ptr && game_state_str_ptr < img->mem_size)
        ? (const char*)(img->mem + game_state_str_ptr) : "";

    // 4. Check scene pause flags (bit 4 of scene flags)
    bool is_paused = false;
    if (p0_scene && p0_scene + 0x4B4 <= img->mem_size) {
        uint32_t scene_flags = *(uint32_t*)(img->mem + p0_scene + 0x4B0);
        if ((scene_flags & 0x10) != 0) {
            is_paused = true;
        }
    }

    // Active gameplay is strictly when:
    // - Not in Main Menu (in_main_menu == 0)
    // - Current state is in gameplay (matches "stInGame", "InGame", or "In Game")
    // - Scene is not paused (!is_paused)
    bool in_gameplay = (!in_main_menu &&
                        (strcmp(state_str, "stInGame") == 0 ||
                         strcmp(state_str, "InGame") == 0 ||
                         strcmp(state_str, "In Game") == 0 ||
                         strcmp(game_state_str, "InGame") == 0 ||
                         strcmp(game_state_str, "In Game") == 0) &&
                        !is_paused);
    if (in_gameplay != s_is_in_gameplay) {
        s_is_in_gameplay = in_gameplay;
    }

    static char s_last_state[32] = {0};
    static char s_last_game_state[32] = {0};
    static uint8_t s_last_mm = 0xFF;
    static bool s_last_paused = false;
    if (strcmp(state_str, s_last_state) != 0 ||
        strcmp(game_state_str, s_last_game_state) != 0 ||
        in_main_menu != s_last_mm ||
        is_paused != s_last_paused) {
        snprintf(s_last_state, sizeof(s_last_state), "%s", state_str);
        snprintf(s_last_game_state, sizeof(s_last_game_state), "%s", game_state_str);
        s_last_mm = in_main_menu;
        s_last_paused = is_paused;
        printf("[Input Gating] State: '%s', GameState: '%s' (in_main_menu=%u)%s -> mouse action keys %s\n",
               state_str[0] ? state_str : "(null)",
               game_state_str[0] ? game_state_str : "(null)",
               in_main_menu, is_paused ? " [PAUSED]" : "",
               in_gameplay ? "ENABLED (combat)" : "DISABLED (pure UI clicks)");
    }
}

void soft_mods_apply_hud(elf32_image_t *img, uint32_t p0, uint32_t p0_hud, uint32_t p0_scene) {
    // 1. Evaluate gameplay state using authoritative MainAI state machine
    soft_mods_update_gameplay_state(img, p0, p0_scene);

    if (!p0_hud || !img || !img->mem) return;

    // 2. Dispatch HUD soft-mods
    if (s_soft_mods_cfg.clean_main_menu) {
        mod_clean_main_menu_apply(img, p0_hud);
    }
    if (s_soft_mods_cfg.clean_shop) {
        mod_clean_shop_apply(img, p0_hud);
    }
    if (s_soft_mods_cfg.clean_level_complete) {
        mod_clean_level_complete_apply(img, p0_hud);
    }
    if (s_soft_mods_cfg.clean_touch_hud) {
        mod_clean_touch_hud_apply(img, p0_hud);
    }
}

void soft_mods_patch_compressed_texture(int level, int w, int h, int imageSize, void *data) {
    if (!data) return;

    uint8_t *d = (uint8_t*)data;

    // Patch Title Screen Logo to "Prince of Persia: Shadow and the Flame - Reignited"
    if (w == 512 && h == 256 && level == 0 && imageSize >= 131072) {
        static const uint8_t logo_sig[8] = { 0, 5, 0, 2, 0, 0, 0, 0 };
        if (memcmp(d + 0x10000, logo_sig, 8) == 0) {
            memcpy(d, s_reignited_logo_dxt5, 131072);
            printf("[Reignited] Title screen logo texture patched in memory.\n");
            return;
        }
    }

    if (!s_soft_mods_cfg.clean_shop) return;

    // Level 0: 1024x1024 (256x256 blocks, 16 bytes per block)
    if (w == 1024 && h == 1024 && level == 0 && imageSize >= 1048576) {
        static const uint8_t sig1[16] = { 255, 255, 0, 0, 0, 0, 0, 0, 151, 189, 1, 106, 139, 114, 92, 214 };
        static const uint8_t sig2[16] = { 255, 255, 0, 0, 0, 0, 0, 0, 46, 246, 236, 213, 87, 170, 0, 0 };

        if (memcmp(d + 0x6b040, sig1, 16) == 0 && memcmp(d + 0xb3c50, sig2, 16) == 0) {
            // Patch bar1 (standard coin counter): replace plus button with dark red velvet
            for (int by = 107; by <= 116; ++by) {
                for (int bx = 4; bx <= 13; ++bx) {
                    int src_bx = bx + 12;
                    int src_off = (by * 256 + src_bx) * 16;
                    int dst_off = (by * 256 + bx) * 16;
                    memcpy(d + dst_off, d + src_off, 16);
                }
            }
            // Patch bar2 (highlighted coin counter): replace plus button with dark red velvet
            for (int by = 179; by <= 188; ++by) {
                for (int bx = 197; bx <= 206; ++bx) {
                    int src_bx = bx + 12;
                    int src_off = (by * 256 + src_bx) * 16;
                    int dst_off = (by * 256 + bx) * 16;
                    memcpy(d + dst_off, d + src_off, 16);
                }
            }
            printf("[Soft-Mods] UI_mainmenu_store.dds Clean Shop texture patch applied.\n");
        }
    }
}
