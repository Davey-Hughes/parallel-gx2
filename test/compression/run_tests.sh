#!/bin/sh
# zlib and Zstandard through libretro-common. RUNNER prefixes the test (wine
# for a MinGW build; the pack tool is the host's, built with HOSTCC).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 600 $RUN"
$RUN ./compression_test${EXE:-} fixture
