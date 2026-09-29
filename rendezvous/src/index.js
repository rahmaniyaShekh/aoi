// AOI rendezvous: a sealed-mailbox signalling server for Audio Over Internet.
//
// It swaps ~1 KB of *encrypted* SDP between a host (the Windows app) and a
// joiner (the web page) and then leaves the path. Media flows peer to peer
// over WebRTC. The server never sees the join code, the SDP, or either peer's
// addresses in plaintext -- only SHA-256(code) as an address and AES-GCM
// ciphertext sealed with a key derived from the code.
//
// Differences from the reference pattern (P2P_SHORT_CODE_CONNECT.md), and why:
//
// * Hosts hold a hibernating WebSocket instead of polling every 2 s. Answers
//   are pushed the moment a joiner posts one (joins are faster), and an idle
//   host costs nothing: a hibernated socket bills no duration, and the
//   ping/pong keepalive is answered by the runtime without waking the object.
//   Polling at 2 s would burn ~43k of the free plan's 100k requests a day for
//   every host left running. HTTP polling is still supported as a fallback for
//   networks that break WebSockets.
// * A room remembers SHA-256 of an owner token on first publish, so nobody who
//   learns a room id can overwrite the host's offer with their own.
// * One host serves several listeners. After each answer the host publishes a
//   fresh session, and an answer claims its session so two joiners racing for
//   the same offer cannot both take it -- the loser gets 409 and simply picks
//   up the next offer.
// * A WebSocket host's offer is only served while the host is connected (plus
//   a short grace), so joiners never burn a 25 s timeout on a dead offer.

const TTL_SECONDS = 600;               // idle rooms evaporate after 10 min
const OWNER_TTL_SECONDS = 30 * 86400;  // an owner claim outlives its room by 30 days
const HOST_GRACE_MS = 20000;           // offer stays valid this long after the host socket drops
const MAX_BODY = 16 * 1024;            // a sealed SDP is ~1-2 KB
const PREFIX = 'AOI1:';
const BASE = '/aoi';

const CORS = {
  'access-control-allow-origin': '*',
  'access-control-allow-methods': 'GET, POST, DELETE, OPTIONS',
  'access-control-allow-headers': 'content-type',
  'access-control-max-age': '86400',
};
const json = (o, status = 200) => new Response(JSON.stringify(o), {
  status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store', ...CORS },
});
const empty = status => new Response(null, { status, headers: { ...CORS, 'cache-control': 'no-store' } });

const isRoomId = s => typeof s === 'string' && /^[0-9a-f]{64}$/.test(s);
const isSession = s => typeof s === 'string' && /^[0-9a-f]{16}$/.test(s);
const isOwner = s => typeof s === 'string' && /^[0-9a-f]{32,64}$/.test(s);
const isBlob = s => typeof s === 'string' && s.length > 16 && s.length < MAX_BODY &&
  s.startsWith(PREFIX) && /^[A-Za-z0-9_\-:]+$/.test(s);
const isName = s => typeof s === 'string' && s.length <= 64;

async function readJson(req) {
  const raw = await req.text();
  if (raw.length > MAX_BODY) return null;
  try { return JSON.parse(raw); } catch { return null; }
}

async function sha256hex(s) {
  const h = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(h)].map(b => b.toString(16).padStart(2, '0')).join('');
}

// ---------------------------------------------------------------------------
export class AoiRoom {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    // Answered by the runtime itself: keeps a host socket alive through
    // proxies and NATs without waking (or billing) the object.
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  get s() { return this.ctx.storage; }
  hosts() { return this.ctx.getWebSockets('host'); }

  // Any activity extends the room's life; only rewrite the alarm past half-life
  // so a busy room is not one storage write per request.
  async touch() {
    const now = Date.now();
    const cur = await this.s.getAlarm();
    if (cur && cur - now > (TTL_SECONDS * 1000) / 2) return;
    await this.s.setAlarm(now + TTL_SECONDS * 1000);
  }

  async alarm() {
    if (this.hosts().length > 0) {            // a connected host keeps its room
      await this.s.setAlarm(Date.now() + TTL_SECONDS * 1000);
      return;
    }
    const room = await this.s.get('room');
    if (room) {
      // Room expired: drop the offer and answers, keep the owner claim so the
      // same host can come back and nobody can squat its code meanwhile.
      const keys = [...(await this.s.list({ prefix: 'ans:' })).keys(), 'room'];
      await this.s.delete(keys);
      await this.s.setAlarm(Date.now() + OWNER_TTL_SECONDS * 1000);
    } else {
      await this.s.deleteAll();
    }
  }

  // Trust on first use: the first publisher of a room owns it.
  async checkOwner(owner) {
    if (!isOwner(owner)) return false;
    const hash = await sha256hex(owner);
    const cur = await this.s.get('owner');
    if (!cur) { await this.s.put('owner', hash); return true; }
    return cur === hash;
  }

  async publish(b, mode) {
    if (!(await this.checkOwner(b.owner))) return { status: 403, body: { error: 'room owned by another host' } };
    const full = b.full === true;
    if (!full && (!isBlob(b.offer) || !isSession(b.session))) return { status: 400, body: { error: 'bad request' } };
    const room = {
      offer: full ? null : b.offer,
      session: full ? null : b.session,
      full,
      mode,
      claimed: false,
      name: isName(b.name) ? b.name : '',
      at: Date.now(),
    };
    await this.s.put('room', room);
    const old = await this.s.list({ prefix: 'ans:' });
    if (old.size) await this.s.delete([...old.keys()]);
    await this.touch();
    return { status: 201, body: { ok: true, expiresIn: TTL_SECONDS } };
  }

  hostAlive(room) {
    if (room.mode !== 'ws') return true;
    if (this.hosts().length > 0) return true;
    return Date.now() - (room.gone || 0) < HOST_GRACE_MS;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const op = url.searchParams.get('op');
    const s = this.s;

    if (op === 'host-ws') {
      if (request.headers.get('upgrade') !== 'websocket') return json({ error: 'expected websocket' }, 426);
      const owner = url.searchParams.get('owner');
      if (!(await this.checkOwner(owner))) return json({ error: 'room owned by another host' }, 403);
      // One host per room: a restarted host replaces its own stale socket.
      for (const ws of this.hosts()) { try { ws.close(4000, 'replaced'); } catch { } }
      const pair = new WebSocketPair();
      this.ctx.acceptWebSocket(pair[1], ['host']);
      pair[1].serializeAttachment({ owner: await sha256hex(owner) });
      await this.touch();
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    if (op === 'publish') {
      const r = await this.publish(await request.json(), 'poll');
      return json(r.body, r.status);
    }

    if (op === 'offer') {
      const room = await s.get('room');
      if (!room || !this.hostAlive(room)) return json({ error: 'unknown or expired code' }, 404);
      if (room.full) return json({ full: true, name: room.name });
      return json({ offer: room.offer, session: room.session, name: room.name });
    }

    if (op === 'answer-post') {
      const { answer, session } = await request.json();
      const room = await s.get('room');
      if (!room || !this.hostAlive(room)) return json({ error: 'unknown or expired code' }, 404);
      // Replaced, or already taken by another joiner: go back for the next offer.
      if (room.full || room.session !== session || room.claimed)
        return json({ error: 'stale', session: room.session }, 409);
      room.claimed = true;
      await s.put('room', room);
      await s.put(`ans:${session}`, answer);
      for (const ws of this.hosts()) {
        try { ws.send(JSON.stringify({ t: 'ans', session, answer })); } catch { }
      }
      await this.touch();
      return empty(204);
    }

    if (op === 'answer-get') {
      await this.touch();   // a polling host is alive: never expire under it
      const key = `ans:${url.searchParams.get('session')}`;
      const answer = await s.get(key);
      if (!answer) return empty(204);
      await s.delete(key);
      return json({ answer });
    }

    if (op === 'delete') {
      const owner = url.searchParams.get('owner');
      if (!(await this.checkOwner(owner))) return json({ error: 'forbidden' }, 403);
      const keys = [...(await s.list({ prefix: 'ans:' })).keys(), 'room'];
      await s.delete(keys);
      return empty(204);
    }
    return json({ error: 'not found' }, 404);
  }

  // --- hibernatable host socket ---------------------------------------------
  async webSocketMessage(ws, msg) {
    if (typeof msg !== 'string' || msg.length > MAX_BODY) return;
    let m; try { m = JSON.parse(msg); } catch { return; }
    const { owner } = ws.deserializeAttachment() || {};
    const reply = o => { try { ws.send(JSON.stringify(o)); } catch { } };

    if (m.t === 'pub') {
      // The socket was authenticated at upgrade; reuse that proof.
      const room = {
        offer: m.full ? null : m.offer, session: m.full ? null : m.session,
        full: m.full === true, mode: 'ws', claimed: false,
        name: isName(m.name) ? m.name : '', at: Date.now(),
      };
      if (!room.full && (!isBlob(room.offer) || !isSession(room.session)))
        return reply({ t: 'err', error: 'bad offer', rid: m.rid });
      if ((await this.s.get('owner')) !== owner) return reply({ t: 'err', error: 'forbidden', rid: m.rid });
      await this.s.put('room', room);
      const old = await this.s.list({ prefix: 'ans:' });
      if (old.size) await this.s.delete([...old.keys()]);
      await this.touch();
      return reply({ t: 'ok', rid: m.rid, expiresIn: TTL_SECONDS });
    }
    if (m.t === 'ack' && isSession(m.session)) {      // host consumed the pushed answer
      await this.s.delete(`ans:${m.session}`);
      return;
    }
    if (m.t === 'get' && isSession(m.session)) {      // host lost a push: fetch it
      const answer = await this.s.get(`ans:${m.session}`);
      if (answer) reply({ t: 'ans', session: m.session, answer });
      return;
    }
    if (m.t === 'del') {
      const keys = [...(await this.s.list({ prefix: 'ans:' })).keys(), 'room'];
      await this.s.delete(keys);
      return reply({ t: 'ok', rid: m.rid });
    }
  }

  async webSocketClose(ws) {
    // Start the grace clock; joiners stop being handed this offer shortly.
    if (this.hosts().filter(x => x !== ws).length === 0) {
      const room = await this.s.get('room');
      if (room && room.mode === 'ws') { room.gone = Date.now(); await this.s.put('room', room); }
    }
  }
  async webSocketError(ws) { await this.webSocketClose(ws); }
}

// ---------------------------------------------------------------------------
const room = (env, id, op, init = {}) =>
  env.ROOMS.get(env.ROOMS.idFromName(id)).fetch(`https://room/?op=${op}${init.qs || ''}`, init.req);

// TURN credentials, minted from a Cloudflare Realtime TURN key when one is
// configured (wrangler secret put TURN_KEY_ID / TURN_KEY_TOKEN). Cached per
// isolate for an hour of a 24 h credential, so a busy page is one API call.
let turnCache = { at: 0, servers: null };
const STUN = [{ urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.l.google.com:19302'] }];

async function iceServers(env) {
  if (!env.TURN_KEY_ID || !env.TURN_KEY_TOKEN) return { iceServers: STUN, turn: false };
  if (turnCache.servers && Date.now() - turnCache.at < 3600e3) return { iceServers: turnCache.servers, turn: true };
  try {
    const r = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${env.TURN_KEY_ID}/credentials/generate-ice-servers`, {
      method: 'POST',
      headers: { authorization: `Bearer ${env.TURN_KEY_TOKEN}`, 'content-type': 'application/json' },
      body: JSON.stringify({ ttl: 86400 }),
    });
    if (!r.ok) throw new Error(`turn ${r.status}`);
    const j = await r.json();
    const servers = (j.iceServers || []).map(s => ({
      ...s,
      // Port 53 is blocked by most browsers and pointless for us.
      urls: [].concat(s.urls).filter(u => !/:53(\?|$)/.test(u)),
    })).filter(s => s.urls.length);
    turnCache = { at: Date.now(), servers };
    return { iceServers: servers, turn: true };
  } catch {
    return { iceServers: STUN, turn: false };
  }
}

function secure(resp, html) {
  const out = new Response(resp.body, resp);
  if (html) {
    out.headers.set('content-security-policy',
      "default-src 'none'; connect-src 'self'; media-src 'self' blob:; img-src 'self' data:; " +
      "style-src 'unsafe-inline'; script-src 'unsafe-inline'; manifest-src 'self'; " +
      "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    out.headers.set('permissions-policy', 'microphone=(self), screen-wake-lock=(self), autoplay=(self)');
    out.headers.set('cache-control', 'no-cache');
  }
  out.headers.set('referrer-policy', 'no-referrer');
  out.headers.set('x-content-type-options', 'nosniff');
  return out;
}

async function limited(env, request, key) {
  if (!env.LOOKUP_LIMIT) return false;
  const ip = request.headers.get('cf-connecting-ip') || 'anon';
  const { success } = await env.LOOKUP_LIMIT.limit({ key: `${key}:${ip}` });
  return !success;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    let path = url.pathname;
    if (request.method === 'OPTIONS') return empty(204);
    if (path !== BASE && !path.startsWith(BASE + '/')) return json({ error: 'not found' }, 404);
    path = path.slice(BASE.length) || '/';

    // ---- API ---------------------------------------------------------------
    if (path.startsWith('/api/')) {
      if (path === '/api/health') return json({ ok: true, service: 'aoi', version: 1 });

      if (path === '/api/turn' && request.method === 'GET') {
        if (await limited(env, request, 'turn')) return json({ error: 'slow down' }, 429);
        return json(await iceServers(env));
      }

      if (path === '/api/room' && request.method === 'POST') {
        const b = await readJson(request);
        if (!b || !isRoomId(b.id) || !isOwner(b.owner)) return json({ error: 'bad request' }, 400);
        if (!b.full && (!isBlob(b.offer) || !isSession(b.session))) return json({ error: 'bad request' }, 400);
        return room(env, b.id, 'publish', { req: { method: 'POST', body: JSON.stringify(b) } });
      }

      const m = path.match(/^\/api\/room\/([0-9a-f]{64})(\/answer|\/host)?$/);
      if (m) {
        const [, id, sub] = m;
        if (!sub && request.method === 'GET') {
          if (await limited(env, request, 'room')) return json({ error: 'slow down' }, 429);
          return room(env, id, 'offer');
        }
        if (!sub && request.method === 'DELETE') {
          const owner = url.searchParams.get('owner');
          if (!isOwner(owner)) return json({ error: 'bad request' }, 400);
          return room(env, id, 'delete', { qs: `&owner=${owner}` });
        }
        if (sub === '/host' && request.method === 'GET') {
          const owner = url.searchParams.get('owner');
          if (!isOwner(owner)) return json({ error: 'bad request' }, 400);
          // Hand the upgrade request itself to the object.
          return env.ROOMS.get(env.ROOMS.idFromName(id))
            .fetch(new Request(`https://room/?op=host-ws&owner=${owner}`, request));
        }
        if (sub === '/answer' && request.method === 'POST') {
          const b = await readJson(request);
          if (!b || !isBlob(b.answer) || !isSession(b.session)) return json({ error: 'bad request' }, 400);
          return room(env, id, 'answer-post',
            { req: { method: 'POST', body: JSON.stringify({ answer: b.answer, session: b.session }) } });
        }
        if (sub === '/answer' && request.method === 'GET') {
          const session = url.searchParams.get('session');
          if (!isSession(session)) return json({ error: 'bad request' }, 400);
          return room(env, id, 'answer-get', { qs: `&session=${session}` });
        }
      }
      return json({ error: 'not found' }, 404);
    }

    // ---- static ------------------------------------------------------------
    if (/\.(svg|png|webmanifest|ico|txt)$/.test(path)) {
      return secure(await env.ASSETS.fetch(request), false);
    }
    // The installer is not published here (it is a private GitHub release):
    // any other file-like path is simply not found.
    if (/\.[A-Za-z0-9]+$/.test(path) || path.startsWith('/download')) return json({ error: 'not found' }, 404);
    // Everything else is the listener page: /aoi, /aoi/, /aoi/K7Q-4MX deep links.
    const page = await env.ASSETS.fetch(new Request(new URL(`${BASE}/`, url), request));
    return secure(page, true);
  },
};
