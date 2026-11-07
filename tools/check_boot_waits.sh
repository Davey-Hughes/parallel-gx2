#!/bin/sh
# The waits on the way from loading a title to its first frame sleep until
# what they wait for has happened (LatteGpuWake, ThreadStartedFlag), not by
# the clock. Each of them used to check every 1 to 50 ms, and the boot paid for
# every one: the GPU thread's start, the title starting, the graphic packs,
# the GPU's init, GX2Init, the IOSU services' threads. The GPU thread's pause
# gate went the same way, from a mutex and a condition variable to the wake.
#
# Checked in the source as the libretro build sees it (includes dropped, so
# only the file's own conditionals decide): no sleep_for in any of these
# functions, and the wake each one waits on is there.
cd "$(dirname "$0")/.." || exit 1
CXX=${CXX:-c++}
fail=0

pp()
{
	sed '/^[[:space:]]*#[[:space:]]*include/d' "$1" |
		$CXX -E -P -DENABLE_LIBRETRO -DENABLE_VULKAN -x c++ - 2>/dev/null
}

# the body of the function whose definition line matches $2, in file $1
body()
{
	pp "$1" | awk -v start="$2" '
		!on && $0 ~ start { on = 1 }
		on {
			print
			n += gsub(/\{/, "{"); n -= gsub(/\}/, "}")
			if (seen && n == 0) exit
			if (n > 0) seen = 1
		}'
}

# check FILE FUNCTION-REGEX MUST-CONTAIN
check()
{
	b=$(body "$1" "$2")
	if [ -z "$b" ]; then
		echo "$1: $2 not found"; fail=1; return
	fi
	if echo "$b" | grep -q 'sleep_for'; then
		echo "$1: $2 sleeps by the clock:"; echo "$b" | grep -n 'sleep_for'; fail=1
	fi
	if ! echo "$b" | grep -q "$3"; then
		echo "$1: $2 does not wait on $3"; fail=1
	fi
}

L=src/Cafe/HW/Latte/Core/LatteThread.cpp
check $L '^void Latte_Start\(\)' LatteGpuWake_WaitUntil
check $L '^int Latte_ThreadEntry\(\)' LatteGpuWake_WaitUntil
check $L '^void Latte_GpuPauseGate\(\)' LatteGpuWake_WaitUntil
check src/Cafe/GraphicPack/GraphicPack2.cpp '^void GraphicPack2::WaitUntilReady\(\)' LatteGpuWake_WaitUntil
check src/Cafe/CafeSystem.cpp '^void cemu_initForGame\(\)' LatteGpuWake_WaitUntil
check src/libretro/CemuLibretro.cpp 'libretro_prepare_and_launch_title\(\)$' LatteGpuWake_WaitUntil
check src/Cafe/IOSU/nn/iosu_nn_service.cpp 'void IPCSimpleService::Start\(\)' 'm_threadInitialized.Wait()'
check src/Cafe/IOSU/nn/iosu_nn_service.cpp 'void IPCService::Start\(\)' 'm_threadInitialized.Wait()'
check src/Cafe/IOSU/ODM/iosu_odm.cpp 'void Initialize\(\)' 's_threadInitialized.Wait()'

if pp $L | grep -q 'condition_variable'; then
	echo "$L: the pause gate is back on a condition variable"; fail=1
fi

# What wakes them: every one of these has to notify, or a wait above lasts
# until something else happens to.
need()
{
	if ! pp "$1" | grep -A2 -- "$2" | grep -q "$3"; then
		echo "$1: $2 is not followed by $3"; fail=1
	fi
}
need src/Cafe/CafeSystem.cpp 'sSystemRunning = true;' LatteGpuWake_Notify
need $L 'sLatteThreadFinishedInit = true;' LatteGpuWake_Notify
need $L 'g_isGPUInitFinished = true;' LatteGpuWake_Notify
need $L 'sLatteThreadAlive.store(false' LatteGpuWake_Notify
need $L 'sGpuPauseRequested.store(false' LatteGpuWake_Notify
need src/Cafe/GraphicPack/GraphicPack2.cpp 's_isReady = true;' LatteGpuWake_Notify
need src/Cafe/OS/libs/gx2/GX2_Misc.cpp 'gx2InitCalled++;' LatteGpuWake_Notify
need src/Cafe/OS/libs/coreinit/coreinit_OSScreen.cpp 'flipRequestCount++;' LatteGpuWake_Notify

[ $fail -eq 0 ] && echo "boot waits sleep until woken, none by the clock"
exit $fail
