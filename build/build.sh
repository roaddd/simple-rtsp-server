#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/rk3568-build"
OUTPUT_DIR="${ROOT_DIR}/output"
TOOLCHAIN_FILE="${SCRIPT_DIR}/rk3568.cmake"

if [[ "${1:-}" == "clean" ]]; then
    rm -rf "${BUILD_DIR}" "${OUTPUT_DIR}"
    exit 0
fi

cmake -S "${ROOT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}" --parallel
cmake --install "${BUILD_DIR}" --prefix "${OUTPUT_DIR}"
