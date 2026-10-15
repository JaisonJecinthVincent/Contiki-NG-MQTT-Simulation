#!/usr/bin/env python3
"""
Contiki-NG SLIP to MQTT Host Bridge / Gateway
---------------------------------------------
Connects to Cooja's Serial Socket (TCP port 60001) where Mote 1 (RPL Border Router)
is attached. Handles:
  1. Border router prefix configuration handshake ('?P' -> '!P' + prefix).
  2. ICMPv6 Echo Request / Reply (ping).
  3. TCP/IPv6 stream proxying between simulated motes and the host Mosquitto broker (127.0.0.1:1883).
  4. Logging and diagnostics.

This enables end-to-end MQTT communication across simulated 6LoWPAN/RPL motes
without requiring root privileges or a Linux kernel TUN device.
"""

import sys
import os
import socket
import select
import struct
import time
import logging

SLIP_END     = 0xC0
SLIP_ESC     = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

TCP_FLAG_FIN = 0x01
TCP_FLAG_SYN = 0x02
TCP_FLAG_RST = 0x04
TCP_FLAG_PSH = 0x08
TCP_FLAG_ACK = 0x10

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] [Gateway] %(message)s",
    datefmt="%H:%M:%S"
)
logger = logging.getLogger("SLIP-Gateway")


def slip_encode(data: bytes) -> bytes:
    encoded = bytearray()
    for b in data:
        if b == SLIP_END:
            encoded.extend([SLIP_ESC, SLIP_ESC_END])
        elif b == SLIP_ESC:
            encoded.extend([SLIP_ESC, SLIP_ESC_ESC])
        else:
            encoded.append(b)
    encoded.append(SLIP_END)
    return bytes(encoded)


def in_cksum(buf: bytes) -> int:
    if len(buf) % 2 == 1:
        buf += b"\x00"
    s = 0
    for i in range(0, len(buf), 2):
        w = (buf[i] << 8) + buf[i + 1]
        s += w
    while (s >> 16):
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


class MoteTCPConnection:
    def __init__(self, mote_ip: bytes, mote_port: int, broker_host: str, broker_port: int):
        self.mote_ip = mote_ip
        self.mote_port = mote_port
        self.broker_host = broker_host
        self.broker_port = broker_port

        self.broker_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.broker_sock.setblocking(False)
        self.connected_to_broker = False

        self.mote_seq = 0
        self.gw_seq = 1000
        self.last_active = time.time()
        self.last_broker_data = None
        self.last_broker_seq = 0

    def connect_broker(self):
        try:
            self.broker_sock.connect((self.broker_host, self.broker_port))
            self.connected_to_broker = True
        except BlockingIOError:
            self.connected_to_broker = True
        except Exception as e:
            logger.error(f"Failed to connect to broker: {e}")
            self.connected_to_broker = False

    def close(self):
        try:
            self.broker_sock.close()
        except Exception:
            pass


class SlipMqttGateway:
    def __init__(self, cooja_host="127.0.0.1", cooja_port=60001,
                 broker_host="127.0.0.1", broker_port=1883,
                 prefix_str="fd00::1/64"):
        self.cooja_host = cooja_host
        self.cooja_port = cooja_port
        self.broker_host = broker_host
        self.broker_port = broker_port

        self.prefix_bytes = bytes([0xFD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        self.gw_ip_bytes = socket.inet_pton(socket.AF_INET6, "fd00::1")

        self.cooja_sock = None
        self.rx_buffer = bytearray()
        self.tcp_conns = {}  # (mote_ip, mote_port) -> MoteTCPConnection
        self.running = False

    def connect_cooja(self):
        while self.running:
            try:
                logger.info(f"Connecting to Cooja Serial Socket at {self.cooja_host}:{self.cooja_port}...")
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.connect((self.cooja_host, self.cooja_port))
                s.setblocking(False)
                self.cooja_sock = s
                logger.info("Connected to Cooja Serial Socket successfully!")
                self.send_prefix()
                return True
            except ConnectionRefusedError:
                logger.warning("Cooja serial socket not ready yet. Retrying in 2 seconds...")
                time.sleep(2)
            except Exception as e:
                logger.error(f"Error connecting to Cooja: {e}. Retrying in 2 seconds...")
                time.sleep(2)
        return False

    def send_prefix(self):
        # !P + 8 prefix bytes + SLIP_END
        pkt = bytearray([ord('!'), ord('P')]) + self.prefix_bytes
        self.cooja_sock.sendall(slip_encode(pkt))
        logger.info("Sent IPv6 prefix advertisement (fd00::/64) to Border Router")

    def send_ipv6_packet(self, dst_ip: bytes, next_header: int, payload: bytes):
        ip_hdr = struct.pack(
            "!IHBB16s16s",
            0x60000000,
            len(payload),
            next_header,
            64,
            self.gw_ip_bytes,
            dst_ip
        )
        packet = ip_hdr + payload
        self.cooja_sock.sendall(slip_encode(packet))

    def send_tcp_packet(self, dst_ip: bytes, dst_port: int, seq: int, ack: int, flags: int, payload: bytes = b""):
        tcp_hdr_noflag = struct.pack(
            "!HHIIBBHHH",
            self.broker_port,
            dst_port,
            seq,
            ack,
            (5 << 4),
            flags,
            2048,  # Window size
            0,     # Checksum placeholder
            0      # Urgent pointer
        )
        # Calculate TCP Checksum using IPv6 pseudo-header (RFC 2460 / RFC 8200)
        ph = self.gw_ip_bytes + dst_ip + struct.pack("!II", len(tcp_hdr_noflag) + len(payload), 6)
        csum = in_cksum(ph + tcp_hdr_noflag + payload)
        tcp_hdr = struct.pack(
            "!HHIIBBHHH",
            self.broker_port,
            dst_port,
            seq,
            ack,
            (5 << 4),
            flags,
            2048,
            csum,
            0
        )
        self.send_ipv6_packet(dst_ip, 6, tcp_hdr + payload)

    def handle_icmpv6(self, src_ip: bytes, icmp_data: bytes):
        if len(icmp_data) < 8:
            return
        icmp_type, icmp_code = icmp_data[0], icmp_data[1]
        if icmp_type == 128:  # Echo Request
            # Form Echo Reply (type 129)
            reply = bytearray(icmp_data)
            reply[0] = 129
            reply[2] = 0
            reply[3] = 0
            # Calculate ICMPv6 checksum
            ph = self.gw_ip_bytes + src_ip + struct.pack("!II", len(reply), 58)
            csum = in_cksum(ph + bytes(reply))
            struct.pack_into("!H", reply, 2, csum)
            self.send_ipv6_packet(src_ip, 58, bytes(reply))
            logger.info(f"Replied to ICMPv6 Echo Request from {socket.inet_ntop(socket.AF_INET6, src_ip)}")

    def handle_tcp(self, src_ip: bytes, tcp_data: bytes):
        if len(tcp_data) < 20:
            return
        src_port, dst_port, seq, ack, data_offset, flags, window, csum, urg = struct.unpack("!HHIIBBHHH", tcp_data[:20])
        header_len = (data_offset >> 4) * 4
        payload = tcp_data[header_len:]
        conn_key = (src_ip, src_port)
        src_ip_str = socket.inet_ntop(socket.AF_INET6, src_ip)

        # SYN received: initiate broker connection or handle duplicate SYN
        if (flags & TCP_FLAG_SYN) and not (flags & TCP_FLAG_ACK):
            if conn_key in self.tcp_conns:
                conn = self.tcp_conns[conn_key]
                if conn.mote_seq == ((seq + 1) & 0xFFFFFFFF) and conn.connected_to_broker:
                    # Re-send SYN-ACK without closing or recreating the connection
                    self.send_tcp_packet(src_ip, src_port, 1000, conn.mote_seq, TCP_FLAG_SYN | TCP_FLAG_ACK)
                    logger.debug(f"Re-sent TCP SYN-ACK for retransmitted SYN from [{src_ip_str}]:{src_port}")
                    return
                else:
                    conn.close()

            logger.info(f"TCP SYN received from [{src_ip_str}]:{src_port} -> Port {dst_port}")
            conn = MoteTCPConnection(src_ip, src_port, self.broker_host, self.broker_port)
            conn.mote_seq = (seq + 1) & 0xFFFFFFFF
            conn.gw_seq = 1000
            conn.connect_broker()
            self.tcp_conns[conn_key] = conn

            # Reply SYN-ACK
            self.send_tcp_packet(src_ip, src_port, conn.gw_seq, conn.mote_seq, TCP_FLAG_SYN | TCP_FLAG_ACK)
            conn.gw_seq = (conn.gw_seq + 1) & 0xFFFFFFFF
            logger.info(f"Sent TCP SYN-ACK to [{src_ip_str}]:{src_port}")
            return

        conn = self.tcp_conns.get(conn_key)
        if not conn:
            return

        conn.last_active = time.time()

        # Handle incoming data (MQTT payload)
        if len(payload) > 0:
            seq_end = (seq + len(payload)) & 0xFFFFFFFF

            # Check for duplicate retransmission of already acknowledged data
            diff = (conn.mote_seq - seq_end) & 0xFFFFFFFF
            is_dup = (diff < 0x80000000)

            if is_dup:
                # Retransmission: re-send ACK
                self.send_tcp_packet(src_ip, src_port, conn.gw_seq, conn.mote_seq, TCP_FLAG_ACK)
                if conn.last_broker_data:
                    self.send_tcp_packet(conn.mote_ip, conn.mote_port,
                                         conn.last_broker_seq, conn.mote_seq,
                                         TCP_FLAG_PSH | TCP_FLAG_ACK, conn.last_broker_data)
                logger.debug(f"Duplicate TCP segment ({len(payload)} bytes) from [{src_ip_str}]:{src_port} (re-sent ACK)")
                return

            # If partially overlapping retransmission
            new_payload = payload
            overlap = (conn.mote_seq - seq) & 0xFFFFFFFF
            if overlap < len(payload) and overlap < 0x80000000:
                new_payload = payload[overlap:]

            conn.mote_seq = seq_end
            # Send TCP ACK for the received payload
            self.send_tcp_packet(src_ip, src_port, conn.gw_seq, conn.mote_seq, TCP_FLAG_ACK)

            # Forward only new payload to Mosquitto
            if len(new_payload) > 0:
                try:
                    conn.broker_sock.sendall(new_payload)
                    mqtt_cmd = new_payload[0] >> 4
                    cmd_names = {1: "CONNECT", 3: "PUBLISH", 8: "SUBSCRIBE", 12: "PINGREQ", 14: "DISCONNECT"}
                    name = cmd_names.get(mqtt_cmd, f"CMD_{mqtt_cmd}")
                    logger.info(f"Forwarded {name} ({len(new_payload)} bytes) from [{src_ip_str}]:{src_port} to Mosquitto broker")
                except Exception as e:
                    logger.error(f"Broker forward error: {e}")

        # Handle FIN
        if flags & TCP_FLAG_FIN:
            conn.mote_seq = (seq + 1) & 0xFFFFFFFF
            self.send_tcp_packet(src_ip, src_port, conn.gw_seq, conn.mote_seq, TCP_FLAG_FIN | TCP_FLAG_ACK)
            conn.close()
            self.tcp_conns.pop(conn_key, None)
            logger.info(f"TCP connection closed for [{src_ip_str}]:{src_port}")

    def handle_packet(self, packet: bytes):
        if len(packet) == 0:
            return

        # Check for SLIP prefix inquiry '?P'
        if packet.startswith(b"?P"):
            logger.info("Border router requested prefix (?P)")
            self.send_prefix()
            return

        # Check for ASCII log message from Border router
        if packet[0] not in (0x60, ord('?'), ord('!')):
            try:
                line = packet.decode("utf-8", errors="ignore").strip()
                if line:
                    logger.debug(f"[BR Serial] {line}")
            except Exception:
                pass
            return

        # IPv6 Packet parsing (Version = 6)
        if (packet[0] >> 4) == 6 and len(packet) >= 40:
            version_tc_fl, payload_len, next_header, hop_limit = struct.unpack("!IHBB", packet[:8])
            src_ip = packet[8:24]
            dst_ip = packet[24:40]
            ip_payload = packet[40:40 + payload_len]

            if next_header == 58:  # ICMPv6
                self.handle_icmpv6(src_ip, ip_payload)
            elif next_header == 6:   # TCP
                self.handle_tcp(src_ip, ip_payload)

    def run(self):
        self.running = True
        if not self.connect_cooja():
            return

        logger.info("Gateway bridge running. Listening for mote traffic...")

        while self.running:
            read_socks = [self.cooja_sock]
            for conn in list(self.tcp_conns.values()):
                if conn.broker_sock:
                    read_socks.append(conn.broker_sock)

            try:
                readable, _, _ = select.select(read_socks, [], [], 0.05)
            except Exception:
                break

            for s in readable:
                if s == self.cooja_sock:
                    try:
                        chunk = self.cooja_sock.recv(4096)
                        if not chunk:
                            logger.warning("Cooja serial socket closed. Reconnecting...")
                            self.connect_cooja()
                            break
                        self.rx_buffer.extend(chunk)

                        # Parse SLIP frames
                        while SLIP_END in self.rx_buffer:
                            end_idx = self.rx_buffer.index(SLIP_END)
                            raw_frame = self.rx_buffer[:end_idx]
                            self.rx_buffer = self.rx_buffer[end_idx + 1:]

                            if len(raw_frame) > 0:
                                # SLIP Unescape
                                unescaped = bytearray()
                                i = 0
                                while i < len(raw_frame):
                                    if raw_frame[i] == SLIP_ESC and i + 1 < len(raw_frame):
                                        if raw_frame[i + 1] == SLIP_ESC_END:
                                            unescaped.append(SLIP_END)
                                        elif raw_frame[i + 1] == SLIP_ESC_ESC:
                                            unescaped.append(SLIP_ESC)
                                        else:
                                            unescaped.append(raw_frame[i + 1])
                                        i += 2
                                    else:
                                        unescaped.append(raw_frame[i])
                                        i += 1
                                self.handle_packet(bytes(unescaped))
                    except Exception as e:
                        logger.error(f"Error reading from Cooja socket: {e}")

                else:
                    # Data received from Mosquitto broker -> forward to matching mote
                    for conn_key, conn in list(self.tcp_conns.items()):
                        if conn.broker_sock == s:
                            try:
                                data = s.recv(4096)
                                if data:
                                    mqtt_cmd = data[0] >> 4
                                    cmd_names = {2: "CONNACK", 4: "PUBACK", 9: "SUBACK", 3: "PUBLISH", 13: "PINGRESP"}
                                    name = cmd_names.get(mqtt_cmd, f"RESP_{mqtt_cmd}")
                                    mote_ip_str = socket.inet_ntop(socket.AF_INET6, conn.mote_ip)
                                    # Save for potential retransmission
                                    conn.last_broker_data = data
                                    conn.last_broker_seq = conn.gw_seq

                                    # Send TCP PSH+ACK packet to mote
                                    self.send_tcp_packet(conn.mote_ip, conn.mote_port,
                                                         conn.gw_seq, conn.mote_seq,
                                                         TCP_FLAG_PSH | TCP_FLAG_ACK, data)
                                    conn.gw_seq = (conn.gw_seq + len(data)) & 0xFFFFFFFF
                                else:
                                    conn.close()
                                    self.tcp_conns.pop(conn_key, None)
                            except Exception as e:
                                logger.error(f"Error reading from broker socket: {e}")

    def stop(self):
        self.running = False
        if self.cooja_sock:
            self.cooja_sock.close()
        for conn in self.tcp_conns.values():
            conn.close()
        logger.info("Gateway bridge stopped.")


if __name__ == "__main__":
    gateway = SlipMqttGateway()
    try:
        gateway.run()
    except KeyboardInterrupt:
        gateway.stop()
