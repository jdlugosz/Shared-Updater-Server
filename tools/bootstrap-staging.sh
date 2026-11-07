#!/usr/bin/env bash
# Shared Updater Server - tools/bootstrap-staging.sh
# <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

set -euo pipefail

if [[ $(id -u) -ne 0 || $(hostname -s) != vps-stage ]]; then
    printf 'Run this as root on vps-stage.\n' >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install --yes cmake ninja-build libsqlite3-dev libtomlplusplus-dev libcatch2-dev \
    libboost-dev libxml2-dev libsystemd-dev libc6-dev python3 openssl git curl xz-utils binutils

toolchains=/opt/shared-updater-server/toolchains
gcc_release=16.2.0
gcc_root="$toolchains/gcc-$gcc_release"
install -d -m 755 "$toolchains"
install -d -o john -g john -m 755 /opt/shared-updater-server/build

if [[ ! -x "$gcc_root/bin/g++" ]]; then
    download_dir=$(mktemp -d "$toolchains/.gcc-download.XXXXXXXX")
    trap 'rm -rf -- "$download_dir"' EXIT
    archive="$download_dir/gcc.tar.xz"
    curl --fail --silent --show-error --location --proto '=https' --max-time 300 \
        --output "$archive" "https://s3.amazonaws.com/compiler-explorer/opt/gcc-$gcc_release.tar.xz"
    printf '%s  %s\n' '7bffbfc6b2eb997a7a6e3cbd013372b41de5746850ded8b6453f9820e5678828' "$archive" \
        | sha256sum --check --status
    tar -xJf "$archive" --no-same-owner -C "$download_dir"
    candidate="$download_dir/gcc-$gcc_release"
    [[ "$("$candidate/bin/g++" -dumpfullversion)" == "$gcc_release" ]]
    [[ -f "$candidate/lib64/libstdc++.so.6" ]]
    mv --no-clobber -T -- "$candidate" "$gcc_root"
fi

[[ "$("$gcc_root/bin/g++" -dumpfullversion)" == "$gcc_release" ]]
[[ -f "$gcc_root/lib64/libstdc++.so.6" ]]
printf '\nCompiler and dependency versions:\n'
"$gcc_root/bin/g++" --version | head -n 1
dpkg-query -W cmake ninja-build libsqlite3-dev libtomlplusplus-dev libcatch2-dev \
    libboost-dev libxml2-dev libsystemd-dev
