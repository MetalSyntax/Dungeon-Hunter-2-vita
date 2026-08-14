/*
 * java.c
 *
 * "Java-side" native method handlers for FalsoJNI.
 */

/**
 * @file  java.c
 * @brief Native C/C++ static Java method handlers intercepted by FalsoJNI.
 * @details Refer to technical documentation in Docs/java_comments.md for details on
 *          GLResLoader, GLMediaPlayer, Musicplayer, DungeonHunter2, and Verizon billing stubs (VZ*).
 */

#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "video.h"

#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <kubridge.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

#include "sounddefs.h"
#include "audio.h"

/**
 * @brief Resolves relative asset file path under DATA_PATH"assets/<name>".
 */
static int dh2_resolve_asset_path(const char *name, char *out, size_t out_size) {
    if (!name) return 0;
    snprintf(out, out_size, DATA_PATH "assets/%s", name);
    return access(out, F_OK) == 0;
}

/**
 * @brief GLResLoader JNI method implementations.
 */
jobject GLResLoader_getResourceFull(jmethodID id, va_list args) {
    const char *name = (const char *) va_arg(args, jstring);
    char path[512];

    if (!dh2_resolve_asset_path(name, path, sizeof(path))) {
        l_warn("[Java] GLResLoader.getResourceFull(%s): not found", name ? name : "(null)");
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }

    JavaDynArray *jda = jda_alloc((jsize) st.st_size, FIELD_TYPE_BYTE);
    if (!jda) {
        fclose(f);
        return NULL;
    }
    fread(jda->array, 1, (size_t) st.st_size, f);
    fclose(f);

    l_info("[Java] GLResLoader.getResourceFull(%s): %ld bytes", name, (long) st.st_size);
    return (jobject) jda;
}

jint GLResLoader_getResourceLength(jmethodID id, va_list args) {
    const char *name = (const char *) va_arg(args, jstring);
    char path[512];

    if (!dh2_resolve_asset_path(name, path, sizeof(path))) {
        l_warn("[Java] GLResLoader.getResourceLength(%s): not found", name ? name : "(null)");
        return 0;
    }

    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (jint) st.st_size;
}

jobject GLResLoader_getResourceBytes(jmethodID id, va_list args) {
    const char *name = (const char *) va_arg(args, jstring);
    jint offset = va_arg(args, jint);
    jint length = va_arg(args, jint);
    char path[512];

    if (length <= 0 || !dh2_resolve_asset_path(name, path, sizeof(path))) {
        l_warn("[Java] GLResLoader.getResourceBytes(%s, %d, %d): not found",
               name ? name : "(null)", (int) offset, (int) length);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (offset > 0) fseek(f, offset, SEEK_SET);

    JavaDynArray *jda = jda_alloc(length, FIELD_TYPE_BYTE);
    if (!jda) {
        fclose(f);
        return NULL;
    }
    fread(jda->array, 1, (size_t) length, f);
    fclose(f);

    return (jobject) jda;
}

/**
 * @brief GLMediaPlayer JNI method implementations.
 */
void GLMediaPlayer_unloadSound(jmethodID id, va_list args) { (void) args; }
void GLMediaPlayer_unloadSoundBig(jmethodID id, va_list args) { (void) args; }
void GLMediaPlayer_resetSound(jmethodID id, va_list args) { (void) args; }
void GLMediaPlayer_destroySoundPool(jmethodID id, va_list args) { (void) args; }
void GLMediaPlayer_initSoundPoolArray(jmethodID id, va_list args) { (void) args; }

jint GLMediaPlayer_loadMovie(jmethodID id, va_list args) {
    const char *name = (const char *) va_arg(args, jstring);
    (void) va_arg(args, jint);
    l_info("[Java] GLMediaPlayer.loadMovie(%s): playing via SceAvPlayer", name ? name : "(null)");

    if (name) {
        video_play(name);
    }

    extern so_module so_mod;
    volatile uint8_t *videoDone = (volatile uint8_t *) so_symbol(&so_mod, "videoDone");
    if (videoDone) {
        __sync_synchronize();
        *videoDone = 1;
        __sync_synchronize();
        kuKernelFlushCaches((void *)videoDone, sizeof(uint8_t));
    } else {
        l_warn("[Java] GLMediaPlayer.loadMovie: 'videoDone' symbol not found, GSInit may hang");
    }

    return name ? 1 : 0;
}

jint GLMediaPlayer_getWidth(jmethodID id, va_list args) {
    (void) args;
    return 960;
}

jint GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args) {
    (void) va_arg(args, jint);
    return 0;
}

jint GLMediaPlayer_detectPhoneLang(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

jobject GLMediaPlayer_getSDFolder(jmethodID id, va_list args) {
    (void) args;
    return (jobject) DATA_PATH;
}

/**
 * @brief Musicplayer JNI method implementations.
 */
jint Musicplayer_GetNumPlaylists(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

jobject Musicplayer_GetPlayListName(jmethodID id, va_list args) {
    (void) va_arg(args, jint);
    return NULL;
}

void Musicplayer_SetPlaylist(jmethodID id, va_list args) { (void) args; }
void Musicplayer_PlayBGMusic(jmethodID id, va_list args) { (void) args; }
void Musicplayer_ChangeMusic(jmethodID id, va_list args) { (void) args; }
void Musicplayer_ResumeMusicBG(jmethodID id, va_list args) { (void) args; }
void Musicplayer_PauseMusicBG(jmethodID id, va_list args) { (void) args; }
void Musicplayer_StopMusicBG(jmethodID id, va_list args) { (void) args; }

jint Musicplayer_Getisplaying(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

/**
 * @brief DungeonHunter2 JNI method implementations and network/billing stubs.
 */
void DungeonHunter2_sendAppToBackground(jmethodID id, va_list args) {
    (void) args;
    l_info("[Java] DungeonHunter2.sendAppToBackground() (no-op)");
}

void DungeonHunter2_OpenGLive(jmethodID id, va_list args) {
    int lang = va_arg(args, jint);
    l_info("[Java] DungeonHunter2.OpenGLive(%d) (no-op, Gameloft Live not implemented)", lang);
}

void DungeonHunter2_OpenIGP(jmethodID id, va_list args) {
    int lang = va_arg(args, jint);
    l_info("[Java] DungeonHunter2.OpenIGP(%d) (no-op, IGP cross-promo not implemented)", lang);
}

void DungeonHunter2_NotifyTrophy(jmethodID id, va_list args) {
    int trophyId = va_arg(args, jint);
    l_info("[Java] DungeonHunter2.NotifyTrophy(%d) (no-op)", trophyId);
}

jint DungeonHunter2_Get_PhoneLanguage(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

jint DungeonHunter2_Get_PhoneManufacturer(jmethodID id, va_list args) {
    (void) args;
    return 1;
}

jint DungeonHunter2_Get_PhoneModel(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

void DungeonHunter2_Exit(jmethodID id, va_list args) {
    (void) args;
    l_info("[Java] DungeonHunter2.Exit(): engine requested exit, terminating");
    sceKernelExitProcess(0);
}

jint DungeonHunter2_isWifiAlive(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

jint DungeonHunter2_isSupportMM(jmethodID id, va_list args) {
    (void) args;
    return -1;
}

void DungeonHunter2_openBrowser(jmethodID id, va_list args) {
    const char *url = (const char *) va_arg(args, jstring);
    l_info("[Java] DungeonHunter2.openBrowser(%s) (no-op, no browser on Vita)", url ? url : "(null)");
}

jint DungeonHunter2_unlockDemo(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

void DungeonHunter2_lockDemo(jmethodID id, va_list args) { (void) args; }

jint DungeonHunter2_DisableLaunchGame(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

void DungeonHunter2_IncreaseLaunchTimes(jmethodID id, va_list args) { (void) args; }

jint DungeonHunter2_VZIsInProgress(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

jint DungeonHunter2_VZIsErrorOcurred(jmethodID id, va_list args) {
    (void) args;
    return 1;
}

void DungeonHunter2_VZRequestLogin(jmethodID id, va_list args) {
    (void) args;
    l_warn("[Java] DungeonHunter2.VZRequestLogin() called -- Verizon billing should never be reachable here");
}

void DungeonHunter2_VZRequestPurchaseGame(jmethodID id, va_list args) {
    (void) args;
    l_warn("[Java] DungeonHunter2.VZRequestPurchaseGame() called -- Verizon billing should never be reachable here");
}

jobject DungeonHunter2_VZGetGamePrice(jmethodID id, va_list args) { (void) args; return NULL; }
jobject DungeonHunter2_VZGetGameName(jmethodID id, va_list args) { (void) args; return NULL; }
jobject DungeonHunter2_VZGetLastServerMsg(jmethodID id, va_list args) { (void) args; return NULL; }

void DungeonHunter2_VZInitMobileNetwork(jmethodID id, va_list args) { (void) args; }

jint DungeonHunter2_VZIsMobileNetworkReady(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

void DungeonHunter2_VZRestoreNetworkState(jmethodID id, va_list args) { (void) args; }

/**
 * @brief Miscellaneous auxiliary and dummy Java methods.
 */
jobject Misc_DummyByteArray(jmethodID id, va_list args) {
    (void) args;
    JavaDynArray *jda = jda_alloc(1, FIELD_TYPE_BYTE);
    if (jda) {
        ((char*)jda->array)[0] = 0;
    }
    return (jobject) jda;
}

jint Misc_IsWifiEnable(jmethodID id, va_list args) {
    (void) args;
    return 0;
}

void Misc_DummyVoidInt(jmethodID id, va_list args) {
    (void) va_arg(args, jint);
}

jobject Misc_DummyAudioTrack(jmethodID id, va_list args) {
    (void) args;
    return (jobject) 0x12345678;
}

jint Misc_GetMinBufferSize(jmethodID id, va_list args) {
    (void) args;
    return 16384;
}

void Misc_DummyVoid(jmethodID id, va_list args) {
    (void) args;
}

/**
 * @brief FalsoJNI method ID enumeration.
 */
enum {
    M_getResourceFull = 1,
    M_getResourceBytes,
    M_getResourceLength,

    M_isSoundLoaded,
    M_isSoundLoadedBig,
    M_unloadSound,
    M_unloadSoundBig,
    M_loadSound,
    M_loadSoundBig,
    M_playSound,
    M_playSoundBig,
    M_pauseSound,
    M_pauseSoundBig,
    M_resumeSound,
    M_resumeSoundBig,
    M_stopSound,
    M_stopSoundBig,
    M_setVolume,
    M_setVolumeBig,
    M_resetSound,
    M_setPitch,
    M_stopAllSounds,
    M_stopAllPool,
    M_stopAllBig,
    M_destroySoundPool,
    M_initSoundPoolArray,
    M_loadMovie,
    M_getWidth,
    M_isMediaPlaying,
    M_detectPhoneLang,
    M_getSDFolder,

    M_GetNumPlaylists,
    M_GetPlayListName,
    M_SetPlaylist,
    M_PlayBGMusic,
    M_ChangeMusic,
    M_ResumeMusicBG,
    M_PauseMusicBG,
    M_StopMusicBG,
    M_Getisplaying,

    M_sendAppToBackground,
    M_OpenGLive,
    M_OpenIGP,
    M_NotifyTrophy,
    M_Get_PhoneLanguage,
    M_Get_PhoneManufacturer,
    M_Get_PhoneModel,
    M_Exit,
    M_isWifiAlive,
    M_isSupportMM,
    M_openBrowser,
    M_unlockDemo,
    M_lockDemo,
    M_DisableLaunchGame,
    M_IncreaseLaunchTimes,
    M_VZIsInProgress,
    M_VZIsErrorOcurred,
    M_VZRequestLogin,
    M_VZRequestPurchaseGame,
    M_VZGetGamePrice,
    M_VZGetGameName,
    M_VZGetLastServerMsg,
    M_VZInitMobileNetwork,
    M_VZIsMobileNetworkReady,
    M_VZRestoreNetworkState,

    M_IsWifiEnable,
    M_getHostName,
    M_method_a,
    M_method_b,
    M_method_c,
    M_method_d,
    M_method_e,
    M_method_f,

    M_AudioTrack_init,
    M_AudioTrack_getMinBufferSize,
    M_AudioTrack_write,
    M_AudioTrack_play,
    M_AudioTrack_pause,
    M_AudioTrack_stop,
    M_AudioTrack_release,
};

/**
 * @brief Mapping table of Java method names to FalsoJNI IDs (nameToMethodId).
 */
NameToMethodID nameToMethodId[] = {
        { M_getResourceFull,          "getResourceFull",         METHOD_TYPE_OBJECT },
        { M_getResourceBytes,         "getResourceBytes",        METHOD_TYPE_OBJECT },
        { M_getResourceLength,        "getResourceLength",       METHOD_TYPE_INT },

        { M_isSoundLoaded,            "isSoundLoaded",           METHOD_TYPE_INT },
        { M_isSoundLoadedBig,         "isSoundLoadedBig",        METHOD_TYPE_INT },
        { M_unloadSound,              "unloadSound",             METHOD_TYPE_VOID },
        { M_unloadSoundBig,           "unloadSoundBig",          METHOD_TYPE_VOID },
        { M_loadSound,                "loadSound",               METHOD_TYPE_VOID },
        { M_loadSoundBig,             "loadSoundBig",            METHOD_TYPE_VOID },
        { M_playSound,                "playSound",               METHOD_TYPE_VOID },
        { M_playSoundBig,             "playSoundBig",            METHOD_TYPE_VOID },
        { M_pauseSound,               "pauseSound",              METHOD_TYPE_VOID },
        { M_pauseSoundBig,            "pauseSoundBig",           METHOD_TYPE_VOID },
        { M_resumeSound,              "resumeSound",             METHOD_TYPE_VOID },
        { M_resumeSoundBig,           "resumeSoundBig",          METHOD_TYPE_VOID },
        { M_stopSound,                "stopSound",               METHOD_TYPE_VOID },
        { M_stopSoundBig,             "stopSoundBig",            METHOD_TYPE_VOID },
        { M_setVolume,                "setVolume",               METHOD_TYPE_VOID },
        { M_setVolumeBig,             "setVolumeBig",            METHOD_TYPE_VOID },
        { M_resetSound,               "resetSound",              METHOD_TYPE_VOID },
        { M_setPitch,                 "setPitch",                METHOD_TYPE_VOID },
        { M_stopAllSounds,            "stopAllSounds",           METHOD_TYPE_VOID },
        { M_stopAllPool,              "stopAllPool",             METHOD_TYPE_VOID },
        { M_stopAllBig,               "stopAllBig",              METHOD_TYPE_VOID },
        { M_destroySoundPool,         "destroySoundPool",        METHOD_TYPE_VOID },
        { M_initSoundPoolArray,       "initSoundPoolArray",      METHOD_TYPE_VOID },
        { M_loadMovie,                "loadMovie",               METHOD_TYPE_INT },
        { M_getWidth,                 "getWidth",                METHOD_TYPE_INT },
        { M_isMediaPlaying,           "isMediaPlaying",          METHOD_TYPE_INT },
        { M_detectPhoneLang,          "detectPhoneLang",         METHOD_TYPE_INT },
        { M_getSDFolder,              "getSDFolder",             METHOD_TYPE_OBJECT },

        { M_GetNumPlaylists,          "GetNumPlaylists",         METHOD_TYPE_INT },
        { M_GetPlayListName,          "GetPlayListName",         METHOD_TYPE_OBJECT },
        { M_SetPlaylist,              "SetPlaylist",             METHOD_TYPE_VOID },
        { M_PlayBGMusic,              "PlayBGMusic",             METHOD_TYPE_VOID },
        { M_ChangeMusic,              "ChangeMusic",             METHOD_TYPE_VOID },
        { M_ResumeMusicBG,            "ResumeMusicBG",           METHOD_TYPE_VOID },
        { M_PauseMusicBG,             "PauseMusicBG",            METHOD_TYPE_VOID },
        { M_StopMusicBG,              "StopMusicBG",             METHOD_TYPE_VOID },
        { M_Getisplaying,             "Getisplaying",            METHOD_TYPE_INT },

        { M_sendAppToBackground,      "sendAppToBackground",     METHOD_TYPE_VOID },
        { M_OpenGLive,                "OpenGLive",               METHOD_TYPE_VOID },
        { M_OpenIGP,                  "OpenIGP",                 METHOD_TYPE_VOID },
        { M_NotifyTrophy,             "NotifyTrophy",            METHOD_TYPE_VOID },
        { M_Get_PhoneLanguage,        "Get_PhoneLanguage",       METHOD_TYPE_INT },
        { M_Get_PhoneManufacturer,    "Get_PhoneManufacturer",   METHOD_TYPE_INT },
        { M_Get_PhoneModel,           "Get_PhoneModel",          METHOD_TYPE_INT },
        { M_Exit,                     "Exit",                    METHOD_TYPE_VOID },
        { M_isWifiAlive,              "isWifiAlive",             METHOD_TYPE_INT },
        { M_isSupportMM,              "isSupportMM",             METHOD_TYPE_INT },
        { M_openBrowser,              "openBrowser",             METHOD_TYPE_VOID },
        { M_unlockDemo,               "unlockDemo",              METHOD_TYPE_INT },
        { M_lockDemo,                 "lockDemo",                METHOD_TYPE_VOID },
        { M_DisableLaunchGame,        "DisableLaunchGame",       METHOD_TYPE_INT },
        { M_IncreaseLaunchTimes,      "IncreaseLaunchTimes",     METHOD_TYPE_VOID },
        { M_VZIsInProgress,           "VZIsInProgress",          METHOD_TYPE_INT },
        { M_VZIsErrorOcurred,         "VZIsErrorOcurred",        METHOD_TYPE_INT },
        { M_VZRequestLogin,           "VZRequestLogin",          METHOD_TYPE_VOID },
        { M_VZRequestPurchaseGame,    "VZRequestPurchaseGame",   METHOD_TYPE_VOID },
        { M_VZGetGamePrice,           "VZGetGamePrice",          METHOD_TYPE_OBJECT },
        { M_VZGetGameName,            "VZGetGameName",           METHOD_TYPE_OBJECT },
        { M_VZGetLastServerMsg,       "VZGetLastServerMsg",      METHOD_TYPE_OBJECT },
        { M_VZInitMobileNetwork,      "VZInitMobileNetwork",     METHOD_TYPE_VOID },
        { M_VZIsMobileNetworkReady,   "VZIsMobileNetworkReady",  METHOD_TYPE_INT },
        { M_VZRestoreNetworkState,    "VZRestoreNetworkState",   METHOD_TYPE_VOID },
        { M_IsWifiEnable,             "IsWifiEnable",            METHOD_TYPE_INT },
        { M_getHostName,              "getHostName",             METHOD_TYPE_OBJECT },
        { M_method_a,                 "a",                       METHOD_TYPE_OBJECT },
        { M_method_b,                 "b",                       METHOD_TYPE_OBJECT },
        { M_method_c,                 "c",                       METHOD_TYPE_OBJECT },
        { M_method_d,                 "d",                       METHOD_TYPE_OBJECT },
        { M_method_e,                 "e",                       METHOD_TYPE_VOID },
        { M_method_f,                 "f",                       METHOD_TYPE_OBJECT },
        { M_AudioTrack_init,          "android/media/AudioTrack/<init>", METHOD_TYPE_OBJECT },
        { M_AudioTrack_getMinBufferSize, "getMinBufferSize",     METHOD_TYPE_INT },
        { M_AudioTrack_write,         "write",                   METHOD_TYPE_INT },
        { M_AudioTrack_play,          "play",                    METHOD_TYPE_VOID },
        { M_AudioTrack_pause,         "pause",                   METHOD_TYPE_VOID },
        { M_AudioTrack_stop,          "stop",                    METHOD_TYPE_VOID },
        { M_AudioTrack_release,       "release",                 METHOD_TYPE_VOID },
};

MethodsBoolean methodsBoolean[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};

MethodsInt methodsInt[] = {
        { M_getResourceLength,      GLResLoader_getResourceLength },
        { M_isSoundLoaded,          GLMediaPlayer_isSoundLoaded },
        { M_isSoundLoadedBig,       GLMediaPlayer_isSoundLoadedBig },
        { M_loadMovie,              GLMediaPlayer_loadMovie },
        { M_getWidth,               GLMediaPlayer_getWidth },
        { M_isMediaPlaying,         GLMediaPlayer_isMediaPlaying },
        { M_detectPhoneLang,        GLMediaPlayer_detectPhoneLang },
        { M_GetNumPlaylists,        Musicplayer_GetNumPlaylists },
        { M_Getisplaying,           Musicplayer_Getisplaying },
        { M_Get_PhoneLanguage,      DungeonHunter2_Get_PhoneLanguage },
        { M_Get_PhoneManufacturer,  DungeonHunter2_Get_PhoneManufacturer },
        { M_Get_PhoneModel,         DungeonHunter2_Get_PhoneModel },
        { M_isWifiAlive,            DungeonHunter2_isWifiAlive },
        { M_isSupportMM,            DungeonHunter2_isSupportMM },
        { M_unlockDemo,             DungeonHunter2_unlockDemo },
        { M_DisableLaunchGame,      DungeonHunter2_DisableLaunchGame },
        { M_VZIsInProgress,         DungeonHunter2_VZIsInProgress },
        { M_VZIsErrorOcurred,       DungeonHunter2_VZIsErrorOcurred },
        { M_VZIsMobileNetworkReady, DungeonHunter2_VZIsMobileNetworkReady },
        { M_IsWifiEnable,           Misc_IsWifiEnable },
        { M_AudioTrack_getMinBufferSize, Misc_GetMinBufferSize },
        { M_AudioTrack_write,       Misc_AudioTrackWrite },
};

MethodsLong methodsLong[] = {};

MethodsObject methodsObject[] = {
        { M_getResourceFull,     GLResLoader_getResourceFull },
        { M_getResourceBytes,    GLResLoader_getResourceBytes },
        { M_getSDFolder,         GLMediaPlayer_getSDFolder },
        { M_GetPlayListName,     Musicplayer_GetPlayListName },
        { M_VZGetGamePrice,      DungeonHunter2_VZGetGamePrice },
        { M_VZGetGameName,       DungeonHunter2_VZGetGameName },
        { M_VZGetLastServerMsg,  DungeonHunter2_VZGetLastServerMsg },
        { M_getHostName,         Misc_DummyByteArray },
        { M_method_a,            Misc_DummyByteArray },
        { M_method_b,            Misc_DummyByteArray },
        { M_method_c,            Misc_DummyByteArray },
        { M_method_d,            Misc_DummyByteArray },
        { M_method_f,            Misc_DummyByteArray },
        { M_AudioTrack_init,     Misc_DummyAudioTrack },
};

MethodsShort methodsShort[] = {};

MethodsVoid methodsVoid[] = {
        { M_unloadSound,            GLMediaPlayer_unloadSound },
        { M_unloadSoundBig,         GLMediaPlayer_unloadSoundBig },
        { M_loadSound,              GLMediaPlayer_loadSound },
        { M_loadSoundBig,           GLMediaPlayer_loadSoundBig },
        { M_playSound,              GLMediaPlayer_playSound },
        { M_playSoundBig,           GLMediaPlayer_playSoundBig },
        { M_pauseSound,             GLMediaPlayer_pauseSound },
        { M_pauseSoundBig,          GLMediaPlayer_pauseSoundBig },
        { M_resumeSound,            GLMediaPlayer_resumeSound },
        { M_resumeSoundBig,         GLMediaPlayer_resumeSoundBig },
        { M_stopSound,              GLMediaPlayer_stopSound },
        { M_stopSoundBig,           GLMediaPlayer_stopSoundBig },
        { M_setVolume,              GLMediaPlayer_setVolume },
        { M_setVolumeBig,           GLMediaPlayer_setVolumeBig },
        { M_resetSound,             GLMediaPlayer_resetSound },
        { M_setPitch,               GLMediaPlayer_setPitch },
        { M_stopAllSounds,          GLMediaPlayer_stopAllSounds },
        { M_stopAllPool,            GLMediaPlayer_stopAllPool },
        { M_stopAllBig,             GLMediaPlayer_stopAllBig },
        { M_destroySoundPool,       GLMediaPlayer_destroySoundPool },
        { M_initSoundPoolArray,     GLMediaPlayer_initSoundPoolArray },
        { M_SetPlaylist,            Musicplayer_SetPlaylist },
        { M_PlayBGMusic,            Musicplayer_PlayBGMusic },
        { M_ChangeMusic,            Musicplayer_ChangeMusic },
        { M_ResumeMusicBG,          Musicplayer_ResumeMusicBG },
        { M_PauseMusicBG,           Musicplayer_PauseMusicBG },
        { M_StopMusicBG,            Musicplayer_StopMusicBG },
        { M_sendAppToBackground,    DungeonHunter2_sendAppToBackground },
        { M_OpenGLive,              DungeonHunter2_OpenGLive },
        { M_OpenIGP,                DungeonHunter2_OpenIGP },
        { M_NotifyTrophy,           DungeonHunter2_NotifyTrophy },
        { M_Exit,                   DungeonHunter2_Exit },
        { M_openBrowser,            DungeonHunter2_openBrowser },
        { M_lockDemo,               DungeonHunter2_lockDemo },
        { M_IncreaseLaunchTimes,    DungeonHunter2_IncreaseLaunchTimes },
        { M_VZRequestLogin,         DungeonHunter2_VZRequestLogin },
        { M_VZRequestPurchaseGame,  DungeonHunter2_VZRequestPurchaseGame },
        { M_VZInitMobileNetwork,    DungeonHunter2_VZInitMobileNetwork },
        { M_VZRestoreNetworkState,  DungeonHunter2_VZRestoreNetworkState },
        { M_method_e,               Misc_DummyVoidInt },
        { M_AudioTrack_play,        Misc_DummyVoid },
        { M_AudioTrack_pause,       Misc_DummyVoid },
        { M_AudioTrack_stop,        Misc_DummyVoid },
        { M_AudioTrack_release,     Misc_AudioTrackRelease },
};

/**
 * @brief FalsoJNI field definitions (nameToFieldId).
 */
char WINDOW_SERVICE[] = "window";

const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
        { 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
        { 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
        { 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
        { 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
