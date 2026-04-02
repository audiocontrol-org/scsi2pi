#!/usr/bin/env python3
"""
SCSI MIDI bridge test harness.

Acts as the bridge daemon: connects to the SCMP device's Unix socket
and runs an SDS round-trip test:

1. Send a small test sample to the S3000XL via SDS
2. Request the sample back
3. Compare

Usage:
    python3 scripts/scsi-midi-test.py [sample_number]

The SCMP device (in s2p) must be running and listening on the socket.
Start s2p first, attach SCMP, then run this script.
"""

import socket
import sys
import time
import struct
import os

SOCKET_PATH = "/tmp/scsi-midi-bridge.sock"
SAMPLE_NUMBER = int(sys.argv[1]) if len(sys.argv) > 1 else 99
CHANNEL = 0
NUM_SAMPLES = 64
SAMPLE_RATE = 44100

def build_sds_dump_header(sample_num, bits, period_ns, length, loop_start=0, loop_end=0, loop_type=0x7f):
    """Build an SDS Dump Header message."""
    def enc14(v): return [v & 0x7f, (v >> 7) & 0x7f]
    def enc21(v): return [v & 0x7f, (v >> 7) & 0x7f, (v >> 14) & 0x7f]
    sl, sh = enc14(sample_num)
    pl, pm, ph = enc21(period_ns)
    al, am, ah = enc21(length)
    bl, bm, bh = enc21(loop_start)
    cl, cm, ch = enc21(loop_end)
    return bytes([0xf0, 0x7e, CHANNEL, 0x01, sl, sh, bits,
                  pl, pm, ph, al, am, ah, bl, bm, bh, cl, cm, ch, loop_type, 0xf7])

def build_sds_data_packet(packet_num, data_120):
    """Build an SDS Data Packet with checksum."""
    assert len(data_120) == 120
    cs = 0x7e ^ CHANNEL ^ 0x02 ^ (packet_num & 0x7f)
    for b in data_120:
        cs ^= b
    cs &= 0x7f
    return bytes([0xf0, 0x7e, CHANNEL, 0x02, packet_num & 0x7f] + list(data_120) + [cs, 0xf7])

def build_sds_dump_request(sample_num):
    """Build an SDS Dump Request message."""
    sl = sample_num & 0x7f
    sh = (sample_num >> 7) & 0x7f
    return bytes([0xf0, 0x7e, CHANNEL, 0x03, sl, sh, 0xf7])

def encode_samples_to_packets(samples, bits_per_word=16):
    """Encode 16-bit samples into 7-bit SDS packets (120 bytes each)."""
    bytes_per_word = (bits_per_word + 6) // 7  # ceil(bits/7)
    encoded = []
    for s in samples:
        val = s & 0xffff  # unsigned
        total_bits = bytes_per_word * 7
        shifted = val << (total_bits - bits_per_word)
        for i in range(bytes_per_word):
            bit_offset = total_bits - 7 * (i + 1)
            encoded.append((shifted >> bit_offset) & 0x7f)

    # Split into 120-byte packets
    packets = []
    for i in range(0, len(encoded), 120):
        chunk = encoded[i:i+120]
        chunk += [0] * (120 - len(chunk))  # zero-pad
        packets.append(bytes(chunk))
    return packets

def generate_test_samples(n):
    """Generate a triangle wave for testing."""
    samples = []
    for i in range(n):
        phase = (i % 256) / 256
        if phase < 0.5:
            val = int(-32768 + phase * 2 * 65535)
        else:
            val = int(32767 - (phase - 0.5) * 2 * 65535)
        samples.append(val & 0xffff)
    return samples

def parse_sds_message(data):
    """Parse a SysEx message and return type + info."""
    if len(data) < 4 or data[0] != 0xf0:
        return f"Unknown ({len(data)} bytes)"
    if data[1] == 0x7e:
        cmd = data[3] if len(data) > 3 else -1
        if cmd == 0x01:
            return "SDS Dump Header"
        elif cmd == 0x02:
            pkt = data[4] if len(data) > 4 else -1
            return f"SDS Data Packet #{pkt}"
        elif cmd == 0x03:
            return "SDS Dump Request"
        elif cmd == 0x7f:
            pkt = data[4] if len(data) > 4 else -1
            return f"SDS ACK (packet {pkt})"
        elif cmd == 0x7e:
            return "SDS NAK"
        elif cmd == 0x7c:
            return "SDS WAIT"
        elif cmd == 0x7d:
            return "SDS CANCEL"
    return f"SysEx ({len(data)} bytes, id=0x{data[1]:02x})"

def main():
    print(f"=== SCSI MIDI Test: sample #{SAMPLE_NUMBER}, {NUM_SAMPLES} samples ===")
    print()

    # Create socket server
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    server.listen(1)
    server.settimeout(30)
    print(f"Listening on {SOCKET_PATH}")
    print("Waiting for SCMP to connect...")

    try:
        conn, _ = server.accept()
        conn.settimeout(5)
        print("SCMP connected!")
        print()

        # === Phase 1: Send sample to S3000XL ===
        print("--- Phase 1: Sending test sample to S3000XL ---")

        samples = generate_test_samples(NUM_SAMPLES)
        period_ns = round(1_000_000_000 / SAMPLE_RATE)
        header = build_sds_dump_header(SAMPLE_NUMBER, 16, period_ns, NUM_SAMPLES)
        packets = encode_samples_to_packets(samples)

        print(f"Dump Header: {header.hex()}")
        print(f"Data Packets: {len(packets)}")

        # Send dump header
        conn.sendall(header)
        print(f"Sent: {parse_sds_message(header)}")

        # Wait for ACK
        time.sleep(0.5)
        try:
            ack = conn.recv(256)
            print(f"Received: {parse_sds_message(ack)} ({ack.hex()})")
        except socket.timeout:
            print("Timeout waiting for ACK")

        # Send data packets
        for i, pkt_data in enumerate(packets):
            pkt = build_sds_data_packet(i, pkt_data)
            conn.sendall(pkt)
            print(f"Sent: {parse_sds_message(pkt)}")

            time.sleep(0.2)
            try:
                ack = conn.recv(256)
                print(f"Received: {parse_sds_message(ack)} ({ack.hex()})")
            except socket.timeout:
                print("Timeout waiting for ACK")

        print()
        print("--- Phase 1 complete: sample sent ---")
        print()

        # === Phase 2: Request sample back ===
        print("--- Phase 2: Requesting sample back ---")
        time.sleep(2)

        dump_req = build_sds_dump_request(SAMPLE_NUMBER)
        conn.sendall(dump_req)
        print(f"Sent: {parse_sds_message(dump_req)}")

        # Read responses
        received_messages = []
        for _ in range(len(packets) + 5):  # header + packets + margin
            try:
                data = conn.recv(4096)
                if not data:
                    break
                received_messages.append(data)
                msg_type = parse_sds_message(data)
                print(f"Received: {msg_type} ({len(data)} bytes)")

                # Send ACK for dump header and data packets
                if len(data) >= 4 and data[0] == 0xf0 and data[1] == 0x7e:
                    cmd = data[3]
                    if cmd == 0x01:  # Dump Header
                        ack = bytes([0xf0, 0x7e, CHANNEL, 0x7f, 0x00, 0xf7])
                        conn.sendall(ack)
                        print(f"Sent: ACK for header")
                    elif cmd == 0x02:  # Data Packet
                        pkt_num = data[4]
                        ack = bytes([0xf0, 0x7e, CHANNEL, 0x7f, pkt_num, 0xf7])
                        conn.sendall(ack)
                        print(f"Sent: ACK for packet {pkt_num}")
            except socket.timeout:
                print("No more data (timeout)")
                break

        print()
        print(f"=== Results: sent {len(packets)} packets, received {len(received_messages)} messages ===")

    except socket.timeout:
        print("ERROR: Timeout waiting for SCMP connection")
    except Exception as e:
        print(f"ERROR: {e}")
    finally:
        server.close()
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)

if __name__ == "__main__":
    main()
