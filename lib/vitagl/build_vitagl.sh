#!/bin/bash
MAKE_FLAGS="$1"
STAMP="$2"
SRC_DIR="$3"

echo "$MAKE_FLAGS" > "${STAMP}.tmp"
if ! cmp -s "${STAMP}.tmp" "$STAMP"; then
    echo "vitaGL flags changed, rebuilding..."
    cd "$SRC_DIR" && make clean
    cp "${STAMP}.tmp" "$STAMP"
fi
