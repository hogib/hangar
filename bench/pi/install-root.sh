#!/bin/sh
# install-root.sh: lays out a hangar release under one directory, $HANGAR_ROOT,
# for a test run without touching /etc, /usr or /var (bench/PI-TEST.md).
#
#   sh share/hangar/install-root.sh [--system] [--role vehicle|topside] <dir>
#
# Run from the extracted release (it finds its own files relative to itself).
# Copies bin/ and share/, writes <dir>/etc/hangard.conf and the phase-A
# manifests of docs/11 to <dir>/etc/modules/ with @ROOT@ replaced by <dir>,
# and gives eeye and hellion their configuration files under <dir>/var/eeye
# and <dir>/etc/hellion if they have none yet. Without --system the manifests'
# User= and Group= lines are commented out, since hangard can switch users
# only when it runs as root. Idempotent: existing configuration is kept.
set -eu

here=$(cd "$(dirname "$0")/../.." && pwd)   # the release root: bin/, share/
system=no
role=vehicle
root=
while [ $# -gt 0 ]; do
	case "$1" in
	--system) system=yes ;;
	--role) shift; role=$1 ;;
	-h|--help) sed -n '2,15p' "$0"; exit 0 ;;
	-*) echo "install-root.sh: unknown option $1" >&2; exit 2 ;;
	*) root=$1 ;;
	esac
	shift
done
[ -n "$root" ] || { echo "usage: install-root.sh [--system] [--role vehicle|topside] <dir>" >&2; exit 2; }
mkdir -p "$root"
root=$(cd "$root" && pwd)

for d in bin share etc/modules etc/hellion var/eeye var/hellion var/recordings log; do
	mkdir -p "$root/$d"
done
cp -p "$here"/bin/* "$root/bin/"
cp -Rp "$here"/share/. "$root/share/"
chmod 755 "$root"/bin/*

subst() { sed -e "s|@ROOT@|$root|g" "$1"; }

subst "$here/share/hangar/hangard.conf" | sed -e "s|^Role=.*|Role=$role|" > "$root/etc/hangard.conf"
for m in "$here"/share/hangar/modules/*.module; do
	name=$(basename "$m")
	if [ "$system" = yes ]; then
		subst "$m" > "$root/etc/modules/$name"
	else
		subst "$m" | sed -e 's|^User=|# User=|' -e 's|^Group=|# Group=|' > "$root/etc/modules/$name"
	fi
done
[ -e "$root/var/eeye/effects.json" ] || cp "$here/share/hangar/eeye/effects.json" "$root/var/eeye/effects.json"
if [ ! -e "$root/etc/hellion/hellion.json" ]; then
	sed -e "s|/var/lib/eeye/effects.json|$root/var/eeye/effects.json|" \
	    "$here/share/hangar/hellion/hellion.json" > "$root/etc/hellion/hellion.json"
fi

echo "hangar root: $root"
echo "  binaries   $(ls "$root/bin" | tr '\n' ' ')"
echo "  manifests  $(ls "$root/etc/modules" | tr '\n' ' ')"
echo "  manager    $root/etc/hangard.conf (Role=$role)"
echo "run:  HANGAR_ROOT=$root $root/bin/hangard -c $root/etc/hangard.conf"
