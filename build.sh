#!/bin/bash
set -e

# Uso: ./build.sh                 -> build normal, build/dungeon_hunter_2.vpk
#      ./build.sh debug|release|relwithdebinfo|minsizerel
#                                   -> mismo build normal pero con CMAKE_BUILD_TYPE
#                                    fijo y SIN preguntar por stdin (release ->
#                                    Release, etc.) -- convencion que espera
#                                    psvita-toolkit al invocar
#                                    `bash build.sh <preset>` de forma no
#                                    interactiva. Se puede combinar con
#                                    cualquiera de los flags de diagnostico de
#                                    abajo, ej. `./build.sh release --downsample-test`.
#      ./build.sh --no-vsync-test  -> build de diagnostico (Fase performance,
#                                    ver PORTING_PLAN.md) que deshabilita la
#                                    espera de vblank de eglSwapInterval
#                                    (DISABLE_VSYNC=ON), build/
#                                    dungeon_hunter_2_novsync_test.vpk --
#                                    causa tearing, solo sirve para responder
#                                    "¿nuestro FPS medido esta atado
#                                    artificialmente al refresco de pantalla,
#                                    o ya estamos genuinamente por debajo de
#                                    eso?" antes de invertir en algo mas
#                                    invasivo como --downsample-test.
#      ./build.sh --culling-test [MODE]
#                                   -> LA palanca de perf mas grande. MODE 1
#                                    (default) restaura el culling de update;
#                                    MODE 2 restaura el culling stock completo
#                                    (riesgo alto de devolver el bug de
#                                    enemigos invisibles). Ver source/patch.c.
#      ./build.sh --speedhacks-test -> build con el set agresivo de speedhacks
#                                    de vitaGL (DRAW_SPEEDHACK=1,
#                                    INDICES_DRAW/BUFFERS/MATH/CIRCULAR_POOL/
#                                    TEXTURE_UPLOADS), a resolucion nativa.
#                                    Puede crashear o glitchear: si falla, ir
#                                    quitando flags de VITAGL_EXTRA_SPEEDHACKS
#                                    en CMakeLists.txt de a uno.
#      ./build.sh --downsample-test [DS_NUM] [DS_DEN]
#                                   -> build experimental que renderiza toda
#                                    la escena (mundo 3D + HUD 2D) a un FBO
#                                    fuera de pantalla en resolucion reducida
#                                    (DS_NUM/DS_DEN de la nativa, default 2/3)
#                                    y lo escala a la pantalla real con un
#                                    blit GL_LINEAR (DOWNSAMPLE_RENDER=ON),
#                                    build/dungeon_hunter_2_downsample_test.vpk
#                                    -- reduce el costo de fill-rate/shading
#                                    de la GPU a costa de nitidez. Ratio mas
#                                    leve: --downsample-test 3 4 (3/4 en vez
#                                    de 2/3).
#      ./build.sh --profile-frame-time
#                                   -> build de diagnostico (PROFILE_FRAME_TIME=ON,
#                                    junto con DISABLE_VSYNC=ON para que el
#                                    numero de swap no quede tapado por la
#                                    espera de vblank) que loguea cada ~60
#                                    frames cuanto tiempo se va en trabajo de
#                                    CPU (logica de juego + todas las llamadas
#                                    GL de ese frame) vs. cuanto tiempo se va
#                                    DENTRO de eglSwapBuffers (flush/present de
#                                    GPU) -- responde "es esto CPU-bound
#                                    (logica de juego, una conversion rara,
#                                    etc.) o GPU-bound" con datos en vez de
#                                    adivinar, ya que DOWNSAMPLE_RENDER (menos
#                                    pixels a sombrear) no mostro ninguna
#                                    mejora medible -- descarta fill-rate como
#                                    sospechoso principal.
#                                    build/dungeon_hunter_2_profile_test.vpk
#      ./build.sh --shader-cache-test
#                                   -> build experimental (DUMP_COMPILED_SHADERS=ON,
#                                    ver PORTING_PLAN.md Fase 20) que cachea a
#                                    disco (ux0:data/dungeon-hunter-2/shader_cache/)
#                                    el binario COMPILADO Y LINKEADO de cada
#                                    shader program via GL_OES_get_program_binary,
#                                    para saltear glLinkProgram (y probablemente
#                                    buena parte del compile GLSL en el driver
#                                    PowerVR) la proxima vez que se vea el mismo
#                                    par de shaders -- reduce el costo de
#                                    pantallas de carga, no el FPS en combate.
#                                    Soporte de esa extension en este build de
#                                    PVR_PSP2 esta SIN CONFIRMAR -- si no esta
#                                    disponible el codigo lo detecta en runtime
#                                    y compila/linkea todo normal, sin cambio de
#                                    comportamiento. build/dungeon_hunter_2_shader_cache_test.vpk
#
# Los flags de perf/diagnostico son opciones de CACHE de CMake -- invocar
# este script con un modo y despues con otro en el MISMO BUILD_DIR (siempre
# /tmp/dh2-build) haria que un flag no mencionado en la segunda invocacion
# quedara "pegado" del valor cacheado de la primera (mismo bug real que
# encontro el port de Zenonia4 en un escenario equivalente). Fix: SIEMPRE se
# fijan todos los flags de forma explicita (ON u OFF) y se borra
# CMakeCache.txt antes de configurar, nunca se deja ninguno sin mencionar.

VPK_OUTPUT_NAME="dungeon_hunter_2"
CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=OFF -DDOWNSAMPLE_RENDER=OFF -DPROFILE_FRAME_TIME=OFF -DDUMP_COMPILED_SHADERS=OFF)

# Extraemos un preset universal (debug/release/relwithdebinfo/minsizerel) de
# los argumentos si esta presente, dejando el resto (los flags de diagnostico
# de abajo, que siguen usando posicion $1/$2/$3) intacto -- ver comentario de
# cabecera. psvita-toolkit siempre pasa el preset como primer argumento.
BUILD_TYPE_EXPLICIT=""
REMAINING_ARGS=()
for arg in "$@"; do
    case "$arg" in
        debug|Debug|DEBUG) BUILD_TYPE_EXPLICIT="Debug" ;;
        release|Release|RELEASE) BUILD_TYPE_EXPLICIT="Release" ;;
        relwithdebinfo|RelWithDebInfo|RELWITHDEBINFO) BUILD_TYPE_EXPLICIT="RelWithDebInfo" ;;
        minsizerel|MinSizeRel|MINSIZEREL) BUILD_TYPE_EXPLICIT="MinSizeRel" ;;
        *) REMAINING_ARGS+=("$arg") ;;
    esac
done
set -- "${REMAINING_ARGS[@]}"

if [ "$1" = "--no-vsync-test" ]; then
    VPK_OUTPUT_NAME="dungeon_hunter_2_novsync_test"
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=ON -DDOWNSAMPLE_RENDER=OFF -DPROFILE_FRAME_TIME=OFF -DDUMP_COMPILED_SHADERS=OFF)
elif [ "$1" = "--profile-frame-time" ]; then
    VPK_OUTPUT_NAME="dungeon_hunter_2_profile_test"
    # DISABLE_VSYNC=ON junto con esto -- ver el comentario de cabecera: si no,
    # el numero de "eglSwapBuffers" queda dominado por la espera de vblank y
    # no dice nada sobre cuanto tiempo de GPU real se esta usando.
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=ON -DDOWNSAMPLE_RENDER=OFF -DPROFILE_FRAME_TIME=ON -DDUMP_COMPILED_SHADERS=OFF)
elif [ "$1" = "--downsample-test" ]; then
    VPK_OUTPUT_NAME="dungeon_hunter_2_downsample_test"
    # Default 3/4 = 720x408: exacto en los dos ejes, mismo aspect ratio de la
    # Vita, 56% de los pixeles. Ver el comentario de DS_NUM/DS_DEN en
    # CMakeLists.txt.
    DS_N="${2:-3}"; DS_D="${3:-4}"
    # PROFILE_FRAME_TIME=ON: el punto entero de esta variante es MEDIR si bajar
    # la resolucion mueve la aguja, y sin el split CPU-submission/eglSwapBuffers
    # el log solo da [fps] y no se puede saber que mitad cambio.
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=OFF -DDOWNSAMPLE_RENDER=ON -DPROFILE_FRAME_TIME=ON -DDUMP_COMPILED_SHADERS=OFF -DDS_NUM="$DS_N" -DDS_DEN="$DS_D")
elif [ "$1" = "--culling-test" ]; then
    # CULLING_MODE=1 por default: restaura solo el culling del lado del UPDATE y
    # deja los dos isCulled bypasseados, que es el escalon medible de menor
    # riesgo (ver source/patch.c). `--culling-test 2` prueba el culling stock
    # completo, mucho mas probable que devuelva el bug de invisibilidad.
    CULL_M="${2:-1}"
    VPK_OUTPUT_NAME="dungeon_hunter_2_culling_test${CULL_M}"
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=OFF -DDOWNSAMPLE_RENDER=OFF -DPROFILE_FRAME_TIME=ON -DDUMP_COMPILED_SHADERS=OFF -DCULLING_MODE="$CULL_M")
elif [ "$1" = "--speedhacks-test" ]; then
    VPK_OUTPUT_NAME="dungeon_hunter_2_speedhacks_test"
    # Set agresivo de speedhacks de vitaGL, a resolucion NATIVA para que sea
    # comparable 1:1 contra la build normal (una variable a la vez).
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=OFF -DDOWNSAMPLE_RENDER=OFF -DPROFILE_FRAME_TIME=ON -DDUMP_COMPILED_SHADERS=OFF -DVITAGL_EXTRA_SPEEDHACKS=ON)
elif [ "$1" = "--shader-cache-test" ]; then
    VPK_OUTPUT_NAME="dungeon_hunter_2_shader_cache_test"
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=OFF -DDOWNSAMPLE_RENDER=OFF -DPROFILE_FRAME_TIME=OFF -DDUMP_COMPILED_SHADERS=ON)
elif [ -n "$1" ]; then
    echo "Error: flag desconocido '$1'. Ver el comentario de cabecera de este script para la lista completa."
    exit 1
fi

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="/tmp/dh2-build"
SRC_DIR="/tmp/dh2-src"
VPK_NAME="${VPK_OUTPUT_NAME}.vpk"

echo "================================================================"
echo "  Script de Build para Dungeon Hunter 2 (PS Vita)"
echo "================================================================"

echo "[1/3] Preparando entorno de compilacion..."
# Evitamos el bug de vita-pack-vpk con rutas que contienen espacios
# ("PSVITA Develop") usando un directorio temporal en /tmp -- ver
# PORTING_PLAN.md Fase 0 / psvita-porting skill, toolchain_gotchas.md.
mkdir -p "$BUILD_DIR"
mkdir -p "$SRC_DIR"

if [ -z "$VITASDK" ]; then
    if [ -d "/usr/local/vitasdk" ]; then
        export VITASDK="/usr/local/vitasdk"
    elif [ -d "$HOME/vitasdk" ]; then
        export VITASDK="$HOME/vitasdk"
    else
        echo "Error: La variable de entorno VITASDK no esta definida y no se encontro en rutas por defecto."
        exit 1
    fi
    export PATH="$VITASDK/bin:$PATH"
fi

rsync -a \
    --exclude '.git' --exclude 'build' --exclude '.*' \
    --exclude 'decompiled' \
    --exclude 'Dungeon-Hunter-2-HD-v1-0-2' \
    --exclude 'com.gameloft.android.GAND.GloftD2SS' \
    --exclude '*.apk' --exclude '*.zip' \
    "$PROJECT_DIR/" "$SRC_DIR/"

echo "[2/3] Ejecutando CMake y Make (${1:-build normal})..."

if [ -n "$BUILD_TYPE_EXPLICIT" ]; then
    BUILD_TYPE="$BUILD_TYPE_EXPLICIT"
    echo "Preset explicito: CMAKE_BUILD_TYPE=$BUILD_TYPE (sin prompt)"
else
    read -p "¿Build de depuracion (logging detallado, DEBUG_SOLOADER)? [S/n] " DEBUG_OPTION
    if [[ "$DEBUG_OPTION" =~ ^[nN]$ ]]; then
        BUILD_TYPE="Release"
    else
        BUILD_TYPE="Debug"
    fi
fi

# Ver el comentario de cabecera: nunca dejar un flag sin mencionar entre
# corridas con distinto modo. Borrar solo CMakeCache.txt NO alcanza para
# CMAKE_BUILD_TYPE -- con el generador "Unix Makefiles" (el default de este
# script, no se pasa -G), make no recompila un .c/.cpp cuyo mtime no cambio
# aunque los flags del compilador sí (a diferencia de Ninja, que hashea la
# linea de comando). Resultado real: alternar Debug/Release en el mismo
# BUILD_DIR podía linkear un binario mitad compilado con -DDEBUG_SOLOADER y
# mitad sin él, dependiendo de qué .o sobrevivía de la corrida anterior --
# esto es lo que hacía parecer que "los parches gráficos dependen de modo
# debug" al probar Release por primera vez. Borrar todo el directorio (es
# solo un scratch dir en /tmp) garantiza una recompilación limpia siempre.
# Importante: pararse afuera de BUILD_DIR (cd "$SRC_DIR") ANTES del rm -rf --
# borrar el cwd actual del proceso lo deja sin working directory valido
# ("Current working directory cannot be established"), haciendo fallar el
# cmake/make que sigue.
cd "$SRC_DIR"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SRC_DIR" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${CMAKE_EXTRA_ARGS[@]}"
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

echo "[3/3] Exportando archivos generados..."
mkdir -p "$PROJECT_DIR/build"
cp "$VPK_NAME" "$PROJECT_DIR/build/$VPK_NAME"
if [ -f "eboot.bin" ]; then
    cp "eboot.bin" "$PROJECT_DIR/build/eboot_${VPK_OUTPUT_NAME}.bin"
    if [ -z "$1" ]; then
        # SOLO el build normal (sin flag) toca la copia "canonica" sin sufijo
        # que usa manage_vita.py -> "Subir SOLO el eboot.bin". Un build con
        # flag (--no-vsync-test, --downsample-test, etc.) pisaba esta copia
        # sin avisar -- si compilabas dos variantes en la misma sesion,
        # "subir solo eboot.bin" terminaba subiendo la ULTIMA compilada sin
        # que nada lo indicara. Cada variante ahora vive solo en su
        # eboot_<nombre>.bin, nunca en el eboot.bin generico.
        cp "eboot.bin" "$PROJECT_DIR/build/eboot.bin"
    fi
fi
# El ELF con simbolos es imprescindible para simbolizar un .psp2dmp con
# vita-parse-core; /tmp se borra al reiniciar, asi que se archiva junto al VPK.
if [ -f "dungeon_hunter_2" ]; then
    cp "dungeon_hunter_2" "$PROJECT_DIR/build/${VPK_OUTPUT_NAME}.elf"
fi

echo ""
echo "Build exitoso: $PROJECT_DIR/build/$VPK_NAME"
echo "eboot.bin de ESTA build: $PROJECT_DIR/build/eboot_${VPK_OUTPUT_NAME}.bin"
if [ -z "$1" ]; then
    echo "(build normal -- tambien actualizo el eboot.bin generico que usa 'Subir SOLO el eboot.bin')"
else
    echo "(build con flag -- el eboot.bin generico NO se tocó; usa el VPK completo o elegí el eboot_${VPK_OUTPUT_NAME}.bin a mano)"
fi
echo "Para instalar en un Vita real, usar: psvita-toolkit deploy --project \"$PROJECT_DIR\""
