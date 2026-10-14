#!/usr/bin/env bash
# Start Mosquitto MQTT Broker for Contiki-NG IoT Simulation
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${PROJECT_DIR}/config/mosquitto.conf"
LOG_DIR="${PROJECT_DIR}/logs"
PID_FILE="${LOG_DIR}/mosquitto.pid"
LOG_FILE="${LOG_DIR}/mosquitto.log"

mkdir -p "${LOG_DIR}"

MOSQUITTO_BIN="$(which mosquitto 2>/dev/null || echo "/home/sasi/.local/sbin/mosquitto")"

if [ ! -x "${MOSQUITTO_BIN}" ]; then
  echo "[-] Mosquitto binary not found at ${MOSQUITTO_BIN}" >&2
  exit 1
fi

if [ -f "${PID_FILE}" ] && kill -0 "$(cat "${PID_FILE}")" 2>/dev/null; then
  echo "[*] Mosquitto broker already running with PID $(cat "${PID_FILE}")"
  exit 0
fi

echo "[*] Starting Mosquitto broker on port 1883..."
"${MOSQUITTO_BIN}" -c "${CONFIG_FILE}" > "${LOG_FILE}" 2>&1 &
BROKER_PID=$!
echo "${BROKER_PID}" > "${PID_FILE}"

# Wait up to 5 seconds for broker socket to appear
for i in {1..10}; do
  if ss -tlpn | grep -q ":1883 "; then
    echo "[+] Mosquitto broker is active and listening on port 1883 (PID: ${BROKER_PID})"
    exit 0
  fi
  sleep 0.5
done

echo "[-] Timed out waiting for Mosquitto to listen on port 1883" >&2
exit 1
