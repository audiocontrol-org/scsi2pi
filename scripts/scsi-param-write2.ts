#!/usr/bin/env tsx
/**
 * Clean parameter write test: read → modify → write → re-init → re-read → compare.
 *
 * Key findings so far:
 * - Writes get no response (poll returns 0) — S3000XL accepts silently
 * - Must re-init between write and read for clean buffer state
 * - Previous "corruption" was stale queued data, not write failure
 *
 * Usage: tsx scripts/scsi-param-write2.ts [host] [target-id]
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
/** Send, wait, read first SysEx response */
async function sendAndRead(sysex: Buffer): Promise<Buffer> {
  if (!await send(sysex)) throw new Error('SEND failed');
  await sleep(200);
  const p = await poll();
  if (p <= 0) return Buffer.alloc(0);
  const raw = await read(p);
  // Extract first SysEx message (up to first F7)
  const endIdx = raw.indexOf(0xf7);
  return endIdx >= 0 ? raw.subarray(0, endIdx + 1) : raw;
}
function sleep(ms: number): Promise<void> { return new Promise(r => setTimeout(r, ms)); }

const RPDATA = Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]);

async function main(): Promise<void> {
  console.log(`Clean Parameter Write Test — ${S2P_HOST}:${S2P_PORT}, target ${TARGET_ID}\n`);

  // Step 1: Fresh init
  console.log('Step 1: Init');
  console.log(`  ${await init() ? 'OK' : 'FAILED'}\n`);

  // Step 2: Read program 0
  console.log('Step 2: Read program 0');
  const original = await sendAndRead(RPDATA);
  console.log(`  Got ${original.length} bytes`);
  if (original.length < 20 || original[0] !== 0xf0) {
    console.error('  Invalid response'); process.exit(1);
  }

  // Pick a byte to modify — use a few different offsets
  // Data starts at byte 5 (after F0 47 00 07 48)
  // The Akai data is nibblized: each parameter byte is stored as 2 nibbles
  // Let's modify something safe — Soft Pedal Loudness is near the end
  const testOffsets = [131, 133, 135]; // offset into raw SysEx buffer
  console.log(`  Testing offsets: ${testOffsets.join(', ')}`);
  for (const off of testOffsets) {
    if (off < original.length) {
      console.log(`    byte[${off}] = 0x${original[off].toString(16).padStart(2, '0')} (${original[off]})`);
    }
  }

  // Step 3: Modify and write
  const testOff = testOffsets[0];
  const origByte = original[testOff];
  const testByte = origByte === 0x0a ? 0x05 : 0x0a; // toggle between values

  console.log(`\nStep 3: Modify byte[${testOff}]: 0x${origByte.toString(16)} → 0x${testByte.toString(16)}`);
  const modified = Buffer.from(original);
  modified[testOff] = testByte;

  console.log(`  Sending ${modified.length} bytes...`);
  const sendOk = await send(modified);
  console.log(`  MIDI_SEND: ${sendOk ? 'OK' : 'FAILED'}`);

  // Check poll — should be 0 (no response for writes)
  await sleep(300);
  const postWritePoll = await poll();
  console.log(`  Post-write poll: ${postWritePoll} bytes`);
  if (postWritePoll > 0) {
    const resp = await read(postWritePoll);
    console.log(`  Unexpected response: ${Array.from(resp.subarray(0, 20)).map(b => b.toString(16).padStart(2, '0')).join(' ')}`);
  }

  // Step 4: Re-init (clean buffer state)
  console.log(`\nStep 4: Re-init`);
  console.log(`  ${await init() ? 'OK' : 'FAILED'}`);
  await sleep(300);

  // Step 5: Re-read
  console.log(`\nStep 5: Re-read program 0`);
  const reread = await sendAndRead(RPDATA);
  console.log(`  Got ${reread.length} bytes`);

  if (reread.length > testOff) {
    const newByte = reread[testOff];
    console.log(`  byte[${testOff}] = 0x${newByte.toString(16).padStart(2, '0')} (${newByte})`);

    if (newByte === testByte) {
      console.log(`  ✓ WRITE PERSISTED!`);

      // Restore original
      console.log(`\nStep 6: Restore original`);
      await send(original);
      await sleep(300);
      await init();
      await sleep(300);
      const restored = await sendAndRead(RPDATA);
      console.log(`  Restored byte[${testOff}] = 0x${restored.length > testOff ? restored[testOff].toString(16) : '?'}`);
    } else if (newByte === origByte) {
      console.log(`  ✗ Write did NOT persist — still original value`);

      // Detailed comparison
      let diffs = 0;
      for (let i = 0; i < Math.min(original.length, reread.length); i++) {
        if (original[i] !== reread[i]) {
          if (diffs < 5) console.log(`    diff byte[${i}]: 0x${original[i].toString(16)} → 0x${reread[i].toString(16)}`);
          diffs++;
        }
      }
      if (diffs === 0) console.log(`  Re-read is identical to original (${reread.length} bytes)`);
      else console.log(`  ${diffs} total differences`);
    } else {
      console.log(`  ? Unexpected value: 0x${newByte.toString(16)}`);
    }
  }

  console.log('\nDone.');
}

main().catch(err => { console.error('Fatal:', err); process.exit(1); });
