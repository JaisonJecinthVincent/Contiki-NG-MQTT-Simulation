#!/usr/bin/env bash
# Monitor all live MQTT topics traversing Mosquitto broker
echo "=========================================================="
echo "    Live Mosquitto MQTT Broker Monitor (# wildcard)"
echo "    Listening on localhost:1883... (Press Ctrl+C to exit)"
echo "=========================================================="
mosquitto_sub -h 127.0.0.1 -p 1883 -t '#' -v
