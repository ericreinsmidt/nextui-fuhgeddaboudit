#!/bin/sh
set -eu

PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

BIN="${PAK_DIR}/bin/fuhgeddaboudit"

if [ -x "${BIN}" ]; then
  export FUHGEDDABOUDIT_PAK_DIR="${PAK_DIR}"

  cd "${PAK_DIR}"
  exec "${BIN}"
else
  echo "Executable not found: ${BIN}"
  exit 0
fi
