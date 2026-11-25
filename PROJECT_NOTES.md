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

This table is the source of truth for the dashboard's open-questions list. Answers are typed on the dashboard; they are copied here (Answer column) when received, then the sync step is run (see §14).

| ID | Question | Status | Answer |
|---|---|---|---|
| Q-01 | Where is the Wwise SDK 2024.1.18 installed on this machine, and should the Authoring project live in this repo? | Answered | Wwise is at `C:\Audiokinetic\Wwise2024.1.18.9194`. The Authoring project goes in this repo. (D-019) |
| Q-02 | Top-level GNU `Makefile` wrapping CMake, or a Windows `build.bat` / `nmake` file? | Answered, to confirm (D-017) | CMake alone is enough. The original dhewm3 builds with CMake and has no top-level Makefile. A Makefile would only wrap CMake for convenience, so we keep CMake only unless you ask for one. |
| Q-03 | Exact Doom 3 folder under `C:\Program Files (x86)\Steam\steamapps\common\`? | Open: choose one | Elaboration: dhewm3 runs the original Doom 3 game data only. Use the classic folder `C:\Program Files (x86)\Steam\steamapps\common\Doom 3`. The upstream README says DOOM 3: BFG Edition is not supported by dhewm3. Recommendation: classic. Needs your choice. |
| Q-04 | Should `dhewm3/` stay a submodule, or become a fork of dhewm3? | Open: choose one | Elaboration: a submodule is a separate git repo kept inside ours. Our repo stores only which commit it points to. The question is which remote it points to. Upstream (`dhewm/dhewm3`) is read-only for us, so our changes cannot be saved there. Phase 1 changes upstream code, so we need a fork (for example `Dreezn/dhewm3`) that the submodule points to. Recommendation: fork on GitHub and point the submodule at the fork. Needs your choice. |
| Q-05 | Do students get the original Doom 3 audio files, or only placeholder audio in their Wwise project? | Answered (D-016) | Students get no ID Software audio, only a written list of events and game syncs. We make a template Wwise project ourselves, not shared. Sound choices are ours; using the originals is a good idea. The originals stay on local disk and are never uploaded to GitHub. |
| Q-06 | Windows only for now, or also Linux and macOS? | Answered (D-018) | Windows only. |

## 12. Progress log

Rules for this log:
- One row per step or attempt, in order. Never delete or rewrite a row; if something is wrong, add a new row that corrects it.
- Each row says what was tried, the result (OK / FAILED / PARTIAL / BLOCKED), and any decision taken.
- Decisions get their own entry in §13 with an ID (D-xx) and are referenced from the log row.
- Failed attempts are kept, with the cause if known.

### 12.0 Current next step

This is the banner on the dashboard. Update it whenever the next step changes.

| ID | Current next step | Status |
|---|---|---|
| N-01 | Answer the open questions on the dashboard and press Proceed. Then Phase 0: build unmodified dhewm3 on Windows and confirm audio plays | Current |

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
| S-012 | 2026-11-25 | Dashboard | Replaced the main-goal banner with the current next step (N-01, §12.0). Added `next` to the sync script | OK (not yet tested in browser) | D-014 | Main goal stays in §1 of the notes. |

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
| D-021 | 2026-11-25 | Repository & build | Use the classic Doom 3 Steam folder (`...\common\Doom 3`) as the game data source. DOOM 3 BFG Edition is not supported by dhewm3 | Upstream README states BFG is not supported. Answer to Q-03 | Proposed, pending user choice | Q-03 decided |
| D-022 | 2026-11-25 | Repository & build | Fork upstream to Dreezn/dhewm3 and point the submodule at the fork before Phase 1 | Phase 1 edits upstream code, and upstream is read-only for us. Answer to Q-04 | Proposed, pending user choice | Q-04 decided |
| D-023 | 2026-11-25 | Dashboard & process | Summary dashboard is a static page in docs/, published with GitHub Pages, read-only. It replaces the Claude artifact dashboard | User request. Pages is free on a public repo, and the artifact dashboard could not be used for direction | Accepted | Pages policy changes |
| D-024 | 2026-11-25 | Repository & build | Repository made public. It keeps its link to upstream dhewm3 through the submodule, for reading upstream changes. No GitHub fork is created unless asked | User wants it public and linked as a fork. GitHub cannot turn an existing repo into a fork | Accepted | User asks for a real GitHub fork |
| D-025 | 2026-11-25 | Repository & build | Never make a formal pull request to upstream dhewm3, and never push to it | User rule. Project must not affect upstream | Accepted | Never, unless the user changes it |
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
