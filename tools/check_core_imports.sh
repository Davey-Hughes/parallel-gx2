#!/bin/sh
# Fails if the core links a GL, EGL, GLX or X11 library, or imports a symbol
# from one: under libretro the GL context and every GL function belong to the
# frontend (get_proc_address), and a core that pulls in libGL or libX11 does
# not even load on a system without them. ELF (.so) and PE (.dll).
set -e
core=$1
[ -f "$core" ] || { echo "usage: $0 <core library>"; exit 2; }
case "$core" in
*.dll)
	OBJDUMP=${OBJDUMP:-x86_64-w64-mingw32-objdump}
	libs=$($OBJDUMP -p "$core" | sed -n 's/^\s*DLL Name: //p')
	bad=$(printf '%s\n' "$libs" | grep -i -E '^(opengl32|glu32)\.dll$' || true)
	syms=$($OBJDUMP -p "$core" | grep -E '\b(wgl|gl)[A-Z][A-Za-z0-9]*$' || true)
	;;
*)
	READELF=${READELF:-readelf}
	libs=$($READELF -d "$core" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
	bad=$(printf '%s\n' "$libs" | grep -E '^lib(GL|GLX|EGL|OpenGL|GLESv[0-9]|X11|xcb|Xrender|wayland-)' || true)
	syms=$($READELF --dyn-syms -W "$core" | awk '$7 == "UND" {print $8}' | grep -E '^(gl|glX|egl|X)[A-Z]' || true)
	;;
esac
if [ -n "$bad$syms" ]; then
	echo "$core takes from the system what is the frontend's:"
	[ -n "$bad" ] && printf '  library %s\n' $bad
	[ -n "$syms" ] && printf '  symbol %s\n' $syms
	exit 1
fi
echo "$core: no GL, EGL, GLX or X11 linked"
