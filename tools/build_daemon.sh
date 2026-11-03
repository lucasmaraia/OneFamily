#!/bin/sh
# Build the OneFamily daemon payload (bin/onefamily.bin).
#
# Mirrors build_daemon.ps1, but takes the toolchain from OO_PS4_TOOLCHAIN instead
# of a hardcoded path, so it runs in the Docker image or on any host that has the
# OpenOrbis toolchain plus clang/ld.lld.
set -eu

TOOLCHAIN="${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN to the OpenOrbis PS4Toolchain directory}"
test -f "$TOOLCHAIN/link.x" || { echo "no link.x in $TOOLCHAIN" >&2; exit 1; }

# EXTRA_CFLAGS adds flags -- the diagnostic build passes -DONEFAMILY_BT_DIAG.
# OUT_NAME picks the output basename, so a diag build sits beside the normal one
# and keeps its own object directory.
EXTRA_CFLAGS="${EXTRA_CFLAGS:-}"
OUT_NAME="${OUT_NAME:-onefamily}"

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON="$PROJECT/daemon"
OBJ="$DAEMON/obj/$OUT_NAME"
OUT="$PROJECT/bin"
mkdir -p "$OBJ" "$OUT"

# Source list and flags kept in step with build_daemon.ps1.
SRCS="daemon_syscalls main vpad usb_driver config log controller_detect translator generic_hid bt_driver"

echo "=== Compiling OneFamily daemon sources (toolchain: $TOOLCHAIN) ==="
OBJS=""
for n in $SRCS; do
    echo "  CC $n.c"
    clang \
        --target=x86_64-pc-freebsd12-elf \
        -fPIC -funwind-tables -fno-delete-null-pointer-checks \
        -c -Wall \
        -isysroot "$TOOLCHAIN" \
        -isystem "$TOOLCHAIN/include" \
        -isystem "$TOOLCHAIN/include/orbis/_types" \
        -I"$DAEMON/include" \
        -D__PS4__ -D__ORBIS__ $EXTRA_CFLAGS \
        -o "$OBJ/$n.o" "$DAEMON/src/$n.c"
    OBJS="$OBJS $OBJ/$n.o"
done

echo "=== Linking bin/$OUT_NAME.elf ==="
# shellcheck disable=SC2086
ld.lld \
    -m elf_x86_64 -pie \
    --script "$TOOLCHAIN/link.x" \
    --eh-frame-hdr \
    -L"$TOOLCHAIN/lib" \
    -o "$OUT/$OUT_NAME.elf" \
    $OBJS -lc -lkernel

cp "$OUT/$OUT_NAME.elf" "$OUT/$OUT_NAME.bin"
echo "=== Done: bin/$OUT_NAME.elf and bin/$OUT_NAME.bin ==="
ls -l "$OUT/$OUT_NAME.elf" "$OUT/$OUT_NAME.bin"
