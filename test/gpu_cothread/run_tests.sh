#!/bin/sh
# The OpenGL GPU as a cothread of the frontend's thread (LatteGpuCothread.h):
# the run/yield protocol, its exit and restart, who may run it, and libco
# switching on several threads at once the way the core does. RUNNER prefixes
# the test (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 120 $RUN"
$RUN ./gpu_cothread_test${EXE:-}
