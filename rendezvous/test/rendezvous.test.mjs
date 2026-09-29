// Rendezvous contract tests: simulates hosts and listeners against a deployed
// (or `wrangler dev`) Worker.
//
//   node test/rendezvous.test.mjs https://share.mdarif.online/aoi
//   node test/rendezvous.test.mjs http://127.0.0.1:8787/aoi
import { webcrypto as crypto } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const BASE = (process.argv[2] || 'https://share.mdarif.online/aoi').replace(/\/$/, '');
const EXE = process.argv[3] || fileURLToPath(new URL('../../dist/aoi.exe', import.meta.url));
let pass = 0, fail = 0;
const ok = (c, name, detail = '') => { (c ? pass++ : fail++); console.log(`  ${c ? 'PASS' : 'FAIL'}  ${name}${detail ? '  (' + detail + ')' : ''}`); };
const hex = n => [...crypto.getRandomValues(new Uint8Array(n))].map(b => b.toString(16).padStart(2, '0')).join('');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const api = (p, init) => fetch(BASE + p, { cache: 'no-store', ...init });
const post = (p, body) => api(p, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });

// --- the same blob format as the page and the host ---
const PREFIX = 'AOI1:', MAGIC = [0x41, 0x4f, 0x49, 0x31];
const b64u = b => Buffer.from(b).toString('base64url');
const unb64u = t => new Uint8Array(Buffer.from(t, 'base64url'));
const pipe = (t, b) => new Response(new Blob([b]).stream().pipeThrough(t)).arrayBuffer().then(a => new Uint8Array(a));
const concat = (...p) => { const o = new Uint8Array(p.reduce((a, x) => a + x.length, 0)); let k = 0; for (const x of p) { o.set(x, k); k += x.length; } return o; };
async function key(pass, salt) {
  const m = await crypto.subtle.importKey('raw', new TextEncoder().encode(pass), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey({ name: 'PBKDF2', salt, iterations: 200000, hash: 'SHA-256' }, m, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}
async function seal(text, code) {
  const body = await pipe(new CompressionStream('deflate-raw'), new TextEncoder().encode(text));
  const salt = crypto.getRandomValues(new Uint8Array(16)), iv = crypto.getRandomValues(new Uint8Array(12));
  const head = concat(new Uint8Array(MAGIC), new Uint8Array([1]), salt, iv);
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: head }, await key(code, salt), body));
  return PREFIX + b64u(concat(head, ct));
}
async function open(blob, code) {
  const raw = unb64u(blob.slice(PREFIX.length));
  const head = raw.subarray(0, 33);
  const plain = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: raw.subarray(21, 33), additionalData: head }, await key(code, raw.subarray(5, 21)), raw.subarray(33));
  return new TextDecoder().decode(await pipe(new DecompressionStream('deflate-raw'), new Uint8Array(plain)));
}
const roomId = async code => Buffer.from(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(code))).toString('hex');

console.log(`\nAOI rendezvous tests against ${BASE}\n`);

// ---------------------------------------------------------------------------
{
  const r = await api('/api/health');
  ok(r.status === 200 && (await r.json()).service === 'aoi', 'health endpoint');
  const t = await api('/api/turn');
  const tj = await t.json();
  ok(t.status === 200 && Array.isArray(tj.iceServers) && tj.iceServers.length > 0, 'ICE servers endpoint', tj.turn ? 'TURN configured' : 'STUN only');
  const page = await api('/K7Q-4MX');
  const html = await page.text();
  ok(page.status === 200 && html.includes('Listen to a PC'), 'deep link serves the listener page');
  const csp = page.headers.get('content-security-policy') || '';
  ok(csp.includes("frame-ancestors 'none'") && page.headers.get('x-content-type-options') === 'nosniff', 'security headers on the page');
  for (const p of ['/install.ps1', '/download/aoi.exe', '/download', '/aoi.exe']) {
    const r = await api(p);
    ok(r.status === 404, `installer is not published on the website: ${p} -> 404`);
  }
  const icon = await api('/icon.svg');
  ok(icon.status === 200, 'page assets still served');
}

// ---------------------------------------------------------------------------
const code = 'T' + hex(3).toUpperCase().replace(/[^23456789ABCDEFGHJKMNPQRSTUVWXYZ]/g, '7').slice(0, 5).padEnd(5, '7');
const id = await roomId(code);
const owner = hex(16), s1 = hex(8), s2 = hex(8);
const sdp = 'v=0\r\na=candidate:1 1 UDP 2122 10.1.2.3 50000 typ host\r\n' + 'a=x\r\n'.repeat(200);
const offer = await seal(sdp, code);
{
  let r = await post('/api/room', { id, owner, session: s1, offer, name: 'Test PC' });
  ok(r.status === 201, 'publish (HTTP) -> 201');
  r = await api(`/api/room/${id}`);
  const j = await r.json();
  ok(r.status === 200 && j.offer === offer && j.session === s1 && j.name === 'Test PC', 'fetch offer returns the same blob');
  ok(!j.offer.includes('10.1.2.3') && !j.offer.includes(code), 'stored blob contains neither IP nor code');
  ok((await open(j.offer, code)) === sdp, 'blob opens with the code');
  let wrong = false; try { await open(j.offer, 'WRONG1'); } catch { wrong = true; }
  ok(wrong, 'blob does not open with a wrong code');

  r = await post(`/api/room/${id}/answer`, { answer: await seal('answer', code), session: s1 });
  ok(r.status === 204, 'answer with current session -> 204');
  r = await api(`/api/room/${id}/answer?session=${s1}`);
  ok(r.status === 200 && (await open((await r.json()).answer, code)) === 'answer', 'host poll gets the answer immediately (consistent store)');
  r = await api(`/api/room/${id}/answer?session=${s1}`);
  ok(r.status === 204, 'answer is read-once');
  r = await post(`/api/room/${id}/answer`, { answer: await seal('second', code), session: s1 });
  ok(r.status === 409, 'second listener on a claimed offer -> 409 (takes the next one)');

  r = await post('/api/room', { id, owner: hex(16), session: s2, offer, name: 'Evil' });
  ok(r.status === 403, 'another owner cannot overwrite the room');
  r = await post('/api/room', { id, owner, session: s2, offer });
  ok(r.status === 201, 'owner republishes a new session');
  r = await post(`/api/room/${id}/answer`, { answer: await seal('late', code), session: s1 });
  ok(r.status === 409 && (await r.json()).session === s2, 'stale session -> 409 with the current session');
  r = await post('/api/room', { id, owner, full: true, name: 'Test PC' });
  r = await api(`/api/room/${id}`);
  ok((await r.json()).full === true, 'full host is reported as full');

  for (const [p, b, name] of [
    ['/api/room', { id: 'x', owner, session: s1, offer }, 'bad room id'],
    ['/api/room', { id, owner, session: 'zz', offer }, 'bad session'],
    ['/api/room', { id, owner, session: s1, offer: 'APP1:abc' }, 'foreign prefix'],
    ['/api/room', { id, owner, session: s1, offer: PREFIX + 'A'.repeat(20000) }, 'oversized blob'],
  ]) { r = await post(p, b); ok(r.status === 400, `rejects ${name}`); }

  r = await api(`/api/room/${id}?owner=${hex(16)}`, { method: 'DELETE' });
  ok(r.status === 403, 'delete needs the owner');
  r = await api(`/api/room/${id}?owner=${owner}`, { method: 'DELETE' });
  ok(r.status === 204, 'owner withdraws the code');
  r = await api(`/api/room/${id}`);
  ok(r.status === 404, 'withdrawn code -> 404');
}

// ---------------------------------------------------------------------------
// Host over a WebSocket: publish, answer pushed instantly, grace after close.
{
  const code2 = code.slice(0, 5) + '9', id2 = await roomId(code2), owner2 = hex(16), s = hex(8);
  const wsUrl = BASE.replace(/^http/, 'ws') + `/api/room/${id2}/host?owner=${owner2}`;
  const ws = new WebSocket(wsUrl);
  const inbox = [];
  let waiters = [];
  ws.onmessage = e => { inbox.push(e.data); waiters.forEach(w => w()); };
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  const next = pred => new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error('timeout')), 5000);
    const chk = () => { const i = inbox.findIndex(pred); if (i >= 0) { clearTimeout(t); waiters = waiters.filter(w => w !== chk); res(inbox.splice(i, 1)[0]); } };
    waiters.push(chk); chk();
  });
  ok(true, 'host WebSocket upgrade');
  ws.send('ping');
  ok((await next(m => m === 'pong')) === 'pong', 'ping answered by the runtime (hibernation-safe)');
  ws.send(JSON.stringify({ t: 'pub', session: s, offer: await seal('ws offer', code2), name: 'WS PC', rid: 7 }));
  const okMsg = JSON.parse(await next(m => m.includes('"rid":7')));
  ok(okMsg.t === 'ok', 'publish over the socket');
  const room = await (await api(`/api/room/${id2}`)).json();
  ok(room.session === s && (await open(room.offer, code2)) === 'ws offer', 'listener sees the socket-published offer');
  const t0 = Date.now();
  await post(`/api/room/${id2}/answer`, { answer: await seal('pushed', code2), session: s });
  const pushed = JSON.parse(await next(m => m.includes('"t":"ans"')));
  ok(pushed.session === s && (await open(pushed.answer, code2)) === 'pushed', 'answer pushed to the host', `${Date.now() - t0} ms`);
  ws.send(JSON.stringify({ t: 'ack', session: s }));
  ws.close();
  await sleep(1500);
  let r = await api(`/api/room/${id2}`);
  ok(r.status === 200, 'offer survives a brief host disconnect (grace)');
  // clean up
  const ws2 = new WebSocket(wsUrl);
  await new Promise(res => { ws2.onopen = res; });
  ws2.send(JSON.stringify({ t: 'del', rid: 1 }));
  await sleep(500);
  ws2.close();
  r = await api(`/api/room/${id2}`);
  ok(r.status === 404, 'host deletes the room over the socket');
}

// ---------------------------------------------------------------------------
// Cross implementation: C++ host <-> JS page blob format.
if (existsSync(EXE)) {
  const c = 'K7Q4MX';
  const fromHost = execFileSync(EXE, ['selftest', 'seal', c, 'hello from C++ ♪']).toString().trim();
  ok((await open(fromHost, c)) === 'hello from C++ ♪', 'blob sealed by the host opens in JS');
  const fromJs = await seal('hello from JS', c);
  const opened = execFileSync(EXE, ['selftest', 'open', c, fromJs]).toString();
  ok(opened === 'hello from JS', 'blob sealed in JS opens in the host');
} else {
  console.log(`  skip  cross-implementation (no ${EXE})`);
}

console.log(`\n  ${pass} passed, ${fail} failed\n`);
process.exit(fail ? 1 : 0);
