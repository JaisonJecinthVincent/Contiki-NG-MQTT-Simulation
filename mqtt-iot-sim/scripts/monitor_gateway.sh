#!/usr/bin/env bash
# Monitor live SLIP / TUN Gateway activity
LOG_FILE="/home/sasi/.gemini/antigravity/brain/defe2084-6c09-4d77-9ca2-c259cbf916d8/.system_generated/tasks/task-751.log"
echo "=========================================================="
echo "    Live SLIP Tunnel / Gateway Monitor"
echo "    Streaming packets from Cooja Mote 1 -> Host Mosquitto"
echo "    (Press Ctrl+C to exit)"
echo "=========================================================="
tail -n 20 -f "$LOG_FILE"
