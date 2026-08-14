#!/usr/bin/env bash
# Runs the ELF -> C# recompilation step (once, ahead of the AOT iOS build)
# and drops the generated sources where CrashBandicoot.IosHost.csproj picks
# them up (Recompiled/**/*.cs). See tools/CrashBandicoot.PreRecompiler for
# why this has to happen before `dotnet build`, not inside the app.
#
# Usage:
#   ./scripts/prerecompile.sh /path/to/game.cue
#
# The .cue/.bin are never committed to the repo (copyright) - you provide
# your own legally-dumped disc image, locally or as a GitHub Actions secret
# path on a self-hosted runner.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CUE_PATH="${1:?Usage: prerecompile.sh <path-to-game.cue>}"
CONFIG_PATH="$ROOT/CrashBandicoot.Launcher/Recomp/CrashBandicoot.json"
OUT_DIR="$ROOT/CrashBandicoot.IosHost/Recompiled"

echo "[prerecompile] config = $CONFIG_PATH"
echo "[prerecompile] cue    = $CUE_PATH"
echo "[prerecompile] out    = $OUT_DIR"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

dotnet run --project "$ROOT/tools/CrashBandicoot.PreRecompiler" -c Release -- \
    "$CONFIG_PATH" "$CUE_PATH" "$OUT_DIR"

count=$(find "$OUT_DIR" -name '*.cs' | wc -l | tr -d ' ')
echo "[prerecompile] wrote $count .cs files to $OUT_DIR"
