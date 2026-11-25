#!/usr/bin/env node
/*
 * Builds the Phase 4 weapon-fire test content in wwise/DhewmTest through the Wwise
 * Authoring API (WAAPI). Design: PROJECT_NOTES.md section 8.4, D-052.
 *
 * Input: the manifest written by tools/extract_doom_sounds.py (deps/doom3-sounds/weapon_fire.json).
 * The samples are the user's own Doom 3 files; Wwise copies them into the project's
 * Originals/ folder, which is git-ignored. Nothing from Doom 3 is committed.
 *
 * Needs a running WAAPI server for the project (Node 22 or newer):
 *   WwiseConsole.exe waapi-server <repo>/wwise/DhewmTest/DhewmTest.wproj
 *
 * Creates (re-running merges into the existing objects):
 *   Attenuation   \Attenuations\Default Work Unit\Doom_Weapon     (max distance 30 m, the weapon shaders' maxDistance)
 *   Actor-Mixer   \Actor-Mixer Hierarchy\Default Work Unit\WeaponFire   (3D position, attenuation)
 *     Random Container per shader (avoid repeating), one Sound SFX per sample
 *   Event         \Events\Default Work Unit\Play_<shader>   (Play action on the container)
 *   SoundBank     WeaponFire (all those events)
 * then generates the banks, saves the project and, with --deploy <dir>, copies the .bnk files there
 * (for example build/RelWithDebInfo/base/wwise/Windows).
 *
 * Usage: node tools/wwise_weapon_fire.js [--manifest <file>] [--deploy <dir>]
 */

const fs = require('fs');
const path = require('path');

const REPO = path.resolve(__dirname, '..');
const args = process.argv.slice(2);
function arg(name, fallback) {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
}
const MANIFEST = arg('--manifest', path.join(REPO, 'deps', 'doom3-sounds', 'weapon_fire.json'));
const DEPLOY = arg('--deploy', null);
const URL_WAAPI = process.env.WAAPI_URL || 'ws://127.0.0.1:8080/waapi';

const WORK_UNIT = '\\Actor-Mixer Hierarchy\\Default Work Unit';
const MIXER = `${WORK_UNIT}\\WeaponFire`;
const ATTENUATION_PARENT = '\\Attenuations\\Default Work Unit';
const ATTENUATION = `${ATTENUATION_PARENT}\\Doom_Weapon`;
const EVENT_PARENT = '\\Events\\Default Work Unit';
const BANK_PARENT = '\\SoundBanks\\Default Work Unit';
const BANK = 'WeaponFire';
const GENERATED = path.join(REPO, 'wwise', 'DhewmTest', 'GeneratedSoundBanks', 'Windows');

const WAMP_HELLO = 1, WAMP_WELCOME = 2, WAMP_CALL = 48, WAMP_RESULT = 50, WAMP_ERROR = 8;
const ws = new WebSocket(URL_WAAPI);
let nextId = 10;
const pending = new Map();

function call(procedure, kwargs) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject, procedure });
    ws.send(JSON.stringify([WAMP_CALL, id, {}, procedure, [], kwargs]));
  });
}

ws.onmessage = (msg) => {
  const m = JSON.parse(String(msg.data));
  if (m[0] === WAMP_WELCOME) { return; }
  const p = pending.get(m[1]);
  if (!p) { return; }
  pending.delete(m[1]);
  if (m[0] === WAMP_RESULT) { p.resolve(m[4] ?? {}); }
  else if (m[0] === WAMP_ERROR) { p.reject(new Error(`${p.procedure}: ${m[3]}: ${JSON.stringify(m[4])}`)); }
};

ws.onopen = () => {
  ws.send(JSON.stringify([WAMP_HELLO, 'realm1', { roles: { caller: {} } }]));
  run().catch((e) => { console.error('FAILED:', e.message); process.exitCode = 1; })
       .finally(() => { ws.__done = true; ws.close(); setTimeout(() => process.exit(process.exitCode || 0), 200); });
};

ws.onerror = () => { if (!ws.__done) { console.error('FAILED: cannot reach the WAAPI server at ' + URL_WAAPI); process.exitCode = 1; } };

function sampleName(file) {
  return path.basename(file, path.extname(file)).replace(/[^A-Za-z0-9_]/g, '_');
}

async function run() {
  const manifest = JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
  console.log(`manifest: ${manifest.length} shaders`);

  // Shared attenuation: the weapon shaders all use maxDistance 30 (meters, D-050).
  // Per-shader distances and volumes come in Phase 5.
  await call('ak.wwise.core.object.create', {
    parent: ATTENUATION_PARENT, type: 'Attenuation', name: 'Doom_Weapon', onNameConflict: 'merge',
    '@RadiusMax': 30,
  });

  // Parent of all weapon-fire sounds: 3D positioned, with the shared attenuation.
  await call('ak.wwise.core.object.create', {
    parent: WORK_UNIT, type: 'ActorMixer', name: 'WeaponFire', onNameConflict: 'merge',
  });
  await call('ak.wwise.core.object.setProperty', { object: MIXER, property: 'OverridePositioning', value: true });
  await call('ak.wwise.core.object.setProperty', { object: MIXER, property: '3DSpatialization', value: 1 });
  await call('ak.wwise.core.object.setProperty', { object: MIXER, property: 'EnableAttenuation', value: true });
  await call('ak.wwise.core.object.setReference', { object: MIXER, reference: 'Attenuation', value: ATTENUATION });
  console.log('actor-mixer WeaponFire: 3D position, attenuation Doom_Weapon');

  const events = [];
  for (const entry of manifest) {
    const container = `${MIXER}\\${entry.shader}`;
    await call('ak.wwise.core.object.create', {
      parent: MIXER, type: 'RandomSequenceContainer', name: entry.shader, onNameConflict: 'merge',
      '@RandomOrSequence': 1,              // random
      '@RandomAvoidRepeating': entry.samples.length > 1,
    });

    // One Sound SFX per sample, imported into the container (Wwise copies the file into Originals/).
    await call('ak.wwise.core.audio.import', {
      importOperation: 'useExisting',
      default: { importLanguage: 'SFX' },
      imports: entry.samples.map((file) => ({
        audioFile: file,
        objectPath: `${container}\\<Sound SFX>${sampleName(file)}`,
      })),
    });

    // SSF_PLAY_ONCE in the shader becomes a playback limit of one per game object (event setup, S-069).
    if (entry.flags.includes('play_once')) {
      await call('ak.wwise.core.object.setProperty', { object: container, property: 'UseMaxSoundPerInstance', value: true });
      await call('ak.wwise.core.object.setProperty', { object: container, property: 'MaxSoundPerInstance', value: 1 });
    }

    await call('ak.wwise.core.object.create', {
      parent: EVENT_PARENT, type: 'Event', name: entry.event, onNameConflict: 'merge',
      children: [{ type: 'Action', name: '', '@ActionType': 1, '@Target': container }],
    });
    events.push(`${EVENT_PARENT}\\${entry.event}`);
    console.log(`${entry.event}: ${entry.samples.length} sample(s)${entry.flags.length ? ' flags ' + entry.flags.join(',') : ''}`);
  }

  await call('ak.wwise.core.object.create', { parent: BANK_PARENT, type: 'SoundBank', name: BANK, onNameConflict: 'merge' });
  await call('ak.wwise.core.soundbank.setInclusions', {
    soundbank: `${BANK_PARENT}\\${BANK}`,
    operation: 'add',
    inclusions: events.map((object) => ({ object, filter: ['events', 'structures', 'media'] })),
  });

  const gen = await call('ak.wwise.core.soundbank.generate', { soundbanks: [], writeToDisk: true, platforms: ['Windows'] });
  console.log('generated:', (gen.soundbanks || []).map((b) => b.name).join(', '));
  await call('ak.wwise.core.project.save', {});
  console.log('project saved');

  if (DEPLOY) {
    fs.mkdirSync(DEPLOY, { recursive: true });
    for (const f of fs.readdirSync(GENERATED).filter((n) => n.endsWith('.bnk'))) {
      fs.copyFileSync(path.join(GENERATED, f), path.join(DEPLOY, f));
      console.log('deployed', f, '->', DEPLOY);
    }
  }
}
