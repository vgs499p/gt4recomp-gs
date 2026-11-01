#!/bin/sh
# GT4Recomp fork: lays out the macOS gt4-gs folder (the game runs it as <game folder>/gs/gt4-gs).
#
#   gt4gs/package-macos.sh <build dir> <empty output dir>
#
# Output: gt4-gs, resources/ (bin/resources without the Windows-only dx11 shaders) and the Metal shader library
# resources/Metal23.metallib (macOS 11+ target, the library GSMTLDeviceInfo.mm loads first), plus COPYING.GPLv3,
# NOTICE, README.md and SOURCE.txt. The CMake build of the Qt app compiles the shaders into its bundle; gt4-gs is a
# plain executable, so the library sits in resources/ (EmuFolders::Resources = <executable folder>/resources).
set -eu
build="$1"
out="$2"
root=$(cd "$(dirname "$0")/.." && pwd)
if [ -e "$out" ] && [ -n "$(ls -A "$out")" ]; then
	echo "output directory is not empty: $out" >&2
	exit 1
fi
mkdir -p "$out/resources" "$out/air"
out=$(cd "$out" && pwd)
build=$(cd "$build" && pwd)
cp "$build/pcsx2-gt4gs/gt4-gs" "$out/gt4-gs"
(cd "$root/bin/resources" && find . -type f ! -name '.*' ! -path '*/dx11/*' -print0 | xargs -0 -I{} sh -c \
	'mkdir -p "$1/$(dirname "$2")" && cp "$2" "$1/$2"' _ "$out/resources" {})
for shader in cas convert present merge misc interlace tfx fxaa; do
	xcrun metal -ffast-math -std=macos-metal2.3 -target air64-apple-macos11.0 \
		-o "$out/air/$shader.air" -c "$root/pcsx2/GS/Renderers/Metal/$shader.metal"
done
xcrun metallib -o "$out/resources/Metal23.metallib" "$out"/air/*.air
rm -rf "$out/air"
cp "$root/COPYING.GPLv3" "$root/gt4gs/NOTICE" "$root/gt4gs/README.md" "$out/"
{
	echo "source: https://github.com/moesuito/gt4recomp-gs (GT4Recomp fork of PCSX2, GPL-3.0-or-later)"
	echo "commit: $(git -C "$root" rev-parse HEAD)"
	echo "platform: macOS $(uname -m)"
} > "$out/SOURCE.txt"
echo "gt4-gs folder ready: $out"
