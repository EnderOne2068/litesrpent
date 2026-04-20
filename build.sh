#!/bin/bash
# build.sh -- Build Litesrpent using Git Bash / MSYS2 / Linux
#
# Usage:
#   ./build.sh              -- standard debug build
#   ./build.sh release      -- optimized release build
#   ./build.sh clean        -- remove build artifacts
#   ./build.sh amalgamate   -- create single-file amalgamation

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$PROJECT_ROOT/src"
INC="$PROJECT_ROOT/include"
BUILD="$PROJECT_ROOT/build"
OUT="$BUILD/litesrpent"

# Find GCC
GCC=""
if [ -x "$PROJECT_ROOT/third_party/mingw64/bin/gcc.exe" ]; then
    GCC="$PROJECT_ROOT/third_party/mingw64/bin/gcc.exe"
    echo "Using bundled MinGW: $GCC"
elif command -v gcc &>/dev/null; then
    GCC="gcc"
    echo "Using system GCC: $(which gcc)"
else
    echo "ERROR: No C compiler found"
    exit 1
fi

SOURCES="$SRC"/*.c

CFLAGS="-I$INC -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -D_CRT_SECURE_NO_WARNINGS"

# Platform-specific libs and Windows resource (icon + version info).
RESOURCE_OBJ=""
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "mingw"* || "$OSTYPE" == "cygwin" ]]; then
    OUT="$BUILD/litesrpent.exe"
    LIBS="-lm -lkernel32 -luser32 -lgdi32"

    # Compile litesrpent.rc -> litesrpent.res.o so the icon ends up in
    # the PE resource section.  Skip silently if windres isn't there.
    if [ -f "$PROJECT_ROOT/litesrpent.rc" ]; then
        WINDRES=""
        if [ -x "$PROJECT_ROOT/third_party/mingw64/bin/windres.exe" ]; then
            WINDRES="$PROJECT_ROOT/third_party/mingw64/bin/windres.exe"
        elif command -v windres &>/dev/null; then
            WINDRES="windres"
        fi
        if [ -n "$WINDRES" ]; then
            mkdir -p "$BUILD"
            RESOURCE_OBJ="$BUILD/litesrpent.res.o"
            echo "Compiling resource: litesrpent.rc -> $(basename "$RESOURCE_OBJ")"
            "$WINDRES" -I "$PROJECT_ROOT" \
                       -i "$PROJECT_ROOT/litesrpent.rc" \
                       -O coff \
                       -o "$RESOURCE_OBJ"
        else
            echo "Note: windres not found; building without embedded icon."
        fi
    fi
else
    LIBS="-lm -ldl"
fi

case "$1" in
    clean)
        echo "Cleaning..."
        rm -rf "$BUILD"
        echo "Done."
        exit 0
        ;;
    amalgamate)
        exec bash "$PROJECT_ROOT/tools/amalgamate.sh"
        ;;
    release)
        echo "Building Litesrpent [RELEASE]..."
        CFLAGS="$CFLAGS -O2 -DNDEBUG -flto"
        ;;
    *)
        echo "Building Litesrpent [DEBUG]..."
        CFLAGS="$CFLAGS -g -O0"
        ;;
esac

mkdir -p "$BUILD"

echo "Compiling $(echo $SOURCES | wc -w) source files..."
$GCC $CFLAGS $SOURCES $RESOURCE_OBJ -o "$OUT" $LIBS

echo ""
echo "Build successful: $OUT"
echo "Size: $(du -h "$OUT" | cut -f1)"
"$OUT" --version
