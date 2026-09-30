#!/usr/bin/env bash
# ELF -> C# (existing recompiler, incl. the post-pass patch) -> C (cs2c.py)
# for the native armv7 runtime in CrashBandicoot.Native.
#
#   ./scripts/prerecompile_native.sh /path/to/game.cue
#
# Needs: .NET 10 SDK on PATH, python3. The disc image is only read at build
# time to recompile the executable; it is never copied into the app.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CUE_PATH="${1:?Usage: prerecompile_native.sh <path-to-game.cue>}"
CONFIG_PATH="$ROOT/CrashBandicoot.Launcher/Recomp/CrashBandicoot.json"
CS_DIR="$ROOT/build/recompiled_cs"
GEN_DIR="$ROOT/CrashBandicoot.Native/gen"

rm -rf "$CS_DIR" "$GEN_DIR"
mkdir -p "$CS_DIR" "$GEN_DIR"

dotnet run --project "$ROOT/tools/CrashBandicoot.PreRecompiler" -c Release -- \
    "$CONFIG_PATH" "$CUE_PATH" "$CS_DIR"

echo "[native] converting C# -> C"
python3 "$ROOT/CrashBandicoot.Native/tools/cs2c.py" "$CS_DIR" "$GEN_DIR" --split "${SPLIT:-150}"
ls "$GEN_DIR" | head -5
echo "[native] $(ls "$GEN_DIR"/*.c | wc -l) C files, $(du -sh "$GEN_DIR" | cut -f1) total"
