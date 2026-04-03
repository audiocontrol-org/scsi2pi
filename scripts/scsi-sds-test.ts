#!/usr/bin/env tsx
/**
 * Test SDS sample transfer over SCSI and re-examine writes.
 *
 * MESA worked over SCSI — writes MUST be possible. Maybe the issue is:
 * 1. The 172-byte "response" after a write is the S3000XL's acknowledgment
 *    that we need to drain before re-reading
 * 2. We need to poll+drain after writes, not skip the poll
 * 3. The write DID persist but re-read is returning stale/cached data
 * 4. The Akai SysEx data is nibblized and we need to handle it differently
 *
 * This script tests both SDS and parameter writes with proper drain logic.
 *
 * Usage: tsx scripts/scsi-sds-test.ts [host] [target-id]
 */

import * as net from 'node:net';

const S2P_HOST = process.argv[2] || '10.0.0.57';
const S2P_PORT = 6868;
const TARGET_ID = parseInt(process.argv[3] || '6', 10);

// --- Protobuf helpers ---

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
async function sendAndRead(sysex: Buffer): Promise<Buffer> {
  if (!await send(sysex)) throw new Error('SEND failed');
  await sleep(150);
  const p = await poll();
  if (p <= 0) return Buffer.alloc(0);
  return read(p);
}
/** Send, poll+drain response, return it */
async function sendAndDrain(sysex: Buffer): Promise<Buffer> {
  if (!await send(sysex)) throw new Error('SEND failed');
  await sleep(150);
  const p = await poll();
  if (p > 0) return read(p);
  return Buffer.alloc(0);
}
/** Drain any buffered data without sending */
async function drain(): Promise<Buffer> {
  const p = await poll();
  if (p > 0) return read(p);
  return Buffer.alloc(0);
}

function sleep(ms: number): Promise<void> { return new Promise(r => setTimeout(r, ms)); }
function hex(b: Buffer, n = 40): string {
  const h = Array.from(b.subarray(0, n)).map(x => x.toString(16).padStart(2, '0')).join(' ');
  return b.length > n ? h + ` ... (${b.length}b)` : h;
}

// --- Tests ---

async function main(): Promise<void> {
  console.log(`SDS + Write Test — ${S2P_HOST}:${S2P_PORT}, target ${TARGET_ID}\n`);

  console.log('=== INIT ===');
  console.log(`  ${await init() ? 'OK' : 'FAILED'}\n`);

  // ------------------------------------------------------------------
  // Test 1: Read program, write with DRAIN, re-read
  // Maybe the 172-byte post-write response is important — drain it
  // ------------------------------------------------------------------
  console.log('=== Test 1: PDATA write with full drain cycle ===');
  const prog = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
  console.log(`  Read: ${prog.length}b`);

  if (prog.length >= 30) {
    const testOff = 25;
    const origVal = prog[testOff];
    const testVal = origVal === 0 ? 1 : (origVal - 1);
    console.log(`  Byte[${testOff}]: ${origVal} → ${testVal}`);

    const mod = Buffer.from(prog);
    mod[testOff] = testVal;

    // Send the write
    console.log(`  Sending PDATA write (${mod.length}b)...`);
    const sendOk = await send(mod);
    console.log(`  Send: ${sendOk ? 'OK' : 'FAILED'}`);

    // Drain whatever the S3000XL responds with
    await sleep(300);
    const writeResp = await drain();
    console.log(`  Write response: ${writeResp.length}b${writeResp.length > 0 ? ' → ' + hex(writeResp, 20) : ''}`);

    // If there's more, keep draining
    await sleep(100);
    const extra = await drain();
    if (extra.length > 0) console.log(`  Extra drain: ${extra.length}b → ${hex(extra, 20)}`);

    // Now re-read
    console.log(`  Re-reading...`);
    const reread = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
    console.log(`  Re-read: ${reread.length}b`);
    if (reread.length > testOff) {
      const newVal = reread[testOff];
      console.log(`  Byte[${testOff}]: ${newVal} (expected ${testVal})`);
      console.log(`  ${newVal === testVal ? '✓ PERSISTED!' : '✗ unchanged'}`);
    }
  }

  // Re-init between tests
  console.log(`\n  Re-init: ${await init() ? 'OK' : 'FAILED'}`);
  await sleep(200);

  // ------------------------------------------------------------------
  // Test 2: Try writing program name only (first 12 bytes of data)
  // Maybe the S3000XL has a size limit per SCSI MIDI message
  // ------------------------------------------------------------------
  console.log('\n=== Test 2: Small write — program name only ===');
  const prog2 = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
  if (prog2.length >= 20) {
    // Program name is the first ~24 nibblized bytes in the data portion
    // Let's read the current name from the program list instead
    const nameList = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x02, 0x48, 0xf7]));
    console.log(`  RPLIST: ${nameList.length}b → ${hex(nameList, 30)}`);

    // Try APDATA partial write: opcode 0x4A
    // But this time, let's examine the 172-byte response more carefully
    const apdata = Buffer.from([
      0xf0, 0x47, 0x00, 0x4a, 0x48,
      0x00, 0x00,       // program 0
      0x00, 0x00,       // offset 0 (start of name)
      0x01, 0x00,       // count 1 byte
      0x0f,             // value: 0x0f (should change first name nibble)
      0xf7,
    ]);
    console.log(`  APDATA: ${hex(apdata)}`);
    const apdataResp = await sendAndDrain(apdata);
    console.log(`  APDATA response: ${apdataResp.length}b${apdataResp.length > 0 ? ' → ' + hex(apdataResp, 20) : ' (empty)'}`);
    if (apdataResp.length > 0 && apdataResp[0] === 0xf0) {
      console.log(`  Response opcode: 0x${apdataResp[3]?.toString(16).padStart(2, '0')}`);
    }
  }

  console.log(`\n  Re-init: ${await init() ? 'OK' : 'FAILED'}`);
  await sleep(200);

  // ------------------------------------------------------------------
  // Test 3: SDS Dump Request — send a sample TO the S3000XL
  // SDS uses standard MIDI SDS protocol (manufacturer 0x7E)
  // ------------------------------------------------------------------
  console.log('\n=== Test 3: SDS Dump Header (send sample to device) ===');
  {
    // SDS Dump Header: F0 7E <channel> 01 <sample_num_lo> <sample_num_hi>
    //   <bits_per_word> <sample_period_lo> <sample_period_mid> <sample_period_hi>
    //   <sample_length_lo> <sample_length_mid> <sample_length_hi>
    //   <sustain_loop_start_lo> ... <sustain_loop_end_lo> ... <loop_type> F7

    // Send a tiny 100-sample test tone at 44100Hz, 16-bit
    // Sample period = 1/44100 * 10^9 ns = 22675.7 ns
    // Encoded as 3 x 7-bit: 22676 = 0x5894
    //   lo = 22676 & 0x7f = 0x14 = 20
    //   mid = (22676 >> 7) & 0x7f = 0x31 = 49
    //   hi = (22676 >> 14) & 0x7f = 0x01 = 1
    // Sample length = 100
    //   lo = 100 & 0x7f = 100
    //   mid = 0, hi = 0

    const sampleNum = 99; // use high number to not conflict with existing samples
    const dumpHeader = Buffer.from([
      0xf0, 0x7e, 0x00, 0x01,   // SDS Dump Header
      sampleNum & 0x7f, (sampleNum >> 7) & 0x7f,  // sample number
      16,                         // bits per word
      20, 49, 1,                  // sample period (44100 Hz)
      100, 0, 0,                  // sample length = 100
      0, 0, 0,                    // sustain loop start
      100, 0, 0,                  // sustain loop end
      0x7f,                       // loop type: forward+release
      0xf7,
    ]);
    console.log(`  Dump Header: ${hex(dumpHeader)}`);
    const resp = await sendAndDrain(dumpHeader);
    console.log(`  Response: ${resp.length}b${resp.length > 0 ? ' → ' + hex(resp) : ' (empty)'}`);
    if (resp.length > 0) {
      // Check for ACK (F0 7E ch 7F pp F7) or NAK (F0 7E ch 7E pp F7)
      // or WAIT (F0 7E ch 7C pp F7) or CANCEL (F0 7E ch 7D pp F7)
      if (resp[0] === 0xf0 && resp[1] === 0x7e) {
        const msgType = resp[3];
        const typeNames: Record<number, string> = {
          0x7f: 'ACK', 0x7e: 'NAK', 0x7d: 'CANCEL', 0x7c: 'WAIT',
        };
        console.log(`  SDS response: ${typeNames[msgType] || `0x${msgType.toString(16)}`}`);
      }
    }

    // If we got an ACK, the device is ready for data packets
    // Send one data packet
    if (resp.length > 0 && resp[3] === 0x7f) {
      console.log(`  Device ACKed — sending data packet 0...`);
      // SDS Data Packet: F0 7E ch 02 <packet_num> <120 bytes of data> <checksum> F7
      const packetData = Buffer.alloc(120, 0); // silence
      let checksum = 0;
      for (const b of packetData) checksum ^= b;
      checksum &= 0x7f;

      const dataPacket = Buffer.from([
        0xf0, 0x7e, 0x00, 0x02,  // SDS Data Packet
        0x00,                      // packet number 0
        ...packetData,
        checksum,
        0xf7,
      ]);
      console.log(`  Data packet: ${dataPacket.length} bytes`);
      const pktResp = await sendAndDrain(dataPacket);
      console.log(`  Response: ${pktResp.length}b${pktResp.length > 0 ? ' → ' + hex(pktResp) : ' (empty)'}`);
      if (pktResp.length > 0 && pktResp[1] === 0x7e) {
        const msgType = pktResp[3];
        const typeNames: Record<number, string> = { 0x7f: 'ACK', 0x7e: 'NAK', 0x7d: 'CANCEL', 0x7c: 'WAIT' };
        console.log(`  SDS response: ${typeNames[msgType] || `0x${msgType.toString(16)}`}`);
      }
    }
  }

  // ------------------------------------------------------------------
  // Test 4: Check sample list to see if SDS created anything
  // ------------------------------------------------------------------
  console.log('\n=== Test 4: Check sample list after SDS ===');
  await init(); await sleep(200);
  const slist = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x04, 0x48, 0xf7]));
  console.log(`  RSLIST: ${slist.length}b, samples: ${slist.length > 5 ? slist[5] : '?'}`);

  console.log('\nDone.');
}

main().catch(err => { console.error('Fatal:', err); process.exit(1); });
