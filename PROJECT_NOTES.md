# dhewm3 → Wwise Port: Goals, Plan & Progress

Status: **Planning / review draft** (no code changed yet)

---

## 1. Goal

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
| Save/load of sound state | `WriteToSaveGame` | Re-post Events/RTPCs on load | Port (later) |
| Demo recording of sound | `StartWritingDemo` | Drop initially | Drop |

## 8. Phases (each one testable)

| # | Phase | Done when |
|---|---|---|
| 0 | Clone upstream, build unmodified dhewm3 on Windows with its CMake, play a map with sound | Baseline runs and plays audio with OpenAL |
| 1 | Add Wwise SDK to the build (`WWISE_SDK_ROOT`), init/shutdown the sound engine, no playback yet | Game starts, Wwise initializes, logs version 2024.1.18.9194 |
| 2 | Load `Init.bnk` + one test bank built by us; play one Event from a debug console command | Test event audible |
| 3 | Route `StartSound` through the bridge for **one** sound class (e.g. weapon fire); keep OpenAL disabled for it | Sound plays from the right position and stops with the entity |
| 4 | Port the remaining `StartSound` paths, distances, loops, random, fades; RTPCs for slowmo/volume | Map plays with no OpenAL calls left in game sound path |
| 5 | Music/cinematics (`AVI`, `idSoundWorld` streams), save/load | Campaign start-to-finish playable |
| 6 | Remove OpenAL from the build (or keep as optional fallback), remove `tools/sound` | Clean build with Wwise only |
| 7 | Automation: converter script `.sndshd` → Events/RTPC list + mapping table; test banks for students | Students can add an Event and hear it in game |

Each phase gets its own test checklist (sections below). I'll not move to the next phase until the previous one is validated with you.

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

1. **Wwise SDK path** on this machine, and whether the Wwise Authoring project lives in this repo or somewhere else.
2. **Makefile:** a top-level GNU `Makefile` that wraps CMake, or Windows `build.bat`/`nmake`?
3. **Doom 3 data path:** confirm `C:\Program Files (x86)\Steam\steamapps\common\...` folder name (Doom 3 is under `common\Doom 3`?).
4. **Repo:** GitHub/GitLab? Public or private? Fork of dhewm3 or submodule?
5. **Student audio content:** do students get the original Doom 3 wav/ogg files, or only a generated Wwise project with placeholder audio?
6. **Platforms:** Windows only for now, or also Linux/macOS?

## 12. Progress log

Rules for this log:
- One row per step or attempt, in order. Never delete or rewrite a row; if something is wrong, add a new row that corrects it.
- Each row says what was tried, the result (OK / FAILED / PARTIAL / BLOCKED), and any decision taken.
- Decisions get their own entry in §13 with an ID (D-xx) and are referenced from the log row.
- Failed attempts are kept, with the cause if known.

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

**Next step:** answer §11 questions (in dashboard comments or chat), then Phase 0 (build unmodified dhewm3 on Windows, confirm audio plays). Not started.

### 12.2 Attempts not yet made
- Phase 0 build: not attempted.
- Wwise SDK check: not attempted (SDK path unknown, §11 Q1).

## 13. Decisions

| ID | Date | Decision | Reason | Status | Revisit when |
|---|---|---|---|---|---|
| D-001 | 2026-11-25 | Clone upstream into sub-folder `dhewm3/`, not the workspace root | Keeps upstream history separate from our own files, so both can be in one repo later | Accepted (proposed by assistant, not yet confirmed by user) | Repo layout decision (§11 Q4) |
| D-002 | 2026-11-25 | Keep the upstream clone unmodified until Phase 1 | Phase 0 needs a clean baseline build | Accepted | Phase 1 start |
| D-003 | 2026-11-25 | Keep CMake as the primary build; add a Makefile wrapper only if asked | Upstream uses CMake; no top-level Makefile exists | Proposed | §11 Q2 |
| D-004 | 2026-11-25 | Replace the back end under `sound.h`, not the game call sites | Keeps about 200 `StartSound` call sites unchanged | Proposed | Phase 3 review |
| D-005 | 2026-11-25 | Do not publish to any remote yet | Outward-facing; needs user approval of host and visibility | Accepted | §11 Q4 answered |
| D-006 | 2026-11-25 | Doom 3 game data and Wwise SDK never committed | Licensing (§2) | Accepted | Never (unless licence changes) |
| D-005 | 2026-11-25 | Publish to `https://github.com/Dreezn/dhewm3_Wwise.git` (user's repo) | User asked to host the project there (S-005/S-006) | Accepted | Never, unless the repo or visibility changes |
| D-007 | 2026-11-25 | `dhewm3/` is a git submodule pinned to `505c5c8`, not vendored | Keeps upstream history and a clear pin; can be re-pinned by a logged step | Accepted | Phase 1 (when we start editing upstream code, we may need a fork) |
| D-008 | 2026-11-25 | Project dashboard hosted as an Artifact page; questions and answers stored in the page database | User wants to follow progress and direct the project from a browser | Accepted | Dashboard review |
| D-009 | 2026-11-25 | No GitHub Pages site: dashboard stays a Claude artifact | User chose this over Pages. Repo is private (Pages needs a paid plan on private repos), and a static Pages copy would lose comments and Q&A | Accepted | Repo made public, or Pages needed |
