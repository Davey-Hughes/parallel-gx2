#!/bin/sh
# The Vulkan renderer's shader compiler. The graphic packs are a submodule
# (dependencies/cemu_graphic_packs); with spirv-val on the PATH every module
# is validated as well. RUNNER prefixes the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 600 $RUN"
$RUN ./shaders_test${EXE:-} ../../dependencies/cemu_graphic_packs "$OUT"
if command -v spirv-val >/dev/null 2>&1; then
	n=0
	for f in "$OUT"/*.spv; do
		spirv-val --target-env vulkan1.1 "$f" || { echo "invalid: $f"; exit 1; }
		n=$((n + 1))
	done
	echo "spirv-val: $n modules valid"
fi
