#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
force=false
while (($#)); do case "$1" in --force) force=true;; -h|--help) echo "usage: $0 [--force]"; exit 0;; *) echo "unknown option: $1" >&2; exit 2;; esac; shift; done
image="$root/image/root.img"
builder_image="$root/emulation/image/root.img"
if [[ -s "$image" && "$force" != true ]]; then echo "VM image already exists: $image"; exit 0; fi
[[ -x "$root/emulation/image/make_vm_img.sh" ]] || { echo "missing repository image builder" >&2; exit 2; }
echo "building independent tigonkv image via emulation/image/make_vm_img.sh"
if [[ "$force" == true && -e "$image" ]]; then
  echo "refusing to overwrite existing image without an explicit destination" >&2
  exit 2
fi
"$root/emulation/image/make_vm_img.sh"
[[ -s "$builder_image" ]] || { echo "image builder did not produce $builder_image" >&2; exit 2; }
mkdir -p "$root/image"
cp --reflink=auto "$builder_image" "$image"
echo "TIGONKV_VM_IMAGE image=$image"
