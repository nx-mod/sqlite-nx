#!/bin/bash
# Builds sqlite-nx (static lib) inside the devkitPro MSYS2 shell.
# Invoked standalone from msys2:
#   bash build.sh [target] [jobs]
#     target  all (default) | clean

source /etc/profile.d/devkit-env.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

# Point at the flat-cloned libnx fork, not devkitPro's stock system libnx --
# matches every other project's build.sh in this repo.
export LIBNX="$SCRIPT_DIR/../libnx/nx"

TARGET="${1:-all}"
JOBS="${2:-2}"

echo "DEVKITPRO=$DEVKITPRO"
echo "Target=$TARGET Jobs=$JOBS CWD=$(pwd)"

if [ "$TARGET" = "clean" ]; then
    make clean
    exit $?
fi

make -j"$JOBS" "$TARGET"
STATUS=$?

if [ $STATUS -eq 0 ]; then
    echo "== Build ok. Output: lib/libSQLite.a =="
fi

exit $STATUS
