#!/usr/bin/env node
/*
 * Builds the dhewm3 Wwise test content through the Wwise Authoring API (WAAPI).
 *
 * Needs a running WAAPI server for wwise/DhewmTest, started with:
 *   WwiseConsole.exe waapi-server <repo>/wwise/DhewmTest/DhewmTest.wproj
 * (default address ws://127.0.0.1:8080/waapi). Node 22 or newer (built-in WebSocket).
 *
 * Creates:
 *   Event  \Events\Default Work Unit\Play_TestTone  (Play action on TestTone)
 *   SoundBank TestBank containing that event
 * then generates all SoundBanks (including Init.bnk) and saves the project.
 *
 * Usage: node tools/wwise_test_project.js
 */

const URL_WAAPI = process.env.WAAPI_URL || 'ws://127.0.0.1:8080/waapi';
const TONE = '\\Actor-Mixer Hierarchy\\Default Work Unit\\TestTone';
const EVENT_PARENT = '\\Events\\Default Work Unit';
const BANK_PARENT = '\\SoundBanks\\Default Work Unit';
const WAMP_HELLO = 1, WAMP_WELCOME = 2, WAMP_CALL = 48, WAMP_RESULT = 50, WAMP_ERROR = 8;

const ws = new WebSocket(URL_WAAPI);
let nextId = 10;
const pending = new Map();

function call(procedure, args) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify([WAMP_CALL, id, {}, procedure, [], args]));
  });
}

ws.onmessage = (msg) => {
  const m = JSON.parse(String(msg.data));
  if (m[0] === WAMP_WELCOME) { return; }
  const p = pending.get(m[1]);
  if (!p) { return; }
  pending.delete(m[1]);
  if (m[0] === WAMP_RESULT) { p.resolve(m[4] ?? {}); }
  else if (m[0] === WAMP_ERROR) { p.reject(new Error(`${m[3]}: ${JSON.stringify(m[4])}`)); }
};

ws.onopen = () => {
  ws.send(JSON.stringify([WAMP_HELLO, 'realm1', { roles: { caller: {} } }]));
  run().catch((e) => { console.error('FAILED:', e.message); process.exitCode = 1; })
       .finally(() => { ws.__done = true; ws.close(); setTimeout(() => process.exit(process.exitCode || 0), 200); });
};

ws.onerror = () => { if (!ws.__done) { console.error('FAILED: cannot reach the WAAPI server at ' + URL_WAAPI); process.exitCode = 1; } };

async function run() {
  const event = await call('ak.wwise.core.object.create', {
    parent: EVENT_PARENT,
    type: 'Event',
    name: 'Play_TestTone',
    onNameConflict: 'merge',
    children: [{ type: 'Action', name: '', '@ActionType': 1, '@Target': TONE }],
  });
  console.log('event:', event.name ?? event.id ?? 'ok');

  const bank = await call('ak.wwise.core.object.create', {
    parent: BANK_PARENT,
    type: 'SoundBank',
    name: 'TestBank',
    onNameConflict: 'merge',
  });
  console.log('soundbank:', bank.name ?? bank.id ?? 'ok');

  await call('ak.wwise.core.soundbank.setInclusions', {
    soundbank: `${BANK_PARENT}\\TestBank`,
    operation: 'add',
    inclusions: [{ object: `${EVENT_PARENT}\\Play_TestTone`, filter: ['events', 'structures', 'media'] }],
  });
  console.log('inclusions set');

  const gen = await call('ak.wwise.core.soundbank.generate', { soundbanks: [], writeToDisk: true, platforms: ['Windows'] });
  console.log('generated:', JSON.stringify(gen.soundbanks ? gen.soundbanks.map((b) => b.name) : gen).slice(0, 300));

  await call('ak.wwise.core.project.save', {});
  console.log('project saved');
}
