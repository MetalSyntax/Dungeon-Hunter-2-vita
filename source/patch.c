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

/**
 * @brief Save-pipeline hooks: prove WHERE a lost save dies.
 * @details Player/level saves are async: SG_Save -> Savegame::saveAll ->
 *          AddJob (in-RAM queue) -> ... -> Savegame::UpdateJobs (one chunk per
 *          call, driven by fire-and-forget worker threads) -> CFileSystem
 *          open("w+b")/write/close. Settings saves are synchronous
 *          (saveSettings does open/write/close inline on the caller thread).
 *          Every stage is infrequent (a few calls per save, not per frame),
 *          so logging each at WARN (Release-visible) costs nothing and turns
 *          the next hardware log into a definitive answer:
 *            saveAll/AddJob logged + fopen "w+b" logged  = writer runs,
 *              look at data/flush bugs instead;
 *            saveAll/AddJob logged + NO fopen "w+b"       = queue never
 *              drained (dead workers, no flush) -> the periodic/exit drain
 *              in main.c covers exactly this;
 *            saveSettings never logged on option change  = the menu never
 *              even tried to persist (UI/script issue, not I/O).
 *          Hook bodies forward to the original via SO_CONTINUE and are
 *          declared int-returning even for void originals: on ARM the caller
 *          of a void function ignores r0, so forwarding the register is
 *          harmless and avoids a separate trampoline per signature.
 */
static so_hook s_hook_save_settings;
static so_hook s_hook_save_all;
static so_hook s_hook_add_job;
static so_hook s_hook_flush_jobs;
static so_hook s_hook_job_start;
static so_hook s_hook_job_start2;

// Set by the AddJob hook, cleared once a drain runs. Lets main.c flush the
// async queue on a timer and at exit even if the engine's worker threads
// never run -- and the log shows whether the flag was ever set.
volatile int g_savejobs_pending = 0;
static int (*s_save_flush_jobs)(const char *name) = NULL;

static int hook_SavegameManager_saveSettings(void *this_) {
    l_warn("[save_io] SavegameManager::saveSettings(this=%p)", this_);
    int r = SO_CONTINUE(int, s_hook_save_settings, this_);
    l_warn("[save_io] SavegameManager::saveSettings done");
    return r;
}

static int hook_Savegame_saveAll(void *this_) {
    l_warn("[save_io] Savegame::saveAll(this=%p) -- queueing async write job(s)", this_);
    return SO_CONTINUE(int, s_hook_save_all, this_);
}

static int hook_Savegame_AddJob(void *job) {
    g_savejobs_pending = 1;
    __sync_synchronize();
    l_warn("[save_io] Savegame::AddJob(job=%p) -- async save queued (pending=1)", job);
    return SO_CONTINUE(int, s_hook_add_job, job);
}

static int hook_Savegame_FlushJobs(const char *name) {
    // Deliberately l_debug (compiled out in Release): FlushJobs also runs on
    // every savefile OPEN (read path), ~700 calls/session -- warning each
    // would be pure storage-spam. Real drains (periodic/exit) are logged by
    // savejobs_drain() itself.
    l_debug("[save_io] Savegame::FlushJobs(%s) -- draining queue...",
            name ? name : "(null/all)");
    int r = SO_CONTINUE(int, s_hook_flush_jobs, name);
    l_debug("[save_io] Savegame::FlushJobs done (ret=%d)", r);
    return r;
}

static int hook_updateJob_Start(void *this_) {
    int r = SO_CONTINUE(int, s_hook_job_start, this_);
    // Start fires ~10K times/session (generic worker pump, not just saves):
    // only failures are Release-worthy. The engine's own "Not Created" goes
    // through _DEBUG_OUT (hidden in Release), so a nonzero return warned here
    // is the only visible signal that a save chunk lost its worker.
    if (r != 0)
        l_warn("[save_io] updateJob_thread::Start(this=%p) FAILED -> %d", this_, r);
    else
        l_debug("[save_io] updateJob_thread::Start(this=%p) -> %d", this_, r);
    return r;
}

static int hook_updateJob_Start2(void *this_) {
    int r = SO_CONTINUE(int, s_hook_job_start2, this_);
    if (r != 0)
        l_warn("[save_io] updateJob_thread::Start2(this=%p) FAILED -> %d", this_, r);
    else
        l_debug("[save_io] updateJob_thread::Start2(this=%p) -> %d", this_, r);
    return r;
}

// Engine's own drain-all (the exact call Application::Quit makes). Called
// from main.c on a timer (only when g_savejobs_pending) and at exit.
void savejobs_drain(void) {
    if (!s_save_flush_jobs) {
        l_warn("[save_io] savejobs_drain: FlushJobs not resolved, cannot drain");
        return;
    }
    l_warn("[save_io] savejobs_drain: flushing pending save jobs...");
    s_save_flush_jobs(NULL);
    g_savejobs_pending = 0;
    __sync_synchronize();
    l_warn("[save_io] savejobs_drain: done");
}

int savejobs_has_pending(void) {
    __sync_synchronize();
    return g_savejobs_pending;
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
    // CULLING_MODE controla cuanto de este bypass se aplica. Es la palanca de
    // rendimiento mas grande que tiene el port, y a la vez la mas riesgosa:
    //
    // Forzar isCulled -> 0 significa "NADA esta nunca fuera de camara", asi que
    // el motor corre onAnimate() (esqueletos, huesos, skinning) de TODO el nivel
    // cada frame, actualiza todos los objetos y encola todos los draws, visibles
    // o no. Eso encaja exacto con lo medido en hardware: CPU-submission de
    // 120-185ms por frame con el GPU libre en 0.18ms, y -- lo decisivo -- que
    // bajar la resolucion de render a 480x272 (25% de los pixeles) no cambiara
    // NADA el FPS de combate (log_003 vs log_004). El costo no esta en pixeles,
    // esta en animar y actualizar un nivel entero por frame.
    //
    //   0 = bypass completo (los 3 hooks). Comportamiento historico y DEFAULT:
    //       es lo que arreglo el bug de enemigos invisibles (commit 853ac40) y
    //       lo unico confirmado bueno visualmente en hardware. No cambiar el
    //       default sin una prueba que demuestre que la alternativa no regresa
    //       ese bug.
    //   1 = se restaura SOLO ObjectBase::TestCullingBeforeUpdate (culling del
    //       lado del UPDATE), y los dos isCulled siguen bypasseados (culling del
    //       lado de la VISIBILIDAD). El bug de invisibilidad era que cosas no se
    //       DIBUJABAN, y esto no toca ese camino -- separa el costo de update del
    //       de render para poder medirlos por separado. Artefacto posible y mas
    //       leve: entidades fuera de camara que se congelan en vez de desaparecer.
    //   2 = culling stock del motor, sin ningun hook. Es el modo mas rapido y el
    //       que con mas probabilidad devuelve el bug de enemigos invisibles.
#ifndef CULLING_MODE
#define CULLING_MODE 0
#endif

#if CULLING_MODE < 2
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
#endif

#if CULLING_MODE < 1
    void *sym_test_culling = (void *)so_symbol(&so_mod, "_ZN10ObjectBase23TestCullingBeforeUpdateERK4aabbIfE");
    if (sym_test_culling) {
        hook_addr((uintptr_t)sym_test_culling, (uintptr_t)&ret1);
        l_success("Hooked ObjectBase::TestCullingBeforeUpdate -> ret1");
    }
#endif
    l_error("[culling] CULLING_MODE=%d (0=bypass total, 1=update-culling activo, 2=culling stock)",
            CULLING_MODE);

    /**
     * @brief Save-pipeline diagnostic hooks (see the hook bodies above).
     * @details Each install is best-effort: a missing symbol only skips that
     *          hook with a warning, never aborts the boot.
     */
    {
        void *sym = (void *)so_symbol(&so_mod, "_ZN15SavegameManager12saveSettingsEv");
        if (sym) {
            s_hook_save_settings = hook_addr((uintptr_t)sym, (uintptr_t)&hook_SavegameManager_saveSettings);
            l_success("Hooked SavegameManager::saveSettings (save_io diag)");
        } else {
            l_warn("save_io diag: SavegameManager::saveSettings not found, settings saves untraced");
        }
    }
    {
        void *sym = (void *)so_symbol(&so_mod, "_ZN8Savegame7saveAllEv");
        if (sym) {
            s_hook_save_all = hook_addr((uintptr_t)sym, (uintptr_t)&hook_Savegame_saveAll);
            l_success("Hooked Savegame::saveAll (save_io diag)");
        } else {
            l_warn("save_io diag: Savegame::saveAll not found, async saves untraced");
        }
    }
    {
        void *sym = (void *)so_symbol(&so_mod, "_ZN8Savegame6AddJobERNS_3JobE");
        if (sym) {
            s_hook_add_job = hook_addr((uintptr_t)sym, (uintptr_t)&hook_Savegame_AddJob);
            l_success("Hooked Savegame::AddJob (save_io diag)");
        } else {
            l_warn("save_io diag: Savegame::AddJob not found, pending-job tracking off");
        }
    }
    {
        void *sym = (void *)so_symbol(&so_mod, "_ZN8Savegame9FlushJobsEPKc");
        if (sym) {
            s_save_flush_jobs = (int (*)(const char *))sym;
            s_hook_flush_jobs = hook_addr((uintptr_t)sym, (uintptr_t)&hook_Savegame_FlushJobs);
            l_success("Hooked Savegame::FlushJobs (save_io diag + drain)");
        } else {
            l_warn("save_io diag: Savegame::FlushJobs not found, queue drain unavailable");
        }
    }
    {
        void *sym = (void *)so_symbol(&so_mod, "_ZN16updateJob_thread5StartEv");
        if (sym) {
            s_hook_job_start = hook_addr((uintptr_t)sym, (uintptr_t)&hook_updateJob_Start);
            l_success("Hooked updateJob_thread::Start (save_io diag)");
        }
    }
    {
        void *sym = (void *)so_symbol(&so_mod, "_ZN16updateJob_thread6Start2Ev");
        if (sym) {
            s_hook_job_start2 = hook_addr((uintptr_t)sym, (uintptr_t)&hook_updateJob_Start2);
            l_success("Hooked updateJob_thread::Start2 (save_io diag)");
        }
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
