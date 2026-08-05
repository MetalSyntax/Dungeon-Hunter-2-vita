#!/bin/bash
set -e

# Configuración
PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_SH="$PROJECT_DIR/build.sh"

echo "================================================================"
echo "  Script de Build Automatico para Dungeon Hunter 2 (PS Vita)"
echo "================================================================"

if [ ! -x "$BUILD_SH" ]; then
    echo "Error: no se encontro (o no es ejecutable) $BUILD_SH"
    exit 1
fi

# Descripciones cortas para el menu. La lista de flags REALES se descubre
# dinamicamente de build.sh (grep de sus propios "elif [ "$1" = "--xxx" ]" /
# "if [ "$1" = "--xxx" ]") en vez de mantenerse hardcodeada aca -- asi este
# script nunca queda desincronizado cuando se agrega un flag nuevo a
# build.sh (si aparece un flag sin descripcion en el case de abajo, el menu
# lo muestra igual con una descripcion generica, ver flag_desc()). Mismo
# patron que porting_tools/build/build_and_install.sh del port de Zenonia4.
# Un `case` en vez de un array asociativo porque el bash 3.2 que trae macOS
# por default no tiene arrays asociativos (`declare -A`).
flag_desc() {
    case "$1" in
        --no-vsync-test) echo "Diagnostico de performance: deshabilita la espera de vblank (causa tearing, solo para medir el techo real de FPS)" ;;
        --downsample-test) echo "Performance: renderiza a resolucion interna reducida + upscale GPU (pide ratio DS_NUM/DS_DEN, default 2/3)" ;;
        *) echo "(sin descripcion corta -- ver el comentario de este flag en build.sh)" ;;
    esac
}

# Flags reales: se extraen de las condiciones "$1" = "--xxx" de build.sh, en
# el orden en que aparecen en el archivo. Loop portable en vez de `mapfile`
# (no existe en el bash 3.2 que trae macOS por default).
FLAGS=()
while IFS= read -r line; do
    FLAGS+=("$line")
done < <(grep -o '"\-\-[a-z0-9-]\+"' "$BUILD_SH" | tr -d '"' | awk '!seen[$0]++')

echo ""
echo "[1/2] Elegi que build de $(basename "$BUILD_SH") queres compilar:"
echo "  0) (build normal, sin flag) -- build/dungeon_hunter_2.vpk"
i=1
for f in "${FLAGS[@]}"; do
    printf "  %d) %-20s -- %s\n" "$i" "$f" "$(flag_desc "$f")"
    i=$((i + 1))
done
echo ""
read -p "Numero de opcion [0]: " FLAG_CHOICE
FLAG_CHOICE="${FLAG_CHOICE:-0}"

BUILD_FLAG=""
if [ "$FLAG_CHOICE" != "0" ]; then
    idx=$((FLAG_CHOICE - 1))
    if [ "$idx" -lt 0 ] || [ "$idx" -ge "${#FLAGS[@]}" ]; then
        echo "Opcion invalida."
        exit 1
    fi
    BUILD_FLAG="${FLAGS[$idx]}"
fi

BUILD_ARGS=()
if [ -n "$BUILD_FLAG" ]; then
    BUILD_ARGS+=("$BUILD_FLAG")
fi

# --downsample-test acepta 2 argumentos posicionales extra (DS_NUM/DS_DEN,
# default 2/3 -- ver el comentario de este flag en build.sh).
if [ "$BUILD_FLAG" = "--downsample-test" ]; then
    read -p "Ratio de downsample DS_NUM/DS_DEN [2/3]: " DS_RATIO
    DS_RATIO="${DS_RATIO:-2/3}"
    DS_NUM="${DS_RATIO%%/*}"
    DS_DEN="${DS_RATIO##*/}"
    BUILD_ARGS+=("$DS_NUM" "$DS_DEN")
fi

echo ""
echo "[2/2] Ejecutando build.sh ${BUILD_ARGS[*]} ..."
echo "(build.sh ya se encarga del staging en /tmp para evitar el bug de"
echo " vita-pack-vpk con rutas con espacios -- ver toolchain_gotchas.md)"
"$BUILD_SH" "${BUILD_ARGS[@]}"

# VPK_OUTPUT_NAME real que usa build.sh para este flag (misma logica de
# nombres que build.sh, para saber que archivo buscar en build/ despues).
VPK_OUTPUT_NAME="dungeon_hunter_2"
case "$BUILD_FLAG" in
    --no-vsync-test) VPK_OUTPUT_NAME="dungeon_hunter_2_novsync_test" ;;
    --downsample-test) VPK_OUTPUT_NAME="dungeon_hunter_2_downsample_test" ;;
esac
VPK_NAME="${VPK_OUTPUT_NAME}.vpk"
VPK_PATH="$PROJECT_DIR/build/$VPK_NAME"

if [ ! -f "$VPK_PATH" ]; then
    echo "Error: build.sh termino pero no encuentro $VPK_PATH -- revisar el nombre de VPK_OUTPUT_NAME arriba."
    exit 1
fi
echo ""
echo "Build exitoso: $VPK_PATH"
echo "ELF con simbolos: $PROJECT_DIR/build/${VPK_OUTPUT_NAME}.elf"
echo "eboot.bin (para 'Subir SOLO el eboot.bin' de manage_vita.py): $PROJECT_DIR/build/eboot.bin"

VITA3K_APP="/Applications/Vita3K.app/Contents/MacOS/Vita3K"
if [ -x "$VITA3K_APP" ]; then
    read -p "¿Deseas instalar y ejecutar $VPK_NAME en Vita3K ahora? [s/N] " INSTALL_VITA3K
    if [[ "$INSTALL_VITA3K" =~ ^[sS]$ ]]; then
        echo "Instalando VPK y lanzando el emulador (backend OpenGL)..."
        "$VITA3K_APP" -B OpenGL "$VPK_PATH" > /dev/null 2>&1 &
        echo "Listo."
    else
        echo "Omitiendo instalacion automatica en Vita3K."
    fi
else
    echo "Vita3K no encontrado en la ruta por defecto (/Applications/Vita3K.app)."
    echo "Puedes instalar el archivo $VPK_PATH manualmente en tu emulador o consola."
fi
echo "Para instalar en un Vita real (FTP+comando de VitaShell), usar porting_tools/manage_vita.py."
