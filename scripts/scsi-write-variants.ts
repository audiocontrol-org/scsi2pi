#!/usr/bin/env tsx
/**
 * Try different write formats to find what the S3000XL accepts via SCSI.
 *
 * Usage: tsx scripts/scsi-write-variants.ts [host] [target-id]
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
    const sock = new net.Socket(); sock.setTimeout(10_000);
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
  await sleep(100);
  const p = await poll();
  if (p <= 0) return Buffer.alloc(0);
  return read(p);
}
function sleep(ms: number): Promise<void> { return new Promise(r => setTimeout(r, ms)); }
function hex(b: Buffer, n = 30): string {
  const h = Array.from(b.subarray(0, n)).map(x => x.toString(16).padStart(2, '0')).join(' ');
  return b.length > n ? h + ` ... (${b.length}b)` : h;
}

// --- Tests ---

async function main(): Promise<void> {
  console.log(`Write Variants — ${S2P_HOST}:${S2P_PORT}, target ${TARGET_ID}\n`);

  // Init and read baseline
  console.log('Init:', await init() ? 'OK' : 'FAILED');
  const baseline = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
  console.log(`Baseline RPDATA: ${baseline.length} bytes`);
  console.log(`  ${hex(baseline)}\n`);

  if (baseline.length < 20) {
    console.error('Cannot read program header — aborting');
    process.exit(1);
  }

  // Save the data portion (everything between SysEx header and footer)
  // Full SysEx: F0 47 00 07 48 [data...] F7
  // Data starts at index 5, ends at length-1
  const dataStart = 5;
  const dataEnd = baseline.length - 1; // exclude F7
  const origData = Buffer.from(baseline.subarray(dataStart, dataEnd));
  console.log(`Program data: ${origData.length} bytes (offsets ${dataStart}-${dataEnd})\n`);

  // Pick a test byte
  const testIdx = 20;
  const origByte = origData[testIdx];
  const testByte = origByte === 0 ? 1 : 0;

  // -----------------------------------------------------------------------
  // Variant A: Full SysEx (F0 47 00 07 48 ... F7) — what we've been doing
  // -----------------------------------------------------------------------
  console.log('=== Variant A: Full SysEx with PDATA opcode 0x07 ===');
  {
    const mod = Buffer.from(baseline);
    mod[dataStart + testIdx] = testByte;
    console.log(`  Sending ${mod.length} bytes: ${hex(mod, 10)}`);
    console.log(`  Send: ${await send(mod) ? 'OK' : 'FAILED'}`);
    await sleep(200);
    // Drain any response
    const p = await poll(); if (p > 0) { const d = await read(p); console.log(`  Post-write poll: ${p}b → ${hex(d, 10)}`); }
    // Re-read
    await init(); await sleep(200);
    const re = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
    console.log(`  Re-read: ${re.length}b, byte[${dataStart + testIdx}] = ${re.length > dataStart + testIdx ? re[dataStart + testIdx] : '?'} (want ${testByte})`);
    console.log(`  Result: ${re.length > dataStart + testIdx && re[dataStart + testIdx] === testByte ? '✓ PERSISTED' : '✗ unchanged'}\n`);
  }

  // Re-init for next test
  await init(); await sleep(200);

  // -----------------------------------------------------------------------
  // Variant B: Just the data (no SysEx framing, no opcode)
  // -----------------------------------------------------------------------
  console.log('=== Variant B: Raw data only (no F0/F7/opcode) ===');
  {
    const mod = Buffer.from(origData);
    mod[testIdx] = testByte;
    console.log(`  Sending ${mod.length} bytes: ${hex(mod, 10)}`);
    console.log(`  Send: ${await send(mod) ? 'OK' : 'FAILED'}`);
    await sleep(200);
    const p = await poll(); if (p > 0) { const d = await read(p); console.log(`  Post-write poll: ${p}b → ${hex(d, 10)}`); }
    await init(); await sleep(200);
    const re = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
    console.log(`  Re-read: ${re.length}b, byte[${dataStart + testIdx}] = ${re.length > dataStart + testIdx ? re[dataStart + testIdx] : '?'} (want ${testByte})`);
    console.log(`  Result: ${re.length > dataStart + testIdx && re[dataStart + testIdx] === testByte ? '✓ PERSISTED' : '✗ unchanged'}\n`);
  }

  await init(); await sleep(200);

  // -----------------------------------------------------------------------
  // Variant C: SysEx with APDATA opcode (0x4A = partial write)
  // APDATA format: F0 47 CH 4A 48 <prog_lo> <prog_hi> <off_lo> <off_hi> <count_lo> <count_hi> <data...> F7
  // -----------------------------------------------------------------------
  console.log('=== Variant C: APDATA (0x4A) partial write ===');
  {
    // Write 1 byte at offset testIdx in program 0
    const offLo = testIdx & 0x7f;
    const offHi = (testIdx >> 7) & 0x7f;
    const sysex = Buffer.from([
      0xf0, 0x47, 0x00, 0x4a, 0x48,  // header
      0x00, 0x00,                      // program 0
      offLo, offHi,                    // offset
      0x01, 0x00,                      // count = 1
      testByte,                        // data
      0xf7,
    ]);
    console.log(`  Sending APDATA: ${hex(sysex)}`);
    console.log(`  Send: ${await send(sysex) ? 'OK' : 'FAILED'}`);
    await sleep(200);
    const p = await poll(); if (p > 0) { const d = await read(p); console.log(`  Post-write poll: ${p}b → ${hex(d, 10)}`); }
    await init(); await sleep(200);
    const re = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
    console.log(`  Re-read: ${re.length}b, byte[${dataStart + testIdx}] = ${re.length > dataStart + testIdx ? re[dataStart + testIdx] : '?'} (want ${testByte})`);
    console.log(`  Result: ${re.length > dataStart + testIdx && re[dataStart + testIdx] === testByte ? '✓ PERSISTED' : '✗ unchanged'}\n`);
  }

  await init(); await sleep(200);

  // -----------------------------------------------------------------------
  // Variant D: ASPDATA opcode (0x48 — write sample data by name)
  // Actually let's just try RSTAT after each variant to check buffer health
  // -----------------------------------------------------------------------
  console.log('=== Final: Buffer health check ===');
  const healthCheck = await sendAndRead(Buffer.from([0xf0, 0x47, 0x00, 0x00, 0x48, 0xf7]));
  console.log(`  RSTAT: ${healthCheck.length} bytes, ${hex(healthCheck)}`);
  console.log(`  Healthy: ${healthCheck.length === 21 && healthCheck[0] === 0xf0 ? 'YES' : 'NO'}\n`);

  console.log('Done.');
}

main().catch(err => { console.error('Fatal:', err); process.exit(1); });
