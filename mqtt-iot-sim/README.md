# Contiki-NG & Cooja MQTT IoT Simulation Platform

This project provides a complete, production-grade, end-to-end MQTT IoT simulation built on **Contiki-NG**, **Cooja**, **IPv6/6LoWPAN**, **RPL-Lite**, **Mosquitto**, and **Embedded C**.

---

## 1. System Architecture

```
+------------------------------------------------------------------------------------------------------+
|                                    COOJA SIMULATION DOMAIN (IEEE 802.15.4)                           |
|                                                                                                      |
|  [Mote 2: Temp Pub]        [Mote 3: Humidity Pub]      [Mote 4: Sensor Sub]    [Mote 5: Actuator]    |
|   fd00::202:2:2:2            fd00::203:3:3:3             fd00::204:4:4:4        fd00::205:5:5:5      |
|    (Native MQTT/TCP)          (Native MQTT/TCP)           (Native MQTT/TCP)      (Native MQTT/TCP)   |
|            \                         |                           |                       /           |
|             \                        |                           |                      /            |
|              \                       v                           v                     /             |
|               +----------------> [6LoWPAN / RPL Wireless Mesh] <----------------------+              |
|                                              |                                                       |
|                                              v                                                       |
|                                   [Mote 1: Border Router]                                            |
|                                      fd00::201:1:1:1                                                 |
|                                              |                                                       |
|                                    (SLIP over RS-232)                                                |
|                                              v                                                       |
|                             [Cooja SerialSocketServer: TCP 60001]                                    |
+----------------------------------------------|-------------------------------------------------------+
                                               | (Local TCP Stream)
+----------------------------------------------v-------------------------------------------------------+
|                                        HOST LINUX DOMAIN                                             |
|                                                                                                      |
|      Option A: tunslip6 (Kernel TUN tun0)        Option B: slip_mqtt_gateway.py (User-Space Bridge)  |
|      Requires: root / CAP_NET_ADMIN               Requires: Standard User Space                      |
|                                              \   /                                                   |
|                                               v v                                                    |
|                                 [Mosquitto MQTT Broker: 1883]                                        |
|                                       (IPv4 & IPv6 [::])                                             |
|                                              ^                                                       |
|                                              |                                                       |
|                                 [Host CLI Tools & Clients]                                           |
|                                (mosquitto_sub / mosquitto_pub)                                       |
+------------------------------------------------------------------------------------------------------+
```

### Node Roles

| Mote ID | Role | IP Address | Topic / Responsibility | Payload Format |
|---|---|---|---|---|
| **Mote 1** | RPL Border Router | `fd00::201:1:1:1` | RPL DAG Root, prefix propagation (`fd00::/64`), SLIP bridge | Binary SLIP / IPv6 |
| **Mote 2** | Temperature Publisher | `fd00::202:2:2:2` | Publishes `iot-2/evt/temperature/fmt/json` (QoS 1) | `{"sensor":"temperature","value":27.5,"unit":"C"}` |
| **Mote 3** | Humidity Publisher | `fd00::203:3:3:3` | Publishes `iot-2/evt/humidity/fmt/json` (QoS 1) | `{"sensor":"humidity","value":64,"unit":"percent"}` |
| **Mote 4** | Sensor Event Subscriber | `fd00::204:4:4:4` | Subscribes to `iot-2/evt/+/fmt/json` wildcard | Validates JSON, logs temp & humidity |
| **Mote 5** | Actuator Subscriber | `fd00::205:5:5:5` | Subscribes to `iot-2/cmd/+/fmt/json` | `{"device":"led","state":"on\|off"}` (Controls LED) |
| **Host** | MQTT Broker & Tools | `fd00::1` / `127.0.0.1` | Mosquitto broker listening on port 1883 | Host subscribers & command publishers |

---

## 2. Exact Eight-Hop Message-Flow Analysis

This section maps the conceptual 8-hop architecture against the real packet-level implementation:

### Uplink Flow: Sensor Event Generation to Host MQTT Broker

1. **Hop 1 (Mote 2/3 Application -> IEEE 802.15.4 Radio):**
   - The sensor application generates periodic deterministic sensor telemetry (`current_temp_x10` or `current_humidity`).
   - JSON payload is formatted into `app_buffer`.
   - `mqtt_publish()` creates an MQTT PUBLISH packet (QoS 1) with message ID `MID`.
   - The uIP TCP/IP stack segments the payload, wraps it in a TCP packet (`src_port=1032`, `dst_port=1883`), and an IPv6 packet (`src=fd00::202:2:2:2`, `dest=fd00::1`).
   - 6LoWPAN compresses headers; CSMA/IEEE 802.15.4 transmits radio frames.

2. **Hop 2 (Wireless Mesh -> Mote 1 Border Router):**
   - Mote 1's radio receives IEEE 802.15.4 frames.
   - 6LoWPAN reconstructs the uncompressed IPv6 datagram.
   - RPL routing verifies upward routing to DAG root.

3. **Hop 3 (Border Router -> Cooja Serial Socket):**
   - The embedded border router SLIP driver (`os/services/rpl-border-router/embedded/slip-bridge.c`) encapsulates the IPv6 datagram using SLIP framing (escaping `0xC0` to `0xDB 0xDC`).
   - Cooja's `SerialSocketServer` plugin receives raw bytes from Mote 1's simulated UART and emits them on TCP port `60001` on `127.0.0.1`.

4. **Hop 4 (Host Transport -> Mosquitto Broker):**
   - **Under Kernel TUN (`tunslip6`):** `tunslip6` reads SLIP, writes IPv6 to `/dev/net/tun0`. The Linux kernel routes the packet to Mosquitto listening on `[::]:1883` or `[fd00::1]:1883`.
   - **Under User-Space Gateway (`slip_mqtt_gateway.py`):** The Python gateway unescapes SLIP, decodes IPv6/TCP, proxies the stream to Mosquitto at `127.0.0.1:1883`, receives Mosquitto's `PUBACK`, wraps it into a TCP ACK/PUBACK, and injects it back down.

### Broker Forwarding & Routing

- Mosquitto matches published topics against active topic subscriptions:
  - Host subscriber: `iot-2/evt/#`
  - Mote 4 wildcard subscriber: `iot-2/evt/+/fmt/json`
- Mosquitto generates outgoing MQTT PUBLISH packets to each subscriber.

### Downlink Flow: MQTT Actuator Command to Mote 5

5. **Hop 5 (Host Publisher -> MQTT Broker):**
   - Host runs `mosquitto_pub -t "iot-2/cmd/led/fmt/json" -m '{"device":"led","state":"on"}'`.
   - Mosquitto receives command and identifies Mote 5's subscription `iot-2/cmd/+/fmt/json`.

6. **Hop 6 (Broker -> Gateway / Tunnel):**
   - Mosquitto sends the MQTT PUBLISH packet over Mote 5's established TCP socket connection.

7. **Hop 7 (Gateway / Tunnel -> Border Router SLIP):**
   - Packet is framed into an IPv6 datagram (`src=fd00::1`, `dst=fd00::205:5:5:5`).
   - Encapsulated into SLIP and written to port `60001` into Cooja's SerialSocketServer.

8. **Hop 8 (Border Router -> Mote 5 Actuator):**
   - Border router SLIP driver receives the frame.
   - RPL Source Routing (`rpl-ext-header.c`) looks up Mote 5 in the DAO routing table, inserts the Source Routing Header (SRH), and transmits it over IEEE 802.15.4.
   - Mote 5 receives the packet, passes it to `mqtt.c`, which invokes `mqtt_event()` with `MQTT_EVENT_PUBLISH`.
   - `actuator-subscriber.c` parses the JSON payload using `jsonparse.h`:
     - If `device == "led"` and `state == "on"`: calls `leds_on(LEDS_RED)`.
     - If `device == "led"` and `state == "off"`: calls `leds_off(LEDS_RED)`.
     - If malformed or unrecognized: safely rejects without crash.

---

## 3. Directory Layout

```
mqtt-iot-sim/
├── README.md                           # Comprehensive documentation
├── apps/                               # Contiki-NG embedded applications
│   ├── border-router/                  # Mote 1: RPL Border Router
│   │   ├── Makefile
│   │   ├── border-router.c
│   │   └── project-conf.h
│   ├── temperature-publisher/          # Mote 2: Periodic Temperature Sensor
│   │   ├── Makefile
│   │   ├── project-conf.h
│   │   └── temperature-publisher.c
│   ├── humidity-publisher/             # Mote 3: Periodic Humidity Sensor
│   │   ├── Makefile
│   │   ├── project-conf.h
│   │   └── humidity-publisher.c
│   ├── sensor-subscriber/              # Mote 4: Wildcard Event Subscriber
│   │   ├── Makefile
│   │   ├── project-conf.h
│   │   └── sensor-subscriber.c
│   └── actuator-subscriber/            # Mote 5: LED Actuator Subscriber
│       ├── Makefile
│       ├── project-conf.h
│       └── actuator-subscriber.c
├── config/
│   ├── mosquitto.conf                  # Host MQTT Broker Configuration
│   └── network.conf                    # Network & Topic Taxonomy Config
├── gateway/
│   └── slip_mqtt_gateway.py            # User-Space SLIP <-> Mosquitto Bridge
├── scripts/
│   ├── start_broker.sh                 # Starts Mosquitto daemon
│   ├── stop_broker.sh                  # Stops Mosquitto daemon
│   ├── run_cooja_sim.sh                # Launches Cooja (GUI or headless)
│   ├── connect_border_router.sh        # Starts tunslip6 (root/TUN systems)
│   ├── test_mqtt_flow.py               # 15-point automated verification engine
│   └── test_mqtt_flow.sh               # Execution wrapper
├── simulation/
│   └── mqtt-simulation.csc             # 5-mote Cooja XML simulation configuration
└── logs/                               # Runtime logs and PID files
```

---

## 4. Build Instructions

All 5 motes compile directly for the `cooja` target:

```bash
# Compile Mote 1 (Border Router)
make -C apps/border-router TARGET=cooja

# Compile Mote 2 (Temperature Publisher)
make -C apps/temperature-publisher TARGET=cooja

# Compile Mote 3 (Humidity Publisher)
make -C apps/humidity-publisher TARGET=cooja

# Compile Mote 4 (Sensor Subscriber)
make -C apps/sensor-subscriber TARGET=cooja

# Compile Mote 5 (Actuator Subscriber)
make -C apps/actuator-subscriber TARGET=cooja
```

---

## 5. Startup & Execution Instructions

### Step 1: Start Host MQTT Broker

```bash
./scripts/start_broker.sh
```

Verifies that Mosquitto is active on port 1883.

### Step 2: Start Cooja Simulation

#### Headless Mode (CLI / Automated):
```bash
./scripts/run_cooja_sim.sh --headless
```

#### Graphical Mode (GUI):
```bash
./scripts/run_cooja_sim.sh
```

Cooja loads all 5 motes, starts the simulation, and opens the serial socket server on port `60001`.

### Step 3: Connect Network Interface

#### Option A: User-Space Bridge (Recommended for standard / container environments):
```bash
python3 gateway/slip_mqtt_gateway.py
```
This handles the prefix handshake (`?P` -> `!P`), responds to ICMPv6 echo requests, and proxies mote TCP/MQTT connections directly to Mosquitto on port 1883.

#### Option B: Native Linux TUN Interface (Requires root / sudo):
```bash
sudo ./scripts/connect_border_router.sh
```

---

## 6. Verification and Diagnostics

### Automated Test Suite

Run the complete 15-test verification engine:

```bash
./scripts/test_mqtt_flow.sh
```

### Manual Host CLI Commands

#### 1. Subscribe to all sensor events:
```bash
mosquitto_sub -h 127.0.0.1 -t "iot-2/evt/#" -v
```

Expected output:
```
iot-2/evt/temperature/fmt/json {"sensor":"temperature","value":27.5,"unit":"C"}
iot-2/evt/humidity/fmt/json {"sensor":"humidity","value":64,"unit":"percent"}
```

#### 2. Send Actuator Commands:

Turn simulated LED ON:
```bash
mosquitto_pub -h 127.0.0.1 -t "iot-2/cmd/led/fmt/json" -m '{"device":"led","state":"on"}'
```

Turn simulated LED OFF:
```bash
mosquitto_pub -h 127.0.0.1 -t "iot-2/cmd/led/fmt/json" -m '{"device":"led","state":"off"}'
```

Test malformed payload rejection:
```bash
mosquitto_pub -h 127.0.0.1 -t "iot-2/cmd/led/fmt/json" -m '{"invalid":123'
```

---

## 7. Verification Evidence & Diagnostics Table

| Milestone | Command / Inspection | Expected Result | What It Proves | What It Does NOT Prove | Diagnostic Action If Failed |
|---|---|---|---|---|---|
| **Compilation** | `make TARGET=cooja` | Binaries in `build/cooja/*.cooja` | C syntax and Contiki-NG API compatibility | Runtime memory or radio reachability | Check missing header definitions or Makefile variables |
| **Broker Listener** | `ss -tlpn \| grep 1883` | `LISTEN ...:1883` | Mosquitto daemon is running and bound | Simulated motes can reach the port | Check `logs/mosquitto.log` for bind errors |
| **Serial Socket** | `ss -tlpn \| grep 60001` | `LISTEN ...:60001` | Cooja loaded and SerialSocketServer is listening | RPL network converged | Verify `mqtt-simulation.csc` SerialSocketServer configuration |
| **Prefix Handshake** | `python3 gateway/slip_mqtt_gateway.py` | `Sent IPv6 prefix advertisement (fd00::/64)` | Border router accepted prefix and became DAG root | Motes have joined the DAG | Check border router serial output for `created DAG` |
| **RPL Convergence** | Cooja log inspection | `received a DAO from fd00::202:2:2:2` | Downlink source routing table populated | TCP handshake succeeded | Allow 10-15 seconds for RPL DIO/DAO trickle timers |
| **MQTT Connect** | Mote log / broker log | `MQTT connection established` | Complete 3-way TCP handshake + MQTT CONNECT/CONNACK | Publications delivered | Check broker authorization and keepalive settings |
| **Delivery Confirmation** | Mote log: `PUBACK received` | `Broker delivery CONFIRMED (MID: X)` | Broker received publication and acknowledged it (QoS 1) | End-to-end subscriber consumption | Check QoS level in `mqtt_publish()` |
| **Actuator Execution** | Mote 5 log: `Actuator State Change` | `LED set to ON (active mask: 0x01)` | JSON parsed safely and `leds_on()` invoked | Hardware physical LED on real board | Check JSON keys `device` and `state` |
