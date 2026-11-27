#!/bin/sh
# bench/pairs/container/lcm.sh ARCH: inside the build container (Dockerfile here), LCM's
# library from its pinned source and bench/pairs' two LCM nodes linked with it, for
# x86_64 or aarch64. tools/baselines.sh runs it; nothing here is run on the Pi.
#
#   /src   this checkout, read-only          /lcm   LCM's unpacked source, read-only
#   /out   where bin/lcm_pair, bin/lcm_graph and the build tree go
#
# LCM is linked in statically (its lcm-static target); GLib, which LCM needs, and the C
# library are the machine's own, shared: an Ubuntu 24.04 has both.
set -eu
arch=$1
case $arch in
x86_64) cc=gcc; pc=/usr/lib/x86_64-linux-gnu/pkgconfig; tc= ;;
aarch64)
	cc=aarch64-linux-gnu-gcc; pc=/usr/lib/aarch64-linux-gnu/pkgconfig
	cat > /out/toolchain-aarch64.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
	tc=-DCMAKE_TOOLCHAIN_FILE=/out/toolchain-aarch64.cmake ;;
*) echo "lcm.sh: x86_64 or aarch64" >&2; exit 2 ;;
esac
export PKG_CONFIG_LIBDIR=$pc:/usr/share/pkgconfig
mkdir -p /out/build-$arch /out/bin
cd /out/build-$arch
# shellcheck disable=SC2086
cmake -S /lcm -B . $tc -DCMAKE_BUILD_TYPE=Release -DLCM_ENABLE_EXAMPLES=OFF -DLCM_ENABLE_TESTS=OFF \
	-DLCM_ENABLE_PYTHON=OFF -DLCM_ENABLE_JAVA=OFF -DLCM_ENABLE_LUA=OFF -DLCM_ENABLE_GO=OFF > cmake.log 2>&1 ||
	{ tail -30 cmake.log >&2; exit 1; }
cmake --build . --target lcm-static -j "$(nproc)" > build.log 2>&1 || { tail -30 build.log >&2; exit 1; }
lib=$(find . -name 'liblcm.a' -o -name 'liblcm-static.a' | head -1)
[ -n "$lib" ] || { echo "lcm.sh: no static LCM library was built" >&2; exit 1; }
# The headers as an installed LCM has them: <lcm/lcm.h>, and the export header the
# build made.
mkdir -p inc/lcm
cp /lcm/lcm/*.h inc/lcm/
find . -path ./inc -prune -o -name '*.h' -exec cp {} inc/lcm/ \;
flags="-std=c11 -Wall -Wextra -O3"
for n in lcm_pair lcm_graph; do
	# shellcheck disable=SC2046
	$cc $flags -DPAIR_CFLAGS="\"$cc $flags\"" -I/src/bench/pairs -Iinc -o /out/bin/$n /src/bench/pairs/$n.c "$lib" \
		$(pkg-config --libs glib-2.0) -lpthread -lm
done
echo "lcm.sh: $arch: /out/bin/lcm_pair /out/bin/lcm_graph"
