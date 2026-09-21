#!/usr/bin/env bash
# Build the distributable plugin zip (UE 5.8 editor, Win64) into Release/.
#
# Usage:  ./build_release.sh
#
# Output: Release/UIWidgetToolPlugin-<version>-UE5.8-Win64.zip, where <version>
# is VersionName from the .uplugin. A zip with the same name is overwritten;
# bump the version first for a new release.
#
# The zip is editor-only on purpose: it carries the UnrealEditor DLLs and no
# precompiled UnrealGame objects, so the plugin is not built into packaged games
# (-NoTargetPlatforms below). Recipients drop the folder into <Project>/Plugins/.
#
# The build runs through RunUAT BuildPlugin, which compiles from scratch in a
# throwaway host project (~2 min). It is deliberately separate from build.sh's
# host at C:/_uiwt: BuildPlugin wipes its -Package path on every run and would
# destroy that incremental build. Keep PKG short or UBT rejects the action graph
# on CheckPathLengths. See DEVELOPER.md "Packaging for distribution" for what
# ends up in the zip and how to verify it in a fresh Blueprint project.
set -u

UE="${UE:-/c/Program Files/Epic Games/UE_5.8}"
PKG="${PKG:-/c/_uiwtpkg}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE="$SRC/Release"
LOG="$PKG/build.log"

VERSION="$(grep -oE '"VersionName"\s*:\s*"[^"]+"' "$SRC/UIWidgetToolPlugin.uplugin" | sed -E 's/.*"([^"]+)"$/\1/')"
if [ -z "$VERSION" ]; then
  echo "Could not read VersionName from UIWidgetToolPlugin.uplugin" >&2
  exit 2
fi
ZIP="$RELEASE/UIWidgetToolPlugin-$VERSION-UE5.8-Win64.zip"

OUT="$PKG/UIWidgetToolPlugin"
rm -rf "$PKG"
mkdir -p "$PKG"

# --- 1. BuildPlugin -------------------------------------------------------
echo "=== BuildPlugin $VERSION -> $OUT (log: $LOG)"
"$UE/Engine/Build/BatchFiles/RunUAT.bat" BuildPlugin \
  -Plugin="$(cygpath -w "$SRC/UIWidgetToolPlugin.uplugin")" \
  -Package="$(cygpath -w "$OUT")" \
  -NoTargetPlatforms >"$LOG" 2>&1
STATUS=$?
if [ $STATUS -ne 0 ]; then
  echo "=== BuildPlugin failed (exit $STATUS)"
  grep -nE "error|Error:" "$LOG" | grep -viE "0 error" | tail -30
  exit $STATUS
fi
grep -E "warning C" "$LOG" | sort -u | head -20

# --- 2. Stage and zip -----------------------------------------------------
# Stage a copy under a folder literally named UIWidgetToolPlugin so that is the
# zip's top-level entry, minus the PDBs (~144 MB) and the build intermediates.
# Windows' bsdtar writes a zip when the output name ends in .zip; Git Bash has
# no zip binary of its own.
STAGE="$PKG/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE" "$RELEASE"
cp -r "$OUT" "$STAGE/UIWidgetToolPlugin"
rm -f "$STAGE"/UIWidgetToolPlugin/Binaries/Win64/*.pdb
rm -rf "$STAGE/UIWidgetToolPlugin/Intermediate"
rm -f "$ZIP"
(cd "$STAGE" && /c/Windows/System32/tar.exe -a -c -f "$(cygpath -w "$ZIP")" UIWidgetToolPlugin) || exit 1
rm -rf "$STAGE"

# The BuildPlugin output with PDBs stays in $PKG until the next run.
echo "=== $(du -h "$ZIP" | cut -f1)  $ZIP"
echo "=== done; symbols kept at $OUT/Binaries/Win64"
