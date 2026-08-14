/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching internal .so functions or bridging them to native for better compatibility.
 * @details Refer to technical documentation in Docs/patch_comments.md for full details on
 *          DRM bypasses, HUDStyle overrides, and disabled diagnostic hooks.
 */

#include <string.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/glutil.h"

extern so_module so_mod;

void ret_void(void) {
    return;
}

extern int ret1(void);

/**
 * @brief Application::GetSavedOption hook to force fixed HUD style.
 * @details On Android, if SavegameManager::hasOption("HUDStyle") is 0, the function returns garbage from r0.
 *          Although real hardware tests (log_093.txt) verified that the key exists and equals 0, this defensive
 *          hook forces HUDStyle=0 for the Vita's 960x544 screen.
 */
typedef int (*hasOption_t)(void *savegame_mgr, const char *key);
static hasOption_t s_hasOption;
static so_hook s_hook_get_saved_option;

static int hook_GetSavedOption(void *this_, const char *key) {
    if (key && strcmp(key, "HUDStyle") == 0) {
        return 0;
    }
    return SO_CONTINUE(int, s_hook_get_saved_option, this_, key);
}

void so_patch(void) {
    /**
     * @brief Bypass Gameloft DRM license validation checks.
     */
    hook_addr((uintptr_t)so_symbol(&so_mod, "ALicenseCheck_ValidateLicense"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck14ValidateServerEb"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck14ValidateNativeEv"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck7LoadRMSEv"), (uintptr_t)&ret1);

    /**
     * @brief Patch "HUDStyle" return value to fixed phone 3-icon style.
     */
    s_hasOption = (hasOption_t)so_symbol(&so_mod, "_ZNK15SavegameManager9hasOptionEPKc");
    s_hook_get_saved_option = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN11Application14GetSavedOptionEPKc"),
        (uintptr_t)&hook_GetSavedOption);
}
