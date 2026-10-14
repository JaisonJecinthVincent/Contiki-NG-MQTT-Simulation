#!/usr/bin/env bash
# Connect host Linux TUN interface to Cooja RPL Border Router using tunslip6
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONTIKI_DIR="$(cd "${PROJECT_DIR}/.." && pwd)"
TUNSLIP6="${CONTIKI_DIR}/tools/serial-io/tunslip6"

PREFIX="${1:-fd00::1/64}"
COOJA_HOST="${2:-127.0.0.1}"
COOJA_PORT="${3:-60001}"

if [ ! -x "${TUNSLIP6}" ]; then
  echo "[*] Building tunslip6..."
  make -C "${CONTIKI_DIR}/tools/serial-io" tunslip6
fi

echo "=========================================================="
echo " Starting tunslip6 for Cooja Border Router integration"
echo " Target Serial Socket: ${COOJA_HOST}:${COOJA_PORT}"
echo " Assigned IPv6 Prefix: ${PREFIX}"
echo " Interface: tun0 (requires root / CAP_NET_ADMIN)"
echo "=========================================================="

if [ "$(id -u)" -ne 0 ]; then
  echo "[*] Note: tunslip6 requires root to create the kernel tun0 interface."
  echo "[*] Running with sudo..."
  exec sudo "${TUNSLIP6}" -a "${COOJA_HOST}" -p "${COOJA_PORT}" "${PREFIX}"
else
  exec "${TUNSLIP6}" -a "${COOJA_HOST}" -p "${COOJA_PORT}" "${PREFIX}"
fi
