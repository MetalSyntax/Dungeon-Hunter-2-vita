/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

FILE * fopen_soloader(const char * filename, const char * mode) {
    // The engine's own on-demand asset streaming (glitch::collada::COnDemandReader,
    // used for .bdae model/character data) sometimes re-resolves an already-absolute
    // path through its own "prepend base data path" logic a second time, producing
    // DATA_PATH DATA_PATH <relative path...> (confirmed via crash triage on a real
    // hardware dump: PC landed in COnDemandReader::read() dereferencing a NULL
    // stream member -- the doubled path's fopen() had failed). Collapse the
    // duplicate prefix instead of letting that second, broken open silently fail.
    if (strncmp(filename, DATA_PATH DATA_PATH, strlen(DATA_PATH DATA_PATH)) == 0) {
        return fopen_soloader(filename + strlen(DATA_PATH), mode);
    }

    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    } else if (strcmp(filename, "/system/fonts/droidsans.ttf") == 0 ||
               strcmp(filename, "#/system/fonts/droidsans.ttf") == 0) {
        // DH2's engine (FreeType, likely via the embedded GameSWF/Flash UI layer --
        // see gameswf_effects.bdae) asks for the real Android system font, which
        // obviously isn't present on Vita. Redirect to a bundled substitute rather
        // than let every text/UI element that needs a glyph silently fail to draw.
        return fopen_soloader("app0:DejaVuSans.ttf", mode);
    }

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (!ret) {
        // Dungeon environment alpha masks (data/3d/textures/env_<theme>_alpha.tga)
        // are shipped on disk under a "pvr2_" prefix for most level themes (e.g.
        // pvr2_env_swamp_alpha.tga -- confirmed by inspecting the real extracted
        // app-data dump: no plain env_swamp_alpha.tga exists anywhere, only the
        // pvr2_-prefixed one) while the engine always requests the plain name.
        // Not every theme follows this (env_darkwoods_alpha.tga exists under
        // BOTH names), so only redirect after the plain name genuinely fails.
        const char *slash = strrchr(filename, '/');
        const char *base = slash ? slash + 1 : filename;
        size_t base_len = strlen(base);
        static const char alpha_suffix[] = "_alpha.tga";
        if (strncmp(base, "env_", 4) == 0 && base_len > sizeof(alpha_suffix) - 1 &&
            strcmp(base + base_len - (sizeof(alpha_suffix) - 1), alpha_suffix) == 0) {
            char redirected[512];
            snprintf(redirected, sizeof(redirected), "%.*spvr2_%s",
                     (int) (base - filename), filename, base);
            ret = fopen_soloader(redirected, mode);
            if (ret) return ret;
        }

        // A handful of character textures referenced by the swamp-intro cutscene NPC
        // (cs_swamp_intro_prisonner_scene01.bdae) and the Faerie companion
        // (faeries_template_anim.bdae) are missing from every real app-data dump we have
        // access to (two independent Android installs checked, neither has them, and
        // neither has the "qata" directory the engine's own on-demand reader falls back
        // to on failure -- confirmed that fallback is genuine, unmodified engine
        // behavior, not something this port introduced). These are cosmetic-only:
        // a one-time intro cutscene and a companion model, not core gameplay. Since the
        // real Gameloft art for these specific files is unobtainable from any source we
        // have, redirect to the closest real, already-legitimate in-game texture instead
        // of leaving the model with no texture at all (flat aquamarine).
        if (strcmp(base, "tex_prince.tga") == 0 ||
            strcmp(base, "tex_princehair.tga") == 0 ||
            strcmp(base, "tex_prince_pants.tga") == 0 ||
            strcmp(base, "tex_plate_shoulder_default_000.tga") == 0) {
            char redirected[512];
            snprintf(redirected, sizeof(redirected), "%.*sprince-warrior.tga",
                     (int) (base - filename), filename);
            ret = fopen_soloader(redirected, mode);
            if (ret) return ret;
        } else if (strcmp(base, "char_faerie.tga") == 0) {
            char redirected[512];
            snprintf(redirected, sizeof(redirected), "%.*sfx_sparkles_01.tga",
                     (int) (base - filename), filename);
            ret = fopen_soloader(redirected, mode);
            if (ret) return ret;
        } else if (strcmp(base, "fx_spark.tga") == 0) {
            char redirected[512];
            snprintf(redirected, sizeof(redirected), "%.*sfx_spark_01.tga",
                     (int) (base - filename), filename);
            ret = fopen_soloader(redirected, mode);
            if (ret) return ret;
        }
    }

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
