#!/usr/bin/env tsx
/**
 * SCSI MIDI protocol probe — test various SysEx commands to understand
 * what the S3000XL accepts/rejects via SCSI CDB 0x0C.
 *
 * Usage: tsx scripts/scsi-protocol-probe.ts [host] [target-id]
 */

import * as net from 'node:net';

const S2P_HOST = process.argv[2] || '10.0.0.57';
const S2P_PORT = 6868;
const TARGET_ID = parseInt(process.argv[3] || '6', 10);

// ---------------------------------------------------------------------------
// Protobuf helpers (same as scsi-write-test.ts)
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
  let val = 0, shift = 0, pos = offset;
  while (pos < buf.length) {
    const b = buf[pos]; val |= (b & 0x7f) << shift; shift += 7; pos++;
    if (!(b & 0x80)) break;
  }
  return [val, pos];
}

interface Fields { varints: Map<number, number>; bytes: Map<number, Buffer>; }

function parseProtobuf(data: Buffer): Fields {
  const fields: Fields = { varints: new Map(), bytes: new Map() };
  let pos = 0;
  while (pos < data.length) {
    const [tag, np] = decodeVarint(data, pos); pos = np;
    const fn = tag >>> 3, wt = tag & 7;
    if (wt === 0) { const [v, np2] = decodeVarint(data, pos); pos = np2; fields.varints.set(fn, v); }
    else if (wt === 2) { const [l, np2] = decodeVarint(data, pos); pos = np2; fields.bytes.set(fn, data.subarray(pos, pos + l)); pos += l; }
    else if (wt === 5) pos += 4;
    else if (wt === 1) pos += 8;
    else break;
  }
  return fields;
}

function buildMidiRequest(tid: number): Buffer {
  return Buffer.concat([Buffer.from([0x08]), encodeVarint(tid)]);
}

function buildCommand(op: number, req: Buffer): Buffer {
  return Buffer.concat([
    Buffer.from([0x08]), encodeVarint(op),
    Buffer.from([0xa2, 0x01]), encodeVarint(req.length), req,
  ]);
}

function buildMidiInit(tid: number): Buffer { return buildCommand(200, buildMidiRequest(tid)); }

function buildMidiSend(tid: number, sysex: Buffer): Buffer {
  return buildCommand(201, Buffer.concat([
    buildMidiRequest(tid), Buffer.from([0x12]), encodeVarint(sysex.length), sysex,
  ]));
}

function buildMidiPoll(tid: number): Buffer { return buildCommand(202, buildMidiRequest(tid)); }

function buildMidiRead(tid: number, length: number): Buffer {
  return buildCommand(203, Buffer.concat([
    buildMidiRequest(tid), Buffer.from([0x18]), encodeVarint(length),
  ]));
}

async function sendCommand(payload: Buffer): Promise<Fields> {
  return new Promise((resolve, reject) => {
    const sock = new net.Socket();
    sock.setTimeout(10_000);
    sock.connect(S2P_PORT, S2P_HOST, () => {
      const magic = Buffer.from('RASCSI');
      const lenBuf = Buffer.alloc(4);
      lenBuf.writeUInt32LE(payload.length);
      sock.write(Buffer.concat([magic, lenBuf, payload]));
    });
    let headerBuf = Buffer.alloc(0);
    let respLen = -1;
    let bodyBuf = Buffer.alloc(0);
    sock.on('data', (chunk) => {
      if (respLen === -1) {
        headerBuf = Buffer.concat([headerBuf, chunk]);
        if (headerBuf.length >= 4) {
          respLen = headerBuf.readUInt32LE(0);
          bodyBuf = headerBuf.subarray(4);
          if (bodyBuf.length >= respLen) { sock.destroy(); resolve(parseProtobuf(bodyBuf.subarray(0, respLen))); }
        }
      } else {
        bodyBuf = Buffer.concat([bodyBuf, chunk]);
        if (bodyBuf.length >= respLen) { sock.destroy(); resolve(parseProtobuf(bodyBuf.subarray(0, respLen))); }
      }
    });
    sock.on('end', () => { if (respLen === -1) reject(new Error('Connection closed')); });
    sock.on('error', reject);
    sock.on('timeout', () => { sock.destroy(); reject(new Error('Timeout')); });
  });
}

// ---------------------------------------------------------------------------
// High-level ops
// ---------------------------------------------------------------------------

async function midiInit(): Promise<boolean> {
  const r = await sendCommand(buildMidiInit(TARGET_ID));
  return (r.varints.get(1) ?? 0) !== 0;
}

async function midiSend(sysex: Buffer): Promise<boolean> {
  const r = await sendCommand(buildMidiSend(TARGET_ID, sysex));
  return (r.varints.get(1) ?? 0) !== 0;
}

async function midiPoll(): Promise<number> {
  const r = await sendCommand(buildMidiPoll(TARGET_ID));
  if (!(r.varints.get(1) ?? 0)) return -1;
  const resp = r.bytes.get(101);
  if (!resp) return 0;
  return parseProtobuf(resp).varints.get(2) ?? 0;
}

async function midiRead(len: number): Promise<Buffer> {
  const r = await sendCommand(buildMidiRead(TARGET_ID, len));
  if (!(r.varints.get(1) ?? 0)) return Buffer.alloc(0);
  const resp = r.bytes.get(101);
  if (!resp) return Buffer.alloc(0);
  return parseProtobuf(resp).bytes.get(1) ?? Buffer.alloc(0);
}

async function sendAndRead(sysex: Buffer): Promise<Buffer> {
  const ok = await midiSend(sysex);
  if (!ok) throw new Error('MIDI_SEND failed');
  await sleep(100);
  const pending = await midiPoll();
  if (pending <= 0) return Buffer.alloc(0);
  return midiRead(pending);
}

function sleep(ms: number): Promise<void> {
  return new Promise(r => setTimeout(r, ms));
}

function hexDump(buf: Buffer, maxBytes = 30): string {
  const hex = Array.from(buf.subarray(0, maxBytes)).map(b => b.toString(16).padStart(2, '0')).join(' ');
  return buf.length > maxBytes ? hex + ` ... (${buf.length} bytes total)` : hex;
}

function akaiSysEx(opcode: number, data: number[]): Buffer {
  return Buffer.from([0xf0, 0x47, 0x00, opcode, 0x48, ...data, 0xf7]);
}

// ---------------------------------------------------------------------------
// Probes
// ---------------------------------------------------------------------------

async function main(): Promise<void> {
  console.log(`SCSI Protocol Probe — s2p at ${S2P_HOST}:${S2P_PORT}, target ${TARGET_ID}\n`);

  // Init
  console.log('=== INIT ===');
  console.log(`  Result: ${await midiInit() ? 'OK' : 'FAILED'}\n`);

  // Probe 1: RSTAT (known working read, 6 bytes)
  console.log('=== Probe 1: RSTAT (request status, 6 bytes) ===');
  const stat = await sendAndRead(akaiSysEx(0x00, []));
  console.log(`  Response: ${stat.length} bytes`);
  console.log(`  Hex: ${hexDump(stat)}\n`);

  // Probe 2: RSLIST (known working read, 6 bytes)
  console.log('=== Probe 2: RSLIST (request sample list, 6 bytes) ===');
  const slist = await sendAndRead(akaiSysEx(0x04, []));
  console.log(`  Response: ${slist.length} bytes`);
  console.log(`  Hex: ${hexDump(slist)}\n`);

  // Probe 3: RPDATA (read program 0, 8 bytes)
  console.log('=== Probe 3: RPDATA (read program 0, 8 bytes) ===');
  const pdata = await sendAndRead(akaiSysEx(0x06, [0x00, 0x00]));
  console.log(`  Response: ${pdata.length} bytes`);
  console.log(`  Hex: ${hexDump(pdata)}\n`);

  // Probe 4: Send PDATA (write) WITHOUT poll — then re-read
  console.log('=== Probe 4: PDATA write (no poll), then re-read ===');
  if (pdata.length > 10) {
    // Modify one nibble deep in the data (offset 20 in the data portion)
    const modified = Buffer.from(pdata);
    const testOffset = 20;
    const origVal = modified[5 + testOffset];
    modified[5 + testOffset] = origVal === 0 ? 1 : 0;
    console.log(`  Modifying byte at data offset ${testOffset}: ${origVal} → ${modified[5 + testOffset]}`);
    console.log(`  Sending ${modified.length} bytes via MIDI_SEND...`);
    const sendOk = await midiSend(modified);
    console.log(`  MIDI_SEND: ${sendOk ? 'OK' : 'FAILED'}`);
    await sleep(200);

    // Check what's in the poll buffer after the write
    console.log(`  Polling after write...`);
    const postWritePending = await midiPoll();
    console.log(`  Pending after write: ${postWritePending} bytes`);
    if (postWritePending > 0) {
      const postWriteData = await midiRead(postWritePending);
      console.log(`  Post-write response: ${hexDump(postWriteData)}`);
    }

    // Re-read
    console.log(`  Re-reading program 0...`);
    const reread = await sendAndRead(akaiSysEx(0x06, [0x00, 0x00]));
    console.log(`  Re-read: ${reread.length} bytes`);
    if (reread.length > 5 + testOffset) {
      const newVal = reread[5 + testOffset];
      console.log(`  Byte at offset ${testOffset}: ${newVal} (was ${origVal})`);
      if (newVal === modified[5 + testOffset]) {
        console.log(`  ✓ WRITE PERSISTED`);
      } else if (newVal === origVal) {
        console.log(`  ✗ Write did not persist (original value)`);
      } else {
        console.log(`  ? Unexpected value`);
      }
    }

    // Restore
    console.log(`  Restoring original...`);
    await midiSend(pdata);
    await sleep(200);
  }

  // Probe 5: Send a small write — just the program name
  // Use a different approach: send ONLY the SysEx, poll immediately
  console.log('\n=== Probe 5: PDATA write WITH immediate poll ===');
  if (pdata.length > 10) {
    const modified = Buffer.from(pdata);
    modified[5 + 20] = modified[5 + 20] === 0 ? 1 : 0;
    console.log(`  Sending ${modified.length} bytes...`);
    const sendOk = await midiSend(modified);
    console.log(`  MIDI_SEND: ${sendOk ? 'OK' : 'FAILED'}`);
    // Poll immediately
    const pending = await midiPoll();
    console.log(`  Immediate poll: ${pending} bytes pending`);
    if (pending > 0) {
      const resp = await midiRead(pending);
      console.log(`  Response: ${hexDump(resp)}`);
      if (resp.length > 3) {
        console.log(`  Response opcode: 0x${resp[3].toString(16).padStart(2, '0')}`);
      }
    }

    // Restore
    await midiSend(pdata);
    await sleep(200);
  }

  // Probe 6: Re-init and try fresh read to check buffer health
  console.log('\n=== Probe 6: Re-init + fresh read (buffer health check) ===');
  console.log(`  Re-init: ${await midiInit() ? 'OK' : 'FAILED'}`);
  await sleep(200);
  const fresh = await sendAndRead(akaiSysEx(0x06, [0x00, 0x00]));
  console.log(`  Fresh read: ${fresh.length} bytes`);
  console.log(`  Match original: ${fresh.length === pdata.length && Buffer.compare(fresh, pdata) === 0}`);
  console.log(`  Hex: ${hexDump(fresh)}\n`);

  console.log('Done.');
}

main().catch(err => { console.error('Fatal:', err); process.exit(1); });
