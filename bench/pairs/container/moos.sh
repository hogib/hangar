#!/bin/sh
# bench/pairs/container/moos.sh ARCH TAG: inside the build container (Dockerfile here),
# core-moos from its pinned source (its library and its database, MOOSDB) and
# bench/pairs' two MOOS nodes linked with the library, for x86_64 or aarch64.
# tools/baselines.sh runs it, and only when asked to (--with-moos: the script's header
# says why). Nothing here is run on the Pi.
#
#   /src   this checkout, read-only          /moos  core-moos's unpacked source, read-only
#   /out   where bin/moos_pair, bin/moos_graph, bin/MOOSDB and the build tree go
#
# libMOOS is linked in statically, and so are libstdc++ and libgcc: the three programs
# need only the C library of the machine.
set -eu
arch=$1
tag=$2
case $arch in
x86_64) cxx=g++; tc= ;;
aarch64)
	cxx=aarch64-linux-gnu-g++
	cat > /out/toolchain-aarch64.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
	tc=-DCMAKE_TOOLCHAIN_FILE=/out/toolchain-aarch64.cmake ;;
*) echo "moos.sh: x86_64 or aarch64" >&2; exit 2 ;;
esac
# core-moos's build writes a header into its own source tree: a copy to build from,
# and a build tree made anew for it.
src=/out/src-$arch
rm -rf "$src" "/out/build-$arch"
mkdir -p "/out/build-$arch" /out/bin
cp -a /moos "$src"
# The header core-moos makes from git, which a release's source archive is not in:
# the tag in its place.
printf '#pragma once\n#define MOOS_GIT_VERSION "libMOOS from the source archive of tag %s"\n' "$tag" \
	> "$src/Core/libMOOS/include/MOOS/libMOOS/GitVersion.h"
cd /out/build-$arch
# shellcheck disable=SC2086
cmake -S "$src" -B . $tc -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DUPDATE_GIT_VERSION_INFO=OFF \
	-DENABLE_EXPORT=OFF -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" > cmake.log 2>&1 ||
	{ tail -30 cmake.log >&2; exit 1; }
cmake --build . -j "$(nproc)" > build.log 2>&1 || { tail -30 build.log >&2; exit 1; }
lib=$(find . -name 'libMOOS.a' | head -1)
db=$(find . -type f -name MOOSDB -perm -u+x | head -1)
[ -n "$lib" ] && [ -n "$db" ] || { echo "moos.sh: libMOOS.a or MOOSDB was not built" >&2; exit 1; }
cp "$db" /out/bin/MOOSDB
inc=$(find "$src/Core/libMOOS" -type d -name include | sed 's/^/-I/' | tr '\n' ' ')
flags="-std=c++11 -Wall -Wextra -O3"
for n in moos_pair moos_graph; do
	# shellcheck disable=SC2086
	$cxx $flags -DMOOS_TAG="\"$tag\"" -DPAIR_CFLAGS="\"$cxx $flags\"" -I/src/bench/pairs $inc -o /out/bin/$n \
		/src/bench/pairs/$n.cpp "$lib" -static-libstdc++ -static-libgcc -lpthread -lm
done
echo "moos.sh: $arch: /out/bin/moos_pair /out/bin/moos_graph /out/bin/MOOSDB"
