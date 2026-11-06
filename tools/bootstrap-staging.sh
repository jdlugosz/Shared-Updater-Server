#!/usr/bin/env bash
set -euo pipefail

if [[ $(id -u) -ne 0 || $(hostname -s) != vps-stage ]]; then
    printf 'Run this as root on vps-stage.\n' >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install --yes g++-16 cmake ninja-build libsqlite3-dev libtomlplusplus-dev libcatch2-dev \
    libboost-dev libxml2-dev libsystemd-dev python3 openssl git

install -d -m 755 /opt/shared-updater-server
install -d -o john -g john -m 755 /opt/shared-updater-server/build

printf '\nCompiler and dependency versions:\n'
g++-16 --version | head -n 1
dpkg-query -W g++-16 cmake ninja-build libsqlite3-dev libtomlplusplus-dev libcatch2-dev \
    libboost-dev libxml2-dev libsystemd-dev
