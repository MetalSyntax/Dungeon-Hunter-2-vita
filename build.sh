#!/bin/bash
set -e

# Uso: ./build.sh                 -> build normal, build/dungeon_hunter_2.vpk
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
#
# Los flags de perf/diagnostico son opciones de CACHE de CMake -- invocar
# este script con un modo y despues con otro en el MISMO BUILD_DIR (siempre
# /tmp/dh2-build) haria que un flag no mencionado en la segunda invocacion
# quedara "pegado" del valor cacheado de la primera (mismo bug real que
# encontro el port de Zenonia4 en un escenario equivalente). Fix: SIEMPRE se
# fijan todos los flags de forma explicita (ON u OFF) y se borra
# CMakeCache.txt antes de configurar, nunca se deja ninguno sin mencionar.

VPK_OUTPUT_NAME="dungeon_hunter_2"
CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=OFF -DDOWNSAMPLE_RENDER=OFF)

if [ "$1" = "--no-vsync-test" ]; then
    VPK_OUTPUT_NAME="dungeon_hunter_2_novsync_test"
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=ON -DDOWNSAMPLE_RENDER=OFF)
elif [ "$1" = "--downsample-test" ]; then
    VPK_OUTPUT_NAME="dungeon_hunter_2_downsample_test"
    DS_N="${2:-2}"; DS_D="${3:-3}"
    CMAKE_EXTRA_ARGS=(-DVITA_VPKNAME="$VPK_OUTPUT_NAME" -DDISABLE_VSYNC=OFF -DDOWNSAMPLE_RENDER=ON -DDS_NUM="$DS_N" -DDS_DEN="$DS_D")
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
cd "$BUILD_DIR"

read -p "¿Build de depuracion (logging detallado, DEBUG_SOLOADER)? [S/n] " DEBUG_OPTION
if [[ "$DEBUG_OPTION" =~ ^[nN]$ ]]; then
    BUILD_TYPE="Release"
else
    BUILD_TYPE="Debug"
fi

# Ver el comentario de cabecera: nunca dejar un flag sin mencionar entre
# corridas con distinto modo.
rm -f "$BUILD_DIR/CMakeCache.txt"

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
echo "Para instalar en un Vita real o Vita3K, usar porting_tools/manage_vita.py."
