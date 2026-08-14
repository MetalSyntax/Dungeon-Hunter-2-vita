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

extern int ret0(void);
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

typedef struct sp_counted_base {
    void **_vptr;
    int use_count_;
    int weak_count_;
} sp_counted_base;

typedef struct shared_count {
    sp_counted_base *pi_;
} shared_count;

typedef void (*sp_vfn)(sp_counted_base *this_);

static void hook_sp_counted_base_weak_release(sp_counted_base *this_) {
    if (!this_) return;
    if (__atomic_sub_fetch(&this_->weak_count_, 1, __ATOMIC_ACQ_REL) == 0) {
        if (this_->_vptr && this_->_vptr[3]) {
            sp_vfn destroy = (sp_vfn) this_->_vptr[3];
            destroy(this_);
        }
    }
}

static void hook_sp_counted_base_release(sp_counted_base *this_) {
    if (!this_) return;
    if (__atomic_sub_fetch(&this_->use_count_, 1, __ATOMIC_ACQ_REL) == 0) {
        if (this_->_vptr && this_->_vptr[2]) {
            sp_vfn dispose = (sp_vfn) this_->_vptr[2];
            dispose(this_);
        }
        hook_sp_counted_base_weak_release(this_);
    }
}

static shared_count *hook_shared_count_copy_ctor(shared_count *this_, const shared_count *r) {
    sp_counted_base *pi = r ? r->pi_ : NULL;
    this_->pi_ = pi;
    if (pi != NULL) {
        __atomic_add_fetch(&pi->use_count_, 1, __ATOMIC_RELAXED);
    }
    return this_;
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

    /**
     * @brief Disable frustum/bounding box culling for scene nodes and objects.
     * @details Prevents enemies/NPCs/objects from disappearing erratically during gameplay
     *          due to dynamic bounding box / aspect ratio culling mismatches.
     */
    void *sym_is_culled_node = (void *)so_symbol(&so_mod, "_ZNK6glitch5scene13CSceneManager8isCulledEPKNS0_10ISceneNodeE");
    if (sym_is_culled_node) {
        hook_addr((uintptr_t)sym_is_culled_node, (uintptr_t)&ret0);
        l_success("Hooked CSceneManager::isCulled(ISceneNode*) -> ret0");
    }

    void *sym_is_culled_box = (void *)so_symbol(&so_mod, "_ZNK6glitch5scene13CSceneManager8isCulledERKNS_4core8aabbox3dIfEENS0_14E_CULLING_TYPEE");
    if (sym_is_culled_box) {
        hook_addr((uintptr_t)sym_is_culled_box, (uintptr_t)&ret0);
        l_success("Hooked CSceneManager::isCulled(aabbox3d, E_CULLING_TYPE) -> ret0");
    }

    void *sym_test_culling = (void *)so_symbol(&so_mod, "_ZN10ObjectBase23TestCullingBeforeUpdateERK4aabbIfE");
    if (sym_test_culling) {
        hook_addr((uintptr_t)sym_test_culling, (uintptr_t)&ret1);
        l_success("Hooked ObjectBase::TestCullingBeforeUpdate -> ret1");
    }

    /**
     * @brief Replace obsolete ARMv5 'SWP' atomic spinlocks in Boost with ARMv7 atomics.
     * @details The ARM 'SWP' instruction was deprecated in ARMv6 and removed in ARMv7-A (Cortex-A9),
     *          triggering an Undefined Instruction exception (0x30002) when sp_counted_base::release()
     *          executes. These hooks provide clean ARMv7 atomic implementations.
     */
    void *sym_boost_release = (void *)so_symbol(&so_mod, "_ZN5boost6detail15sp_counted_base7releaseEv");
    if (sym_boost_release) {
        hook_addr((uintptr_t)sym_boost_release, (uintptr_t)&hook_sp_counted_base_release);
        l_success("Hooked boost::detail::sp_counted_base::release -> ARMv7 atomic");
    }

    void *sym_boost_weak_release = (void *)so_symbol(&so_mod, "_ZN5boost6detail15sp_counted_base12weak_releaseEv");
    if (sym_boost_weak_release) {
        hook_addr((uintptr_t)sym_boost_weak_release, (uintptr_t)&hook_sp_counted_base_weak_release);
        l_success("Hooked boost::detail::sp_counted_base::weak_release -> ARMv7 atomic");
    }

    void *sym_boost_sc_copy = (void *)so_symbol(&so_mod, "_ZN5boost6detail12shared_countC1ERKS1_");
    if (sym_boost_sc_copy) {
        hook_addr((uintptr_t)sym_boost_sc_copy, (uintptr_t)&hook_shared_count_copy_ctor);
        l_success("Hooked boost::detail::shared_count copy ctor -> ARMv7 atomic");
    }

    void *sym_boost_sc_copy2 = (void *)so_symbol(&so_mod, "_ZN5boost6detail12shared_countC2ERKS1_");
    if (sym_boost_sc_copy2 && sym_boost_sc_copy2 != sym_boost_sc_copy) {
        hook_addr((uintptr_t)sym_boost_sc_copy2, (uintptr_t)&hook_shared_count_copy_ctor);
        l_success("Hooked boost::detail::shared_count C2 copy ctor -> ARMv7 atomic");
    }
}
