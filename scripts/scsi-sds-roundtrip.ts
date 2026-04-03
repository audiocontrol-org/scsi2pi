#!/usr/bin/env tsx
/**
 * Full SDS sample round-trip over SCSI: send a sample, then receive it back.
 *
 * Usage: tsx scripts/scsi-sds-roundtrip.ts [host] [target-id]
 */

import * as net from 'node:net';

const S2P_HOST = process.argv[2] || '10.0.0.57';
const S2P_PORT = 6868;
const TARGET_ID = parseInt(process.argv[3] || '6', 10);

// --- Protobuf helpers (compact) ---
function ev(v: number): Buffer { const b: number[] = []; while (v > 127) { b.push((v & 0x7f) | 0x80); v >>>= 7; } b.push(v & 0x7f); return Buffer.from(b); }
function dv(buf: Buffer, o: number): [number, number] { let v = 0, s = 0, p = o; while (p < buf.length) { const b = buf[p]; v |= (b & 0x7f) << s; s += 7; p++; if (!(b & 0x80)) break; } return [v, p]; }
interface F { vi: Map<number, number>; by: Map<number, Buffer>; }
function pp(d: Buffer): F { const f: F = { vi: new Map(), by: new Map() }; let p = 0; while (p < d.length) { const [t, np] = dv(d, p); p = np; const fn = t >>> 3, wt = t & 7; if (wt === 0) { const [v, np2] = dv(d, p); p = np2; f.vi.set(fn, v); } else if (wt === 2) { const [l, np2] = dv(d, p); p = np2; f.by.set(fn, d.subarray(p, p + l)); p += l; } else if (wt === 5) p += 4; else if (wt === 1) p += 8; else break; } return f; }
function mr(t: number): Buffer { return Buffer.concat([Buffer.from([0x08]), ev(t)]); }
function bc(op: number, req: Buffer): Buffer { return Buffer.concat([Buffer.from([0x08]), ev(op), Buffer.from([0xa2, 0x01]), ev(req.length), req]); }

async function sc(payload: Buffer): Promise<F> {
  return new Promise((resolve, reject) => {
    const sock = new net.Socket(); sock.setTimeout(15_000);
    sock.connect(S2P_PORT, S2P_HOST, () => { const m = Buffer.from('RASCSI'); const l = Buffer.alloc(4); l.writeUInt32LE(payload.length); sock.write(Buffer.concat([m, l, payload])); });
    let hb = Buffer.alloc(0); let rl = -1; let bb = Buffer.alloc(0);
    sock.on('data', (c) => { if (rl === -1) { hb = Buffer.concat([hb, c]); if (hb.length >= 4) { rl = hb.readUInt32LE(0); bb = hb.subarray(4); if (bb.length >= rl) { sock.destroy(); resolve(pp(bb.subarray(0, rl))); } } } else { bb = Buffer.concat([bb, c]); if (bb.length >= rl) { sock.destroy(); resolve(pp(bb.subarray(0, rl))); } } });
    sock.on('end', () => { if (rl === -1) reject(new Error('Closed')); });
    sock.on('error', reject); sock.on('timeout', () => { sock.destroy(); reject(new Error('Timeout')); });
  });
}

async function init(): Promise<boolean> { return ((await sc(bc(200, mr(TARGET_ID)))).vi.get(1) ?? 0) !== 0; }
async function send(sysex: Buffer): Promise<boolean> {
  const r = Buffer.concat([mr(TARGET_ID), Buffer.from([0x12]), ev(sysex.length), sysex]);
  return ((await sc(bc(201, r))).vi.get(1) ?? 0) !== 0;
}
async function poll(): Promise<number> {
  const r = await sc(bc(202, mr(TARGET_ID)));
  if (!(r.vi.get(1) ?? 0)) return -1;
  const resp = r.by.get(101);
  if (!resp) return 0;
  return pp(resp).vi.get(2) ?? 0;
}
async function read(len: number): Promise<Buffer> {
  const req = Buffer.concat([mr(TARGET_ID), Buffer.from([0x18]), ev(len)]);
  const r = await sc(bc(203, req));
  if (!(r.vi.get(1) ?? 0)) return Buffer.alloc(0);
  const resp = r.by.get(101); if (!resp) return Buffer.alloc(0);
  return pp(resp).by.get(1) ?? Buffer.alloc(0);
}
/** Send SysEx, wait, poll+read response. Handles multi-message responses. */
async function sendAndReadAll(sysex: Buffer): Promise<Buffer[]> {
  if (!await send(sysex)) throw new Error('SEND failed');
  await sleep(200);
  const responses: Buffer[] = [];
  // Poll and drain all available data
  let pending = await poll();
  while (pending > 0) {
    const data = await read(pending);
    if (data.length === 0) break;
    // Split concatenated SysEx messages
    let start = 0;
    for (let i = 0; i < data.length; i++) {
      if (data[i] === 0xf7) {
        responses.push(Buffer.from(data.subarray(start, i + 1)));
        start = i + 1;
      }
    }
    if (start < data.length) {
      responses.push(Buffer.from(data.subarray(start)));
    }
    await sleep(50);
    pending = await poll();
  }
  return responses;
}
function sleep(ms: number): Promise<void> { return new Promise(r => setTimeout(r, ms)); }
function hex(b: Buffer, n = 30): string {
  const h = Array.from(b.subarray(0, n)).map(x => x.toString(16).padStart(2, '0')).join(' ');
  return b.length > n ? h + ` ... (${b.length}b)` : h;
}

// SDS message types
function sdsType(msg: Buffer): string {
  if (msg.length < 4 || msg[0] !== 0xf0 || msg[1] !== 0x7e) return 'not-sds';
  const types: Record<number, string> = {
    0x01: 'DUMP_HEADER', 0x02: 'DATA_PACKET', 0x03: 'DUMP_REQUEST',
    0x7c: 'WAIT', 0x7d: 'CANCEL', 0x7e: 'NAK', 0x7f: 'ACK',
  };
  return types[msg[3]] || `0x${msg[3].toString(16)}`;
}

// --- Main ---

async function main(): Promise<void> {
  console.log(`SDS Round-Trip — ${S2P_HOST}:${S2P_PORT}, target ${TARGET_ID}\n`);

  // Init
  console.log(`Init: ${await init() ? 'OK' : 'FAILED'}`);

  // Check current sample count
  const slistBefore = (await sendAndReadAll(Buffer.from([0xf0, 0x47, 0x00, 0x04, 0x48, 0xf7])))[0];
  const sampleCountBefore = slistBefore && slistBefore.length > 5 ? slistBefore[5] : 0;
  console.log(`Samples before: ${sampleCountBefore}\n`);

  // ------------------------------------------------------------------
  // Step 1: Send SDS Dump Header (44100Hz, 16-bit, 100 samples)
  // ------------------------------------------------------------------
  const sampleNum = 99;
  const sampleLength = 100;
  const dumpHeader = Buffer.from([
    0xf0, 0x7e, 0x00, 0x01,
    sampleNum & 0x7f, (sampleNum >> 7) & 0x7f,
    16,          // bits per word
    20, 49, 1,   // sample period (44100 Hz)
    sampleLength & 0x7f, (sampleLength >> 7) & 0x7f, 0,  // length
    0, 0, 0,     // loop start
    sampleLength & 0x7f, (sampleLength >> 7) & 0x7f, 0,  // loop end
    0x7f,        // loop type
    0xf7,
  ]);

  console.log('Step 1: Send Dump Header');
  console.log(`  ${hex(dumpHeader)}`);
  const headerResps = await sendAndReadAll(dumpHeader);
  for (const r of headerResps) {
    console.log(`  Response: ${sdsType(r)} — ${hex(r)}`);
  }

  // Check for ACK
  const gotAck = headerResps.some(r => r.length >= 4 && r[3] === 0x7f);
  if (!gotAck) {
    console.log('  No ACK received — aborting');
    process.exit(1);
  }
  console.log('  ✓ Device ACKed the header\n');

  // ------------------------------------------------------------------
  // Step 2: Send data packets
  // ------------------------------------------------------------------
  // 100 samples × 16 bits = 200 bytes of audio data
  // SDS packets carry 120 bytes each (= 40 × 3-byte words for 16-bit)
  // So we need ceil(100/40) = 3 packets

  const samplesPerPacket = 40; // 16-bit: 3 bytes per sample, 120 bytes per packet
  const totalPackets = Math.ceil(sampleLength / samplesPerPacket);

  console.log(`Step 2: Send ${totalPackets} data packet(s)`);

  for (let pkt = 0; pkt < totalPackets; pkt++) {
    // Generate test tone: 440Hz sine wave
    const packetData = Buffer.alloc(120, 0);
    for (let s = 0; s < samplesPerPacket; s++) {
      const sampleIdx = pkt * samplesPerPacket + s;
      if (sampleIdx >= sampleLength) break;
      // 16-bit signed sine wave, encode as 3 × 7-bit SDS format
      const value = Math.round(Math.sin(2 * Math.PI * 440 * sampleIdx / 44100) * 32767);
      const unsigned = value < 0 ? value + 65536 : value;
      // SDS 16-bit encoding: MSB first, 7 bits per byte, 3 bytes
      packetData[s * 3] = (unsigned >> 9) & 0x7f;      // bits 15-9
      packetData[s * 3 + 1] = (unsigned >> 2) & 0x7f;   // bits 8-2
      packetData[s * 3 + 2] = (unsigned << 5) & 0x7f;   // bits 1-0 (shifted)
    }

    let checksum = 0x7e ^ 0x00 ^ 0x02 ^ (pkt & 0x7f);
    for (const b of packetData) checksum ^= b;
    checksum &= 0x7f;

    const dataPacket = Buffer.from([
      0xf0, 0x7e, 0x00, 0x02,
      pkt & 0x7f,
      ...packetData,
      checksum,
      0xf7,
    ]);

    console.log(`  Packet ${pkt}: ${dataPacket.length} bytes`);
    const pktResps = await sendAndReadAll(dataPacket);
    for (const r of pktResps) {
      console.log(`    Response: ${sdsType(r)} — ${hex(r)}`);
    }

    const pktAck = pktResps.some(r => r.length >= 4 && r[3] === 0x7f);
    if (!pktAck) {
      console.log(`    No ACK for packet ${pkt} — aborting`);
      break;
    }
  }

  console.log('');

  // ------------------------------------------------------------------
  // Step 3: Check sample list
  // ------------------------------------------------------------------
  console.log('Step 3: Check sample list');
  await init(); await sleep(200);
  const slistAfter = (await sendAndReadAll(Buffer.from([0xf0, 0x47, 0x00, 0x04, 0x48, 0xf7])))[0];
  const sampleCountAfter = slistAfter && slistAfter.length > 5 ? slistAfter[5] : 0;
  console.log(`  Samples: ${sampleCountBefore} → ${sampleCountAfter}`);
  if (sampleCountAfter > sampleCountBefore) {
    console.log(`  ✓ SDS SAMPLE CREATED OVER SCSI!`);
  } else {
    console.log(`  Sample count unchanged`);
  }

  console.log('\nDone.');
}

main().catch(err => { console.error('Fatal:', err); process.exit(1); });
