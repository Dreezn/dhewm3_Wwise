# dhewm3 → Wwise Port: Goals, Plan & Progress

Status: **Planning / review draft** (no code changed yet)

---

## 1. Goal

| ID | Current main goal | Status |
|---|---|---|
| G-01 | Replace dhewm3's OpenAL audio with Wwise 2024.1.18.9194, so students build the audio in a Wwise project and the custom game build plays their banks | Current |

Replace dhewm3's built-in audio (OpenAL + custom soundshader system) with **Audiokinetic Wwise 2024.1.18.9194**, so that:

- The game exposes a **fixed, documented list of Wwise Events and RTPCs** that the game code calls.
- **Students** build the actual audio content in a shared **Wwise project**: they create Events, Sound/Random/Blend containers, RTPC curves, and **generate SoundBanks**.
- The **custom game build** loads those banks at runtime and plays the students' audio.
- The project **builds and runs as-is** with its build files (CMake + whatever Makefile wrapper we decide on, see §6).

## 2. Hard constraints

| Constraint | Detail |
|---|---|
| Licensed content | Doom 3 game data (`base/*.pk4`, etc.) is **never committed**. Tests read it from `C:\Program Files (x86)\Steam\steamapps\common\...` (path to be confirmed). |
| Engine base | dhewm3 upstream: `https://github.com/dhewm/dhewm3` (cloned at commit `505c5c8`). |
| Licence | dhewm3 is GPL-3.0. The Wwise SDK is proprietary (Audiokinetic licence). Check the Audiokinetic terms for distributing a game build that links the SDK before anything is shared outside the course. |
| Wwise version | 2024.1.18.9194 (SDK + Authoring). Must match the SDK headers/libs we link against and the Authoring project the students use. |
| Platform | Windows first (dev machine is Win11, MSVC). Other platforms later. |

## 3. Current state (baseline survey, 2026-11-25)

- Upstream clone: `dhewm3/` (sub-folder of this workspace).
- Build system: **CMake** (`dhewm3/neo/CMakeLists.txt`). There is no top-level GNU Makefile; `neo/mmakefile.src` is the AROS-only build hook.
- Audio code:
  - `neo/sound/` (~8.5k lines excluding `stb_vorbis.h`): `snd_system.cpp` (OpenAL init/device/EFX, 1.5k lines), `snd_world.cpp`, `snd_emitter.cpp`, `snd_shader.cpp` (soundshader parsing), `snd_cache.cpp`, `snd_decoder.cpp` (OGG/WAV), `snd_efxfile.cpp` (EFX reverb), `snd_wavefile.cpp`.
  - Interface: `neo/sound/sound.h` (`idSoundSystem`, `idSoundWorld`, `idSoundEmitter`, `idSoundShader`). This is the seam we replace.
  - OpenAL stub for non-OpenAL builds: `neo/sys/stub/openal_stub.cpp`.
  - Game-side callers: `neo/game`, `neo/d3xp` (`Sound.cpp` in both). About **200 `StartSound(` call sites** in `.cpp` files.
  - Sound editor tool: `neo/tools/sound/` (DialogSound*). Likely to be dropped.
  - Sound-related CVars: `s_*` (e.g. `s_alHRTF`, `s_alOutputLimiter`, `s_scaleDownAndClamp`).
- Data side: `.sndshd` soundshader declarations in `base/` (Doom 3 game data, not in repo).

## 4. Approach

**Principle:** keep the game code calling the same high-level sound *intent* (an emitter plays a shader at a position on a channel), and swap the back end underneath. Replace the back end in layers so the game keeps running at every step.

### Architecture (proposed)

```
Game code (d3xp / game)
    │  StartSound(shader, channel, ...)  ← unchanged call sites
    ▼
idSoundWorld / idSoundEmitter            (neo/sound/, kept, thinned)
    │  maps soundshader → Wwise Event ID + RTPC values
    ▼
WwiseBridge (new: neo/sound/wwise/)      ← AK::SoundEngine calls only here
    │
    ▼
Wwise SDK (AkSoundEngine, StreamMgr, MemoryMgr, low-level IO)
    │
    ▼
Wwise SoundBanks (.bnk) + streamed media, built by students
```

- **Wwise bridge** is the only module that includes `AK/*` headers. Everything else talks to the bridge through `sound.h`-style interfaces.
- **Event table** (single source of truth): a data file (e.g. `base/wwise/events.json` or `.h` generated) listing every Event name, its game-side ID, its emitter usage, and the RTPCs it uses (name, range, meaning). Students work from this list.
- **Init order:** `AK::MemoryMgr` → `AK::StreamMgr` (+ low-level IO) → `AK::SoundEngine` → load `Init.bnk` → load game banks per map.

## 5. Conversion strategy: soundshader → Wwise

Doom 3 soundshaders carry: sound file list, `minDistance`/`maxDistance`, volume/`shakes`/`no_occlusion`/`looping`/`random` choice, `description` and class/fade flags. Plan:

1. **Inventory:** script that parses all `.sndshd` in the Doom 3 base, and writes a CSV: shader name, file list, distances, flags, number of call sites (grep of `.def`/`.script`/`.cpp`).
2. **Map each shader to one Wwise Event** named after the shader (`Play_<shader_name>`), with a sound container holding the same wav list.
3. **Map parameters:**
   - min/max distance → Wwise attenuation (distance curve) attached to the Event/bus.
   - volume / random / looping → container properties (random mode, loop, volume offset).
   - `shakes`, `no_occlusion`, EFX reverb → **dropped in phase 1**, see §7.
4. **Runtime parameters** that the game currently sets through `soundShaderParms_t` become **RTPCs** (e.g. `RTPC_Volume`, `RTPC_Slowmo`, `RTPC_Occlusion`). Game code sets them with `SetRTPCValue`.
5. **Generated list:** the converter emits the **Events list** and **RTPC list** as the deliverable for students, and a `soundshader → event` mapping table the bridge uses.
6. **Asset strategy:** wav/ogg files stay as-is in the Wwise project (imported via the Wwise project, not shipped from `base/` directly). Converter output will be a Wwise `.wproj` skeleton, *not* the licensed audio files themselves, unless the licence for those assets allows it (open question).

## 6. Build & tooling

- Keep **CMake** as the primary build (matches upstream, Windows MSVC works today).
- Add a **Makefile wrapper** (GNU make → calls CMake) so `make` works as the user asked. Open question: does "makefile" mean a top-level GNU Makefile or a `build.bat`/`nmake` file on Windows?
- Wwise SDK location: passed via CMake variable (`WWISE_SDK_ROOT`), never hardcoded. Defaults to the 2024.1.18 SDK path.
- Doom data path: passed via CMake/env (`DOOM3_DATA_DIR`), default to the Steam path above.

## 7. Feature triage (port vs. drop) — to be filled per phase

| Feature | Current | Plan | Decision |
|---|---|---|---|
| Play shader at emitter position (3D) | OpenAL source | Wwise Event with emitter position | Port |
| Shader min/max distance | OpenAL attenuation | Wwise attenuation curve | Port |
| Looping sounds | soundshader `looping` | Container loop | Port |
| Random variants (`random` / `diversity`) | soundshader | Random container | Port |
| Volume classes / fades (`FadeSoundClasses`) | custom | Bus volume + RTPC | Port |
| Slowmo / enviro suit pitch & filter | custom | RTPC | Port (RTPC) |
| Occlusion / portals (`no_occlusion`) | custom | Wwise occlusion or drop | Decide in phase 4 |
| EFX reverb (`.efx` files) | OpenAL EFX | Wwise Reflect / aux bus | Decide in phase 4 |
| HRTF / output limiter CVars | OpenAL | Wwise output settings | Drop (Wwise handles output) |
| Cinematic / AVI audio sync | `AVIOpen/AVIClose` | Wwise stream or drop | Decide in phase 5 |
| Sound editor tool (`tools/sound/`) | Editor | Replaced by Wwise Authoring | Drop |
| Save/load of sound state | `WriteToSaveGame` | Re-post Events/RTPCs on load | Port (later). Phase 3: the Wwise provider writes and reads the OpenAL savegame layout, so saves load with either backend (S-066); playing sounds are not restarted on load yet |
| Demo recording of sound | `StartWritingDemo` | Drop initially | Drop |
| Screen shake from loud sounds (`CurrentShakeAmplitudeForPosition`, shader `shakes`) | samples the playing wav data | Wwise meter or RTPC on the game side | Decide later. Phase 3 stub returns 0 (no shake from sound) |
| Sound-reactive GUIs and level meter (`ImageForTime`) | samples the mix | Wwise meter plug-in or drop | Decide later. Phase 3 stub returns an empty image |
| Light flicker with sound (`CurrentAmplitude`) | samples the playing wav | Wwise meter / RTPC or drop | Decide later. Phase 3 stub returns 0 |
| Menu music and GUI sounds (`PlayShaderDirectly`) | OpenAL | Wwise events | Port (with the event mapping). Phase 3 logs only |
| Mute and pause (`SetMute`, world `Pause`) | OpenAL mix | Master bus volume / Wwise pause | Port. Phase 3 keeps the state only |
| Sounds of a world that is not playing (game world under the menu) | not mixed | One listener per world, or bus muting | Decide in Phase 4 |

## 8. Phases (each one testable)

| # | Phase | Done when |
|---|---|---|
| 0 | Clone upstream, build unmodified dhewm3 on Windows with its CMake, play a map with sound | Baseline runs and plays audio with OpenAL |
| 1 | Add Wwise SDK to the build (`WWISE_SDK_ROOT`), init/shutdown the sound engine, no playback yet | Game starts, Wwise initializes, logs version 2024.1.18.9194 |
| 2 | Load `Init.bnk` + one test bank built by us; play one Event from a debug console command | Test event audible |
| 3 | Sound backend interface: service locator with OpenAL and Wwise providers (added S-061, D-048) | Both backends start, load a map and share savegames |
| 4 | Route `StartSound` through the Wwise provider for **one** sound class (weapon fire) | Sound plays from the right position and stops with the entity |
| 5 | Port the remaining `StartSound` paths, distances, loops, random, fades; RTPCs for slowmo/volume | Map plays with every game sound through Wwise |
| 6 | Music/cinematics (`AVI`, `idSoundWorld` streams), save/load | Campaign start-to-finish playable |
| 7 | Remove OpenAL from the build (or keep as optional fallback), remove `tools/sound` | Clean build with Wwise only |
| 8 | Automation: converter script `.sndshd` → Events/RTPC list + mapping table; test banks for students | Students can add an Event and hear it in game |

Each phase gets its own test checklist (sections below). I'll not move to the next phase until the previous one is validated with you.

### 8.1 Phase progress

The dashboard shows the current phase and the upcoming phase from this table. Status is one of `done`, `current` or `upcoming`. Each step ends with its state in brackets: `[done]`, `[blocked]` or `[todo]`.

| ID | Phase | Status | Steps |
|---|---|---|---|
| P-00 | Phase 0: baseline build and audio | done | Configure with CMake [done]; Build RelWithDebInfo with MSVC [done]; Start the game without crashing [done]; Confirm sound plays on a map [done] |
| P-01 | Phase 1: Wwise SDK in the build, engine init and shutdown | done | Locate the Wwise 2024.1.18.9194 SDK [done]; Add WWISE_ROOT to the CMake build [done]; Initialise and shut down the Wwise engine [done]; Log the Wwise version at start [done]; Game starts with Wwise initialised, no playback yet [done] |
| P-02 | Phase 2: first test bank and one event | done | Create a test bank in the Wwise project [done]; Load Init.bnk at start [done]; Play one event from a debug command [done]; Process Wwise every sound tick and set a listener [done]; Test event rendered, checked in the output capture [done] |
| P-03 | Phase 3: sound backend interface (OpenAL and Wwise behind one interface) | done | Write the interface design for review [done]; Add the sound system locator with OpenAL as the first provider [done]; Add the Wwise backend (system, world, emitter) [done]; Check both backends still start, update and stop [done] |
| P-04 | Phase 4: route one sound class (weapon fire) through Wwise | done | Plan the shader-to-event mapping for review [done]; Post the weapon-fire events from the Wwise emitter [done]; Track playing IDs so StopSound and CurrentlyPlaying work [done]; Hear weapon fire at the right position in a map [done] |
| P-05 | Phase 5: route the remaining sounds (distances, loops, random, fades, menu sounds) | current | Plan the remaining routes for review [todo]; Map all sound shaders to events [todo]; Distances, loops and random variants in Wwise [todo]; Sound class fades, mute and pause [todo]; Menu and GUI sounds [todo] |

### 8.2 Step details

Each phase step has a row here with its key (phase ID and step number). The dashboard shows these as a hover explanation: the files changed, added or removed, and the architecture parts affected. Rows marked "planned" describe work not yet started, and that work needs approval before it begins.

| Key | Files | Architecture |
|---|---|---|
| P-00.1 | None in the repo. Build folder created under build/ (git-ignored) | None |
| P-00.2 | None in the repo. Outputs written to build/RelWithDebInfo (git-ignored) | None |
| P-00.3 | None in the repo. Runtime DLLs copied into build/RelWithDebInfo (git-ignored) | None |
| P-00.4 | None | None. Confirms the unmodified OpenAL audio path works |
| P-01.1 | None. Read-only check of the Wwise SDK folder | None |
| P-01.2 | Planned: changed dhewm3/neo/CMakeLists.txt (WWISE_ROOT option, SDK include path, Wwise libraries, WWISE_ENABLED define) | Build only. Adds an optional Wwise dependency. The OpenAL build stays the default |
| P-01.3 | Planned: added dhewm3/neo/sound/snd_wwise.h and snd_wwise.cpp (engine, memory and stream manager start and stop, using the SDK default file I/O). CMake compiles the SDK's default I/O from the user's installed SDK (WWISE_ROOT/SDK/samples/SoundEngine), nothing copied into the repo. Changed dhewm3/neo/sound/snd_system.cpp (Init and Shutdown choose a backend). Changed dhewm3/neo/sound/snd_local.h (declarations) | Adds a second audio backend beside OpenAL. idSoundSystemLocal picks one at start-up. The sound.h interfaces and all game code stay unchanged |
| P-01.4 | Planned: changed dhewm3/neo/sound/snd_wwise.cpp (print the Wwise version at start) | None beyond P-01.3 |
| P-01.5 | Planned: changed dhewm3/neo/sound/snd_system.cpp (new cvar s_backend, default openal) | Runtime switch only. OpenAL stays the default for players |
| P-02.1 | Planned: added a wwise/ folder with the Wwise project and a generated test sound (no Doom originals). Built banks are git-ignored | None in code. Content only |
| P-02.2 | Planned: changed dhewm3/neo/sound/snd_wwise.cpp (load Init.bnk and the test bank from base/wwise/) | Adds a bank loading step to the Wwise backend. Bank files are never committed |
| P-02.3 | Planned: changed dhewm3/neo/sound/snd_wwise.cpp (PostEvent wrapper). Changed dhewm3/neo/sound/snd_system.cpp (console command to play one event) | Adds a debug path from the console to Wwise. No effect on game code |
| P-02.4 | Planned (approved, D-047): changed dhewm3/neo/sound/snd_wwise_sdk.cpp/.h (RenderAudio wrapper; register a listener game object and make it the default listener; debug PostEvent passes AK_EnableGetSourcePlayPosition). Changed snd_wwise.cpp/.h (idWwiseEngine::Update). Changed snd_system.cpp (AsyncUpdate and AsyncUpdateWrite call wwiseEngine.Update when Wwise runs) | Wwise gets its per-tick update from dhewm3's existing sound tick (about 60 Hz). Temporary hook: Phase 3 moves it behind the backend interface |
| P-02.5 | None. Output capture analysed with tools/analyze_wav.py (S-062, S-066). The listening check by the user moves to P-04.4 | None |
| P-03.1 | Done: design in §8.3 of these notes (no code) | Interface = sound.h (idSoundSystem, idSoundWorld, idSoundEmitter), one provider per backend, chosen by a service locator. Approved with option 1 (D-049) |
| P-03.2 | Planned (approved): added neo/sound/snd_locator.h/.cpp (idSoundSystemLocator). Changed framework/Common.cpp (one line), snd_system.cpp (Phase 1-2 Wwise lines removed), snd_local.h, neo/CMakeLists.txt | Locator picks the provider once from s_backend and points soundSystem at it; no forwarding layer. OpenAL classes unchanged. Game code and the game DLL unchanged |
| P-03.3 | Planned: changed snd_wwise.h/.cpp (become idSoundSystemWwise). Added snd_wwise_world.cpp (listener) and snd_wwise_emitter.cpp (one Wwise game object per emitter; StartSound logs only until Phase 4). snd_wwise_sdk.cpp gets game object, position and stop calls | Wwise implements sound.h in full; features without an equivalent are neutral stubs (list in §8.3). The temporary tick hook from P-02.4 is removed |
| P-03.4 | None (test run with s_backend openal and wwise, console dumps) | None |
| P-05.1 | Planned: design text in these notes, after Phase 4 | Not designed yet; needs review |
| P-05.2 | Planned: depends on P-05.1 | Not designed yet |
| P-05.3 | Planned: depends on P-05.1 | Not designed yet |
| P-05.4 | Planned: depends on P-05.1 | Not designed yet |
| P-05.5 | Planned: depends on P-05.1 | Not designed yet |
| P-04.1 | Done: design in §8.4, revised in S-069 (no code) | Every StartSound posts Play_<shader>; missing events give Wwise's own warning and are listed by wwise_missing. Weapon fire = the shaders in the weapon fire animations |
| P-04.2 | Done (S-071, S-072): added tools/extract_doom_sounds.py (original samples into the git-ignored Originals/) and tools/wwise_weapon_fire.js (WAAPI: events, random containers, playback limit, bank WeaponFire). Changed snd_wwise_emitter.cpp (StartSound posts the event), snd_wwise.h/.cpp (missing list, wwise_missing, loads WeaponFire.bnk), snd_wwise_sdk.h/.cpp (PostEvent on a game object) | First real route from game sound calls to Wwise. OpenAL code untouched |
| P-04.3 | Done (S-072): changed snd_wwise_emitter.cpp (playing IDs per channel, channel override, pruned with GetPlayingIDsFromGameObject) and snd_wwise_sdk.h/.cpp (that query) | Only the sound.h channel contract is in code; random choice, play-once and limits are Wwise event setup. No callbacks |
| P-04.4 | Done (S-073): user listened; all weapon fire plays through Wwise. None (map test, console dump and capture read by the assistant; the user listens to the test tone and the weapon fire) | None |

### 8.3 Phase 3 design: sound backend interface (for review, D-048)

Status: **approved by the user with option 1 (a true service locator, not a forwarding facade or proxy), S-064.** The first version of this design used a forwarding "facade". The user rejected that name, because a facade simplifies a subsystem and this object only forwarded calls (a proxy). The design was changed to the service locator below.

Service-provider roles: **service** = `idSoundSystem` (with `idSoundWorld`, `idSoundEmitter`) from `sound.h`; **providers** = `idSoundSystemLocal` (OpenAL, existing name kept to avoid renaming about 97 references) and `idSoundSystemWwise` (new); **locator** = `idSoundSystemLocator`. No null provider: dhewm3 already handles `s_noSound` inside the OpenAL provider.

**Where the line goes.** The interface is dhewm3's own public sound API in `sound.h`: `idSoundSystem`, `idSoundWorld` and `idSoundEmitter`. They are already abstract classes, and all game code (about 200 `StartSound` call sites, in `game/` and `d3xp/`) uses only them. Each backend implements these three classes in full. This matches D-004 (game call sites stay unchanged).

Rejected alternative: a lower interface at the voice level (replace only the OpenAL sources under `idSoundChannel`). Wwise would then receive dhewm3's own per-sample mixing decisions (channels, sample choice, distance gain). That works against how Wwise is meant to be used (events, game objects, RTPCs and the Wwise mixer), so it fights the convention.

**Structure.**

```
framework/Common.cpp ──> idSoundSystemLocator::Init()   (new, snd_locator.cpp; runs once at start-up)
                             │ reads s_backend, starts that provider (Wwise falls back to OpenAL),
                             │ then sets the global soundSystem pointer to it
game code ──> soundSystem ───┤ (direct calls, no forwarding layer)
                             ├──> OpenAL provider: idSoundSystemLocal / idSoundWorldLocal / idSoundEmitterLocal (existing code)
                             └──> Wwise provider:  idSoundSystemWwise / idSoundWorldWwise / idSoundEmitterWwise (new, snd_wwise_*.cpp)
                                                     └──> snd_wwise_sdk.cpp (only file with SDK headers, D-039)
```

- **Locator.** `idSoundSystemLocator::Init()` replaces the single `soundSystem->Init()` call in `framework/Common.cpp`. It reads `s_backend`, starts the chosen provider (Wwise falls back to OpenAL if it cannot start, as now) and points `soundSystem` at it. After that every call goes straight to the provider. This works because of the start-up order, checked in the console dump: the config files (and so `s_backend`) are read before sound starts, and the game DLL copies `soundSystem` through `gameImport_t` only later, when it loads. The provider is fixed for the run; switching needs a restart (as today). `soundSystem` starts out pointing at the OpenAL provider, so anything that runs before `Init()` behaves as upstream.
- **OpenAL backend.** The existing classes, moved behind the facade. Only the Wwise lines added in Phases 1 and 2 are taken out of `snd_system.cpp` (backend choice, `wwiseEngine.Update()` in the sound tick). OpenAL behaviour stays as upstream.
- **Wwise backend.** `idSoundSystemWwise` owns the engine (today's `idWwiseEngine`): Init and banks, `AsyncUpdate`/`AsyncUpdateWrite` call RenderAudio, Shutdown. `idSoundWorldWwise` owns the listener (`PlaceListener` sets its position). `idSoundEmitterWwise` is one Wwise game object: `UpdateEmitter` sets its position, `StartSound` posts an event, `StopSound` stops that channel's playing IDs. Until Phase 4 the emitter only logs what it would play. The debug commands (`wwise_play`, `wwise_status`, capture) move into this backend.
- **Shared parts.** Sound shader decls (`snd_shader.cpp`) stay shared, because the game looks shaders up by name. In Wwise mode they must not load wav samples: today they reach `soundSystemLocal.soundCache` directly, which stays NULL when OpenAL is not started, so no change is needed there. `Dhewm3SettingsMenu.cpp` also reads `soundSystemLocal` (OpenAL device menu); it stays OpenAL-only and shows nothing in Wwise mode.

**Features without a Wwise equivalent yet.** These `sound.h` calls get a neutral stub in the Wwise backend (no sound, no crash). Each becomes a port-or-drop item in the triage list later: `ImageForTime` (sound-reactive GUIs), `CurrentShakeAmplitudeForPosition` (screen shake from loud sounds), `AVIOpen`/`AVIClose`, demo recording (`StartWritingDemo`, `ProcessDemoCommand`), savegame sound state (`WriteToSaveGame`, `ReadFromSaveGame`), slow motion, enviro suit filter, `IsEFXAvailable` (returns 0), `GetSoundDecoderInfo` (returns -1).

**Thread rule.** dhewm3 calls `AsyncUpdate`/`AsyncUpdateWrite` from its async sound thread. The Wwise backend calls RenderAudio only there. Game-thread calls (`StartSound`, `UpdateEmitter`) only queue commands, which the SDK allows from any thread. The backend's Shutdown stops the tick from calling Wwise before the engine stops.

**Files.** Added: `neo/sound/snd_locator.h/.cpp` (locator), `neo/sound/snd_wwise_world.cpp`, `neo/sound/snd_wwise_emitter.cpp`. Changed: `framework/Common.cpp` (one line: `soundSystem->Init()` becomes `idSoundSystemLocator::Init()`), `snd_system.cpp` (Phase 1-2 Wwise lines removed), `snd_wwise.h/.cpp` (become `idSoundSystemWwise`), `snd_local.h` (declarations), `neo/CMakeLists.txt` (new files). Unchanged: `sound.h`, all game code, `snd_world.cpp`, `snd_emitter.cpp`, `snd_shader.cpp`, `snd_wwise_sdk.cpp` (except new emitter, listener and stop calls).

**Effect on Phase 4.** Weapon fire is implemented in `idSoundEmitterWwise::StartSound` (shader name to event name), not inside `snd_emitter.cpp`. The OpenAL emitter is never touched. The Phase 4 rows in §8.2 are updated when Phase 3 is approved.

**Test (P-03.4).** Both backends, console dumps read by the assistant: `s_backend openal` starts OpenAL as before and a map loads; `s_backend wwise` starts Wwise, `wwise_play Play_TestTone` gives a capture with sound, and a map loads with the Wwise emitters logging their StartSound calls. The default build without `WWISE_ROOT` still builds (OpenAL only, the locator has one provider).

### 8.4 Phase 4 design: sound shader to Wwise event (for review, P-04.1)

Status: **revised after the user's answers to Q-07 and Q-08 (S-069). The "Route" and "Emitter behaviour" parts below are replaced by "Revision S-069" at the end of this section. Waiting for approval of the revision before any Phase 4 code.**

**Facts checked (S-068).**
- Weapon fire is not started by game code but by frame commands in the weapon animations (`def/weapon_*.def`, for example `frame 2 sound_weapon player_pistol_fire`). The sound shader is named directly. The channel differs per weapon (pistol: weapon channel; machinegun: voice channel). So "weapon fire" is a set of shader names, not a channel.
- `PostEvent` with an event that is not in a loaded bank fails at once (no playing ID) and Wwise logs two warnings (`Event ID not found`, `Failed posting event`). The game must therefore know which events exist before posting, or the console fills with warnings for every unmapped shader (36 shaders on mars_city1).
- Wwise already writes bank metadata next to every bank (`<bank>.json`, the SoundBanksInfo format). It lists each event by name, with an estimated duration (`DurationType`, `DurationMin`, `DurationMax`). This is the standard Wwise way for a game to know bank contents.
- The OpenAL emitter's rules for `StartSound` (snd_emitter.cpp): a sound on a channel other than `SCHANNEL_ANY` first stops the sound on that channel; `SSF_PLAY_ONCE` shaders do not restart while playing; the same shader is not started twice in one frame; `diversity` and `SSF_NO_DUPS` pick a random sample; the return value is the sound length in ms (scripts use it to wait).
- `AK::SoundEngine::Query::GetPlayingIDsFromGameObject` lists what still plays on a game object, without the audio lock and without callbacks.

**Route (option A, recommended for Q-07): naming rule plus Wwise bank metadata.**
- Event name = `Play_` + the shader name, with every character that is not a letter, digit or underscore replaced by `_` (`player_pistol_fire` becomes `Play_player_pistol_fire`). `Play_` is the usual Wwise event prefix. Students make an event with that name, and it works with no code or data change.
- When a bank loads, the provider also reads its `<bank>.json` (copied with the bank) and keeps the set of event names with their `DurationMax`. A small reader in the provider handles only the fields it needs.
- `StartSound`: if the event exists, post it on the emitter's game object and return its duration in ms (0 for looping or unknown). If it does not exist, post nothing, remember the shader once, and print it only with `s_wwiseShowStartSound 1`. A new command `wwise_missing` lists the shaders that had no event so far: this is the students' to-do list for the current map.
- Option B (not recommended now): an explicit mapping file `shader event` per line. More flexible (many shaders to one event), but one more file to keep in step. It can be added later as an override on top of option A, without changing the rule.

**Emitter behaviour (same rules as OpenAL).** Channel override (stop the playing IDs of that channel first), `SSF_PLAY_ONCE`, and no double start in one frame are done in `idSoundEmitterWwise`. Random choice (`diversity`, `SSF_NO_DUPS`) is left to Wwise random containers, the Wwise way to vary sounds. Playing IDs are stored per channel and pruned with `GetPlayingIDsFromGameObject`, so `StopSound` per channel and `CurrentlyPlaying` work. Everything runs on the game thread; there are no callbacks.

**Test content (Q-08).** Option 1 (recommended): a tool extracts the weapon-fire samples from the user's own Doom 3 `.pk4` files into `wwise/DhewmTest/Originals/` (git-ignored, never committed, as agreed in D-016). The WAAPI script builds one event per weapon-fire shader (random container when the shader lists several samples), all 3D-positioned with one shared attenuation, in a bank `WeaponFire`. Option 2: generated test tones, one pitch per weapon. Option 1 gives a real listening test; option 2 keeps game data out of the Wwise project entirely. The list of weapon-fire shaders comes from the fire animations in the user's `def/weapon_*.def` files, read locally by the tool.

**Files.** Added: `neo/sound/snd_wwise_events.cpp` (event catalog: reads `<bank>.json`, name rule, missing list), `tools/extract_doom_sounds.py` (option 1 only), `tools/wwise_weapon_fire.js` (WAAPI). Changed: `snd_wwise_emitter.cpp` (StartSound, channel rules, playing IDs), `snd_wwise.h/.cpp` (catalog, `wwise_missing`, loads `WeaponFire.bnk`), `snd_wwise_sdk.h/.cpp` (PostEvent on a game object, playing-ID query). The banks and their `.json` are copied to `base/wwise/Windows/` as today (copy step documented in §15).

**Not in Phase 4 (Phase 5).** Per-shader volume and min/max distance (one shared attenuation for now), looping sounds, fades, mute and pause, menu sounds, sounds that keep playing across a savegame.

**Test (P-04.4).** Map with the pistol and shotgun: console dump shows the posted weapon-fire events and the missing list; the capture has sound; the user hears weapon fire, and the test tone.

**Revision S-069 (after the user's answers).**
- *Route.* Every `StartSound` posts `Play_<shader>` on the emitter's game object. There is no existence check and no bank metadata reader: a missing event gives Wwise's own console warning, as in most Wwise games. The provider remembers each shader whose post failed (the failure is immediate), and `wwise_missing` lists them. `StartSound` returns 0 ms for now; sound lengths for scripts that wait on them are a Phase 5 item (they can come from the bank metadata then).
- *Test content.* Option 1: the original weapon-fire samples are extracted locally from the user's Doom 3 files into the git-ignored `Originals/` folder.
- *Emitter behaviour: what is code and what is event setup.* Sound behaviour belongs in the Wwise event and its objects, set up by the sound designer. Code only keeps the promises that `sound.h` makes to the game, which Wwise cannot know about, because Wwise has no idea of dhewm3 channels:

| dhewm3 behaviour | Where | Why |
|---|---|---|
| `StopSound( channel )` stops what plays on that channel; `CurrentlyPlaying()` | Code (playing IDs per channel) | The game stops a channel without naming the sound (weapon switch, monster death). Only the game knows which playing ID is on which channel |
| A new sound on a channel replaces the old one on that channel | Code | Part of the same channel contract: the game relies on it (a new voice line cuts the previous one, even when it is a different sound) |
| Random sample choice (`diversity`, `SSF_NO_DUPS`) | Event setup (random container, "avoid repeating") | Sound design |
| `SSF_PLAY_ONCE`, no double start in one frame | Event setup (playback limit per game object) | Sound design; Wwise limits instances per game object |
| Looping, volume, distances, priority, voice limits | Event setup (container loop, attenuation, voice settings) | Sound design (Phase 5 for the shader values) |

  So `idSoundEmitterWwise` gets only the channel bookkeeping. The test events get a playback limit and random containers where the Doom shader has several samples.
- *Files.* No `snd_wwise_events.cpp`: the missing list is a few lines in `snd_wwise.cpp`. Added: `tools/extract_doom_sounds.py`, `tools/wwise_weapon_fire.js`. Changed: `snd_wwise_emitter.cpp`, `snd_wwise.h/.cpp`, `snd_wwise_sdk.h/.cpp` (PostEvent on a game object, `GetPlayingIDsFromGameObject`).

## 9. Test plan

- Build: `cmake` configure + build succeeds on Windows (MSVC) with and without `WWISE_SDK_ROOT`.
- Smoke: game starts, main menu sound plays, no crashes at shutdown.
- Per phase: list of in-game steps and expected audible result. Recorded in `docs/test-log.md` (to be created).
- Data: test uses the Steam Doom 3 install; **no Doom 3 files in the git repo**.

## 10. Git / publishing

- Plan: one git repository for **our project files** (this notes file, the converter, the Wwise bridge, the Wwise project skeleton, docs, build scripts). Upstream dhewm3 is either a **git submodule** (pinned to `505c5c8`) or a fork.
- `.gitignore` must exclude: Doom 3 data (`base/*.pk4`, any copied game assets), built `.bnk`/media unless we decide to ship test banks, and the Wwise SDK.
- Publishing to a remote is **not** done yet. It is an outward-facing action and needs your go-ahead (repo host, visibility, name).

## 11. Open questions (need your answer)

This table is the source of truth for the dashboard's open-questions list. Answers are typed on the dashboard; they are copied here (Answer column) when received, then the sync step is run (see §14).

| ID | Question | Status | Answer |
|---|---|---|---|
| Q-01 | Where is the Wwise SDK 2024.1.18 installed on this machine, and should the Authoring project live in this repo? | Answered | Wwise is at `C:\Audiokinetic\Wwise2024.1.18.9194`. The Authoring project goes in this repo. (D-019) |
| Q-02 | Top-level GNU `Makefile` wrapping CMake, or a Windows `build.bat` / `nmake` file? | Answered, to confirm (D-017) | CMake alone is enough. The original dhewm3 builds with CMake and has no top-level Makefile. A Makefile would only wrap CMake for convenience, so we keep CMake only unless you ask for one. |
| Q-03 | Exact Doom 3 folder under `C:\Program Files (x86)\Steam\steamapps\common\`? | Answered (D-021 for Q-03, D-024 for Q-04) | Elaboration: dhewm3 runs the original Doom 3 game data only. Use the classic folder `C:\Program Files (x86)\Steam\steamapps\common\Doom 3`. The upstream README says DOOM 3: BFG Edition is not supported by dhewm3. Recommendation: classic. Needs your choice. |
| Q-04 | Should `dhewm3/` stay a submodule, or become a fork of dhewm3? | Answered (D-021 for Q-03, D-024 for Q-04) | Elaboration: a submodule is a separate git repo kept inside ours. Our repo stores only which commit it points to. The question is which remote it points to. Upstream (`dhewm/dhewm3`) is read-only for us, so our changes cannot be saved there. Phase 1 changes upstream code, so we need a fork (for example `Dreezn/dhewm3`) that the submodule points to. Recommendation: fork on GitHub and point the submodule at the fork. Needs your choice. |
| Q-05 | Do students get the original Doom 3 audio files, or only placeholder audio in their Wwise project? | Answered (D-016) | Students get no ID Software audio, only a written list of events and game syncs. We make a template Wwise project ourselves, not shared. Sound choices are ours; using the originals is a good idea. The originals stay on local disk and are never uploaded to GitHub. |
| Q-06 | Windows only for now, or also Linux and macOS? | Answered (D-018) | Windows only. |
| Q-07 | Phase 4: how does a sound shader find its Wwise event? Recommended: a naming rule (Play_ + shader name) checked against Wwise's own bank metadata (§8.4 option A), instead of a separate mapping file (option B) | Answered (D-052) | Naming rule, but no existence check: post every event and let Wwise warn about missing ones in the console, as most games do. Keep the wwise_missing command. |
| Q-08 | Phase 4 test sounds: extract the original weapon-fire samples locally from your Doom 3 files (git-ignored, recommended), or use generated test tones? (§8.4) | Answered (D-052) | Extract the original weapon-fire sounds (local, git-ignored). |
| Q-09 | Event names: keep the Play_ prefix (Play_<shader>, D-052) or use the bare shader name as the event name? | Answered (D-052) | Keep Play_ (Wwise convention: action prefix, leaves room for Stop_/Pause_/Set_ events and keeps event names apart from the sound objects). |

## 12. Progress log

Rules for this log:
- One row per step or attempt, in order. Never delete or rewrite a row; if something is wrong, add a new row that corrects it.
- New rows are appended at the end of the table (the dashboard also sorts by step number, S-067).
- Each row says what was tried, the result (OK / FAILED / PARTIAL / BLOCKED), and any decision taken.
- Decisions get their own entry in §13 with an ID (D-xx) and are referenced from the log row.
- Failed attempts are kept, with the cause if known.

### 12.0 Current next step

This is the banner on the dashboard. Update it whenever the next step changes.

| ID | Current next step | Status |
|---|---|---|
| N-01 | Phase 5: P-05.1 (design for the remaining sounds, for review) | Current |

### 12.1 Step log

| ID | Date | Phase | Action / attempt | Result | Decision | Notes |
|---|---|---|---|---|---|---|
| S-001 | 2026-11-25 | Setup | Cloned `https://github.com/dhewm/dhewm3` into `dhewm3/` | OK | D-001, D-002 | Commit `505c5c8`. Working folder had no prior content. |
| S-002 | 2026-11-25 | Survey | Inspected `neo/sound/`, `neo/CMakeLists.txt`, `neo/mmakefile.src`, `neo/sys/stub/openal_stub.cpp`, `neo/tools/sound/` | OK | D-003 | Findings in §3. No top-level Makefile found. |
| S-003 | 2026-11-25 | Planning | Wrote `PROJECT_NOTES.md` (goals, architecture, triage, phases, tests) | OK | D-004 | Draft for review. Not yet agreed. |
| S-004 | 2026-11-25 | Publishing | Checked for a remote or upload target | Not done | D-005 | Waiting on repo host and visibility (§11 Q4). |
| S-005 | 2026-11-25 | Publishing | User created `https://github.com/Dreezn/dhewm3_Wwise.git`. Initialised workspace git repo (`main`), added `dhewm3/` as a submodule pinned to `505c5c8`, added root `.gitignore` (excludes `*.pk4`, Wwise SDK, `*.bnk`/`*.wem`, build output) | OK | D-007, D-008 | Existing clone reused as the submodule. Doom 3 data is not in the repo. |
| S-006 | 2026-11-25 | Publishing | Committed `bf2d028` (notes, `.gitignore`, submodule) and pushed to `origin/main` of `Dreezn/dhewm3_Wwise` | OK | D-005 (user asked for this repo) | Remote was empty on `ls-remote`. |
| S-007 | 2026-11-25 | Dashboard | Created dashboard from the Artifact "Data" type (`https://claude.ai/artifact/ST2NMcL3KCa9ZQyreMqjHY`). Loaded steps, decisions and open questions into its database (25 documents) | OK | D-008 | Questions are answered as comments on the dashboard. Page not yet visually checked. |

| S-008 | 2026-11-25 | Dashboard | Dashboard showed a format prompt and no working page because no page file existed. Wrote `files/index.html` (static tables for questions, steps, decisions reading the three datasets). Check passed | OK (page draws; no interactive controls yet) | D-008 | Clicks do nothing because the page has no controls. Waiting on user to say what they clicked and what they expected. |
| S-009 | 2026-11-25 | Hosting | Asked user where to host dashboard (GitHub Pages / keep artifact / make repo public) | Answered: keep artifact | D-009 | See D-009. |
| S-010 | 2026-11-25 | Sync | Made this file the source of truth. Added `tools/sync_dashboard.py` (generates `docs/dashboard-data.json`), a CI check (`.github/workflows/dashboard-sync.yml`), and §14 workflow. Fixed a duplicate decision ID (second D-005 became D-010; first D-005 marked superseded). Dashboard page given answer boxes, answers stored in `data/project/answers` | OK (script and CI written; CI not yet run on GitHub) | D-010, D-011, D-012 | Dashboard data re-synced from the notes. |
| S-011 | 2026-11-25 | Dashboard | Added main goal (G-01, §1) at the top of the dashboard, and a Proceed control gated on all questions answered plus a "state reviewed" confirmation. Proceed writes to `data/project/control/proceed` | OK (page written; not yet tested in browser) | D-013 | Proceed is the user's go-ahead for the assistant to continue. |
| S-013 | 2026-11-25 | Dashboard | User pressed Proceed; desktop app then showed "no other sessions are waiting for you" and hung. Debug: the write reached the store (`data/project/control/proceed`, 09:56:09Z). No other session exists on this machine. The artifact watch is connected with auto-replies armed. Removed the Proceed control from the page (version 8) | OK (control removed; hang cause not confirmed from here) | D-015 | Stored proceed record left in place; deletion awaits user approval. |
| S-014 | 2026-11-25 | Process | Copied the six dashboard answers into §11. Q-02 and Q-04 were questions back to us, so they got explanations; Q-03 and Q-04 still need a choice. Grouped decisions by category in §13. Dashboard step log now shows the last 7 steps | OK | D-016, D-017, D-018, D-019 | Full step log stays in §12.1. |
| S-015 | 2026-11-25 | Dashboard | Dashboard now shows only open questions. Answered questions stay in §11 of the notes and are hidden from the dashboard. The sync script filters on status | OK (not yet checked in browser) | D-020 | Dashboard hides answered questions; the store documents are left in place, so nothing was deleted. |
| S-016 | 2026-11-25 | Process | Elaboration for Q-03 and Q-04 added to §11 after the user asked for more detail. Checked upstream README: DOOM 3 BFG Edition is not supported. Both questions still need a choice | OK | D-021 (proposed), D-022 (proposed) | Waiting on user choice. |
| S-017 | 2026-11-25 | Dashboard | Moved away from the Claude artifact dashboard. Added a read-only summary dashboard in docs/ (GitHub Pages). Sync data now written to docs/dashboard-data.json. Rule recorded: never open a pull request to upstream dhewm3 | OK (page not yet checked on Pages) | D-023, D-024, D-025 | The artifact dashboard is no longer updated. |
| S-018 | 2026-11-25 | Phase 0 | Started Phase 0. Tools found: CMake 4.3.3, Visual Studio 2022 Enterprise (MSVC 14.44), Doom 3 classic folder present in Steam. Cloned dhewm3-libs (MinGW binaries only, not usable with MSVC) into build-deps/. Attempt to clone and bootstrap vcpkg to install OpenAL Soft and SDL2 for MSVC was denied by the automatic permission check, so it was not run | BLOCKED (needs OpenAL and SDL2 for MSVC) | D-021 (classic Doom 3 folder confirmed by the Steam listing) | User must approve vcpkg setup, or provide OpenAL and SDL2 MSVC SDK paths. |
| S-019 | 2026-11-25 | Phase 0 | Reviewed the manual attempt in `C:\Workspaces\Vienne\SoloProjects\dhewm3-1.5.4` (not copied into this repo). Findings recorded in §15 | OK | D-026, D-027 | Origin of the manual dhewm3-libs copy is now known (byte-identical to dhewm3-libs at 57c5659). |
| S-020 | 2026-11-25 | Phase 0 | Moved dhewm3-libs (commit 57c5659) into the git-ignored `deps/dhewm3-libs/` so the project is self-contained locally. Extended `.gitignore` (deps, game archives, audio, Wwise Originals). Added `tools/check_licensed_files.py` and a CI step that checks tracked files and history | OK | D-028, D-029 | Audit run: 9 file names checked, none forbidden. |
| S-021 | 2026-11-25 | Phase 0 | CMake configure: `cmake -S dhewm3/neo -B build -G "Visual Studio 17 2022" -A x64 -DDHEWM3LIBS=<repo>/deps/dhewm3-libs/x86_64-w64-mingw32`. MSVC 19.44 detected. OpenAL, SDL2 and libcurl found in deps | OK | D-026 | Configure accepted the MinGW-format .lib files. Link not yet tested. |
| S-022 | 2026-11-25 | Phase 0 | Build: `cmake --build build --config RelWithDebInfo`. Produced build/RelWithDebInfo/dhewm3.exe with 0 errors | OK | D-026 | Runtime DLLs not yet copied next to the exe. Game not yet started. |
| S-023 | 2026-11-25 | Phase 0 | Startup test: copied runtime DLLs (OpenAL32, SDL2, libcurl, zlib, ogg, vorbis, jpeg) next to dhewm3.exe. Ran with `fs_basepath` set to the classic Doom 3 folder under a 25 s timeout. Process stayed alive until the timeout (no crash). Console output is not written to a file, so audio was not confirmed | PARTIAL (runs; audio not verified) | D-026 | Needs the user to listen for sound on a map. |
| S-024 | 2026-11-25 | Dashboard | Removed Q-03 and Q-04 from the open list: the user has directed the classic Doom 3 folder (build run in S-021 to S-023) and the submodule without a GitHub fork (D-024). D-022 superseded. Added automatic refresh of the summary page every 5 minutes | OK (refresh not yet checked on Pages) | D-021, D-024 | Dashboard shows no open questions until a new one is added to §11. |
| S-025 | 2026-11-25 | Phase 0 | Audio check: the user started dhewm3 from build/RelWithDebInfo with the classic Doom 3 data and confirmed sound plays on a map | OK | D-026 | Phase 0 done. |
| S-026 | 2026-11-25 | Dashboard | Dashboard changes requested: hide the open-questions section when empty, remove the main goal, replace the next-step banner with the current phase (steps as coloured grid) and add the upcoming phase | OK (in progress) | D-030 | Dashboard updated before continuing work, so the user can follow along. |
| S-027 | 2026-11-25 | Phase 1 | Located the Wwise SDK at `C:\Audiokinetic\Wwise2024.1.18.9194\SDK`. Headers report version 2024.1.18 (`AK_WWISESDK_VERSION_*`). Libraries for VS2022 are in `x64_vc170\{Debug,Profile,Release}\lib` | OK | D-031 (proposed) | The manual attempt pointed at `x64_vc160` and an uninstalled 2024.1.2 build, so it did not match. |
| S-028 | 2026-11-25 | Phase 1 | Checked upstream dhewm3 for Wwise hooks: none (no WWISE_ROOT option, no snd_wwise files). Adding a Wwise backend means changing upstream code, which we must not push to upstream, and there is no fork | BLOCKED (decision needed on how to carry code changes, D-031) | D-031 (proposed) | Upstream stays pristine; changes live in our repo. |
| S-029 | 2026-11-25 | Repository | Removed the dhewm3 submodule. Copied the upstream files at commit 505c5c8 into `dhewm3/` as plain files (1551 files, verified identical to the clean upstream checkout). No upstream history is kept. Licence check now covers `dhewm3/` and allows two upstream GLFW libraries | OK | D-032, D-033 | Build paths are unchanged, so the existing build folder still points at dhewm3/neo. |
| S-030 | 2026-11-25 | Dashboard | Dashboard refresh changed to once a minute, paused while the user is active (no input for 20 s is needed before a refresh). A note was added to always refresh the dashboard before continuing work | OK | D-034 | Dashboard refreshes itself; the user can follow along. |
| S-031 | 2026-11-25 | Dashboard | Added hover explanations for every phase step (files and architecture impact, §8.2). Planned Phase 1 and 2 work is documented but not started, pending the user's review of the architecture | OK | D-035 | The user reviews the plan before code changes. |
| S-032 | 2026-11-25 | Phase 1 | User approved the Phase 1 plan (§8.2). Starting P-01.2 to P-01.5 in order | OK | D-036 | Each step is logged and pushed as it finishes. |
| S-033 | 2026-11-25 | Phase 1 | P-01.2: added the optional WWISE_ROOT option to dhewm3/neo/CMakeLists.txt. With it set, WWISE_ENABLED is defined and the SDK include path and libraries (x64_vc170, per configuration) are added. Configure with WWISE_ROOT succeeded. Default build (no WWISE_ROOT) not yet re-built | OK (configure only) | D-036 | Committed with the CMake change. |
| S-034 | 2026-11-25 | Phase 1 | P-01.3 blocked: starting the Wwise engine needs a low-level file I/O hook (batch open, read and write). The SDK's own I/O sample is not in our repo (SDK licence) and writing our own is a large design choice | BLOCKED (decision needed, D-037) | D-037 (proposed) | Waiting on the user's decision. |
| S-035 | 2026-11-25 | Phase 1 | Unblocked by the user: the Wwise SDK supplies its own default file I/O. CMake will compile that from the user's installed SDK when WWISE_ROOT is set, so nothing from the SDK is copied into the repo | OK | D-038 | Installation step recorded in §15.5. |
| S-036 | 2026-11-25 | Phase 1 | P-01.3 and P-01.4 code written and built with WWISE_ROOT (0 errors, links). Files: added dhewm3/neo/sound/snd_wwise.h, snd_wwise.cpp (dhewm3 side, no SDK types) and snd_wwise_sdk.h, snd_wwise_sdk.cpp (only file with SDK headers, no dhewm3 headers, because dhewm3 platform.h redefines vsnprintf). CMake compiles the SDK default I/O (IO hook, dispatcher, file location, multiple-file location) from WWISE_ROOT/SDK/samples. Engine not yet called from the game (P-01.5). | OK (build only; runtime not yet tested) | D-038 | Found while building: snd_local.h needs sys/platform.h first (UsercmdGen.h); SDK headers must be included before platform.h, hence the split. |
| S-037 | 2026-11-25 | Phase 1 | P-01.5: added the s_backend cvar (openal default, or wwise). With wwise, the engine starts in idSoundSystemLocal::Init, OpenAL device creation is skipped as with s_noSound, and the engine stops in Shutdown. If Wwise cannot start, it falls back to OpenAL with a warning. Built (0 errors). Run with +set s_backend wwise for 25 s: no crash (timeout exit). Engine start message not verified: the game writes its console only to the in-game console | BUILT; RUN NO CRASH; START MESSAGE NEEDS USER CHECK | D-039 | User checks the in-game console (~) for 'Wwise 2024.1.18 (build 9194) engine started'. |
| S-038 | 2026-11-25 | Phase 1 | Verified the default OpenAL-only build (no WWISE_ROOT, separate folder build-openal/) compiles and links with 0 errors after the Phase 1 changes | OK | D-039 | Default build is unchanged for players. |
| S-039 | 2026-11-25 | Phase 1 | Read the console without a human check: ran the game with `+set s_backend wwise +conDump wwise_console.txt +quit`. The console dump (in Documents/My Games/dhewm3/base/) shows 'Wwise 2024.1.18 (build 9194) engine started'. Game quit with exit 0 (shutdown ran). No warnings.txt was written | OK | D-039 | Test recipe added to §15.6. |
| S-040 | 2026-11-25 | Phase 1 | Phase 1 steps P-01.3, P-01.4 and P-01.5 marked done from the console dump (S-039) | OK | D-039 | Phase 1 done. Phase 2 is next, pending the user's approval of its plan. |
| S-042 | 2026-11-25 | Dashboard | Decisions on the dashboard show only proposed items. Accepted and superseded decisions stay in §13 | OK | D-041 | The user asked for this. |
| S-043 | 2026-11-25 | Phase 2 | P-02.1 partly done: created the Wwise test project wwise/DhewmTest with WwiseConsole, generated a 1 s test tone with tools/make_test_tone.py (git-ignored .wav), and imported a Sound SFX 'TestTone' with tab-delimited import (columns Object Type, Object Path, Audio File). Creating the Event with a Play action is not allowed by tab-delimited import. Next: create the Event, SoundBank and Init bank through WAAPI (waapi-server) | PARTIAL | D-040 | The import temp files were removed; the project's own files are committed. |
| S-044 | 2026-11-25 | Dashboard | Phase 1 marked done. Phase 2 is the current phase. Phase 3 (route one sound class, weapon fire) is the upcoming phase, with its steps and planned files and architecture (§8.2). Phase 3 code needs the user's approval first | OK | D-042 | Dashboard updated first, as requested. |
| S-045 | 2026-11-25 | Phase 2 | P-02.1 done. Created the event Play_TestTone (Play action on TestTone) and SoundBank TestBank through the Wwise Authoring API (WAAPI) with tools/wwise_test_project.js, then generated the banks. Generated: GeneratedSoundBanks/Windows/Init.bnk and TestBank.bnk (both git-ignored). Tab-delimited import was not enough for events, so WAAPI was used. Generated output is git-ignored | OK | D-043 | Next: P-02.2 (load Init.bnk and TestBank at start). |
| S-046 | 2026-11-25 | Phase 2 | P-02.2: Init.bnk and TestBank.bnk are loaded after the engine starts (base/wwise/Windows/, copied from the generated banks for the test). Stage 5 failures are reported. Console dump shows no bank warning | OK | D-044 | Bank copy into the game folder is manual for now. |
| S-047 | 2026-11-25 | Phase 2 | P-02.3: console command wwise_play <event> posts an event on a debug game object. Tested with +wwise_play Play_TestTone: console dump shows 'posted Play_TestTone (playing ID 1)' and no warning. Files: snd_wwise.cpp (command), snd_wwise_sdk.cpp/.h (PostEvent, debug game object) | OK (posted; audible check is P-02.4) | D-044 | P-02.4 needs the user to listen. |
| S-048 | 2026-11-25 | Phase 2 | P-02.4 attempt: user reports no sound from the test event and no game sound. Game sounds are off by design with s_backend wwise (OpenAL off, Wwise only plays the test event). Added debug output capture (cvar s_wwiseCapture, console wwise_capture_stop, tools/analyze_wav.py) and a Wwise monitor log (wwise_monitor.txt). Built in build2/ because the user's running game locks build/. Result: event posts (playing ID 1), monitor log is empty (no Wwise errors), but no capture file is written, so the render output could not be confirmed | BLOCKED (no audible or captured output yet) | D-044 | Next: check the audio output device settings in the SDK, and check whether the engine renders when a game object plays. |
| S-049 | 2026-11-25 | Phase 2 | Main build (game closed) rebuilt and tested. The capture file is not written. Added wwise_status <playing ID> (GetSourcePlayPosition). Result: wwise_play returns playing ID 1, but wwise_status right after posting already returns AK_PlayingIDNotFound (94), so the event ends at once or never starts. The monitor log stays empty; SetLocalOutput is documented as AK_NotCompatible in optimized builds, and RelWithDebInfo may count as optimized (not yet confirmed) | BLOCKED (event does not play) | D-044 | Next: check the event in a Debug build (monitor messages), and check the media is loaded for the Play action. |
| S-050 | 2026-11-25 | Phase 2 | Debug build (build-debug/, Debug configuration) tested. Monitor works there: a bogus event gives 'Event ID not found' errors in wwise_monitor.txt. Test event Play_TestTone: posted (playing ID 1) with no monitor error, yet the playing ends at once. Capture: stopped, but no file written. Debug run shows a Wwise assertion at shutdown (AkMemoryMgrBase.cpp:363, memory still allocated at MemoryMgr::Term), which the user saw. Next: investigate the shutdown assertion and the render path | BLOCKED (no audible or captured output; Debug shutdown assertion) | D-044 | Needs a decision on the next investigation step. |
| S-051 | 2026-11-25 | Phase 2 | Audio device check. wwise_devices lists 7 output devices (NVIDIA TV and monitors, HDMI, Speakers, Headphones EDIFIER R1280DB). Added s_wwiseDevice (device ID, 0 = default) and passed it to the engine start (settingsMainOutput.idDevice). Tested with the Headphones device: the event still ends at once (AK_PlayingIDNotFound after 6 frames). So the output device is not the cause; the event never becomes an active voice | BLOCKED (event never becomes active; device ruled out as the only cause) | D-044 | Next: check the event's voice creation in the Debug build, e.g. the bank contents and the Master bus, or a second test event. |
| S-052 | 2026-11-25 | Phase 2 | Debug build rerun with the latest code. Event posts (playing ID 1), position check returns AK_PlayingIDNotFound, and the monitor log is still empty. Wwise reports no error for this event, so the cause is not visible from the game side | BLOCKED (no monitor messages; event inactive) | D-044 | Proposed: check the project in Wwise Authoring (Transport Play), to separate project content from the game-side engine code. |
| S-053 | 2026-11-25 | Process | Build folders cleaned: removed build2/, build-openal/ and build-debug/ (about 775 MB). Kept build/ (RelWithDebInfo, the only build in use). Rule: one build folder at a time, unless a debug build is needed for a specific investigation | OK | D-046 | Debug builds are recreated only when needed. |
| S-054 | 2026-11-25 | Phase 2 | Added the Wwise communication module (AK::Comm::Init after the engine starts, Term before it stops) in snd_wwise_sdk.cpp, so Wwise Authoring can connect for remote testing. Built. Game running with s_backend wwise: Wwise engine starts, and UDP 24024 is bound by dhewm3.exe (Authoring discovery port). The TCP connection will be made by Wwise Authoring | OK (listening; Authoring connection not yet tested) | D-044 | User connects Wwise Authoring: Remote Connect to 127.0.0.1 (or the game listed). |
| S-057 | 2026-11-25 | Phase 2 | User confirmed the test event plays in Wwise Authoring (project is fine). Engine side: Audio Devices shows no device (ID 4294967295), with or without an explicit Realtek headphones device. Added CoInitializeEx before engine start (Windows audio sinks need COM). Still no voice. Web search: community reports match 'Output Device could not be initialized. Reverting to Dummy Audio Device (no output)' as the cause of silent Wwise games (Audiokinetic community Q&A, not official SDK docs). Official SDK reference: AK::SoundEngine::Init / AkOutputSettings (SDK Help: WwiseSDK-Windows.chm) | BLOCKED (engine falls back to a dummy output on this machine) | D-044 | Next: get the device-open error from Wwise (monitor/debug log), or test the Init settings in a Debug build. |
| S-058 | 2026-11-25 | Phase 2 | Wwise monitor messages are now forwarded to dhewm3's console (Printf) and warnings (Warning), via WwiseSDK::SetLogFunc. Build OK. Run with the test event: 'Wwise engine started' and the test post appear in the console; no Wwise device message appears in this RelWithDebInfo build. The device fallback message is therefore not posted to the monitor in this build (Debug build earlier did report monitor messages for a wrong event name) | OK (plumbing); device cause still unknown | D-044 | Next: a temporary Debug build to read the device-open message. |
| S-059 | 2026-11-25 | Phase 2 | Temporary Debug build (build-debug/) compiled with 0 errors. Run with the test event: console shows engine start and the post; the Wwise monitor file is empty. So the device-open failure is not reported by Wwise in either build. Debug build folder deleted | BLOCKED (device failure not visible in Wwise messages) | D-044 | Next: check the device state from the engine (GetOutputID, device list) and the Windows audio endpoint, or ask Audiokinetic support with the logs. |
| S-060 | 2026-11-25 | Phase 2 | Engine output IDs: main output was 0 (none opened). Added wwise_addoutput (AK::SoundEngine::AddOutput on the System shareset): returns AK_Success and a valid output ID, so the output can be opened explicitly. Posting the test event after that still ends at once (AK_PlayingIDNotFound). Next suspect: the engine does not have the project's Master bus (from Init.bnk). OpenAL is not opened in wwise mode, so a direct OpenAL conflict is unlikely; a test with OpenAL32.dll removed would confirm | BLOCKED (output opens; event still inactive) | D-044 | Next: check the Master bus is loaded in the engine. |
| S-061 | 2026-11-25 | Phase 2 | Re-assessment of the Wwise code against the SDK headers. Found: (1) AK::SoundEngine::RenderAudio is never called; the header says it processes the command queue and 'has to be called periodically (usually once per game frame)'. (2) No listener is registered or set as default, so the debug emitter has no listener. (3) GetSourcePlayPosition needs AK_EnableGetSourcePlayPosition on PostEvent, which we never passed, so the 'ends at once' result in S-049 to S-060 was a test artefact. The device tests (S-051, S-057, S-059, S-060) looked in the wrong place. User approved fixing these (option A) and a clean sound backend interface (option B, new Phase 3; weapon fire becomes Phase 4) | OK (analysis; no code yet) | D-047, D-048 | Next: P-02.4. |
| S-062 | 2026-11-25 | Phase 2 | P-02.4: added WwiseSDK::RenderAudio and idWwiseEngine::Update, called at the start of idSoundSystemLocal::AsyncUpdate and AsyncUpdateWrite (before the OpenAL isInitialized check, which stays false in Wwise mode). Registered listener game object 2 and set it as the default listener. Debug PostEvent passes AK_EnableGetSourcePlayPosition. Shutdown clears the initialized flag before stopping the engine. Built (0 errors). Run with s_wwiseCapture and +wwise_play Play_TestTone: wwise_status reports position 70 ms (AK_Success), and the captured .wav (2 ch, 48 kHz) has the tone (peak 0.28, rms 0.20; tools/analyze_wav.py: SOUND PRESENT). First capture file ever written | OK (engine renders the event; audible check by the user is P-02.5) | D-047 | Confirms S-061: the missing RenderAudio call was the cause. |
| S-063 | 2026-11-25 | Phase 3 | P-03.1: wrote the backend interface design (§8.3). Interface = sound.h itself; a facade behind the soundSystem pointer picks OpenAL or Wwise once at Init. Checked: the game DLL copies soundSystem through gameImport_t (pointer must not change, so a facade); sound shaders reach soundSystemLocal.soundCache directly (NULL in Wwise mode, so no sample loading); Dhewm3SettingsMenu.cpp reads soundSystemLocal for the OpenAL device menu | OK (design only; waiting for approval) | D-048, D-049 (proposed) | No Phase 3 code until approved. |
| S-064 | 2026-11-25 | Phase 3 | Design review: the user rejected the name 'facade' (a facade simplifies a subsystem; this object only forwarded calls). Two service-provider options offered: (1) a true locator that points soundSystem at the chosen provider at start-up, one changed line in framework/Common.cpp; (2) a forwarding proxy. User chose option 1. §8.3, §8.2 and D-049 updated. Dashboard: the refresh line now counts down the seconds to the next refresh | OK | D-049 | Next: P-03.2. |
| S-065 | 2026-11-25 | Phase 3 | P-03.2 and P-03.3 written. Added snd_locator.h/.cpp (idSoundSystemLocator; owns the s_backend cvar). framework/Common.cpp: soundSystem->Init() replaced by idSoundSystemLocator::Init() (one line plus include). snd_system.cpp and snd_local.h back to upstream (diff against the import 018e040 is empty). snd_wwise.h/.cpp now idSoundSystemWwise (engine, RenderAudio in AsyncUpdate/AsyncUpdateWrite, debug commands). Added snd_wwise_world.cpp (idSoundWorldWwise: listener, emitters, savegame in the OpenAL layout) and snd_wwise_emitter.cpp (idSoundEmitterWwise: one game object per emitter, ID world*0x100000+index, position in meters, StopSound). New cvar s_wwiseShowStartSound. Removed the investigation-only wwise_output and wwise_addoutput commands. snd_wwise_sdk: RegisterObject, UnregisterObject, SetObjectPosition, SetListenerPosition, StopObject, StopPlayingID, StopAll. CMake: snd_locator.cpp always built; the three Wwise provider files only with WWISE_ROOT. Build: 0 errors, first try | OK (build) | D-049, D-050 | Stubs for features without a Wwise equivalent are listed in §7. |
| S-066 | 2026-11-25 | Phase 3 | P-03.4 test, three runs read from console dumps. (A) s_backend wwise: 'Sound system provider: wwise', test event posted, mars_city1 loads, 118 StartSound calls reach Wwise emitters (36 shaders, 117 emitters), saveGame and loadGame work, capture has the test tone (SOUND PRESENT). (B) s_backend openal: 'Sound system provider: openal', OpenAL starts as before, loads the save made under Wwise, saves again. (C) s_backend wwise: loads the save made under OpenAL. All runs exit 0. One OpenAL warning 'Couldn't load sound guisounds.wav' comes from the untouched OpenAL path | OK | D-049 | Phase 3 done. Phase 4 plan needs review. P-02.5 (user listens) still open. |
| S-055 | 2026-11-25 | Phase 2 | Remote connection works: Wwise Authoring (2024.1.18 v23) connected to the game. The game posts Play_TestTone (playing IDs 1 and 2), but the Advanced Profiler shows no voices, and the Capture Log shows no play messages. Project files checked: event Play action targets TestTone, which has AudioFileSource test_tone.wav, and Originals/SFX/test_tone.wav exists | BLOCKED (events are accepted but create no voices) | D-044 | Next: check Audio Devices and SoundBanks tabs in the profiler, and press Play in Authoring. |
| S-041 | 2026-11-25 | Phase 2 | User approved the Phase 2 plan (§8.2, P-02.1 to P-02.4) with 'go'. Starting P-02.1 | OK | D-040 | Phase 2 work follows §8.2. |
| S-012 | 2026-11-25 | Dashboard | Replaced the main-goal banner with the current next step (N-01, §12.0). Added `next` to the sync script | OK (not yet tested in browser) | D-014 | Main goal stays in §1 of the notes. |
| S-067 | 2026-11-25 | Phase 4 | Board clean-up after user feedback. Phase 2 marked done: its last step is now 'test event rendered, checked in the output capture' (S-062, S-066); the user's listening check moves to P-04.4. Phase 4 is current, Phase 5 (remaining sounds) added as upcoming, and the phase overview in §8 renumbered (backend interface is Phase 3). Found that S-061 to S-066 had been inserted above S-055, S-041 and S-012, so the dashboard's 'last 7' showed old steps; the page now sorts the step log by step number, and new rows are appended at the end. Step log rows on the dashboard now use two lines: date, phase and result on the first, the action on the second | OK | D-051 | Dashboard checked in the local preview. |
| S-068 | 2026-11-25 | Phase 4 | P-04.1: wrote the shader-to-event design (§8.4). Checked: weapon fire comes from frame commands in the weapon animations (def/weapon_*.def, read locally from the pk4 files, nothing copied), with shader names such as player_pistol_fire and channels that differ per weapon; wwise_play with an unknown event (Play_player_pistol_fire) fails at once with two Wwise warnings; Wwise writes <bank>.json bank metadata with event names and estimated durations; the OpenAL emitter's channel rules; GetPlayingIDsFromGameObject in the SDK. Open choices added as Q-07 (naming rule plus bank metadata, or a mapping file) and Q-08 (local original samples, or generated tones) | OK (design only; waiting for review) | D-052 (proposed) | No Phase 4 code until approved. |
| S-069 | 2026-11-25 | Phase 4 | User answers: Q-07 naming rule without an existence check (missing events may warn in the console; keep wwise_missing); Q-08 extract the original weapon-fire samples. User question: is emitter behaviour event setup rather than code? Answer: mostly yes. Random choice, play-once, double-start guard, looping and limits move to the Wwise event setup; code keeps only the channel contract of sound.h (StopSound per channel, replace on the same channel, CurrentlyPlaying), because Wwise does not know dhewm3 channels. §8.4 revised (no bank metadata reader, no snd_wwise_events.cpp) | OK (design revised; waiting for approval) | D-052 | Next: approval, then P-04.2. |
| S-070 | 2026-11-25 | Phase 4 | User asked whether channels relate to audio buses. Answer recorded: a channel is a per-emitter slot defined by the game (voice, body, weapon, item), used to stop or replace what one entity plays; it has no role in mixing. The bus-like concept is the shader's soundClass (FadeSoundClasses), which maps to Wwise buses in the project (§7, Phase 5). User approved the revised §8.4 ('let's go'). Starting P-04.2 | OK | D-052 | |
| S-071 | 2026-11-25 | Phase 4 | P-04.2 content: added tools/extract_doom_sounds.py (reads the pk4 files read-only; finds 11 weapon-fire shaders in def/weapon_*.def; extracts their samples to the git-ignored deps/doom3-sounds/, converting the rocket launcher's .ogg files with ffmpeg; writes weapon_fire.json) and tools/wwise_weapon_fire.js (WAAPI: attenuation Doom_Weapon, actor-mixer WeaponFire, one random container and one Play_<shader> event per shader, playback limit for play_once shaders, bank WeaponFire). Ran both: 11 shaders, 0 problems; WeaponFire.bnk with 11 events and 29 media files, deployed to build/RelWithDebInfo/base/wwise/Windows. WAAPI server stopped afterwards | OK | D-052 | Recipe in §15.7. Only the .wwu files are committed, no audio. |
| S-072 | 2026-11-25 | Phase 4 | P-04.2/P-04.3 code: StartSound posts Play_<shader> on the emitter's game object; missing events are listed by the new wwise_missing; playing IDs are kept per channel (replace on the same channel, StopSound per channel, CurrentlyPlaying) and pruned with GetPlayingIDsFromGameObject (posts younger than 500 ms are kept). Added the test command wwise_startsound and the cvar s_wwiseShowStartSound. Build: 0 errors. Test p44 (mars_city1, s_backend wwise, capture): both banks loaded; pistol, shotgun and machinegun fire posted on channel 6; capture SOUND PRESENT (peak 0.70); wwise_missing lists 80 shaders; 231 'Event ID not found' warnings, accepted (Q-07). Test p44b: the emitter reports playing 388 ms after a start and silent after 995 ms, so pruning works. Frames ran at about 460 fps, so +wait counts are not real time | OK (capture checked; not yet listened to) | D-052 | Next: P-04.4, the user's listening test. |
| S-073 | 2026-11-25 | Phase 4 | P-04.4: user ran the listening test with s_backend wwise: 'All sound works!'. Phase 4 done; D-050 (meters) accepted on this test. User question: drop the Play_ prefix, so events are not all named Play_...? Recorded as Q-09 | OK (user listened) | D-050, D-052 | Phase 5 is current. |
| S-074 | 2026-11-25 | Phase 5 | User answered Q-09: keep the Play_ prefix. D-052 unchanged; no code change | OK | D-052 | Next: P-05.1 design. |

**Next step:** see N-01 in §12.0.

### 12.2 Attempts not yet made
- Phase 0 build: not attempted.
- Wwise SDK check: not attempted (SDK path unknown, §11 Q1).

## 13. Decisions

| ID | Date | Category | Decision | Reason | Status | Revisit when |
|---|---|---|---|---|---|---|
| D-001 | 2026-11-25 | Repository & build | Clone upstream into sub-folder `dhewm3/`, not the workspace root | Keeps upstream history separate from our own files, so both can be in one repo later | Accepted (proposed by assistant, not yet confirmed by user) | Repo layout decision (§11 Q4) |
| D-002 | 2026-11-25 | Repository & build | Keep the upstream clone unmodified until Phase 1 | Phase 0 needs a clean baseline build | Accepted | Phase 1 start |
| D-003 | 2026-11-25 | Repository & build | Keep CMake as the primary build; add a Makefile wrapper only if asked | Upstream uses CMake; no top-level Makefile exists | Proposed | §11 Q2 |
| D-004 | 2026-11-25 | Audio design | Replace the back end under `sound.h`, not the game call sites | Keeps about 200 `StartSound` call sites unchanged | Proposed | Phase 3 review |
| D-005 | 2026-11-25 | Repository & build | Do not publish to any remote yet | Outward-facing; needs user approval of host and visibility | Superseded by D-010 | §11 Q4 answered |
| D-006 | 2026-11-25 | Licensing & scope | Doom 3 game data and Wwise SDK never committed | Licensing (§2) | Accepted | Never (unless licence changes) |
| D-010 | 2026-11-25 | Repository & build | Publish to `https://github.com/Dreezn/dhewm3_Wwise.git` (user's repo) | User asked to host the project there (S-005/S-006). Replaces D-005, which had a duplicate ID | Accepted | Never, unless the repo or visibility changes |
| D-007 | 2026-11-25 | Repository & build | `dhewm3/` is a git submodule pinned to `505c5c8`, not vendored | Keeps upstream history and a clear pin; can be re-pinned by a logged step | Accepted | Phase 1 (when we start editing upstream code, we may need a fork) |
| D-008 | 2026-11-25 | Dashboard & process | Project dashboard hosted as an Artifact page; questions and answers stored in the page database | User wants to follow progress and direct the project from a browser | Accepted | Dashboard review |
| D-009 | 2026-11-25 | Dashboard & process | No GitHub Pages site: dashboard stays a Claude artifact | User chose this over Pages. Repo is private (Pages needs a paid plan on private repos), and a static Pages copy would lose comments and Q&A | Accepted | Repo made public, or Pages needed |
| D-011 | 2026-11-25 | Dashboard & process | This file is the source of truth. Dashboard data is generated from it by `tools/sync_dashboard.py` | Avoids two copies drifting apart | Accepted | Dashboard tooling changes |
| D-012 | 2026-11-25 | Dashboard & process | Answers are typed on the dashboard, saved to `data/project/answers/<Q-id>`, and copied into §11 by the assistant when received | User answers alone; the notes stay the record | Accepted | Other people need to answer |
| D-015 | 2026-11-25 | Dashboard & process | Proceed control removed from the dashboard. The dashboard is for review only; the user gives direction in chat. D-013 is superseded | Pressing Proceed left the desktop app waiting ("no other sessions are waiting for you") and the write did not reach the assistant | Accepted | Dashboard gets a reliable direction channel |
| D-016 | 2026-11-25 | Licensing & scope | Doom 3 original audio is never uploaded to GitHub. It stays on local disk. Students get a template Wwise project we make ourselves | Answer to Q-05. Licensing (section 2) | Accepted | Never, unless the licence changes |
| D-017 | 2026-11-25 | Repository & build | Build with CMake only. No Makefile wrapper unless asked | Answer to Q-02. Upstream has no top-level Makefile | Proposed, pending user confirmation | Q-02 confirmed or rejected |
| D-018 | 2026-11-25 | Repository & build | Windows is the only target for now | Answer to Q-06 | Accepted | Phase 6 or when a second platform is requested |
| D-019 | 2026-11-25 | Audio design | Wwise 2024.1.18.9194 is at `C:\Audiokinetic\Wwise2024.1.18.9194`. The Authoring project lives in this repo | Answer to Q-01 | Accepted | Wwise upgrade |
| D-020 | 2026-11-25 | Dashboard & process | The dashboard lists only open questions. Answered questions are removed from the dashboard and recorded as decisions | User request. Decisions record the outcome | Accepted | Question list format changes |
| D-021 | 2026-11-25 | Repository & build | Use the classic Doom 3 Steam folder (`...\common\Doom 3`) as the game data source. DOOM 3 BFG Edition is not supported by dhewm3 | Upstream README states BFG is not supported. Answer to Q-03 | Accepted | Q-03 decided |
| D-022 | 2026-11-25 | Repository & build | Fork upstream to Dreezn/dhewm3 and point the submodule at the fork before Phase 1 | Phase 1 edits upstream code, and upstream is read-only for us. Answer to Q-04 | Superseded by D-024 | Q-04 decided |
| D-023 | 2026-11-25 | Dashboard & process | Summary dashboard is a static page in docs/, published with GitHub Pages, read-only. It replaces the Claude artifact dashboard | User request. Pages is free on a public repo, and the artifact dashboard could not be used for direction | Accepted | Pages policy changes |
| D-024 | 2026-11-25 | Repository & build | Repository made public. It keeps its link to upstream dhewm3 through the submodule, for reading upstream changes. No GitHub fork is created unless asked | User wants it public and linked as a fork. GitHub cannot turn an existing repo into a fork | Superseded by D-032 | User asks for a real GitHub fork |
| D-025 | 2026-11-25 | Repository & build | Never make a formal pull request to upstream dhewm3, and never push to it | User rule. Project must not affect upstream | Accepted | Never, unless the user changes it |
| D-026 | 2026-11-25 | Repository & build | Phase 0 uses the dhewm3-libs `x86_64-w64-mingw32` folder (commit 57c5659) as DHEWM3LIBS, as in the upstream README and the manual attempt. Whether its .lib files link with MSVC is not yet verified | Matches the manual attempt, which produced Debug/dhewm3.exe. Avoids a vcpkg download | Proposed, to verify in Phase 0 | Link fails, or user approves vcpkg |
| D-027 | 2026-11-25 | Audio design | Do not copy code or files from `dhewm3-1.5.4`. Its Wwise backend files are fully commented out and their origin is unknown. Write our own Wwise backend in this repo | Origin of that tree is unknown, so its files cannot be documented. Requirement: every file we use is documented | Accepted | Origin of the tree is confirmed |
| D-028 | 2026-11-25 | Repository & build | Prebuilt libraries stay local in `deps/` (git-ignored) and are never committed. They are copied from dhewm3-libs at commit 57c5659, so anyone can reproduce them | Licensing and the user's request. The libraries are third-party binaries | Accepted | Libraries are cleared for upload |
| D-029 | 2026-11-25 | Licensing & scope | A check runs locally and in CI that fails on any game archive, audio, Wwise bank, Originals folder, binary library or `deps/` file, in tracked files or in history | Ensures no licensed or illegal files go online | Accepted | Rules change |
| D-030 | 2026-11-25 | Dashboard & process | The dashboard shows the current and upcoming phase with their steps (done green, blocked red, todo grey). The open-questions section is hidden when empty. The main goal is not shown | User request to follow phase progress | Accepted | Dashboard layout changes |
| D-031 | 2026-11-25 | Repository & build | Carry our Wwise code changes as a patch series in `patches/` in this repo, applied to the dhewm3 submodule at build time with `git apply`. The submodule stays at upstream commit 505c5c8 and is never committed to | Upstream is read-only for us (D-025), and we have no GitHub fork (D-024). Patches keep upstream untouched and the work reproducible | Superseded by D-032 | User chooses a GitHub fork instead |
| D-032 | 2026-11-25 | Repository & build | dhewm3 is a plain copy in this repo at upstream commit 505c5c8, without upstream history, and the submodule is removed. Our changes are normal commits. This supersedes D-024 and D-031 | No need for upstream history. We keep full control of the project and have no link to the original repo | Accepted | We want upstream fixes (re-import by hand) |
| D-033 | 2026-11-25 | Licensing & scope | Two upstream GLFW libraries (`neo/libs/imgui/examples/libs/glfw/lib-vc2010-32/glfw3.lib` and `-64`) are allowed in the repo. They come with dhewm3 under its own licence, and all other binaries stay forbidden | Needed to keep the copied source complete. Reviewed in this turn | Accepted | Upstream changes its binaries |
| D-034 | 2026-11-25 | Dashboard & process | Summary page refreshes every minute, but only after 20 s without input, so it does not disrupt the reader. The dashboard is always refreshed before the next piece of work | User request | Accepted | Refresh rate changes |
| D-035 | 2026-11-25 | Process | Each planned phase step lists the files and architecture it affects. No code for a phase starts until the user has reviewed those effects | The user wants to supervise architecture decisions and not leave them to the assistant | Accepted | User approves a phase |
| D-036 | 2026-11-25 | Process | Phase 1 approved by the user. Work follows the §8.2 plan; any deviation from that plan is logged and needs approval first | User approval | Accepted | Plan changes |
| D-037 | 2026-11-25 | Audio design | Our Wwise backend needs its own blocking file I/O layer (IAkLowLevelIOHook and IAkFileLocationResolver, with batch open, read and write implemented sequentially). We write it ourselves, in neo/sound/snd_wwise_io.cpp, and do not copy the SDK sample. The SDK sample I/O is not used | The SDK samples are not ours to copy. A own layer keeps licences clear, but it is a large piece of architecture. Needs approval | Proposed, pending user decision | User chooses another route |
| D-038 | 2026-11-25 | Audio design | Use the Wwise SDK's own default file I/O (samples under WWISE_ROOT/SDK/samples/SoundEngine). CMake compiles it from the user's installed SDK when WWISE_ROOT is set. We do not write or copy our own I/O layer, which supersedes D-037 | The user's direction: Wwise handles file loading, use it as intended. Nothing from the SDK is copied into the repo | Accepted | SDK version changes |
| D-039 | 2026-11-25 | Architecture | The Wwise SDK headers are only included by snd_wwise_sdk.cpp. That file includes no dhewm3 headers. dhewm3's platform.h redefines vsnprintf (and BIT), which conflicts with the SDK headers. snd_wwise.cpp exposes only plain types | Build-time conflict found in S-036. Keeps the game code free of SDK types | Accepted | SDK or dhewm3 header changes |
| D-040 | 2026-11-25 | Process | Phase 2 approved by the user with 'go' (§8.2). Same rule as D-036: deviations need approval first | User approval | Accepted | Plan changes |
| D-041 | 2026-11-25 | Dashboard & process | The dashboard lists only proposed decisions. Accepted and superseded decisions remain in the notes (§13) | User request. Logs hold the full record | Accepted | Dashboard layout changes |
| D-042 | 2026-11-25 | Process | Phase status: Phase 0 and Phase 1 done, Phase 2 current, Phase 3 upcoming. Phase 3 code waits for the user's approval of its §8.2 plan | User direction | Accepted | Phase changes |
| D-043 | 2026-11-25 | Audio design | Wwise test content is built through the Wwise Authoring API (WAAPI, WwiseConsole waapi-server) by tools/wwise_test_project.js. Tab-delimited import cannot create events | Tab-delimited import rejects events. WAAPI is the supported scripting route | Accepted | Wwise version changes |
| D-044 | 2026-11-25 | Audio design | Phase 2 uses the SDK default I/O to load banks from base/wwise/Windows/ and a debug console command wwise_play for events. Banks are copied from the generated folder for the test; a build step for this is a later task | Needed for the test. Keeps the game code path to one debug command | Accepted | Phase 3 replaces the debug command with the game route |
| D-046 | 2026-11-25 | Process | Keep one build folder (build/, RelWithDebInfo). Other builds (Debug, OpenAL-only) are created on demand and deleted after use | Disk space and clarity; the user asked for it | Accepted | A debug investigation needs a second build |
| D-047 | 2026-11-25 | Architecture | Wwise gets the standard per-frame calls: RenderAudio once per sound tick, one listener game object set as the default listener, and the debug PostEvent asks for play-position tracking. For now this is called from idSoundSystemLocal::AsyncUpdate / AsyncUpdateWrite (dhewm3's existing sound tick) | Required by the SDK (S-061). User approved option A | Accepted | Phase 3 moves the call behind the backend interface |
| D-048 | 2026-11-25 | Architecture | OpenAL and Wwise go behind one sound backend interface (new Phase 3), even though OpenAL may be removed later. The interface design is written in these notes and approved by the user before any code. Weapon-fire routing moves to Phase 4 | User asked for a clean implementation (option B) | Accepted (design pending review) | Design reviewed |
| D-049 | 2026-11-25 | Architecture | Backend interface is sound.h (idSoundSystem, idSoundWorld, idSoundEmitter), one full provider per backend. A service locator (idSoundSystemLocator) picks the provider once at start-up and points the soundSystem pointer at it (option 1). The forwarding facade/proxy (option 2) was rejected. Features without a Wwise equivalent get neutral stubs and go to the triage list. Design in §8.3 | Textbook service-provider pattern; less code and no forwarding layer to maintain. Costs one changed line in framework/Common.cpp. Game DLL copies the pointer only after sound start-up | Accepted (user chose option 1) | Backend must be switchable without a restart |
| D-050 | 2026-11-25 | Audio design | Positions are sent to Wwise in meters (DOOM_TO_METERS), axes converted to Wwise (X right, Y up, Z forward). Doom 3 sound shaders already give minDistance and maxDistance in meters, so Wwise attenuation curves use the same numbers | Matches the shader data and the usual unit in Wwise projects; students author attenuation in meters | Accepted (S-073: Phase 4 listening test passed) | Phase 4 listening test |
| D-051 | 2026-11-25 | Dashboard & process | Dashboard step log is sorted by step number and shows each step on two lines (date, phase and result; then the action). Phase 2 counts as done on the output capture; the user's listening check is part of P-04.4 | User feedback: the board still showed Phase 2 and was hard to read | Accepted | Dashboard layout changes |
| D-052 | 2026-11-25 | Audio design | A sound shader plays the Wwise event named Play_<shader> (non-alphanumeric characters become _). Every StartSound posts its event; a missing event gives Wwise's console warning, and wwise_missing lists the shaders whose post failed. Code keeps only the sound.h channel contract (stop a channel, a new sound replaces the old one on its channel, CurrentlyPlaying); random choice, play-once, limits and looping are Wwise event setup. Phase 4 test content uses the original weapon-fire samples, extracted locally and never committed. Design in §8.4 (revision S-069) | User answers to Q-07 and Q-08: missing-event warnings are normal game behaviour; sound behaviour belongs to the sound designer in Wwise. Students add events without code or data changes | Accepted (Q-07, Q-08; revision S-069 approved in S-070) | Phase 4 listening test |
| D-014 | 2026-11-25 | Dashboard & process | Dashboard banner shows the current next step (N-01, §12.0) instead of the main goal. The main goal stays in §1 | User asked for the next step to be shown | Accepted | Banner content changes |
| D-013 | 2026-11-25 | Dashboard & process | Dashboard shows the current main goal (G-01, §1) at the top, and a Proceed control that unlocks when every question is answered and the user confirms the state is reviewed | User asked to direct the next step from the dashboard | Accepted | Proceed gate changes |

## 14. Keeping the summary dashboard in sync

The summary dashboard is a static page in `docs/`, published with GitHub Pages from the `main` branch. It is read-only. It loads `docs/dashboard-data.json`, which is generated from this file.

Single source of truth: this file. Do not edit `docs/dashboard-data.json` by hand.

1. Edit this file: the next step (§12.0), a step (§12.1), a decision (§13), a question (§11) or the goal (§1).
2. Regenerate: `python tools/sync_dashboard.py`. This writes `docs/dashboard-data.json`. Only open questions are exported.
3. Commit and push. GitHub Pages republishes the dashboard from the pushed files. The CI workflow `dashboard-sync` fails if the JSON does not match this file (run `python tools/sync_dashboard.py --check` locally to see it).
4. Answers to open questions are given in chat. The assistant copies them into the Answer column in §11, regenerates the data, and pushes.

Rules: IDs are never reused. A duplicate ID is an error in the sync script. A superseded item keeps its row and gets status "Superseded by <ID>".


## 14.1 Dashboard rules

- Every attempt (configure, build, run, test) gets a step row in §12.1, and the dashboard is regenerated and pushed in the same turn.
- Answered questions are removed from the dashboard: only questions whose status starts with "Open" are shown. The answer stays in §11.
- Dashboard items about the dashboard itself are hidden: step phases Dashboard, Hosting, Sync and Process, and decisions in the category "Dashboard & process".
- The dashboard is always refreshed before the next piece of work, so the user can follow along.
- The summary page reloads its data every minute, but only after 20 seconds without input.
- The dashboard shows no IDs. Step results are colour-coded: OK (green), BLOCKED or FAILED (red), anything else (amber). Selecting a step shows its linked decisions.

## 15. Build environment and provenance

Every file used in the build must have a known origin, so the build can be replicated from these notes.

### 15.1 Manual attempt (`C:\Workspaces\Vienne\SoloProjects\dhewm3-1.5.4`)

Read-only review. Nothing from this tree is copied into the repo.

- **Source:** a copy of dhewm3 dated 2025, with a `dhewm3-1.5.4.zip` beside it. Origin of the copy is not known. Its `neo/sound/snd_wwise.*` files are entirely commented out.
- **Configure command (from `LOCAL_CMAKE_BUILD.bat`):** `cmake -S neo -G "Visual Studio 17 2022" -A x64 -DDHEWM3LIBS=<tree>\dhewm3-libs -DWWISE_ROOT="C:\Program Files (x86)\Audiokinetic\Wwise2024.1.2.8726"`.
- **Wwise path:** `Wwise2024.1.2.8726` is not installed on this machine. Installed Wwise folders are `C:\Audiokinetic\Wwise2024.1.18.9194` (the project's version), `C:\Program Files (x86)\Audiokinetic\Wwise2025.1.4.9062` and `Wwise2019.1.11.7296`.
- **Build output:** `Debug\dhewm3.exe` exists. It contains no Wwise strings, so it has no Wwise backend.
- **`dhewm3-libs` folder:** hand-copied and flattened (`bin`, `include`, `lib`, `share` at the top). Matches the x86_64 MinGW folder of `dhewm/dhewm3-libs`, commit `57c5659` (2025-10-11), byte for byte. The `.lib` files are MinGW-format archives. The manual `lib` folder also has `.def` files that are not in the libs repo, so the user probably generated `.lib` files from them. This was not confirmed.

### 15.2 Versions and hashes (dhewm3-libs, commit 57c5659)

| File | SHA-256 (first 16 chars) |
|---|---|
| bin/SDL2.dll (SDL 2.32.6) | ab4af8e2e92d0582 |
| bin/OpenAL32.dll (OpenAL Soft 1.23.1) | 4e2d830d2a3aa85b |
| bin/libcurl-4.dll (libcurl 8.9.1) | e1ec73f4a000cd98 |
| lib/SDL2.lib | 6e40d52f515f2283 |
| lib/OpenAL32.lib | 534e8c9733cf64c3 |
| lib/libcurl.lib | a0d07379df23e226 |

### 15.3 Local copy

The libraries are copied to `deps/dhewm3-libs/` (git-ignored, see D-028). The configure step should point at `deps\dhewm3-libs_64-w64-mingw32`. Run `python tools/check_licensed_files.py` before every push.

### 15.5 Installation step for Wwise (for everyone building with Wwise)

1. Install Wwise 2024.1.18 (build 9194) with the SDK, through the Audiokinetic installer.
2. Configure the build with the install root: `-DWWISE_ROOT="C:/Audiokinetic/Wwise2024.1.18.9194"`.
3. Nothing from the SDK is copied into this repo. The build reads the headers, libraries and default I/O sample code from the installed SDK.

### 15.6 Automated test recipe (no human check needed)

Run the game with commands that write the console to a file and then quit:

`dhewm3.exe +set fs_basepath "<Steam>/common/Doom 3" +set s_backend wwise +conDump wwise_console.txt +quit`

The dump is written to `%USERPROFILE%\Documents\My Games\dhewm3\base\wwise_console.txt`. Warnings are also written to `warnings.txt` in the same folder. Read these files to check start-up messages.

More test commands (Wwise provider): `+set s_wwiseCapture <file.wav>` and `+wwise_capture_stop` write the Wwise output to a .wav file (check it with `py tools/analyze_wav.py <file>`); `+wwise_startsound <shader> [channel]` plays a sound shader through a Wwise emitter at the listener, and `+wwise_startsound` alone reports whether that emitter still plays; `+wwise_missing` lists the shaders that had no event. `+wait N` waits N frames; frames can run far faster than 60 per second while nothing is rendered, so check timings in ms (S-072).

### 15.7 Building the Wwise test content (Phase 4)

Needs the user's own Doom 3 data, Node 22 or newer, and ffmpeg on PATH for the samples that only exist as .ogg (the rocket launcher). Nothing from Doom 3 is committed: the samples go to `deps/` and the Wwise project's `Originals/`, both git-ignored.

1. `py tools/extract_doom_sounds.py` reads the weapon defs and sound shaders from the .pk4 files, extracts the weapon-fire samples to `deps/doom3-sounds/`, and writes `deps/doom3-sounds/weapon_fire.json`.
2. Start the WAAPI server: `WwiseConsole.exe waapi-server wwise/DhewmTest/DhewmTest.wproj` (WwiseConsole.exe is in the Wwise install, under `Authoring/x64/Release/bin`). Wwise Authoring must not have the project open.
3. `node tools/wwise_weapon_fire.js --deploy build/RelWithDebInfo/base/wwise/Windows` builds the attenuation, the WeaponFire actor-mixer, one event per shader and the WeaponFire bank, generates the banks, saves the project and copies the .bnk files next to the game.
4. Stop the WAAPI server.

### 15.4 Replicating Phase 0 (planned, not yet verified)

1. Clone `https://github.com/dhewm/dhewm3-libs` and check out commit `57c5659`.
2. Clone this repo with its submodule: `git clone --recurse-submodules https://github.com/Dreezn/dhewm3_Wwise`.
3. Configure: `cmake -S dhewm3/neo -B build -G "Visual Studio 17 2022" -A x64 -DDHEWM3LIBS=<dhewm3-libs>/x86_64-w64-mingw32`.
4. Build the Debug or RelWithDebInfo configuration with MSVC.
5. Run and check (Phase 0 test, see §8).

Step 3 is not verified. The manual attempt used the flattened folder, so it may differ.
