#!/bin/sh
# Build the firmware in the pinned toolchain container (Dockerfile, gcc
# 10.3.1), exactly as the release workflow does, so building a tag here
# gives the same bytes as the release.
#
#   ./compile-with-docker.sh                           release build
#   ./compile-with-docker.sh VERSION_STRING=v1.2.3     with a version
#   ./compile-with-docker.sh --out out/bench bench     bench build (0x0602 raw register writes)
#
# Output in compiled-firmware/ (or --out DIR): firmware.bin (raw),
# firmware.packed.bin (for the bootloader and flashers), firmware.elf.
# The source is copied to a temporary directory first (tracked files plus
# untracked ones that are not ignored), so the tree is never modified.
set -eu
cd "$(dirname "$0")"

OUT=compiled-firmware
if [ "${1:-}" = "--out" ]; then
	OUT="$2"
	shift 2
fi
IMAGE="k5-packet-fw-build:gcc10.3.1"

docker build -q -t "$IMAGE" - < Dockerfile >/dev/null

WORK="$(mktemp -d "${TMPDIR:-/tmp}/k5-packet-fw.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
git ls-files -z -co --exclude-standard | tar --null -T - -cf - | tar -C "$WORK" -xf -
find "$WORK" \( -name '*.o' -o -name '*.d' -o -name .build-options \) -delete

# the copy has no .git: work the version out here, unless given
case " $* " in
	*" VERSION_STRING="*) ;;
	*)
		v="$(git describe --tags --exact-match 2>/dev/null || git rev-parse --short=7 HEAD)"
		[ -z "$(git status --porcelain --untracked-files=no)" ] || echo "warning: uncommitted changes; the version says $v" >&2
		set -- "VERSION_STRING=$v" "$@"
		;;
esac

docker run --rm -u "$(id -u):$(id -g)" -v "$WORK:/src" -w /src "$IMAGE" \
	make -j"$(nproc 2>/dev/null || echo 4)" "$@"

mkdir -p "$OUT"
cp "$WORK/firmware" "$OUT/firmware.elf"
cp "$WORK/firmware.bin" "$WORK/firmware.packed.bin" "$OUT/"
ls -l "$OUT/firmware.bin" "$OUT/firmware.packed.bin"
