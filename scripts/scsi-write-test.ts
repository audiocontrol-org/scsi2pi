#!/usr/bin/env tsx
/**
 * Direct protobuf test for SCSI MIDI write round-trip.
 *
 * Talks directly to s2p's protobuf API (port 6868) — no bridge, no browser,
 * no Playwright. Isolates the SCSI write path to determine whether PDATA
 * writes via CDB 0x0C persist on the S3000XL.
 *
 * Usage: tsx scripts/scsi-write-test.ts [host] [target-id]
 *   host: s2p host (default: 10.0.0.57)
 *   target-id: SCSI target ID (default: 6)
 *
 * Prerequisites: s2p running on the Pi (sudo /tmp/s2p-midi --port 6868)
 */

import * as net from 'node:net';

const S2P_HOST = process.argv[2] || '10.0.0.57';
const S2P_PORT = 6868;
const TARGET_ID = parseInt(process.argv[3] || '6', 10);

// Akai SysEx constants
const SYSEX_START = 0xf0;
const SYSEX_END = 0xf7;
const AKAI_MFR = 0x47;
const S3K_DEVICE = 0x48;
const CHANNEL = 0x00;

// Akai opcodes
const RPDATA = 0x06; // Request program data
const PDATA = 0x07;  // Program data (read response / write command)

// s2p protobuf operations
const MIDI_INIT = 200;
const MIDI_SEND = 201;
const MIDI_POLL = 202;
const MIDI_READ = 203;

// ---------------------------------------------------------------------------
// Protobuf encoding
// ---------------------------------------------------------------------------

function encodeVarint(value: number): Buffer {
  const bytes: number[] = [];
  while (value > 127) {
    bytes.push((value & 0x7f) | 0x80);
    value >>>= 7;
  }
  bytes.push(value & 0x7f);
  return Buffer.from(bytes);
}

function decodeVarint(buf: Buffer, offset: number): [number, number] {
  let val = 0;
  let shift = 0;
  let pos = offset;
  while (pos < buf.length) {
    const b = buf[pos];
    val |= (b & 0x7f) << shift;
    shift += 7;
    pos++;
    if (!(b & 0x80)) break;
  }
  return [val, pos];
}

interface Fields {
  varints: Map<number, number>;
  bytes: Map<number, Buffer>;
}

function parseProtobuf(data: Buffer): Fields {
  const fields: Fields = { varints: new Map(), bytes: new Map() };
  let pos = 0;
  while (pos < data.length) {
    const [tag, newPos] = decodeVarint(data, pos);
    pos = newPos;
    const fieldNum = tag >>> 3;
    const wireType = tag & 0x07;
    if (wireType === 0) {
      const [val, np] = decodeVarint(data, pos);
      pos = np;
      fields.varints.set(fieldNum, val);
    } else if (wireType === 2) {
      const [len, np] = decodeVarint(data, pos);
      pos = np;
      fields.bytes.set(fieldNum, data.subarray(pos, pos + len));
      pos += len;
    } else if (wireType === 5) {
      pos += 4;
    } else if (wireType === 1) {
      pos += 8;
    } else {
      break;
    }
  }
  return fields;
}

// ---------------------------------------------------------------------------
// s2p command builders
// ---------------------------------------------------------------------------

function buildMidiRequest(targetId: number): Buffer {
  return Buffer.concat([Buffer.from([0x08]), encodeVarint(targetId)]);
}

function buildCommand(operation: number, midiReq: Buffer): Buffer {
  const parts: Buffer[] = [];
  parts.push(Buffer.from([0x08]), encodeVarint(operation));
  // field 20 (midi_request): tag = (20 << 3) | 2 = 0xa2 0x01
  parts.push(Buffer.from([0xa2, 0x01]), encodeVarint(midiReq.length), midiReq);
  return Buffer.concat(parts);
}

function buildMidiInit(targetId: number): Buffer {
  return buildCommand(MIDI_INIT, buildMidiRequest(targetId));
}

function buildMidiSend(targetId: number, sysex: Buffer): Buffer {
  const req = Buffer.concat([
    buildMidiRequest(targetId),
    Buffer.from([0x12]),
    encodeVarint(sysex.length),
    sysex,
  ]);
  return buildCommand(MIDI_SEND, req);
}

function buildMidiPoll(targetId: number): Buffer {
  return buildCommand(MIDI_POLL, buildMidiRequest(targetId));
}

function buildMidiRead(targetId: number, length: number): Buffer {
  const req = Buffer.concat([
    buildMidiRequest(targetId),
    Buffer.from([0x18]),
    encodeVarint(length),
  ]);
  return buildCommand(MIDI_READ, req);
}

// ---------------------------------------------------------------------------
// TCP client
// ---------------------------------------------------------------------------

async function sendCommand(payload: Buffer): Promise<Fields> {
  return new Promise((resolve, reject) => {
    const sock = new net.Socket();
    sock.setTimeout(10_000);

    sock.connect(S2P_PORT, S2P_HOST, () => {
      // Send: "RASCSI" magic + 4-byte LE length + payload
      const magic = Buffer.from('RASCSI');
      const lenBuf = Buffer.alloc(4);
      lenBuf.writeUInt32LE(payload.length);
      sock.write(Buffer.concat([magic, lenBuf, payload]));
    });

    // s2p sends: 4-byte LE length + protobuf payload, then closes.
    // But it may not close immediately — read the length prefix first,
    // then read exactly that many bytes.
    let headerBuf = Buffer.alloc(0);
    let respLen = -1;
    let bodyBuf = Buffer.alloc(0);

    sock.on('data', (chunk) => {
      if (respLen === -1) {
        headerBuf = Buffer.concat([headerBuf, chunk]);
        if (headerBuf.length >= 4) {
          respLen = headerBuf.readUInt32LE(0);
          bodyBuf = headerBuf.subarray(4);
          if (bodyBuf.length >= respLen) {
            sock.destroy();
            resolve(parseProtobuf(bodyBuf.subarray(0, respLen)));
          }
        }
      } else {
        bodyBuf = Buffer.concat([bodyBuf, chunk]);
        if (bodyBuf.length >= respLen) {
          sock.destroy();
          resolve(parseProtobuf(bodyBuf.subarray(0, respLen)));
        }
      }
    });
    sock.on('end', () => {
      if (respLen === -1) {
        reject(new Error('Connection closed before response'));
      }
      // Already resolved above
    });
    sock.on('error', reject);
    sock.on('timeout', () => {
      sock.destroy();
      reject(new Error('Socket timeout'));
    });
  });
}

// ---------------------------------------------------------------------------
// High-level operations
// ---------------------------------------------------------------------------

async function midiInit(): Promise<boolean> {
  const result = await sendCommand(buildMidiInit(TARGET_ID));
  return (result.varints.get(1) ?? 0) !== 0;
}

async function midiSendAndRead(sysex: Buffer): Promise<Buffer> {
  // Send
  const sendResult = await sendCommand(buildMidiSend(TARGET_ID, sysex));
  if (!(sendResult.varints.get(1) ?? 0)) {
    throw new Error('MIDI_SEND failed');
  }

  // Poll
  const pollResult = await sendCommand(buildMidiPoll(TARGET_ID));
  if (!(pollResult.varints.get(1) ?? 0)) {
    throw new Error('MIDI_POLL failed');
  }
  const midiResp = pollResult.bytes.get(101);
  if (!midiResp) {
    throw new Error('No midi_response in POLL result');
  }
  const pollFields = parseProtobuf(midiResp);
  const pending = pollFields.varints.get(2) ?? 0;
  if (pending === 0) {
    return Buffer.alloc(0);
  }

  // Read
  const readResult = await sendCommand(buildMidiRead(TARGET_ID, pending));
  if (!(readResult.varints.get(1) ?? 0)) {
    throw new Error('MIDI_READ failed');
  }
  const readResp = readResult.bytes.get(101);
  if (!readResp) {
    return Buffer.alloc(0);
  }
  const readFields = parseProtobuf(readResp);
  return readFields.bytes.get(1) ?? Buffer.alloc(0);
}

async function midiSendOnly(sysex: Buffer): Promise<boolean> {
  const result = await sendCommand(buildMidiSend(TARGET_ID, sysex));
  return (result.varints.get(1) ?? 0) !== 0;
}

function buildAkaiSysEx(opcode: number, data: number[]): Buffer {
  return Buffer.from([SYSEX_START, AKAI_MFR, CHANNEL, opcode, S3K_DEVICE, ...data, SYSEX_END]);
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

async function main(): Promise<void> {
  console.log(`SCSI Write Test — s2p at ${S2P_HOST}:${S2P_PORT}, target ID ${TARGET_ID}`);
  console.log('');

  // Step 1: Init
  console.log('Step 1: MIDI_INIT');
  const initOk = await midiInit();
  console.log(`  Result: ${initOk ? 'OK' : 'FAILED'}`);
  if (!initOk) process.exit(1);

  // Step 2: Read program 0 header
  console.log('');
  console.log('Step 2: Read program 0 (RPDATA)');
  const rpdata = buildAkaiSysEx(RPDATA, [0x00, 0x00]); // program 0
  const progData = await midiSendAndRead(rpdata);
  console.log(`  Response: ${progData.length} bytes`);
  if (progData.length < 10) {
    console.error('  ERROR: Response too short');
    process.exit(1);
  }
  console.log(`  Opcode: 0x${progData[3].toString(16).padStart(2, '0')}`);
  console.log(`  Data[5..15]: [${Array.from(progData.subarray(5, 15)).join(', ')}]`);

  // The program header data starts at byte 5 (after F0 47 CH OP DEV)
  // Find polyphony — it's encoded in the Akai nibblized format
  // For now, just modify a known byte and see if it persists
  const MODIFY_OFFSET = 10; // pick an offset in the data portion
  const originalByte = progData[5 + MODIFY_OFFSET];
  const testByte = originalByte === 0 ? 1 : 0;
  console.log(`  Byte at offset ${MODIFY_OFFSET}: ${originalByte} (will change to ${testByte})`);

  // Step 3: Write modified program header
  console.log('');
  console.log('Step 3: Write modified program header (PDATA)');
  // The PDATA write is the same as the read response — opcode 0x07
  // We send back the full response with one byte changed
  const writeData = Buffer.from(progData);
  writeData[5 + MODIFY_OFFSET] = testByte;
  console.log(`  Sending ${writeData.length} bytes (PDATA opcode 0x07)`);

  // Try A: Send without poll (fire-and-forget)
  console.log('  Method A: fire-and-forget (no poll after write)');
  const sendOk = await midiSendOnly(writeData);
  console.log(`  MIDI_SEND result: ${sendOk ? 'OK' : 'FAILED'}`);

  // Small delay for S3000XL to process
  await new Promise((r) => setTimeout(r, 500));

  // Step 4: Re-read program 0
  console.log('');
  console.log('Step 4: Re-read program 0 (RPDATA)');
  const progData2 = await midiSendAndRead(rpdata);
  console.log(`  Response: ${progData2.length} bytes`);
  if (progData2.length >= 10) {
    const rereadByte = progData2[5 + MODIFY_OFFSET];
    console.log(`  Byte at offset ${MODIFY_OFFSET}: ${rereadByte}`);
    if (rereadByte === testByte) {
      console.log('  ✓ WRITE PERSISTED — the byte changed!');
    } else if (rereadByte === originalByte) {
      console.log('  ✗ WRITE DID NOT PERSIST — byte is still the original value');
    } else {
      console.log(`  ? UNEXPECTED VALUE: expected ${testByte} or ${originalByte}, got ${rereadByte}`);
    }
  }

  // Step 5: Try method B — send with poll to see what S3000XL responds
  console.log('');
  console.log('Step 5: Send PDATA with poll (to see S3000XL response)');
  const writeData2 = Buffer.from(progData);
  writeData2[5 + MODIFY_OFFSET] = testByte;
  const writeResp = await midiSendAndRead(writeData2);
  console.log(`  Response: ${writeResp.length} bytes`);
  if (writeResp.length > 0) {
    console.log(`  Opcode: 0x${writeResp[3]?.toString(16).padStart(2, '0')}`);
    console.log(`  First 20 bytes: [${Array.from(writeResp.subarray(0, 20)).join(', ')}]`);
  } else {
    console.log('  No response from S3000XL after PDATA write');
  }

  // Step 6: Restore original value
  console.log('');
  console.log('Step 6: Restore original value');
  await midiSendOnly(progData);
  await new Promise((r) => setTimeout(r, 500));

  // Final read to confirm state
  const progData3 = await midiSendAndRead(rpdata);
  const finalByte = progData3.length > 5 + MODIFY_OFFSET ? progData3[5 + MODIFY_OFFSET] : -1;
  console.log(`  Final byte at offset ${MODIFY_OFFSET}: ${finalByte} (expected ${originalByte})`);
  console.log('');
  console.log('Done.');
}

main().catch((err) => {
  console.error('Fatal:', err);
  process.exit(1);
});
