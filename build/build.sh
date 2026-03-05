#!/usr/bin/env sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/rk3568-build"
OUTPUT_DIR="${ROOT_DIR}/output"
TOOLCHAIN_FILE="${SCRIPT_DIR}/rk3568.cmake"

if [ "${1:-}" = "clean" ]; then
    rm -rf "${BUILD_DIR}" "${OUTPUT_DIR}"
    exit 0
fi

mkdir -p "${BUILD_DIR}"

cd "${BUILD_DIR}"
cmake "${ROOT_DIR}" -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${OUTPUT_DIR}"

cmake --build "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target install
