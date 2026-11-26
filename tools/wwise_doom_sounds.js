#!/usr/bin/env node
/*
 * Builds the Doom 3 sounds in wwise/DhewmTest through the Wwise Authoring API (WAAPI):
 * one event per sound shader, one bank per .sndshd file. Design: PROJECT_NOTES.md section 8.5
 * (D-052 to D-055). Replaces tools/wwise_weapon_fire.js (Phase 4).
 *
 * Input: the manifest written by tools/extract_doom_sounds.py (deps/doom3-sounds/doom_sounds.json).
 * The samples are the user's own Doom 3 files; Wwise copies them into the project's Originals/
 * folder, which is git-ignored. No audio from Doom 3 is committed.
 *
 * Needs WAAPI: Wwise Authoring with the project open (WAAPI enabled), or
 *   WwiseConsole.exe waapi-server <repo>/wwise/DhewmTest/DhewmTest.wproj
 *
 * What it builds (re-running replaces the generated objects):
 *   Game Parameters \Game Parameters\Doom: RTPC_Volume, RTPC_Fade, RTPC_Spatialization,
 *                   RTPC_SoundClass0..3, RTPC_Slowmo (set by the game, section 8.5)
 *   States          \States\Doom\EnviroSuit (Off, On)
 *   Buses           Master Audio Bus\Doom\SoundClass_0..3 (shader soundClass; volume = RTPC_SoundClass<n>)
 *   Attenuations    \Attenuations\Doom: one per (minDistance, maxDistance, omnidirectional, volume cap)
 *   Actor-Mixer     \Actor-Mixer Hierarchy\Doom_<group> (work unit) \ <group> (actor-mixer with the RTPCs)
 *                     \ <shader> random container (samples), or sequence container (lead-in, then loop)
 *   Events          \Events\Doom_<group>\Play_<shader>
 *   SoundBanks      \SoundBanks\Doom\<group> (one per .sndshd file, D-055)
 * then removes the Phase 4 WeaponFire objects, generates the banks, saves the project and, with
 * --deploy <dir>, copies the .bnk and .json files there and writes banks.txt (the bank list the game loads).
 * Also writes wwise/DhewmTest/doom_events.txt: the event and game-parameter list for the students.
 *
 * Usage: node tools/wwise_doom_sounds.js [--manifest <file>] [--deploy <dir>] [--only <group,group>]
 *          [--banks <bank,bank>] [--skip-build] [--no-generate]
 *   --only        build these groups only (default: all)
 *   --banks       generate and deploy these banks only, plus Init and TestBank (default: all built banks)
 *   --skip-build  do not change the project, only generate and deploy
 *   --remove-others  with --only: remove the other Doom groups from the project (test without a license key)
 */

const fs = require('fs');
const path = require('path');
const { connect } = require('./waapi_client.js');

const REPO = path.resolve(__dirname, '..');
const args = process.argv.slice(2);
const arg = (name, fallback) => { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback; };
const MANIFEST = arg('--manifest', path.join(REPO, 'deps', 'doom3-sounds', 'doom_sounds.json'));
const DEPLOY = arg('--deploy', null);
const ONLY = arg('--only', null);
const GENERATE = !args.includes('--no-generate');
const SKIP_BUILD = args.includes('--skip-build');
// Banks to generate and deploy (default: all). Without a Wwise license key, a generation may hold
// at most 200 media files, so a test without a license uses a subset (PROJECT_NOTES.md S-081).
const BANKS = arg('--banks', null);
const PROJECT_DIR = path.join(REPO, 'wwise', 'DhewmTest');
const GENERATED = path.join(PROJECT_DIR, 'GeneratedSoundBanks', 'Windows');

const BS = '\\';
const P = (...parts) => BS + parts.join(BS);
const GP = (name) => P('Game Parameters', 'Doom', name);
const MASTER_BUS = P('Master-Mixer Hierarchy', 'Default Work Unit', 'Master Audio Bus');
const CLASS_BUS = (n) => `${MASTER_BUS}${BS}Doom${BS}SoundClass_${n}`;
const CONVERSION = P('Conversion Settings', 'Factory Conversion Settings', 'Vorbis', 'Vorbis Quality Medium');

// Game parameters the game sets (section 8.5). Min, max, default.
const GAME_PARAMETERS = [
  ['RTPC_Volume', -96, 24, 0, 'dB offset: the emitter volume minus the shader volume (speakers, ModifySound). Per playing sound'],
  ['RTPC_Fade', -96, 0, 0, 'dB, FadeSound per channel, with the fade time as interpolation. Per playing sound'],
  ['RTPC_Spatialization', 0, 100, 100, '100 = 3D panning, 0 = no panning (run-time SSF_OMNIDIRECTIONAL). Per playing sound'],
  ['RTPC_SoundClass0', -96, 0, 0, 'dB, FadeSoundClasses( 0 ): bus SoundClass_0. Global'],
  ['RTPC_SoundClass1', -96, 0, 0, 'dB, FadeSoundClasses( 1 ): bus SoundClass_1. Global'],
  ['RTPC_SoundClass2', -96, 0, 0, 'dB, FadeSoundClasses( 2 ): bus SoundClass_2. Global'],
  ['RTPC_SoundClass3', -96, 0, 0, 'dB, FadeSoundClasses( 3 ): bus SoundClass_3. Global'],
  ['RTPC_Slowmo', 0.1, 1, 1, 'Slow motion speed (Resurrection of Evil); 1 = normal. Bound to pitch. Global'],
];
const LINEAR = (x0, x1) => ({ type: 'Curve', points: [{ x: x0, y: x0, shape: 'Linear' }, { x: x1, y: x1, shape: 'Linear' }] });

function sanitize(name) { return name.replace(/[^A-Za-z0-9_]/g, '_'); }
function num(v) { return String(Math.round(v * 100) / 100).replace('-', 'm').replace('.', 'p'); }

/*
 * Doom's distance gain with OpenAL (snd_world.cpp): frac = 1 - (d - min) / (max - min), squared
 * (s_quadraticFalloff 1), times OpenAL's default inverse-distance model with reference distance min
 * (min / d), and the final gain is capped at 1. With the shader volume v (dB) on the Wwise object,
 * the curve that gives the same result is min(-v, a(d)) for v > 0, else a(d) (a = falloff in dB).
 */
function volumeCurve(min, max, vcap) {
  const pts = [];
  const a = (d) => {
    if (d <= min) { return 0; }
    if (d >= max) { return -200; }
    const frac = 1 - (d - min) / (max - min);
    const inv = min > 0 ? min / d : 1;
    return Math.max(-200, 20 * Math.log10(frac * frac * inv));
  };
  const c = (d) => Math.min(a(d), vcap > 0 ? -vcap : 0);
  const xs = [0];
  if (min > 0) { xs.push(min); }
  for (let k = 1; k < 10; k++) { xs.push(min + (max - min) * k / 10); }
  xs.push(max);
  for (const x of xs) { if (!pts.length || x > pts[pts.length - 1].x + 1e-6) { pts.push({ x, y: Math.round(c(x) * 100) / 100, shape: 'Linear' }); } }
  pts[pts.length - 1].y = -200;
  return pts;
}

function spreadCurve(min, max, omni) {
  if (omni) { return [{ x: 0, y: 100, shape: 'Linear' }, { x: max, y: 100, shape: 'Linear' }]; }
  // Doom reduces spatialization inside minDistance (spatialize = d / min); full spread at the listener (D-054).
  const knee = min > 0 ? Math.min(min, max * 0.99) : max * 0.05;
  return [{ x: 0, y: 100, shape: 'Linear' }, { x: knee, y: 0, shape: 'Linear' }, { x: max, y: 0, shape: 'Linear' }];
}

async function setBatched(w, objects, extra = {}, size = 150) {
  for (let i = 0; i < objects.length; i += size) {
    await w.call('ak.wwise.core.object.set', Object.assign({ objects: objects.slice(i, i + size) }, extra));
  }
}

async function exists(w, p) {
  const r = await w.call('ak.wwise.core.object.get', { from: { path: [p] } }, { return: ['id'] }).catch(() => ({ return: [] }));
  return (r.return || []).length > 0;
}

async function ensureWorkUnit(w, root, name) {
  if (!(await exists(w, P(root, name)))) {
    await w.call('ak.wwise.core.object.create', { parent: P(root), type: 'WorkUnit', name, onNameConflict: 'merge' });
  }
}

async function removeIfExists(w, p) {
  if (await exists(w, p)) { await w.call('ak.wwise.core.object.delete', { object: p }); return true; }
  return false;
}

async function run() {
  const manifest = JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
  const only = ONLY ? new Set(ONLY.split(',')) : null;
  const groups = manifest.groups.filter((g) => !only || only.has(g.name));
  const w = await connect();
  const info = await w.call('ak.wwise.core.getProjectInfo');
  console.log(`project ${info.name}; ${groups.length} groups, ${groups.reduce((n, g) => n + g.shaders.length, 0)} shaders`);
  const banks = groups.map((g) => g.name);
  if (!SKIP_BUILD) { await build(w, groups); }
  if (GENERATE) { await generate(w, BANKS ? BANKS.split(',') : banks); }
  w.close();
}

async function build(w, groups) {
  const only = ONLY ? new Set(ONLY.split(',')) : null;

  // --only with --remove-others: the project keeps just these groups. The trial license counts the
  // media of the whole project (at most 200), so a test without a license key needs a small project.
  if (only && args.includes('--remove-others')) {
    for (const root of ['Actor-Mixer Hierarchy', 'Events']) {
      const r = await w.call('ak.wwise.core.object.get', { from: { path: [P(root)] }, transform: [{ select: ['children'] }] }, { return: ['name', 'path', 'type'] });
      for (const o of r.return || []) {
        if (o.type === 'WorkUnit' && o.name.startsWith('Doom_') && !only.has(o.name.slice(5))) { await w.call('ak.wwise.core.object.delete', { object: o.path }); }
      }
    }
    if (await exists(w, P('SoundBanks', 'Doom'))) {
      const r = await w.call('ak.wwise.core.object.get', { from: { path: [P('SoundBanks', 'Doom')] }, transform: [{ select: ['children'] }] }, { return: ['name', 'path'] });
      for (const o of r.return || []) { if (!only.has(o.name)) { await w.call('ak.wwise.core.object.delete', { object: o.path }); } }
    }
    console.log('removed the Doom groups not in --only');
  }

  // Phase 4 content is replaced: same event names would clash.
  for (const p of [P('Actor-Mixer Hierarchy', 'Default Work Unit', 'WeaponFire'), P('SoundBanks', 'Default Work Unit', 'WeaponFire'), P('Attenuations', 'Default Work Unit', 'Doom_Weapon')]) {
    if (await removeIfExists(w, p)) { console.log('removed Phase 4 object', p); }
  }
  const oldEvents = await w.call('ak.wwise.core.object.get', { from: { path: [P('Events', 'Default Work Unit')] }, transform: [{ select: ['children'] }] }, { return: ['path', 'name'] });
  for (const e of oldEvents.return || []) {
    if (e.name !== 'Play_TestTone') { await w.call('ak.wwise.core.object.delete', { object: e.path }); }
  }

  // Shared objects: game parameters, states, buses.
  for (const root of ['Game Parameters', 'States', 'Attenuations', 'SoundBanks']) { await ensureWorkUnit(w, root, 'Doom'); }
  await w.call('ak.wwise.core.object.set', { onNameConflict: 'merge', objects: [{ object: P('Game Parameters', 'Doom'), children: GAME_PARAMETERS.map(([name, min, max, def, notes]) => ({ type: 'GameParameter', name, notes, '@Min': min, '@Max': max, '@InitialValue': def })) }] });
  await w.call('ak.wwise.core.object.set', { onNameConflict: 'merge', objects: [{ object: P('States', 'Doom'), children: [{ type: 'StateGroup', name: 'EnviroSuit', notes: 'SetEnviroSuit (Resurrection of Evil). Not bound to an effect yet', children: [{ type: 'State', name: 'Off' }, { type: 'State', name: 'On' }] }] }] });
  await w.call('ak.wwise.core.object.set', { onNameConflict: 'merge', objects: [{ object: MASTER_BUS, children: [{ type: 'Bus', name: 'Doom', children: [0, 1, 2, 3].map((n) => ({ type: 'Bus', name: `SoundClass_${n}`, notes: `Doom sound class ${n} (shader soundClass, FadeSoundClasses)` })) }] }] });
  await setBatched(w, [0, 1, 2, 3].map((n) => ({ object: CLASS_BUS(n), '@RTPC': [{ type: 'RTPC', name: '', '@Curve': LINEAR(-96, 0), '@PropertyName': 'BusVolume', '@ControlInput': GP(`RTPC_SoundClass${n}`) }] })), { listMode: 'replaceAll' });
  console.log('game parameters, EnviroSuit state, buses SoundClass_0..3');

  // Attenuations: one per distinct (min, max, omni, volume cap).
  const attOf = new Map();
  const attKey = (s) => {
    const min = Math.max(0, s.minDistance), max = Math.max(s.maxDistance, min + 0.1);
    const omni = s.flags.includes('omnidirectional'), vcap = Math.max(0, Math.round(s.volume));
    return { key: `Doom_${num(min)}_${num(max)}${omni ? '_omni' : ''}${vcap > 0 ? '_cap' + vcap : ''}`, min, max, omni, vcap };
  };
  for (const g of groups) { for (const s of g.shaders) { if (!s.flags.includes('global')) { const a = attKey(s); attOf.set(a.key, a); } } }
  await setBatched(w, [{ object: P('Attenuations', 'Doom'), children: [...attOf.values()].map((a) => ({ type: 'Attenuation', name: a.key, '@RadiusMax': a.max })) }], { onNameConflict: 'merge' });
  for (const a of attOf.values()) {
    const object = P('Attenuations', 'Doom', a.key);
    await w.call('ak.wwise.core.object.setAttenuationCurve', { object, curveType: 'VolumeDryUsage', use: 'Custom', points: volumeCurve(a.min, a.max, a.vcap) });
    await w.call('ak.wwise.core.object.setAttenuationCurve', { object, curveType: 'SpreadUsage', use: 'Custom', points: spreadCurve(a.min, a.max, a.omni) });
  }
  console.log(`${attOf.size} attenuations`);

  const eventLines = [];
  for (const g of groups) {
    const wu = `Doom_${g.name}`;
    await removeIfExists(w, P('Actor-Mixer Hierarchy', wu));
    await removeIfExists(w, P('Events', wu));
    await ensureWorkUnit(w, 'Actor-Mixer Hierarchy', wu);
    await ensureWorkUnit(w, 'Events', wu);
    const mixer = P('Actor-Mixer Hierarchy', wu, g.name);

    // Import every sample: <group> actor-mixer \ <shader> container \ sound. Wwise copies the
    // file into Originals/SFX/<sample folder>, so equal file names in different folders stay apart.
    const imports = [];
    const loopSounds = [];
    const containers = [];
    for (const s of g.shaders) {
      const cname = sanitize(s.shader);
      const leadin = s.leadins.length > 0 && s.samples.length > 0;
      const used = new Set();
      const soundName = (file) => { let n = sanitize(path.basename(file, path.extname(file))); let k = n, i = 2; while (used.has(k.toLowerCase())) { k = `${n}_${i++}`; } used.add(k.toLowerCase()); return k; };
      const add = (list, sub) => {
        for (const smp of list) {
          const sn = soundName(smp.file);
          const containerPath = leadin ? `<Sequence Container>${cname}${BS}<Random Container>${sub}` : `<Random Container>${cname}`;
          imports.push({ audioFile: smp.file, objectPath: `${P('Actor-Mixer Hierarchy', wu)}${BS}<Actor-Mixer>${g.name}${BS}${containerPath}${BS}<Sound SFX>${sn}`, originalsSubFolder: path.dirname(smp.sample).replace(/\//g, BS) });
          if (s.flags.includes('looping') && sub !== 'leadin') { loopSounds.push(`${mixer}${BS}${cname}${leadin ? BS + sub : ''}${BS}${sn}`); }
        }
      };
      if (leadin) { add(s.leadins, 'leadin'); add(s.samples, 'loop'); } else { add(s.samples.length ? s.samples : s.leadins, 'samples'); }
      containers.push({ s, cname, leadin });
    }
    for (let i = 0; i < imports.length; i += 200) {
      await w.call('ak.wwise.core.audio.import', { importOperation: 'useExisting', default: { importLanguage: 'SFX' }, imports: imports.slice(i, i + 200), autoAddToSourceControl: false });
    }

    // Group actor-mixer: 3D positioning, output bus SoundClass_0, the per-sound RTPCs.
    await w.call('ak.wwise.core.object.set', { listMode: 'replaceAll', objects: [{
      object: mixer, notes: `Doom 3 sound shaders from sound/${g.name}.sndshd`,
      '@OverridePositioning': true, '@3DSpatialization': 1, '@EnableAttenuation': true,
      '@OverrideOutput': true, '@OutputBus': CLASS_BUS(0),
      // The samples are about 1 GB as PCM; Vorbis keeps the banks (all loaded at start, D-055) small.
      '@OverrideConversion': true, '@Conversion': CONVERSION,
      '@RTPC': [
        { type: 'RTPC', name: '', '@Curve': LINEAR(-96, 24), '@PropertyName': 'Volume', '@ControlInput': GP('RTPC_Volume') },
        { type: 'RTPC', name: '', '@Curve': LINEAR(-96, 0), '@PropertyName': 'Volume', '@ControlInput': GP('RTPC_Fade') },
        { type: 'RTPC', name: '', '@Curve': LINEAR(0, 100), '@PropertyName': 'SpeakerPanning3DSpatializationMix', '@ControlInput': GP('RTPC_Spatialization') },
        { type: 'RTPC', name: '', '@Curve': { type: 'Curve', points: [{ x: 0.1, y: -2400, shape: 'Log1' }, { x: 0.5, y: -1200, shape: 'Log1' }, { x: 1, y: 0, shape: 'Linear' }] }, '@PropertyName': 'Pitch', '@ControlInput': GP('RTPC_Slowmo') },
      ] }] });

    // Per shader: volume, positioning (global = 2D), attenuation, random/sequence, play once, sound class.
    const props = [];
    for (const { s, cname, leadin } of containers) {
      const c = `${mixer}${BS}${cname}`;
      const o = { object: c, notes: `shader ${s.shader}`, '@Volume': Math.max(-96, Math.min(24, s.volume)) };
      if (s.flags.includes('global')) {
        Object.assign(o, { '@OverridePositioning': true, '@3DSpatialization': 0, '@EnableAttenuation': false });
      } else {
        Object.assign(o, { '@OverridePositioning': true, '@3DSpatialization': 1, '@EnableAttenuation': true, '@Attenuation': P('Attenuations', 'Doom', attKey(s).key) });
      }
      if (leadin) {
        Object.assign(o, { '@RandomOrSequence': 0, '@PlayMechanismStepOrContinuous': 0, '@Playlist': [`${c}${BS}leadin`, `${c}${BS}loop`] });
        props.push({ object: `${c}${BS}leadin`, '@RandomOrSequence': 1, '@RandomAvoidRepeating': false, ...(s.leadinVolume !== null ? { '@Volume': Math.max(-96, Math.min(24, s.leadinVolume - s.volume)) } : {}) });
        props.push({ object: `${c}${BS}loop`, '@RandomOrSequence': 1, '@RandomAvoidRepeating': s.flags.includes('no_dups') });
      } else {
        Object.assign(o, { '@RandomOrSequence': 1, '@RandomAvoidRepeating': s.flags.includes('no_dups') });
      }
      if (s.flags.includes('playonce')) {
        Object.assign(o, { '@UseMaxSoundPerInstance': true, '@MaxSoundPerInstance': 1, '@IsGlobalLimit': 0, '@MaxReachedBehavior': 1 });
      }
      if (s.soundClass) { Object.assign(o, { '@OverrideOutput': true, '@OutputBus': CLASS_BUS(s.soundClass) }); }
      props.push(o);
    }
    for (const l of loopSounds) { props.push({ object: l, '@IsLoopingEnabled': true, '@IsLoopingInfinite': true }); }
    await setBatched(w, props, { listMode: 'replaceAll' });

    // Events and the bank.
    const evParent = P('Events', wu);
    await setBatched(w, [{ object: evParent, children: containers.map(({ s, cname }) => ({ type: 'Event', name: s.event, notes: `shader ${s.shader}`, children: [{ type: 'Action', name: '', '@ActionType': 1, '@Target': `${mixer}${BS}${cname}` }] })) }], { onNameConflict: 'merge' });
    // Recreated each run: an inclusion of a removed work unit would stay behind as a missing reference.
    await removeIfExists(w, P('SoundBanks', 'Doom', g.name));
    await w.call('ak.wwise.core.object.set', { onNameConflict: 'merge', objects: [{ object: P('SoundBanks', 'Doom'), children: [{ type: 'SoundBank', name: g.name }] }] });
    await w.call('ak.wwise.core.soundbank.setInclusions', { soundbank: P('SoundBanks', 'Doom', g.name), operation: 'add', inclusions: [{ object: evParent, filter: ['events', 'structures', 'media'] }] });
    for (const { s } of containers) { eventLines.push(`${s.event}\t${g.name}\t${s.flags.join(',')}`); }
    console.log(`${g.name}: ${containers.length} events, ${imports.length} sounds${loopSounds.length ? `, ${loopSounds.length} looping` : ''}`);
  }

  // Student list: events, banks and the game parameters the game sets (names only, no audio).
  if (!only) {
    const lines = ['# Doom 3 events in the DhewmTest project, generated by tools/wwise_doom_sounds.js.',
      '# The game posts Play_<sound shader> (D-052). Columns: event, bank, Doom shader flags.', '',
      '# Game parameters set by the game (section 8.5 of PROJECT_NOTES.md):',
      ...GAME_PARAMETERS.map(([n, min, max, def, notes]) => `#   ${n} [${min}, ${max}] default ${def}: ${notes}`),
      '# State group EnviroSuit (Off, On): SetEnviroSuit.', '', ...eventLines.sort((a, b) => a.localeCompare(b))];
    fs.writeFileSync(path.join(PROJECT_DIR, 'doom_events.txt'), lines.join('\n') + '\n');
    console.log(`wrote doom_events.txt (${eventLines.length} events)`);
  }

  await w.call('ak.wwise.core.project.save', {});
  console.log('project saved');
}

async function generate(w, banks) {
  const wanted = ['Init', 'TestBank', ...banks.filter((b) => b !== 'Init' && b !== 'TestBank')];
  // Init.bnk is always generated and cannot be named here.
  const gen = await w.call('ak.wwise.core.soundbank.generate', { soundbanks: wanted.filter((b) => b !== 'Init').map((name) => ({ name })), writeToDisk: true, platforms: ['Windows'] });
  const errors = (gen.logs || []).filter((l) => l.severity === 'Error' || l.severity === 'Fatal Error');
  console.log(`generated ${(gen.soundbanks || []).length} banks${errors.length ? `, ${errors.length} errors: ` + errors.slice(0, 5).map((e) => e.message).join(' | ') : ''}`);
  if (errors.length) { throw new Error('bank generation failed'); }
  if (DEPLOY) {
    fs.mkdirSync(DEPLOY, { recursive: true });
    for (const f of fs.readdirSync(DEPLOY)) { if (/\.(bnk|json)$/i.test(f) || f === 'banks.txt') { fs.unlinkSync(path.join(DEPLOY, f)); } }
    const present = [];
    for (const b of wanted) {
      for (const ext of ['.bnk', '.json']) {
        const src = path.join(GENERATED, b + ext);
        if (fs.existsSync(src)) { fs.copyFileSync(src, path.join(DEPLOY, b + ext)); }
      }
      if (fs.existsSync(path.join(DEPLOY, b + '.bnk'))) { present.push(b); }
    }
    // banks.txt: the content banks the game loads at start (Init.bnk is always loaded first).
    fs.writeFileSync(path.join(DEPLOY, 'banks.txt'), present.filter((b) => b !== 'Init').join('\n') + '\n');
    console.log(`deployed ${present.length} banks to ${DEPLOY}; banks.txt lists ${present.length - 1}`);
  }
}

run().then(() => setTimeout(() => process.exit(0), 200)).catch((e) => { console.error('FAILED:', e.message); setTimeout(() => process.exit(1), 200); });
