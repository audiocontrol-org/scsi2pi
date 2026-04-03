#!/usr/bin/env tsx
/**
 * Focused parameter write test over SCSI.
 *
 * Try writing a single parameter with various approaches.
 * Log everything to understand what the S3000XL does with writes.
 *
 * Usage: tsx scripts/scsi-param-write.ts [host] [target-id]
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
async function sendAndReadAll(sysex: Buffer, waitMs = 200): Promise<Buffer[]> {
  if (!await send(sysex)) throw new Error('SEND failed');
  await sleep(waitMs);
  const responses: Buffer[] = [];
  let pending = await poll();
  while (pending > 0) {
    const data = await read(pending);
    if (data.length === 0) break;
    let start = 0;
    for (let i = 0; i < data.length; i++) {
      if (data[i] === 0xf7) { responses.push(Buffer.from(data.subarray(start, i + 1))); start = i + 1; }
    }
    if (start < data.length) responses.push(Buffer.from(data.subarray(start)));
    await sleep(50);
    pending = await poll();
  }
  return responses;
}
function sleep(ms: number): Promise<void> { return new Promise(r => setTimeout(r, ms)); }
function hex(b: Buffer, n = 50): string {
  const h = Array.from(b.subarray(0, n)).map(x => x.toString(16).padStart(2, '0')).join(' ');
  return b.length > n ? h + ` ... (${b.length}b)` : h;
}

async function main(): Promise<void> {
  console.log(`Parameter Write Debug — ${S2P_HOST}:${S2P_PORT}, target ${TARGET_ID}\n`);

  console.log(`Init: ${await init() ? 'OK' : 'FAILED'}\n`);

  // Read current program header
  console.log('=== Read program 0 ===');
  const readResps = await sendAndReadAll(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
  for (const r of readResps) console.log(`  ${hex(r)}`);
  const prog = readResps[0];
  if (!prog || prog.length < 20) { console.error('Failed to read'); process.exit(1); }

  // Print key bytes for reference
  console.log(`\n  Full dump (${prog.length} bytes):`);
  for (let i = 0; i < prog.length; i += 16) {
    const slice = Array.from(prog.subarray(i, Math.min(i + 16, prog.length)));
    console.log(`    ${i.toString().padStart(3)}: ${slice.map(b => b.toString(16).padStart(2, '0')).join(' ')}`);
  }

  // Now write it back EXACTLY as-is (no modification)
  // If the re-read matches, the write path at least doesn't corrupt things
  console.log('\n=== Write exact same program data back ===');
  console.log(`  Sending ${prog.length} bytes (PDATA opcode 0x07)`);
  const writeOk = await send(prog);
  console.log(`  MIDI_SEND: ${writeOk ? 'OK' : 'FAILED'}`);

  // Wait and poll to see what the S3000XL says
  await sleep(300);
  console.log('  Polling for response...');
  const p1 = await poll();
  console.log(`  Pending: ${p1} bytes`);
  if (p1 > 0) {
    const resp = await read(p1);
    console.log(`  Response: ${hex(resp)}`);
    // Check if it's valid SysEx
    if (resp[0] === 0xf0) {
      console.log(`  Valid SysEx! Opcode: 0x${resp[3]?.toString(16).padStart(2, '0')}`);
    } else {
      console.log(`  NOT valid SysEx (doesn't start with F0)`);
      console.log(`  This is raw buffer data — the write was not processed as MIDI`);
    }
  } else {
    console.log(`  No response (device accepted silently or ignored)`);
  }

  // Re-init and re-read
  console.log('\n=== Re-init + re-read ===');
  console.log(`  Re-init: ${await init() ? 'OK' : 'FAILED'}`);
  await sleep(200);
  const rereadResps = await sendAndReadAll(Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]));
  const reread = rereadResps[0];
  if (reread) {
    console.log(`  Re-read: ${reread.length} bytes`);
    const match = reread.length === prog.length && Buffer.compare(reread, prog) === 0;
    console.log(`  Matches original: ${match}`);
    if (!match) {
      const diffs: string[] = [];
      for (let i = 0; i < Math.min(prog.length, reread.length); i++) {
        if (prog[i] !== reread[i]) diffs.push(`  byte[${i}]: ${prog[i].toString(16)} → ${reread[i].toString(16)}`);
      }
      if (prog.length !== reread.length) diffs.push(`  length: ${prog.length} → ${reread.length}`);
      console.log(`  Differences (${diffs.length}):`);
      diffs.slice(0, 10).forEach(d => console.log(`  ${d}`));
    }
  } else {
    console.log('  Re-read returned empty');
  }

  // Now try: what if we DON'T poll after the write and DON'T re-init?
  // Just send write then immediately send read
  console.log('\n=== Write then immediate read (no poll, no re-init) ===');
  console.log(`  Sending write...`);
  await send(prog);
  await sleep(100);
  // Don't poll. Just send RPDATA immediately.
  console.log(`  Sending RPDATA immediately...`);
  const immediateResps = await sendAndReadAll(
    Buffer.from([0xf0, 0x47, 0x00, 0x06, 0x48, 0x00, 0x00, 0xf7]),
    500  // longer wait for S3000XL to process
  );
  for (const r of immediateResps) {
    console.log(`  Response: ${r.length}b, ${r[0] === 0xf0 ? `opcode 0x${r[3]?.toString(16)}` : 'not SysEx'}`);
  }

  console.log('\nDone.');
}

main().catch(err => { console.error('Fatal:', err); process.exit(1); });
