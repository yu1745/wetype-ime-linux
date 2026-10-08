#!/usr/bin/env bash
cd "$(dirname "$0")/.."
export LD_LIBRARY_PATH="$PWD/runtime"
exec qemu-aarch64-static -cpu "${QEMU_CPU:-cortex-a72}" -L "$PWD/runtime/sysroot" ./harness/probe "$@"
