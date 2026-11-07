#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
shopt -s nullglob
shaders=("$root"/src/shaders/msl/*.metal)
if (( ${#shaders[@]} == 0 )); then
    echo "No shaders found in src/shaders/msl" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
failures=()
for shader in "${shaders[@]}"; do
    name="${shader##*/}"
    echo "Compiling $name"
    if ! xcrun --sdk iphoneos metal -std=metal3.0 -target air64-apple-ios16.0 -c "$shader" -o "$work/${name%.metal}.air"; then
        failures+=("$name")
    fi
done

if (( ${#failures[@]} != 0 )); then
    printf 'Failed shaders (%s of %s):\n' "${#failures[@]}" "${#shaders[@]}" >&2
    printf '  %s\n' "${failures[@]}" >&2
    exit 1
fi
printf 'Compiled %s Metal shaders.\n' "${#shaders[@]}"
