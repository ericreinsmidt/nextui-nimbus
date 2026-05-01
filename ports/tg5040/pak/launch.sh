#!/bin/sh
set -eu

PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_ID="nimbus"

STATE_DIR="${USERDATA_PATH}/${APP_ID}"
CACHE_DIR="${STATE_DIR}/cache"
CONFIG_DIR="${STATE_DIR}/config"

BIN="${PAK_DIR}/bin/nimbus"

mkdir -p "${STATE_DIR}" "${CACHE_DIR}" "${CONFIG_DIR}"

# CA certificate bundle for HTTPS
if [ -f "${PAK_DIR}/lib/cacert.pem" ]; then
  export CURL_CA_BUNDLE="${PAK_DIR}/lib/cacert.pem"
fi

if [ -x "${BIN}" ]; then
  export NIMBUS_PAK_DIR="${PAK_DIR}"
  export NIMBUS_CONFIG_DIR="${CONFIG_DIR}"
  export NIMBUS_CACHE_DIR="${CACHE_DIR}"

  cd "${PAK_DIR}"
  exec "${BIN}"
else
  echo "Executable not found: ${BIN}"
  exit 0
fi
