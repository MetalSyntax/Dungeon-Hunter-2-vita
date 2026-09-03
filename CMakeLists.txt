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
# 3/4 = 720x408: 960*3/4=720 y 544*3/4=408, los dos exactos, asi que el aspect
# ratio queda CLAVADO en el de la Vita (720/408 == 960/544 == 30:17, sin barras
# ni deformacion) y quedan 56% de los pixeles. Se subio desde 1/2 (480x272, 25%
# de los pixeles) a pedido del usuario: 480x272 se veia bien y funcionaba, pero
# no dio ganancia real de FPS -- confirmando que este port no esta limitado por
# fill-rate -- asi que conviene recuperar nitidez y quedarse con la reduccion
# solo como algo que sume combinado con los otros cambios.
# Otros valores exactos utiles: 1/2 = 480x272, 7/8 = 840x476.
set(DS_NUM "3" CACHE STRING "Downsample numerator -- render at DS_NUM/DS_DEN of native resolution")
set(DS_DEN "4" CACHE STRING "Downsample denominator")
if (DOWNSAMPLE_RENDER)
  add_definitions(-DDOWNSAMPLE_RENDER -DDS_NUM=${DS_NUM} -DDS_DEN=${DS_DEN})
endif()

# OJO (2026-09-02): la frase original de este comentario decia que el usuario
# habia confirmado que DOWNSAMPLE_RENDER no hacia diferencia medible y que eso
# "descartaba un cuello de botella de fill-rate". Esa conclusion era INVALIDA:
# el FBO nunca llegaba a bindearse (glBindFramebuffer_soloader solo redirige el
# bind-a-0 del motor, y este motor nunca llama a glBindFramebuffer), asi que esa
# prueba corrio a 960x544 completos y no midio downsampling en absoluto. Ver
# init_downsample_render() en source/utils/glutil.c. Corregido; queda pendiente
# una medicion de verdad. DISABLE_VSYNC alone isn't enough to tell CPU-bound
# apart from GPU-bound either -- a capped eglSwapBuffers() could be waiting on
# vblank OR just waiting for the GPU to finish real work. This splits gl_swap()
# into two measured halves so which one actually dominates a slow frame is
# visible directly instead of guessed at (e.g. a hypothesized "weird NEON
# conversion" running every frame): time elapsed since the previous gl_swap()
# call returned (all game logic + every GL call this frame, i.e. CPU-side
# submission cost) vs. time spent inside eglSwapBuffers() itself (GPU
# flush/present + vblank wait, if any). Combine with DISABLE_VSYNC to strip
# the vblank wait out of the second number too.
option(PROFILE_FRAME_TIME "Diagnostic: log the CPU-submission vs eglSwapBuffers() time split every ~60 frames" ON)
add_definitions(-DPROFILE_FRAME_TIME)

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

# -O3 -ffast-math: mismo set exacto que usa Asphalt-5-Vita (CMakeLists.txt:139),
# que es el port mas comparable que tenemos -- mismo soloader (so_util +
# FalsoJNI), mismo motor Gameloft, misma vitaGL-desde-fuente -- y que corre a 60
# FPS sostenidos. DH2 estaba en -O2 sin -ffast-math.
# NOTA: -ffast-math relaja la semantica IEEE de punto flotante. Si aparece
# cualquier regresion visual/fisica despues de este cambio, ESTE es el primer
# flag a sacar (antes de revisar cualquier otra cosa de esta tanda).
set(CMAKE_C_FLAGS_RELEASE "-O3 -g -ffast-math" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -ffast-math" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DEBUG "-O3 -g -ffast-math" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG "-O3 -g -ffast-math" CACHE STRING "" FORCE)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O3 -g -ffast-math -mfloat-abi=softfp -Wno-deprecated")
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
               source/reimpl/egl.c
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

# Speedhacks de vitaGL. Los "Hack Flags" del README de vitaGL estan todos
# documentados como "may cause crashes/glitches", asi que el set agresivo vive
# en su propia variante de build (build.sh --speedhacks-test) y NUNCA en la
# build normal.
#
# Del set agresivo se EXCLUYEN a proposito, por razones concretas de este port:
#   * DEPTH_STENCIL_HACK=1  -> hace que depth/stencil no cuesten memoria, pero
#     DH2 depende CRITICAMENTE de un stencil real: toda su UI/HUD son clips de
#     GameSWF dibujados con mascara de stencil. Ya se comprobo (log_001.log) que
#     sin stencil no se dibuja NADA.
#   * PRIMITIVES_SPEEDHACK=1 -> glitchea GL_LINES/GL_POINTS, y GameSWF dibuja
#     formas vectoriales.
#   * INDICES_SPEEDHACK=1    -> rompe draws instanciados e indices de 32 bits.
#   * TEXTURES_SPEEDHACK=1   -> incompatible con HAVE_TEXTURE_CACHE.
# Modo de culling -- ver el comentario extenso en source/patch.c. Es la palanca
# de rendimiento mas grande del port (el bypass total hace que el motor anime y
# actualice el nivel ENTERO cada frame) y tambien la mas riesgosa: el bypass es
# lo que arreglo el bug de enemigos invisibles. Default 0 = comportamiento
# historico, sin cambios.
set(CULLING_MODE "0" CACHE STRING "0=bypass total (default/seguro), 1=culling de update activo, 2=culling stock del motor")
add_definitions(-DCULLING_MODE=${CULLING_MODE})

option(VITAGL_EXTRA_SPEEDHACKS "Compilar vitaGL con el set agresivo de speedhacks (puede crashear/glitchear)" OFF)
if (VITAGL_EXTRA_SPEEDHACKS)
  # DRAW_SPEEDHACK sube de 2 (solo draws con mucha data de vertices) a 1 (todos).
  set(VITAGL_MAKE_FLAGS_STR "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 HAVE_GLSL_UBOS=1 SAMPLERS_SPEEDHACK=1 DRAW_SPEEDHACK=1 INDICES_DRAW_SPEEDHACK=1 BUFFERS_SPEEDHACK=1 MATH_SPEEDHACK=1 CIRCULAR_POOL_SPEEDHACK=1 TEXTURE_UPLOADS_SPEEDHACK=1")
else()
  set(VITAGL_MAKE_FLAGS_STR "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 SAMPLERS_SPEEDHACK=1 HAVE_GLSL_UBOS=1")
endif()
message(STATUS "vitaGL flags: ${VITAGL_MAKE_FLAGS_STR}")
separate_arguments(VITAGL_MAKE_FLAGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS_STR}")
set(VITAGL_STAMP "${CMAKE_BINARY_DIR}/vitagl.stamp")

# NOTE: this is a custom_target (always re-evaluated), not an OUTPUT-based
# add_custom_command. A stamp-file OUTPUT with no DEPENDS is only ever run once
# per build tree: once vitagl.stamp exists, Make/Ninja consider it up to date
# forever and will silently skip this recipe on every later build, even after
# VITAGL_MAKE_FLAGS changes above -- which is exactly what happened across the
# several flag tweaks here (SAMPLERS_SPEEDHACK/HAVE_GLSL_UBOS never actually got
# compiled in unless the build dir was wiped). Making this a custom_target means
# its COMMANDs run every build; build_vitagl.sh's own cmp-against-stamp check
# still decides whether a `make clean` is actually needed, and the following
# `make` is incremental/cheap when nothing changed.
add_custom_target(vitaGL_lib
  COMMAND rm -f "${VITAGL_STAMP}.tmp"
  COMMAND echo "${VITAGL_MAKE_FLAGS_STR}" > "${VITAGL_STAMP}.tmp"
  COMMAND bash "${CMAKE_SOURCE_DIR}/lib/vitagl/build_vitagl.sh" "${VITAGL_MAKE_FLAGS_STR}" "${VITAGL_STAMP}" "${CMAKE_SOURCE_DIR}/lib/vitagl"
  COMMAND make -C "${CMAKE_SOURCE_DIR}/lib/vitagl" ${VITAGL_MAKE_FLAGS}
  BYPRODUCTS "${CMAKE_SOURCE_DIR}/lib/vitagl/libvitaGL.a"
  COMMENT "Building vitaGL with custom flags..."
)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_lib)

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      stdc++
                      ${CMAKE_SOURCE_DIR}/lib/vitagl/libvitaGL.a
                      vitashark
                      SceShaccCgExt
                      SceShaccCg_stub
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
