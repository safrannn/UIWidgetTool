#!/usr/bin/env bash
# Compile check for the plugin (UE 5.8) without closing the editor.
#
# Usage:  ./build.sh            # editor target (both modules)
#         ./build.sh game       # UnrealGame target: the milestone 1 module-split
#                               # regression check, which must compile only the
#                               # runtime module
#
# Why this is not just "Build.bat UIWidgetToolEditor": UBT deletes stale
# hot-reload DLLs on every build and aborts when the running editor holds one,
# before compiling anything. So the plugin is compiled inside a throwaway host
# project instead.
#
# Why this is not "RunUAT BuildPlugin" either: BuildPlugin wipes the package
# directory and recompiles everything from scratch on every run, so there is no
# incremental build. Calling UBT directly against the host project that
# BuildPlugin generated reuses its build output, and it also lets us pass UBT
# flags that BuildPlugin cannot forward. -NoUBA is kept from the 5.6 days, when
# UBA stalled indefinitely on this machine under memory pressure; BuildPlugin on
# 5.8 has been seen to get through UBA, but the local executor is fast enough for
# a plugin this size and has no such failure mode.
#
# The host project is created once by:
#   RunUAT.bat BuildPlugin -Plugin=<uplugin> -Package=C:/_uiwt -NoTargetPlatforms
# and reused afterwards. Keep the package path short or UBT rejects the action
# graph on CheckPathLengths. Everything under C:/_uiwt is disposable: rerunning
# BuildPlugin wipes it (including the build log below).
set -u

UE="${UE:-/c/Program Files/Epic Games/UE_5.8}"
HOST="${HOST:-/c/_uiwt/HostProject}"
LOG="${LOG:-/c/_uiwt/build.log}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TARGET="UnrealEditor"
if [ "${1:-}" = "game" ]; then
  TARGET="UnrealGame"
fi

if [ ! -f "$HOST/HostProject.uproject" ]; then
  echo "No host project at $HOST. Create it once with:"
  echo "  \"$UE/Engine/Build/BatchFiles/RunUAT.bat\" BuildPlugin \\"
  echo "    -Plugin=\"$SRC/UIWidgetToolPlugin.uplugin\" -Package=C:/_uiwt -NoTargetPlatforms"
  exit 2
fi

# Mirror sources into the host project. Build output stays behind so an
# incremental compile is actually incremental.
DEST="$HOST/Plugins/UIWidgetToolPlugin"
rm -rf "$DEST/Source"
cp -r "$SRC/Source" "$DEST/Source"
cp "$SRC/UIWidgetToolPlugin.uplugin" "$DEST/UIWidgetToolPlugin.uplugin"

# 5.8 bundles DotNet/10.0, but the directory name changes with every engine
# release, so take whichever one this engine ships rather than hardcoding it.
DOTNET="$(ls -d "$UE"/Engine/Binaries/ThirdParty/DotNet/*/win-x64/dotnet.exe | head -1)"
UBT="$UE/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll"

"$DOTNET" "$(cygpath -w "$UBT")" "$TARGET" Win64 Development \
  -Project="$(cygpath -w "$HOST/HostProject.uproject")" \
  -plugin="$(cygpath -w "$DEST/UIWidgetToolPlugin.uplugin")" \
  -noubtmakefiles -nohotreload -NoUBA >"$LOG" 2>&1
STATUS=$?

echo "=== $TARGET: exit $STATUS, full log at $LOG ==="
grep -nE "error|warning C|Total execution time|Building [0-9]+ action" "$LOG" \
  | grep -viE "0 error|error-free" | tail -60
exit $STATUS
