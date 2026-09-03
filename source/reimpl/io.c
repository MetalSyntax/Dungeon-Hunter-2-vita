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
#include <pthread.h>
#include <malloc.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

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

#define FCACHE_ENABLED 1
#define FCACHE_MAX_ENTRIES 1024
// Dimensionado con numeros reales medidos (log_000.log / log_001.log,
// 2026-09-02), no adivinado -- que era justo lo que pedia la nota de abajo:
//   * 100 fopen() de archivos por encima del limite viejo de 256KB, sumando
//     368.910.986 bytes (369 MB) releidos del almacenamiento en UNA sola
//     sesion, ninguno de ellos cacheable.
//   * archivo mas grande visto: 11.692.544 bytes (11,7 MB).
//   * hasta la fuente (app0:DejaVuSans.ttf, 757 KB) se releia cada vez.
// El presupuesto de bytes viejo (32MB) NUNCA se llenaba, y eso era engañoso:
// no sobraba lugar, era que los archivos grandes ni siquiera eran candidatos.
// 16MB por archivo deja pasar comodo al mas grande visto; 96MB de presupuesto
// total deja ~160MB del heap de 256MB (_newlib_heap_size_user, source/main.c)
// para el juego. El diagnostico de mallinfo() que ya existe abajo se dispara si
// este techo se llena, y ESE numero es el que debe guiar el proximo ajuste.
#define FCACHE_MAX_FILE_SIZE (16 * 1024 * 1024)
#define FCACHE_MAX_TOTAL_BYTES (96 * 1024 * 1024)
#define FCACHE_MAX_HANDLES 64

typedef struct {
    char path[400];
    unsigned char *data;
    long size;
} FCacheEntry;

typedef struct {
    int entry_idx; // -1 = free slot
    long pos;
} FCacheHandle;

static FCacheEntry s_fcache_entries[FCACHE_MAX_ENTRIES];
static int s_fcache_entry_count = 0;
static long s_fcache_total_bytes = 0;
static FCacheHandle s_fcache_handles[FCACHE_MAX_HANDLES];
static int s_fcache_handles_init = 0;
static pthread_mutex_t s_fcache_lock = PTHREAD_MUTEX_INITIALIZER;

// User asked to size the next FCACHE_MAX_TOTAL_BYTES bump off real numbers
// instead of guessing a bigger constant blind (same discipline as every
// other budget change in this file's history -- 64->512 entries in Phase 13
// was sized off an observed cap-hit, not a round number picked up front).
// Two things are needed to make that call safely next session: (1) how much
// demand the cache is actually turning away once the byte budget is full
// (bytes/files rejected purely for being over budget, not counting the
// separate per-file FCACHE_MAX_FILE_SIZE skip, which is a different knob),
// and (2) how much real headroom exists in the 256MB newlib heap
// (_newlib_heap_size_user, source/main.c) at the moment that happens, via
// mallinfo() -- raising the budget is only actually safe if there's heap
// left to give it. Logged once when the byte cap is first hit (so the
// mallinfo() snapshot reflects real mid-game memory pressure, not an empty
// boot heap) and then as a periodic running total, not per-rejection, since
// a cap-hit session can reject hundreds of files.
static int s_fcache_byte_cap_hit_logged = 0;
static long s_fcache_bytes_rejected = 0;
static int s_fcache_files_rejected = 0;
#define FCACHE_REJECT_LOG_EVERY 50

static void fcache_init_handles_locked(void) {
    if (s_fcache_handles_init) return;
    for (int i = 0; i < FCACHE_MAX_HANDLES; i++) s_fcache_handles[i].entry_idx = -1;
    s_fcache_handles_init = 1;
}

static int fcache_find_entry_locked(const char *path) {
    for (int i = 0; i < s_fcache_entry_count; i++) {
        if (strcmp(s_fcache_entries[i].path, path) == 0) return i;
    }
    return -1;
}

// Real FILE* pointers from the heap/BSS/libc's own static storage will never
// land inside this fixed, page-unaligned-sized array by coincidence -- this
// is the sole test used everywhere below to tell a cache handle apart from
// a genuine FILE*.
static int fcache_is_handle(void *f) {
    uintptr_t p = (uintptr_t) f;
    uintptr_t base = (uintptr_t) s_fcache_handles;
    uintptr_t end = base + sizeof(s_fcache_handles);
    return p >= base && p < end && ((p - base) % sizeof(FCacheHandle)) == 0;
}

static FILE *fcache_open_handle_locked(int entry_idx) {
    fcache_init_handles_locked();
    for (int i = 0; i < FCACHE_MAX_HANDLES; i++) {
        if (s_fcache_handles[i].entry_idx == -1) {
            s_fcache_handles[i].entry_idx = entry_idx;
            s_fcache_handles[i].pos = 0;
            return (FILE *) &s_fcache_handles[i];
        }
    }
    return NULL; // handle pool exhausted -- caller falls back to a real open
}

static int fcache_is_cacheable_mode(const char *mode) {
#if !FCACHE_ENABLED
    (void) mode;
    return 0; // kill switch: see the comment above FCACHE_ENABLED
#else
    return strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0;
#endif
}

static int s_fcache_entry_cap_hit_logged = 0;

// Contadores del knob por-archivo (FCACHE_MAX_FILE_SIZE), ver fcache_populate().
static int s_fcache_oversize_files = 0;
static long s_fcache_oversize_bytes = 0;
static long s_fcache_oversize_largest = 0;
static int s_fcache_oversize_logged = 0;

// Called right after a real fopen() succeeds for a cacheable path not
// already in the cache -- reads the whole file via the SAME real
// (non-wrapped) fread/fseek/ftell the rest of this file already uses, then
// rewinds the real file handle back to position 0 so the caller's own
// subsequent reads are completely unaffected by this out-of-band peek.
static void fcache_populate(const char *path, FILE *real_file) {
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    pthread_mutex_lock(&s_fcache_lock);
    fcache_init_handles_locked();
    if (s_fcache_entry_count >= FCACHE_MAX_ENTRIES) {
        int should_log = !s_fcache_entry_cap_hit_logged;
        if (should_log) s_fcache_entry_cap_hit_logged = 1;
        pthread_mutex_unlock(&s_fcache_lock);
        // Phase 19: the byte budget has been the binding constraint since the
        // entry cap was raised to 512, but log it explicitly if this ever
        // flips back (e.g. after a future FCACHE_MAX_TOTAL_BYTES increase
        // makes the entry count the new bottleneck instead).
        if (should_log) {
            l_warn("[fcache] entry cap (%d files) FULL -- further files skip caching regardless of byte budget "
                   "left (%ld/%d bytes used)", FCACHE_MAX_ENTRIES, s_fcache_total_bytes, FCACHE_MAX_TOTAL_BYTES);
        }
        return;
    }
    if (strlen(path) >= sizeof(s_fcache_entries[0].path)) {
        pthread_mutex_unlock(&s_fcache_lock);
        return;
    }
    pthread_mutex_unlock(&s_fcache_lock);

#ifdef USE_SCELIBC_IO
    sceLibcBridge_fseek(real_file, 0, SEEK_END);
    long size = sceLibcBridge_ftell(real_file);
    sceLibcBridge_fseek(real_file, 0, SEEK_SET);
#else
    fseek(real_file, 0, SEEK_END);
    long size = ftell(real_file);
    fseek(real_file, 0, SEEK_SET);
#endif
    if (size <= 0) return;

    // El OTRO knob (el de la nota de arriba, distinto del presupuesto de bytes):
    // todo archivo mayor a FCACHE_MAX_FILE_SIZE se descartaba aca en silencio,
    // sin ninguna linea de log -- por eso log_172.txt no tiene NI UNA linea
    // [fcache] pese a que el juego relee assets todo el tiempo: el presupuesto
    // de 32MB nunca llego a llenarse porque los archivos grandes (.bdae, atlas
    // de texturas, .swf) ni siquiera son candidatos. Sin datos no se puede
    // dimensionar el bump, asi que se instrumenta igual que el cap de bytes:
    // l_warn (sobrevive Release), primer hit + total periodico, nunca por
    // rechazo. Con el proximo log se puede subir FCACHE_MAX_FILE_SIZE con
    // numeros reales en vez de adivinar una constante mas grande.
    if (size > FCACHE_MAX_FILE_SIZE) {
        pthread_mutex_lock(&s_fcache_lock);
        s_fcache_oversize_files++;
        s_fcache_oversize_bytes += size;
        if (size > s_fcache_oversize_largest) s_fcache_oversize_largest = size;
        int should_log_first = !s_fcache_oversize_logged;
        if (should_log_first) s_fcache_oversize_logged = 1;
        int files_snapshot = s_fcache_oversize_files;
        long bytes_snapshot = s_fcache_oversize_bytes;
        long largest_snapshot = s_fcache_oversize_largest;
        pthread_mutex_unlock(&s_fcache_lock);

        if (should_log_first) {
            l_warn("[fcache] primer archivo por encima del limite por-archivo (%d bytes): %s pesa %ld bytes -- "
                   "NO se cachea, se relee del almacenamiento cada vez. Usar el total periodico de abajo para "
                   "dimensionar el proximo FCACHE_MAX_FILE_SIZE",
                   FCACHE_MAX_FILE_SIZE, path, size);
        } else if (files_snapshot % FCACHE_REJECT_LOG_EVERY == 0) {
            l_warn("[fcache] oversize acumulado: %d archivos / %ld bytes rechazados por FCACHE_MAX_FILE_SIZE (%d); "
                   "el mas grande visto hasta ahora: %ld bytes",
                   files_snapshot, bytes_snapshot, FCACHE_MAX_FILE_SIZE, largest_snapshot);
        }
        return;
    }

    pthread_mutex_lock(&s_fcache_lock);
    if (s_fcache_total_bytes + size > FCACHE_MAX_TOTAL_BYTES) {
        s_fcache_bytes_rejected += size;
        s_fcache_files_rejected++;
        int should_log_first_hit = !s_fcache_byte_cap_hit_logged;
        if (should_log_first_hit) s_fcache_byte_cap_hit_logged = 1;
        int rejected_files_snapshot = s_fcache_files_rejected;
        long rejected_bytes_snapshot = s_fcache_bytes_rejected;
        pthread_mutex_unlock(&s_fcache_lock);

        if (should_log_first_hit) {
            extern int _newlib_heap_size_user;
            struct mallinfo mi = mallinfo();
            l_warn("[fcache] byte budget (%d) FULL -- further files skip caching from here on. "
                   "Heap at this moment: %d bytes used / %d bytes max (_newlib_heap_size_user, source/main.c) -- "
                   "use this + the periodic reject totals below to size the next FCACHE_MAX_TOTAL_BYTES bump",
                   FCACHE_MAX_TOTAL_BYTES, mi.uordblks, _newlib_heap_size_user);
        } else if (rejected_files_snapshot % FCACHE_REJECT_LOG_EVERY == 0) {
            l_warn("[fcache] byte budget still full: %d files / %ld bytes rejected so far this session "
                   "(would need budget >= current %d + this to cache everything seen)",
                   rejected_files_snapshot, rejected_bytes_snapshot, FCACHE_MAX_TOTAL_BYTES);
        }
        return;
    }
    pthread_mutex_unlock(&s_fcache_lock);

    unsigned char *buf = (unsigned char *) malloc((size_t) size);
    if (!buf) return;

#ifdef USE_SCELIBC_IO
    size_t got = sceLibcBridge_fread(buf, 1, (size_t) size, real_file);
    sceLibcBridge_fseek(real_file, 0, SEEK_SET);
#else
    size_t got = fread(buf, 1, (size_t) size, real_file);
    fseek(real_file, 0, SEEK_SET);
#endif
    if (got != (size_t) size) {
        free(buf);
        return;
    }

    pthread_mutex_lock(&s_fcache_lock);
    if (s_fcache_entry_count >= FCACHE_MAX_ENTRIES || fcache_find_entry_locked(path) >= 0) {
        // Lost a race with another thread caching the same file, or the
        // cache filled up while we were reading -- drop our copy, the
        // existing/real path still works fine either way.
        pthread_mutex_unlock(&s_fcache_lock);
        free(buf);
        return;
    }
    FCacheEntry *e = &s_fcache_entries[s_fcache_entry_count++];
    strcpy(e->path, path);
    e->data = buf;
    e->size = size;
    s_fcache_total_bytes += size;
    int total_files = s_fcache_entry_count;
    long total_bytes = s_fcache_total_bytes;
    pthread_mutex_unlock(&s_fcache_lock);

    l_debug("[fcache] cached %s (%ld bytes, %ld/%d bytes total in %d files)",
            path, size, total_bytes, FCACHE_MAX_TOTAL_BYTES, total_files);
}

#define NEG_CACHE_SIZE 2048
static char s_neg_cache[NEG_CACHE_SIZE][256];
static int s_neg_cache_count = 0;

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

    if (strchr(mode, 'r')) {
        pthread_mutex_lock(&s_fcache_lock);
        for (int i = 0; i < s_neg_cache_count; i++) {
            if (strcmp(s_neg_cache[i], filename) == 0) {
                pthread_mutex_unlock(&s_fcache_lock);
                return NULL;
            }
        }
        pthread_mutex_unlock(&s_fcache_lock);
    }

    if (fcache_is_cacheable_mode(mode)) {
        pthread_mutex_lock(&s_fcache_lock);
        fcache_init_handles_locked();
        int entry_idx = fcache_find_entry_locked(filename);
        FILE *cached = (entry_idx >= 0) ? fcache_open_handle_locked(entry_idx) : NULL;
        pthread_mutex_unlock(&s_fcache_lock);
        if (cached) {
            l_debug("fopen(%s, %s): %p (cache hit, no disk I/O)", filename, mode, cached);
            return cached;
        }
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
        // (faeries_template_anim.bdae, requested under at least two different
        // literal filenames -- char_faerie.tga and tex_faerie_001.tga, see
        // log_125.txt) are missing from every real app-data dump we have
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
        } else if (strcmp(base, "char_faerie.tga") == 0 || strcmp(base, "tex_faerie_001.tga") == 0) {
            // Redirect fairy body to pure white radial texture (white glowing fairy)
            char redirected[512];
            snprintf(redirected, sizeof(redirected), "%.*sfx_radial_white.tga",
                     (int) (base - filename), filename);
            ret = fopen_soloader(redirected, mode);
            if (ret) return ret;
        } else if (strcmp(base, "fx_spark.tga") == 0) {
            // Redirect fairy sparkles/aura to golden/yellow lens flare
            char redirected[512];
            snprintf(redirected, sizeof(redirected), "%.*sfx_magic_lenz_flares_002.tga",
                     (int) (base - filename), filename);
            ret = fopen_soloader(redirected, mode);
            if (ret) return ret;
        }
    }

    if (ret && strstr(filename, "pydata") && strstr(filename, "_pyarray.bin")) {
#ifdef USE_SCELIBC_IO
        long pos = sceLibcBridge_ftell(ret);
        sceLibcBridge_fseek(ret, 0, SEEK_END);
        long size = sceLibcBridge_ftell(ret);
        sceLibcBridge_fseek(ret, pos, SEEK_SET);
#else
        long pos = ftell(ret);
        fseek(ret, 0, SEEK_END);
        long size = ftell(ret);
        fseek(ret, pos, SEEK_SET);
#endif
        l_warn("[pydata_diag] %s opened with size=%ld bytes -- compare against the reference copy in "
               "com.gameloft.android.GAND.GloftD2SS/files/data/pydata/ if stats still read wrong",
               filename, size);
    }

    if (ret && fcache_is_cacheable_mode(mode)) {
        fcache_populate(filename, ret);
    }

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else {
        l_warn("fopen(%s, %s): %p", filename, mode, ret);
        if (strchr(mode, 'r')) {
            pthread_mutex_lock(&s_fcache_lock);
            if (s_neg_cache_count < NEG_CACHE_SIZE) {
                strncpy(s_neg_cache[s_neg_cache_count], filename, 255);
                s_neg_cache[s_neg_cache_count][255] = '\0';
                s_neg_cache_count++;
            }
            pthread_mutex_unlock(&s_fcache_lock);
        }
    }

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
    if (fcache_is_handle(f)) {
        pthread_mutex_lock(&s_fcache_lock);
        ((FCacheHandle *) f)->entry_idx = -1;
        pthread_mutex_unlock(&s_fcache_lock);
        l_debug("fclose(%p): 0 (cache handle released, data stays cached)", f);
        return 0;
    }
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

// --- Cache-handle-safe wrappers for every other stdio entry point this
// project's dynlib.c import table exposes to the engine. Each one MUST
// check fcache_is_handle() first and never let a cache handle reach the
// real libc function -- see the cache's own header comment above
// fopen_soloader for why. ---

size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (fcache_is_handle(f)) {
        pthread_mutex_lock(&s_fcache_lock);
        FCacheHandle *h = (FCacheHandle *) f;
        FCacheEntry *e = &s_fcache_entries[h->entry_idx];
        long remaining = e->size - h->pos;
        if (remaining < 0) remaining = 0;
        size_t avail_items = (size == 0) ? 0 : ((size_t) remaining) / size;
        size_t items = avail_items < nmemb ? avail_items : nmemb;
        if (items > 0) {
            memcpy(ptr, e->data + h->pos, items * size);
            h->pos += (long) (items * size);
        }
        pthread_mutex_unlock(&s_fcache_lock);
        return items;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fread(ptr, size, nmemb, f);
#else
    return fread(ptr, size, nmemb, f);
#endif
}

size_t fwrite_soloader(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (fcache_is_handle(f)) {
        // Cache handles only ever come from a plain "r"/"rb" open -- a
        // write here means the engine is misusing a handle we handed it,
        // not something that should ever legitimately happen. Fail the
        // write instead of corrupting the shared cached buffer.
        l_warn("fwrite(%p): refused, this is a read-only cache handle", f);
        return 0;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fwrite(ptr, size, nmemb, f);
#else
    return fwrite(ptr, size, nmemb, f);
#endif
}

int fseek_soloader(FILE *f, long offset, int whence) {
    if (fcache_is_handle(f)) {
        pthread_mutex_lock(&s_fcache_lock);
        FCacheHandle *h = (FCacheHandle *) f;
        FCacheEntry *e = &s_fcache_entries[h->entry_idx];
        long base = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? h->pos : e->size;
        long newpos = base + offset;
        int ok = newpos >= 0;
        if (ok) h->pos = newpos; // seeking past EOF is valid; a read there just returns 0 items
        pthread_mutex_unlock(&s_fcache_lock);
        return ok ? 0 : -1;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fseek(f, offset, whence);
#else
    return fseek(f, offset, whence);
#endif
}

long ftell_soloader(FILE *f) {
    if (fcache_is_handle(f)) {
        return ((FCacheHandle *) f)->pos;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_ftell(f);
#else
    return ftell(f);
#endif
}

// off_t variants -- imported unwrapped before this cache existed (see the
// pre-existing "TODO: wrap normal fseek for SceLibc version?" note this
// project already had on the fseeko import), now a real safety gap since a
// cache handle could reach them. off_t and long are both 32-bit on this
// target, so this can share fseek_soloader/ftell_soloader's logic exactly.
int fseeko_soloader(FILE *f, off_t offset, int whence) {
    return fseek_soloader(f, (long) offset, whence);
}

off_t ftello_soloader(FILE *f) {
    return (off_t) ftell_soloader(f);
}

void rewind_soloader(FILE *f) {
    if (fcache_is_handle(f)) {
        pthread_mutex_lock(&s_fcache_lock);
        ((FCacheHandle *) f)->pos = 0;
        pthread_mutex_unlock(&s_fcache_lock);
        return;
    }
    rewind(f);
}

int feof_soloader(FILE *f) {
    if (fcache_is_handle(f)) {
        pthread_mutex_lock(&s_fcache_lock);
        FCacheHandle *h = (FCacheHandle *) f;
        int at_eof = h->pos >= s_fcache_entries[h->entry_idx].size;
        pthread_mutex_unlock(&s_fcache_lock);
        return at_eof;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_feof(f);
#else
    return feof(f);
#endif
}

int ferror_soloader(FILE *f) {
    if (fcache_is_handle(f)) return 0; // a memory-backed handle never errors
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_ferror(f);
#else
    return ferror(f);
#endif
}

int fflush_soloader(FILE *f) {
    if (fcache_is_handle(f)) return 0; // read-only, nothing to flush
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fflush(f);
#else
    return fflush(f);
#endif
}

int fgetc_soloader(FILE *f) {
    if (fcache_is_handle(f)) {
        unsigned char c;
        return fread_soloader(&c, 1, 1, f) == 1 ? (int) c : EOF;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fgetc(f);
#else
    return fgetc(f);
#endif
}

int getc_soloader(FILE *f) {
    return fgetc_soloader(f);
}

int fputc_soloader(int c, FILE *f) {
    if (fcache_is_handle(f)) {
        l_warn("fputc(%p): refused, this is a read-only cache handle", f);
        return EOF;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fputc(c, f);
#else
    return fputc(c, f);
#endif
}

int putc_soloader(int c, FILE *f) {
    return fputc_soloader(c, f);
}

char *fgets_soloader(char *str, int n, FILE *f) {
    if (fcache_is_handle(f)) {
        if (n <= 0) return NULL;
        int i = 0;
        for (; i < n - 1; i++) {
            int c = fgetc_soloader(f);
            if (c == EOF) {
                if (i == 0) return NULL;
                break;
            }
            str[i] = (char) c;
            if (c == '\n') { i++; break; }
        }
        str[i] = '\0';
        return str;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fgets(str, n, f);
#else
    return fgets(str, n, f);
#endif
}

int fputs_soloader(const char *str, FILE *f) {
    if (fcache_is_handle(f)) {
        l_warn("fputs(%p): refused, this is a read-only cache handle", f);
        return EOF;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fputs(str, f);
#else
    return fputs(str, f);
#endif
}

int fileno_soloader(FILE *f) {
    if (fcache_is_handle(f)) {
        l_warn("fileno(%p): this is a read-only cache handle, no real file descriptor exists", f);
        return -1;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fileno(f);
#else
    return fileno(f);
#endif
}

int setvbuf_soloader(FILE *f, char *buf, int mode, size_t size) {
    if (fcache_is_handle(f)) return 0; // memory-backed, no OS buffering to configure
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_setvbuf(f, buf, mode, size);
#else
    return setvbuf(f, buf, mode, size);
#endif
}

int ungetc_soloader(int c, FILE *f) {
    if (fcache_is_handle(f)) {
        pthread_mutex_lock(&s_fcache_lock);
        FCacheHandle *h = (FCacheHandle *) f;
        int ok = h->pos > 0;
        if (ok) h->pos--;
        pthread_mutex_unlock(&s_fcache_lock);
        return ok ? c : EOF;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_ungetc(c, f);
#else
    return ungetc(c, f);
#endif
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
