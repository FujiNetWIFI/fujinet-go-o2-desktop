#!/usr/bin/env bash
# gen_dasm_golden.sh -- regenerate core/tests/dasm_golden.h from MAME's own
# MCS-48 disassembler (src/devices/cpu/mcs48/mcs48dsm.cpp), compiled
# standalone with a short emu.h shim. Needs a MAME source tree and g++.
#
#   tools/ab/gen_dasm_golden.sh              MAME_SRC defaults to ~/Workspace/mame
#   MAME_SRC=/path/to/mame tools/ab/gen_dasm_golden.sh
#
# The header is committed; this runs only when the table changes or MAME's
# disassembler does.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MAME_SRC="${MAME_SRC:-$HOME/Workspace/mame}/src"
CXX="${CXX:-g++}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/shim"
cat > "$WORK/shim/emu.h" <<'SHIM'
// stand-in for MAME's emu.h: mcs48dsm.cpp needs only these
#pragma once
#include <cstdint>
#include "disasmintf.h"
#include "strformat.h"
using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
using offs_t = u32;
SHIM

"$CXX" -std=c++20 -O1 -I "$WORK/shim" -I "$MAME_SRC/lib/util" -I "$MAME_SRC/osd" \
    -I "$MAME_SRC/devices/cpu/mcs48" \
    "$HERE/mcs48_dasm_golden.cpp" \
    "$MAME_SRC/devices/cpu/mcs48/mcs48dsm.cpp" \
    "$MAME_SRC/lib/util/disasmintf.cpp" "$MAME_SRC/lib/util/strformat.cpp" \
    -o "$WORK/gen"
"$WORK/gen" > "$ROOT/core/tests/dasm_golden.h"
echo "$ROOT/core/tests/dasm_golden.h: $(wc -l < "$ROOT/core/tests/dasm_golden.h") lines"
