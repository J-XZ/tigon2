#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Copyright (C) 2022 Intel Corporation. All rights reserved.

set -euo pipefail

# $1: mkosi extra tree root
if [ $# -ne 1 ] || [ -z "${1:-}" ]; then
	echo "usage: $0 <mkosi.extra path>" >&2
	exit 2
fi

# If set to 0/false/no, skip copying host APT config.
# (Useful when you want the image build to rely only on mkosi/mirror settings.)
COPY_HOST_APT_CONFIG="${COPY_HOST_APT_CONFIG:-1}"
case "${COPY_HOST_APT_CONFIG,,}" in
	0|false|no)
		exit 0
		;;
esac

# Copy host APT configuration into the mkosi extra tree to preserve proxy/mirror settings.
mkdir -p "$1/etc/apt/"

apt_conf=/etc/apt/apt.conf
if [ -f "$apt_conf" ]; then
	cp -L "$apt_conf" "$1/$apt_conf" || true
fi

mkdir -p "$1/etc/apt/apt.conf.d"
apt_conf_d=/etc/apt/apt.conf.d
if [ -d "$apt_conf_d" ]; then
	# Preserve files, symlinks, permissions
	cp -a "$apt_conf_d/." "$1/$apt_conf_d/" || true
fi
