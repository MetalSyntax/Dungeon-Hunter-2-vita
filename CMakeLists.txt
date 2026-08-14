cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(dungeon_hunter_2 C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "Dungeon Hunter 2")
set(VITA_TITLEID "PSVDH0002")
# Overridable so porting_tools/build/build_and_install.sh's performance-test
# flags (see build.sh at the repo root) can produce distinctly-named VPKs
# (e.g. dungeon_hunter_2_downsample_test.vpk) side by side with the normal
# build instead of overwriting it on every experiment.
set(VITA_VPKNAME "dungeon_hunter_2" CACHE STRING "Output VPK base name (without .vpk)")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.3.15" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions.
# Only libDungeonHunter2.so is loaded -- see PORTING_PLAN.md Phase 1/2:
# libStormGLOFT.so is an ARM/THUMB inline-hooking anti-tamper library with no
# JNI exports (not the renderer its name suggests) and is deliberately not
# loaded; libnativeinterface.so is unrelated Samsung Zirconia DRM.
set(DATA_PATH "ux0:data/dungeon-hunter-2/" CACHE STRING "Path to data (with trailing /)")
set(SO_PATH "${DATA_PATH}libDungeonHunter2.so" CACHE STRING "Path to .so")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}")

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL")
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" OFF)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (DEFINED LOG_LEVEL)
  add_definitions(-DACTIVE_LOG_LEVEL=${LOG_LEVEL})
elseif (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER -DACTIVE_LOG_LEVEL=2)
else()
  add_definitions(-DRELEASE_BUILD -DACTIVE_LOG_LEVEL=1)
endif()

# --- Performance-investigation flags (Phase: FPS 8-14 -> target 20-30) -----
# Each is OFF by default so the normal build is never affected; porting_tools/
# build/build_and_install.sh exposes them as named, individually-buildable
# variants (see build.sh at the repo root), same pattern proven in the
# sibling Zenonia4-vita port's own perf-investigation phase.

# Diagnostic only: skips eglSwapInterval's vblank wait entirely. Causes
# visible tearing -- exists purely to answer "is our ~8-14 FPS artificially
# capped by vsync, or genuinely GPU/CPU-bound below the refresh rate?" before
# investing in the more invasive DOWNSAMPLE_RENDER below.
option(DISABLE_VSYNC "Diagnostic: skip eglSwapInterval's vblank wait (tearing, FPS-ceiling test only)" OFF)
if (DISABLE_VSYNC)
  add_definitions(-DDISABLE_VSYNC)
endif()

# Renders the whole scene (3D world + 2D HUD, same as the real device sees)
# into a reduced-resolution offscreen FBO, then upscales that FBO onto the
# real screen with one GL_LINEAR-filtered blit -- trades sharpness for GPU
# fill-rate/shading cost, which matters here because log_078 showed sustained
# combat scenes (particle effects, alpha-blended lighting) running at 8-14
# FPS, consistent with a fill-rate-bound bottleneck rather than draw-call
# count (log_075/076 already confirmed draw calls and GL state are healthy).
option(DOWNSAMPLE_RENDER "Render at reduced internal resolution, upscale via GPU blit (trades sharpness for fill-rate)" OFF)
set(DS_NUM "2" CACHE STRING "Downsample numerator -- render at DS_NUM/DS_DEN of native resolution")
set(DS_DEN "3" CACHE STRING "Downsample denominator")
if (DOWNSAMPLE_RENDER)
  add_definitions(-DDOWNSAMPLE_RENDER -DDS_NUM=${DS_NUM} -DDS_DEN=${DS_DEN})
endif()

# User confirmed DOWNSAMPLE_RENDER made no measurable difference -- rules out
# a fill-rate/pixel-shading bottleneck (fewer pixels to shade should have
# helped if that were it). DISABLE_VSYNC alone isn't enough to tell CPU-bound
# apart from GPU-bound either -- a capped eglSwapBuffers() could be waiting on
# vblank OR just waiting for the GPU to finish real work. This splits gl_swap()
# into two measured halves so which one actually dominates a slow frame is
# visible directly instead of guessed at (e.g. a hypothesized "weird NEON
# conversion" running every frame): time elapsed since the previous gl_swap()
# call returned (all game logic + every GL call this frame, i.e. CPU-side
# submission cost) vs. time spent inside eglSwapBuffers() itself (GPU
# flush/present + vblank wait, if any). Combine with DISABLE_VSYNC to strip
# the vblank wait out of the second number too.
option(PROFILE_FRAME_TIME "Diagnostic: log the CPU-submission vs eglSwapBuffers() time split every ~60 frames" OFF)
if (PROFILE_FRAME_TIME)
  add_definitions(-DPROFILE_FRAME_TIME)
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS_RELEASE "-O2 -g" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DEBUG "-O2 -g" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g" CACHE STRING "" FORCE)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -g -mfloat-abi=softfp -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=gnu++20 -Wno-write-strings -Wno-psabi")

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/video.cpp
               source/audio.cpp
               source/reimpl/bits/_ctype.c
               source/reimpl/errno.c
               source/reimpl/io.c
               source/reimpl/log.c
               source/reimpl/mem.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/asset_manager.cpp
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/init.c
               source/utils/logger.c
               source/utils/settings.c
               source/utils/utils.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      stdc++
                      gpu_es4_ext_stub
                      IMGEGL_stub
                      GLESv2_stub
                      GLESv1_CM_stub
                      taihen_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      SceAvPlayer_stub
                      SceSysmodule_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/sce_sys/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
                # libgpu_es4_ext.suprx and libIMGEGL.suprx are preloaded explicitly by
                # main.c via sceKernelLoadStartModule from app0: root -- matching
                # GrapheneCt/PVR_PSP2's own gles1test1 reference (SCE_USER_MODULE_LIST
                # covers only these two). libIMGEGL then lazily loads the other three
                # itself from "app0:module/..." (PVRSRVInitializeAppHint's own
                # compiled-in default paths) -- do NOT also preload those three
                # ourselves, see the comment in main.c for why that broke eglGetDisplay.
                ${CMAKE_SOURCE_DIR}/module/libgpu_es4_ext.suprx libgpu_es4_ext.suprx
                ${CMAKE_SOURCE_DIR}/module/libIMGEGL.suprx libIMGEGL.suprx
                ${CMAKE_SOURCE_DIR}/module/libGLESv2.suprx module/libGLESv2.suprx
                ${CMAKE_SOURCE_DIR}/module/libGLESv1_CM.suprx module/libGLESv1_CM.suprx
                ${CMAKE_SOURCE_DIR}/module/libpvrPSP2_WSEGL.suprx module/libpvrPSP2_WSEGL.suprx
                # DH2's engine tries to load "/system/fonts/droidsans.ttf" via FreeType (a real
                # Android font path that obviously doesn't exist on Vita) and gets a NULL FILE* --
                # source/reimpl/io.c redirects that exact path to this bundled, freely-redistributable
                # substitute font (Bitstream Vera/DejaVu license, not derived from Gameloft's assets).
                ${CMAKE_SOURCE_DIR}/extras/fonts/DejaVuSans.ttf DejaVuSans.ttf
                )

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
