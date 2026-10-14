#!/usr/bin/env bash
# Run Cooja Simulation with 5-Mote MQTT Network
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONTIKI_DIR="$(cd "${PROJECT_DIR}/.." && pwd)"
COOJA_JAR="${CONTIKI_DIR}/tools/cooja/build/libs/cooja.jar"
SIM_CSC="${PROJECT_DIR}/simulation/mqtt-simulation.csc"
LOG_DIR="${PROJECT_DIR}/logs"
LOG_FILE="${LOG_DIR}/cooja.log"

mkdir -p "${LOG_DIR}"

if [ ! -f "${COOJA_JAR}" ]; then
  echo "[*] Building Cooja jar..."
  (cd "${CONTIKI_DIR}/tools/cooja" && ./gradlew jar)
fi

GUI_FLAG=""
if [ "${1:-}" = "--headless" ] || [ -z "${DISPLAY:-}" ]; then
  GUI_FLAG="--no-gui"
  echo "[*] Running Cooja in headless mode (--no-gui)..."
else
  echo "[*] Running Cooja in graphical mode..."
fi

echo "[*] Launching simulation: ${SIM_CSC}"
echo "[*] Contiki-NG tree: ${CONTIKI_DIR}"
echo "[*] Log file: ${LOG_FILE}"

java -jar "${COOJA_JAR}" \
  --contiki="${CONTIKI_DIR}" \
  ${GUI_FLAG} \
  "${SIM_CSC}" 2>&1 | tee "${LOG_FILE}"
