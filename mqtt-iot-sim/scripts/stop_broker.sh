#!/usr/bin/env bash
# Stop Mosquitto MQTT Broker
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PID_FILE="${PROJECT_DIR}/logs/mosquitto.pid"

if [ -f "${PID_FILE}" ]; then
  PID="$(cat "${PID_FILE}")"
  if kill -0 "${PID}" 2>/dev/null; then
    echo "[*] Stopping Mosquitto broker (PID: ${PID})..."
    kill "${PID}" || true
    sleep 1
    if kill -0 "${PID}" 2>/dev/null; then
      kill -9 "${PID}" || true
    fi
    echo "[+] Mosquitto stopped."
  else
    echo "[*] Process ${PID} not running."
  fi
  rm -f "${PID_FILE}"
else
  pkill -f "mosquitto -c" 2>/dev/null && echo "[+] Killed existing Mosquitto process" || echo "[*] Mosquitto was not running."
fi
