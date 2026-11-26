/*
 * Minimal WAAPI client over WAMP (WebSocket), for the tools in this folder. Node 22 or newer
 * (built-in WebSocket). No dependencies.
 *
 *   const { connect } = require('./waapi_client.js');
 *   const w = await connect();                       // ws://127.0.0.1:8080/waapi, or WAAPI_URL
 *   const info = await w.call('ak.wwise.core.getProjectInfo');
 *   const objs = await w.call('ak.wwise.core.object.get', { from: { path: [...] } }, { return: ['path'] });
 *   w.close();
 *
 * WAMP messages: CALL = [48, id, options, procedure, args, kwargs]; RESULT = [50, id, details, args, kwargs];
 * ERROR = [8, 48, id, details, errorUri, args, kwargs]. WAAPI "options" (such as "return") go in the CALL options.
 */

function connect(url = process.env.WAAPI_URL || 'ws://127.0.0.1:8080/waapi') {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    const pending = new Map();
    let nextId = 10;
    let welcomed = false;

    const client = {
      call(procedure, kwargs = {}, options = {}) {
        return new Promise((res, rej) => {
          const id = nextId++;
          pending.set(id, { res, rej, procedure });
          ws.send(JSON.stringify([48, id, options, procedure, [], kwargs]));
        });
      },
      close() { ws.close(); },
    };

    ws.onmessage = (msg) => {
      const m = JSON.parse(String(msg.data));
      if (m[0] === 2) { welcomed = true; resolve(client); return; }
      const id = m[0] === 8 ? m[2] : m[1];
      const p = pending.get(id);
      if (!p) { return; }
      pending.delete(id);
      if (m[0] === 50) {
        p.res(m[4] ?? {});
      } else if (m[0] === 8) {
        const detail = m[6] && m[6].message ? m[6].message : JSON.stringify(m[6] ?? m[5] ?? {});
        p.rej(new Error(`${p.procedure}: ${m[4]}: ${detail}`));
      }
    };
    ws.onopen = () => ws.send(JSON.stringify([1, 'realm1', { roles: { caller: {} } }]));
    ws.onerror = () => { if (!welcomed) { reject(new Error('cannot reach the WAAPI server at ' + url)); } };
    ws.onclose = () => {
      for (const p of pending.values()) { p.rej(new Error(`${p.procedure}: connection closed`)); }
      pending.clear();
    };
  });
}

module.exports = { connect };
