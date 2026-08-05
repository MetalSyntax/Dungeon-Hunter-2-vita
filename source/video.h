#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

// Loads the AVPLAYER sysmodule. Call once, after gl_init() (video_play()'s
// texture allocator maps memory via sceGxmMapMemory, which needs the GXM
// context PVR_PSP2's EGL init brings up).
void video_init(void);

void video_shutdown(void);

// Plays name (a bare filename under DATA_PATH"files/", e.g. "intro.mp4")
// fullscreen, blocking the calling thread until the video ends naturally,
// the user skips it (Cross/Start), or it fails to open/init -- always
// returns, never hangs, so GLMediaPlayer_loadMovie (java.c) can unblock
// GSInit's videoDone wait unconditionally right after this call.
void video_play(const char *name);

#ifdef __cplusplus
}
#endif

#endif // SOLOADER_VIDEO_H
