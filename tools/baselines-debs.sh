#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/baselines-debs.sh: the .deb files the Pi needs for the ROS 2 baselines of
# bench/pairs (Cyclone DDS, Zenoh), fetched here, on the laptop, so that nothing at
# the lab runs `apt update` (it starts the package machinery the session keeps
# quiet: tools/rov/README.md, "Freeze and thaw").
#
#   tools/baselines-debs.sh [--status FILE] [--out DIR] [--image IMAGE] [PACKAGE...]
#
# It asks apt, in a container, what an arm64 Ubuntu 24.04 with the packages of FILE
# installed would have to download to install PACKAGE... (default:
# ros-jazzy-rmw-cyclonedds-cpp ros-jazzy-rmw-zenoh-cpp), downloads exactly those
# files into DIR (default out/baselines/debs), checks each against the SHA-256 apt
# gave, and writes SHA256SUMS and LIST there. Nothing is installed anywhere.
#
# FILE is the Pi's own package list, one "name version architecture" a line:
#
#   ssh bluerov2@<the Pi> "dpkg-query -W -f='\${Package} \${Version} \${Architecture}\n'" > pi-packages.txt
#
# WITHOUT --status the list of the container image itself (ros:jazzy-ros-base, as
# of today) stands in for the Pi's. That is a guess: a Pi whose ROS 2 was installed
# earlier has older packages, and apt may then want newer versions of some it
# already has (they are listed under "upgrades" at the end: read that line). Give
# the Pi's list before the visit.
#
# On the Pi, frozen, with DIR copied there:   sudo dpkg -i DIR/*.deb
# (lab.sh baselines does it, and shows the command first).
#
# Needs docker and curl. The container only computes the list; the downloads are
# made here, from the URLs apt printed (ports.ubuntu.com, packages.ros.org).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
status= out=$ROOT/out/baselines/debs image=ros:jazzy-ros-base
pkgs=()
while [ $# -gt 0 ]; do
	case $1 in
	--status) shift; status=$1 ;;
	--out) shift; out=$1 ;;
	--image) shift; image=$1 ;;
	-h|--help) sed -n '3,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) echo "baselines-debs.sh: unknown option $1" >&2; exit 2 ;;
	*) pkgs+=("$1") ;;
	esac
	shift
done
[ ${#pkgs[@]} -gt 0 ] || pkgs=(ros-jazzy-rmw-cyclonedds-cpp ros-jazzy-rmw-zenoh-cpp)
for tool in docker curl sha256sum; do
	command -v "$tool" > /dev/null || { echo "baselines-debs.sh: $tool is not installed" >&2; exit 1; }
done
[ -z "$status" ] || [ -r "$status" ] || { echo "baselines-debs.sh: $status cannot be read" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
[ -z "$status" ] || cp "$status" "$work/pi-packages.txt"

# Inside the container: an apt that believes it is an arm64 machine with the given
# packages installed, and prints what it would fetch. Its own lists, cache and
# status, so the container's real package state is not touched either.
cat > "$work/list.sh" <<'EOF'
set -eu
x=/tmp/apt; mkdir -p $x/lists/partial $x/cache/archives/partial
src=$(grep -lE 'packages\.ros\.org/ros2/ubuntu' /etc/apt/sources.list.d/* 2>/dev/null | head -1)
[ -n "$src" ] || { echo "no ROS 2 apt source in this image" >&2; exit 1; }
mkdir -p $x/parts
case $src in
*.sources) # deb822, with its key inside: the same stanza, for arm64 binaries only
	sed -e 's/^Types:.*/Types: deb/' -e '/^Architectures:/d' "$src" | awk '{ print } /^Types:/ { print "Architectures: arm64" }' > $x/parts/ros2.sources ;;
*)	sed -E 's/^deb(-src)? (\[[^]]*\] )?/deb [arch=arm64] /; /^deb-src/d' "$src" | sed -E "s|\[arch=arm64\]|[arch=arm64$(grep -hoE ' signed-by=[^] ]+' "$src" | head -1)]|" > $x/parts/ros2.list ;;
esac
cat > $x/parts/ubuntu-ports.sources <<P
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: noble noble-updates noble-security
Components: main universe
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
P
: > $x/sources.list
if [ -r /w/pi-packages.txt ]; then
	awk 'NF >= 2 { a = ($3 == "" ? "arm64" : $3); printf "Package: %s\nStatus: install ok installed\nVersion: %s\nArchitecture: %s\n\n", $1, $2, a }' /w/pi-packages.txt > $x/status
else
	dpkg-query -W -f='Package: ${Package}\nStatus: install ok installed\nVersion: ${Version}\nArchitecture: ${Architecture}\n\n' \
		| sed 's/^Architecture: amd64$/Architecture: arm64/' > $x/status
fi
o="-o Dir::Etc::sourcelist=$x/sources.list -o Dir::Etc::sourceparts=$x/parts -o Dir::State::lists=$x/lists -o Dir::Cache=$x/cache
   -o Dir::State::status=$x/status -o APT::Architecture=arm64 -o APT::Architectures=arm64 -o Debug::NoLocking=1"
apt-get $o update -q > /w/update.log 2>&1 || { cat /w/update.log >&2; exit 1; }
apt-get $o install -y --no-install-recommends --print-uris "$@" > /w/uris.txt 2> /w/install.err || { cat /w/install.err >&2; exit 1; }
apt-get $o install -y --no-install-recommends -s "$@" > /w/simulate.txt 2>&1 || true
EOF
docker run --rm -v "$work:/w" "$image" sh /w/list.sh "${pkgs[@]}"

mkdir -p "$out"
: > "$out/SHA256SUMS"
: > "$out/LIST"
n=0
# A line of --print-uris: 'URL' file size ALGORITHM:hash (SHA256 from Ubuntu's archive;
# whatever the repository's index gives otherwise). Each file is checked against apt's
# hash with apt's algorithm, and its SHA-256 written down whichever that was.
grep "^'" "$work/uris.txt" | while read -r url file size sum; do
	url=${url#\'}; url=${url%\'}
	algo=${sum%%:*}; want=${sum#*:}
	case $algo in
	SHA256) tool=sha256sum ;;
	SHA512) tool=sha512sum ;;
	SHA1) tool=sha1sum ;;
	MD5Sum) tool=md5sum ;;
	*) echo "baselines-debs.sh: $file: apt gave no hash this script knows ($sum)" >&2; exit 1 ;;
	esac
	if [ ! -f "$out/$file" ] || [ "$($tool < "$out/$file" | cut -d' ' -f1)" != "$want" ]; then
		curl -fsSL --retry 3 -o "$out/$file.part" "$url"
		mv "$out/$file.part" "$out/$file"
	fi
	got=$($tool < "$out/$file" | cut -d' ' -f1)
	[ "$got" = "$want" ] || { echo "baselines-debs.sh: $file: $algo $got, apt said $want" >&2; exit 1; }
	echo "$(sha256sum < "$out/$file" | cut -d' ' -f1)  $file" >> "$out/SHA256SUMS"
	echo "$file $size $algo $url" >> "$out/LIST"
done
n=$(wc -l < "$out/LIST")
# Files of an earlier run that are no longer wanted would be installed too: say so.
for f in "$out"/*.deb; do
	[ -e "$f" ] || continue
	grep -q "  $(basename "$f")\$" "$out/SHA256SUMS" || echo "baselines-debs.sh: NOTE $(basename "$f") is in $out and not in this list: remove it" >&2
done
up=$(grep -E '^[0-9]+ upgraded' "$work/simulate.txt" || true)
{
	echo "packages asked for: ${pkgs[*]}"
	echo "against: ${status:+the package list $status}${status:-the image $image itself (a guess at the Pi: see the script)}"
	echo "apt: ${up:-no summary line}"
	grep -E '^(Inst|Conf) ' "$work/simulate.txt" | grep '^Inst' | sed 's/^Inst //' | sed 's/^/  /'
} > "$out/README.txt"
echo "baselines-debs.sh: $n files in $out ($(du -sh "$out" | cut -f1)); apt: ${up:-?}"
if grep -qE '^[1-9][0-9]* upgraded' "$work/simulate.txt"; then
	echo "baselines-debs.sh: NOTE apt would UPGRADE packages the Pi already has (in $out/README.txt, the lines with a version in [brackets]): look before installing" >&2
fi
