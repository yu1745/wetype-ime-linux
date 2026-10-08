#!/usr/bin/env bash
cd "$(dirname "$0")/.."
export LD_LIBRARY_PATH="$PWD/runtime"
exec qemu-aarch64-static -L "$PWD/runtime/sysroot" ./harness/probe "$@"
