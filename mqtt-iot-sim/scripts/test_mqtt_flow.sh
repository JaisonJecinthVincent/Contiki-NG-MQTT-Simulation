#!/usr/bin/env bash
# Execute the full automated MQTT flow test suite
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "${SCRIPT_DIR}/test_mqtt_flow.py"
