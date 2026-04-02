#!/usr/bin/env python3
"""
Test MIDI-over-SCSI via s2p's protobuf API (port 6868).
Sends protobuf commands directly to the s2p server.

Usage: python3 scripts/midi-test.py [host]
"""

import socket
import struct
import sys

# We need the generated protobuf module. Since we can't import it directly,
# we'll construct the protobuf messages manually using the wire format.
# Alternatively, use s2pctl's approach of connecting to the socket.

# s2p uses a simple protocol: 4-byte big-endian length prefix + serialized PbCommand

HOST = sys.argv[1] if len(sys.argv) > 1 else "localhost"
PORT = 6868

def send_command(sock, serialized_command):
    """Send a protobuf command and receive the result."""
    # Protocol: 6-byte magic "RASCSI" + 4-byte LE length + protobuf payload
    sock.sendall(b"RASCSI")
    sock.sendall(struct.pack("<I", len(serialized_command)))
    sock.sendall(serialized_command)

    # Read response: 4-byte length prefix (LE) + payload (no magic in response)
    length_bytes = b""
    while len(length_bytes) < 4:
        chunk = sock.recv(4 - len(length_bytes))
        if not chunk:
            return None
        length_bytes += chunk
    length = struct.unpack("<I", length_bytes)[0]

    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            break
        data += chunk
    return data

def build_midi_init(target_id=6):
    """Build a PbCommand for MIDI_INIT."""
    # PbOperation MIDI_INIT = 200
    # PbCommand: operation (field 1, varint) + midi_request (field 20, message)
    # PbMidiRequest: target_id (field 1, varint)

    # Encode PbMidiRequest
    midi_req = b""
    midi_req += b"\x08" + _varint(target_id)  # field 1, varint

    # Encode PbCommand
    cmd = b""
    cmd += b"\x08" + _varint(200)  # field 1 (operation), varint = 200 (MIDI_INIT)
    cmd += b"\xa2\x01" + _varint(len(midi_req)) + midi_req  # field 20 (midi_request), length-delimited

    return cmd

def build_midi_send(target_id, sysex_bytes):
    """Build a PbCommand for MIDI_SEND."""
    midi_req = b""
    midi_req += b"\x08" + _varint(target_id)  # field 1
    midi_req += b"\x12" + _varint(len(sysex_bytes)) + sysex_bytes  # field 2, bytes

    cmd = b""
    cmd += b"\x08" + _varint(201)  # MIDI_SEND = 201
    cmd += b"\xa2\x01" + _varint(len(midi_req)) + midi_req

    return cmd

def build_midi_poll(target_id):
    """Build a PbCommand for MIDI_POLL."""
    midi_req = b""
    midi_req += b"\x08" + _varint(target_id)

    cmd = b""
    cmd += b"\x08" + _varint(202)  # MIDI_POLL = 202
    cmd += b"\xa2\x01" + _varint(len(midi_req)) + midi_req

    return cmd

def build_midi_read(target_id, read_length):
    """Build a PbCommand for MIDI_READ."""
    midi_req = b""
    midi_req += b"\x08" + _varint(target_id)
    midi_req += b"\x18" + _varint(read_length)  # field 3

    cmd = b""
    cmd += b"\x08" + _varint(203)  # MIDI_READ = 203
    cmd += b"\xa2\x01" + _varint(len(midi_req)) + midi_req

    return cmd

def _varint(value):
    """Encode an unsigned varint."""
    result = b""
    while value > 127:
        result += bytes([value & 0x7f | 0x80])
        value >>= 7
    result += bytes([value & 0x7f])
    return result

def parse_result(data):
    """Very basic protobuf result parser."""
    # Just look for the midi_response field and extract data
    # This is a simplified parser — for production use the generated protobuf
    i = 0
    fields = {}
    while i < len(data):
        if i >= len(data):
            break
        tag = data[i]
        field_num = tag >> 3
        wire_type = tag & 0x07
        i += 1

        if wire_type == 0:  # varint
            val = 0
            shift = 0
            while i < len(data) and data[i] & 0x80:
                val |= (data[i] & 0x7f) << shift
                shift += 7
                i += 1
            if i < len(data):
                val |= (data[i] & 0x7f) << shift
                i += 1
            fields[field_num] = val
        elif wire_type == 2:  # length-delimited
            length = 0
            shift = 0
            while i < len(data) and data[i] & 0x80:
                length |= (data[i] & 0x7f) << shift
                shift += 7
                i += 1
            if i < len(data):
                length |= (data[i] & 0x7f) << shift
                i += 1
            fields[field_num] = data[i:i+length]
            i += length
        else:
            break

    return fields

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))
    sock.settimeout(5)

    try:
        # Step 1: MIDI_INIT
        print("=== MIDI_INIT ===")
        result = send_command(sock, build_midi_init(6))
        if result:
            fields = parse_result(result)
            status = fields.get(1, -1)  # field 1 = status (bool)
            print(f"  Status: {'OK' if status else 'FAILED'}")

        # Step 2: MIDI_SEND (Akai RSLIST: F0 47 00 04 48 F7)
        print("=== MIDI_SEND (RSLIST) ===")
        sysex = bytes([0xf0, 0x47, 0x00, 0x04, 0x48, 0xf7])
        result = send_command(sock, build_midi_send(6, sysex))
        if result:
            fields = parse_result(result)
            print(f"  Status: {'OK' if fields.get(1) else 'FAILED'}")

        # Step 3: MIDI_POLL
        print("=== MIDI_POLL ===")
        result = send_command(sock, build_midi_poll(6))
        if result:
            fields = parse_result(result)
            # midi_response is in a nested field
            print(f"  Raw fields: {fields}")
            # Look for pending_bytes in the midi_response
            if 101 in fields:  # field 101 = midi_response (oneof)
                midi_resp = parse_result(fields[101])
                pending = midi_resp.get(2, 0)
                print(f"  Pending bytes: {pending}")

                if pending > 0:
                    # Step 4: MIDI_READ
                    print(f"=== MIDI_READ ({pending} bytes) ===")
                    result = send_command(sock, build_midi_read(6, pending))
                    if result:
                        fields = parse_result(result)
                        if 101 in fields:
                            midi_resp = parse_result(fields[101])
                            data = midi_resp.get(1, b"")
                            print(f"  Received {len(data)} bytes:")
                            print(f"  {data.hex()}")
                            # Parse as Akai SysEx
                            if data and data[0] == 0xf0 and data[1] == 0x47:
                                opcode = data[3]
                                print(f"  Akai opcode: 0x{opcode:02x}")
                                if opcode == 0x05:
                                    count = data[5] | (data[6] << 8)
                                    print(f"  SLIST: {count} samples")

        print("\n=== Done ===")

    finally:
        sock.close()

if __name__ == "__main__":
    main()
