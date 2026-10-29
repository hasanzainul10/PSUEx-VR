# VR work handoff (2026-10-20)

State of the `psu_vr` branch at hand-over, for the next session. Everything listed is committed
and pushed to origin `psu_vr` unless noted.

## Environment / workflow

- Build: `D:\VSBuildTools\MSBuild\Current\Bin\MSBuild.exe PSUEx.sln -p:Configuration=Debug|Release -p:Platform=x86 -m -v:minimal` (vswhere doesn't work).
- Deploy (only when PSUC.exe / PSUExVRConfig.exe aren't running): copy `Debug\PSUExD.dll` **and**
  `Debug\PSUExVRConfig.exe` to `D:\Games\PSUClementine_22072025`. The user tests the Debug DLL.
- Log: `D:\Games\PSUClementine_22072025\psuex.log` (huge). Read it with `tools/lastlog.sh [-d] [PATTERN]` (last attach only, repeats collapsed; `-h` for options).
- Settings: `psuex_vr.json` (read last, wins) and `config.json` (`vr` object) in the game folder. Keep both in sync when editing; back up first (`.bakN`).
- Edits with `\n` inside C strings: write a Python script with the Write tool (Bash heredocs turn `\n` into real newlines and break the build).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Features must be toggleable (a setting + a checkbox in `VRSettingsPanel.cpp`).
- Ghidra MCP: often disconnected; fallback `uv run --with capstone --with pefile tools/disasm.py 0xADDR 0xLEN` (set `UV_CACHE_DIR=D:\PSUExTools\uv-cache`).
- The user prefers plain short lists, wants to be told what to test, and reports results; never enter credentials/PINs for them.

## In-flight at hand-over (do these first)

1. **Game folder state (2026-10-20).** Loader `dinput8.dll` in place; the Debug build with the JA combo buzz and the JA combo buffer is deployed (it includes the map fix).
   VR is back on in both settings files (`enabled` true,
   `d3d9exOnly` false; `.bak7` = the earlier d3d9exOnly test state). No test state left behind.
2. **Mission map: terrain fixed (user-confirmed 2026-10-20).** The D3D9Ex upgrade was cleared
   (VR off and `d3d9exOnly` both matched vanilla). Cause: the terrain is a `DrawIndexedPrimitive`
   from `0x0074E4FD` (layer 0x31, FVF XYZ|NORMAL, ~837 triangles), and pass B only captured DP-UPs
   and screen-space DIPs for the HUD panel. `VRHud::CaptureMapMeshDraw` now replays transformed
   DP / DIP / DIP-UP draws in layers 0x31 / 0x32 onto the user panel (`hudMapMeshes`, default on,
   checkbox "Mission map terrain on the panel"). Debug logs: `tools/lastlog.sh 'VRMap'`.
   **Map fit (user-confirmed 2026-10-20):** the game draws the minimap frame at 640 x 480 pixel size
   (layer 0x30, 128 x 128 at (2240,108)-(2368,236) on 2560 x 1440) but sizes the 3D map for the
   whole screen (off-centre perspective, `_31` -0.65 / `_32` -0.583 -> centre (2112,300)), so only
   a sliver showed in the frame (vanilla too). On the panel replay only, `VRHud::MapFitProjection`
   rescales PROJ by 640/W x 480/H and moves the map centre onto the frame centre (`hudMapFit`,
   default on; zoom `hudMapFitScale`). With the fit on, transformed map-layer DP-UPs skip the
   depth routing (eyes / skip-own) and go to the panel. The flat frame is unchanged.
   Not done: the red 2D arrow right of the frame (layer 0x33, DP-UP `0x0074722D`, ~(2437,274)) is
   still where the game puts it; ask the user whether it matters.
3. **Glare (floating glow off enemies, glow through the weapon, "rectangles").** Layer bisect found:
   layer `0x12` = `TLandDisplayerHevenTask` (sky / galaxy), `0x1F` = `TBlur`, `0x20` = `TGlare`.
   These layers make no draw calls themselves; the visible output is a later full-screen blend.
   Hiding layer `0x20` removes glow + rectangles but freezes the right eye (stale image), and
   hiding a layer from area start crashed on mission entry (object never set up). Current attempt
   (`68fa71c`, untested): with `eyeGlare` off, skip the draw at return address `0x004B6331` (full-screen
   alpha blend of a 2560x1440 RT) in eye passes only. If that's wrong, use the Debug draw-site
   bisect (Ctrl+Shift+Insert / Delete) to find the composite draw.
4. **Galaxy**: solved by hiding the sky layer in the headset (`eyeSky` false; set in the user's
   settings). Moving the sky per eye (`skyAtEye`) and keeping the viewport depth range didn't change
   it: the mission's sky is a partial box made for the flat camera.

5. **Combo buzz at Just Attack timing: done (user-confirmed 2026-10-20).** The buzz fires once per
   hit when the current motion's input window opens, read from the weapon type's motion table
   (`0x005366E0`: `[0x00B9DAA4]` by active class `+0x1DD` and motion `+0x170`, entry `+0x08` / `+0x0C`
   in animation frames, `0x005532F0`). Pressing on the buzz gives JUST ATTACK on sword, knuckles,
   spear, twin daggers, saber and dagger. Code: `PSUCombat::ReadMotion` / `JustAttackWindowOpen`,
   `ComboFeedback` (fallback: the old `+0x710 == 0x0E` edge when the table can't be read). `+0x710`
   is the allowed-input mask (`0x0053DEB0`), not a combo state. Debug log: `tools/lastlog.sh VRCombat`.
   **Combo buffer on the JA window (user-confirmed 2026-10-20).** `comboBuffer`
   now holds a mid-combo swing until the current motion's JA window opens (`HoldForCombo` in
   `VRSyntheticPad.cpp`: `motionFrame < jaStart`), timeout `kJustAttackWaitSeconds` 1.5 s (windows
   open up to frame 33, ~1.1 s at 30 fps). Fallback without a table: the old `Swinging` (`0x02`) wait,
   0.25 s. comboStep 0 still goes out at once. Toggle `comboBufferJustAttack` (default on, checkbox
   "Hold until the Just Attack timing" under the combo buffer one): off = the old `Swinging` wait. Confirmed on saber, spear and twin daggers: early swings land on the buzz with JUST ATTACK; off
   sends them early as before. Log: `tools/lastlog.sh VRSwing`
   ("press sent ... frame F JA [a..b]": F should be at or just past a).
   Ghidra: the full analysis came from a headless run (`tools/ghidra/PrepareFullAnalysis.java`, 19,940
   functions; backup `PSUClementine.rep.bak-2026-10-20`).
   Address scan scripts (pefile / capstone, run with `uv run --with capstone --with pefile`): `tools/findref.py` (dword refs),
   `tools/thiscalls.py` (`lea ecx,[reg+DISP]; call` targets), `tools/dispscan.py` (`[reg+DISP]` uses, `w` = writes only).

6. **Speech to text + voice commands (built, untested, uncommitted, 2026-10-20).** Design agreed with
   the user: Moonshine (MIT) in a **64-bit helper** `tools/PSUExSpeech` (`PSUExSpeech.exe`, built x64 from
   the x86 solution), **hold left Square** to talk, the transcript is **sent to chat at once**. Small
   streaming English model bundled (`PSUExSpeech\models\small-streaming-en`, 135 MB), tiny / medium via
   the panel's *Download* (helper `--download`, console window). Library + model live in git-ignored
   `third_party\moonshine` (`tools/fetch-moonshine.ps1`). Helper protocol: stdin `BEGIN / END / CANCEL /
   KEYTERMS a,b / QUIT`, stdout `STATUS / READY / ERROR / PARTIAL / FINAL <text>`; test offline with
   `PSUExSpeech.exe --models <dir> --size small --wav clip.wav` (16 kHz mono; TTS clips worked: 4.4 s
   of speech in 0.6 s, "Barta" / "Resta" exact, keyterms fix "Gibarda" -> "Gibarta").
   DLL: `PSUVR/VRSpeech.cpp` (helper process, toasts, chat sender, voice commands),
   `PSUDetour/Hook_007738E0_KeyboardKeys.hpp` (adds game key codes to the keyboard poll, works unfocused).
   Chat: press the open-chat key (`[0x0099225F]`, Space 0x45) -> wait for `0x00B9D4AB` -> `PostMessage
   WM_CHAR` per char (`0x004C9070` subclass) -> Enter (0x38) -> wait for the box to close. Voice
   commands: `PSUCombat::ReadTechSlots` (weapon `+0x48`, 4 x {id, level}) + `VRSyntheticPad::CastTechSlot`.
   **The technique id -> name table in VRSpeech.cpp is a guess** (groups by element are right, the order
   inside a group isn't verified): fix it from the `VRVoice: slots` log line.
   Settings: `speechToText` (default off), `speechModel` "small", `speechChat` true, `voiceCommands`
   (default off); *Voice* section of the panel. **Game folder:** Debug build + `PSUExSpeech\` deployed,
   `speechToText` / `speechChat` / `voiceCommands` **on** in both settings files (`.bak8` = before).
   **Pending test:** hold left Square in the field, say a sentence, release -> toasts "Listening...",
   "... text", then the chat box opens, fills and sends ("Chat: ..."). Equip a rod / staff with known
   techniques -> `tools/lastlog.sh VRVoice` shows each slot's id: map ids to names. Say a technique
   name -> it casts from its slot. Logs: `tools/lastlog.sh 'VRSpeech|VRVoice'`.

   **Phase 1 of voice expansion (built + deployed 2026-10-29, untested).** Always-on listening +
   route toggle. Helper: `MODE auto|push` (stdin), energy-gate VAD in `CaptureThread` (start after
   2 loud 50 ms buffers, end after 600 ms quiet, max 10 s, gate = max(0.015, noise x 3)), each
   utterance comes back as `AUTO <text>`; `s_sessionMutex` guards the engine between the capture
   thread and BEGIN / END. DLL: `vr.speechAlwaysOn` (default off) and `vr.voiceRoute`
   (`auto` default / `command` / `chat`). Always-on text: route command -> technique match only,
   unmatched dropped (never chat); chat -> game chat; auto -> command when a weapon is out in the
   field (no menu / text box; no enemy test yet, so also true in town), else ignored. The held
   button keeps its fallback to chat. **Tap left Square (<250 ms) cycles the route**
   command -> chat -> auto (toast; kept only if saved in the panel). Panel: Voice tab. Game folder:
   Debug DLL, `PSUExSpeech.exe` and `PSUExVRConfig.exe` deployed; settings files NOT changed
   (`speechAlwaysOn` is off). **Pending test:** turn on *Always listening*, say "Barta" with a
   rod/staff -> casts; talk normally -> nothing happens (`tools/lastlog.sh 'VRVoice|VRSpeech'` shows
   "no command in"/"ignored"); route Chat -> everything reaches chat; tap Square cycles; music or
   room noise doesn't trigger casts. Tune `kAuto*` in `tools/PSUExSpeech/main.cpp` if it clips
   words or never fires. Phase 3 (skills, PB / beast) still needs Ghidra passes.

   **Phase 2 of voice expansion (built + deployed 2026-10-29, untested).** `VRSpeech.cpp` now has a
   command registry (`Command{kind, arg, exact aliases, fuzzy name}`, `RefreshCommands()` every 500 ms,
   `MatchCommand`, `Execute`); techniques behave as in phase 1 (same aliases, fuzzy rule, KEYTERMS; the
   log line gained `; N commands`). New kind Item: "item one..four" / "use item 2" (`voiceItems`, default
   OFF, panel Voice tab) calls `VRSyntheticPad::UseItemSlot(n)`, which runs a button script on the virtual
   pad (`voiceItemScript`, default `hold:B wait:open tap:slot wait:0.3`; `voiceItemButtons` default
   `Up,Right,Down,Left` = pad button for items 1..4; both editable in config.json). **The default script is
   a GUESS**: Ghidra only showed the palette's drawing code (TWindowActionPalette, see ADDRESS_MAP "Item
   palette"); no player-side use function and no readable slot contents, so there is no direct call and no
   item-name matching ("Monomate") yet. Game folder: Debug DLL + `PSUExVRConfig.exe` deployed, settings
   files untouched (`voiceItems` off).
   **Pending tests:** (1) regression: techniques by voice exactly as before. (2) Turn on *Voice items*,
   Always listening, route Command; with a palette item set say "item two" -> `tools/lastlog.sh VRVoice`
   shows `pad script ... (item 2 = Right)`, `done` or `aborted: the palette never opened`. Watch what the
   palette does while B is held and which button picks/uses a slot, then fix `voiceItemScript` /
   `voiceItemButtons` (steps: `hold:<btn>`, `wait:open`, `tap:<btn|slot>`, `wait:<sec>`; buttons a b x y
   l1 r1 l2 r2 l3 r3 select start up right down left). (3) Say it with a menu open / palette already open ->
   toast "can't be used right now", nothing pressed. Next step if the script can't be made to work: a
   Ghidra pass on the pad->palette input (input slot `PL_PALETTE0` 0x10, `g_InputPlayers`).

7. **Controller UI (in progress, 2026-10-20, uncommitted).** Plan agreed with the user, 3 phases, each
   built and tested before the next: (1) right-controller laser pointer for PSUEx ImGui windows,
   (2) floating label cards at each controller (look down; or hold a grip still ~0.4 s; both grips +
   right stick click toggles look-down, replacing the chord R3), (3) setup wizard (recenter, real eye
   height from the OpenXR STAGE floor, hand calibration, controls tour; `setupDone` flag). Full plan:
   `C:\Users\hasan\.claude\plans\i-want-to-plan-concurrent-lake.md`.
   **Phase 1 built and deployed (Debug):** `PSUVR/VRUiPointer.{h,cpp}`; `IVRRuntime::GetUiQuad`
   (OpenXR records the user HUD layer in stereo, the back-buffer panel otherwise, in LOCAL space);
   beam in `VRControllers::DrawMarkers` (needs "Show controllers"; eye pass only, so no beam in 2D
   scenes, only the ImGui cursor); `VRSyntheticPad` sends only both-grip chords while the pointer is
   wanted, and holds buttons back after closing until all are released. Setting `uiPointer` (default on).
   Sliders: hold the trigger and drag (no left-stick nudge). Log: `tools/lastlog.sh VRUi`.
   **Phase 1 user-confirmed (2026-10-20):** the pointer works in the headset.
   **Phase 2 built and deployed (Debug, 2026-10-20 21:43), untested:** `PSUVR/VRControllerLabels.{h,cpp}`
   (look-down / grip-hold logic, billboard pose above the grip, cards drawn with an `ImDrawList` +
   custom `ImDrawData` into `D3D11Bridge::LabelSurface` 1024x512, Segoe UI 32 px font loaded when VR
   is on), `VRSyntheticPad::DescribeControls(gripMask, HandLabels[2])` (labels from the same roles /
   menu / aim state), `OpenXRRuntime::SubmitHandLabels` (own swapchain, two quads from its halves,
   layers array now 5). Chord: right stick click with both grips = `ToggleControllerLabels` (was R3).
   Settings `controllerLabels`, `controllerLabelsGrip`, `controllerLabelsHoldSeconds` 0.4,
   `controllerLabelsSize` 0.14. Not done from the plan: the settings diagram still reads `kLayout`
   (not `DescribeControls`); no leader lines on the cards. The field names of A / B / L1 / R2 etc.
   on the cards are guesses: ask the user. Log: `tools/lastlog.sh 'VRLabels|label'`.
   **Settings panel tabs + grip drag (built and deployed 22:06, untested):** `VRSettingsPanel.cpp` is now tabs
   (General, Camera, HUD, Controllers, Combat, Voice, Keys; `Section(Tab, title)` picks the tab, the tab
   body is a child that scrolls on its own, Save / Reload stay below). In the headset, right grip held
   while pointing moves the panel (`hudDistance`, 0.6-4 m, tick every 25 cm, saved + toast on release).
   It moves the game HUD too (same quad). Label cards user-confirmed (2026-10-20). First drag try:
   the panel didn't visibly move (log: 2.50 -> 2.46 m, hand travel ~1 cm along the aim ray). Fix
   deployed 22:33, untested: level direction hand -> panel, distance x e^(3.5 x hand travel), and the
   right stick moves it 1.5 m/s while the grip is held (no scroll then). Log line now shows hand
   travel and stick metres: `tools/lastlog.sh 'VRUi: panel'`.
   **Left Triangle = R2 (built and deployed 22:18, untested):** with `triangleChatL2` on (name kept)
   left Triangle in the field now sends R2, the game's open-chat-log button (was L2). Labels, panel text,
   `OutputName` and VR_GUIDE updated.
   **Phase 3 setup wizard (built and deployed Debug 2026-10-21 00:14, untested):**
   `PSUOverlay/Interface/VRSetupDisplay.{h,cpp}` (IFWinCtrl `WINDOW_VR_SETUP_DISPLAY`, registered after
   VRDisplay). Steps Welcome, Play position (`System::RequestRecenter`, consumed with the Recenter action in
   VRSystem), Eye height (`System::HeadHeightAboveFloor`, cached per frame from
   `IVRRuntime::GetHeadHeightAboveFloor` = VIEW in STAGE; OpenXR creates STAGE when listed, log
   "OpenXR: STAGE space available"), Hands (motion only; `++m_handCalibRequest`, progress from
   `VRHands::CalibrationStatus()` Waiting / Counting / Done / Missed; "Keep the PS VR2 defaults" restores the
   default quaternions), Controls (motion only; `DrawControllerDiagram` + chord text). Finish / Skip set
   `setupDone` and save. Opens by itself on the first stereo frame with `inputMode` motion and `setupDone`
   false (once per run); gamepad mode only via General > "Run VR setup again" (closes the settings panel).
   `VRUiPointer::SetWizardOpen` ORs into Wanted. Setting `setupDone` (default false, checkbox "VR setup
   done"); `--write-defaults` writes false. Game folder: neither `psuex_vr.json` nor `config.json` has
   `setupDone`, so it reads false and the wizard opens on the next start. Log: `tools/lastlog.sh 'VRSetup|STAGE'`.
   Circle doesn't close the wizard (only Skip / Finish).
   **Phase 3 pending test:** wizard appears on the first stereo frame; Recenter -> "Recentered" toast;
   Measure -> head height ~ your real eye height, eye height set (clamped 1.2-2.0); Calibrate -> countdown,
   "Calibrated (2 hands)"; Finish -> toast "VR setup saved", `setupDone: true` in the file, no wizard next
   start; General > "Run VR setup again" reopens it.
   **Pending test:** look down at a controller -> its card; both grips +
   right stick click -> toast, look-down cards stop; hold a grip still ~0.4 s -> both cards with that
   grip's context; grip + swing -> no flash; both grips -> chord labels; menu / gun / free aim change
   the labels.

8. **Performance pass (started 2026-10-21, uncommitted).** Plan: `C:\Users\hasan\.claude\plans\ok-most-of-what-sprightly-emerson.md`
   (phase 0 timing overlay -> 1 free wins -> 2 mirror the left eye on the monitor, skipping the flat 3D pass in
   stereo -> 3 eye resolution scale / LOCAL-space layer / quality presets -> 4 RE: shadows once, D3D9Ex
   DYNAMIC remap, game frame cap). The user tests on **Release** (`PSUExVR.dll`; the loader prefers
   `PSUExD.dll`, so rename it `PSUExD.dll.off` in the game folder).
   **Game folder (2026-10-21 01:42):** Release `PSUExVR.dll` + `PSUExVRConfig.exe` deployed, `PSUExD.dll` renamed
   `PSUExD.dll.off`, `perfOverlay` true in both settings files (`.bak9` = before).
   **Phase 0 built (Debug + Release), deployed (Release):** `PSUVR/VRPerf.{h,cpp}`, setting
   `perfOverlay` (default off, new *Performance* tab). CPU scopes: flat pass, each eye, pass B, overlay (EndScene),
   capture, Present, submit, xrWaitFrame, GPU wait. GPU: D3D9 timestamp queries (ring of 4, never flushed) for
   flat / eyes / pass B / whole frame. Overlay window top-left, plus a `Perf 5 s:` log line.
   **Baseline (2026-10-29, Debug build, town, stereo 100%):** ~45 fps (22 ms), SteamVR running 90 Hz at half
   rate (predicted period mostly 45 Hz). CPU ms: flat 2.45, eyeL 2.5, eyeR 2.5, passB 1.1, overlay 0.35,
   capture 0.45, present 0.3, **submit 6.3 (waitFrame only 0.02, gpuWait 0.8 -> ~5.5 ms unaccounted inside
   OnPresentSubmit: SubmitStereo / swapchain waits / D3D11 blits / HUD quads / labels / xrEndFrame)**; ~6 ms
   outside the hooks = game logic. GPU ms: flat 2.8, eyes 2.4 + 2.7, passB 1.9, frame 13 -> **CPU-bound**, not
   GPU. Hitches: frame max 28-56 ms, passB GPU max up to 12 ms.
   **Phase 0b built + deployed (Debug, 2026-10-29 19:07):** Submit split into `updates`, `stereo` (SubmitStereo),
   `swapWait` (acquire + wait, all swapchains), `blit` (BlitTexture), `hudQuads`, `labels`, `endFrame` (new log keys;
   VRSystem uses `VRPerf::Begin/End` because OnPresentSubmit has `__try`). **Pending test:** same town run ->
   which key holds the ~5.5 ms and the 453 ms stall.
   **Result (2026-10-29 run, town + a mission, Debug):** the missing time is **`updates` = 3.8-5 ms in town, 7-8 ms in the
   mission (max 10-18 ms)**, only once first person / motion controllers are active (0.25 ms before). Swapchain wait 0.02,
   blit 0.05, stereo 0.2, HUD quads 0.08, labels ~0, xrEndFrame 0.38 -> the OpenXR side is fine. Mission: 33-38 fps, eyes
   3.2-3.5 ms CPU each, pass B GPU spikes 23-27 ms (hitches). Suspect: many `PSUMemory::ReadAbsoluteMemory` /
   `IsValidPointer` calls (each a `VirtualQuery` syscall) in VRSyntheticPad / PSUCombat / VRHands per frame.
   **`updates` fixed (2026-10-29, built Debug + Release, deployed: Debug `PSUExD.dll` + `PSUExVRConfig.exe`, Release
   `PSUExVR.dll`; Debug loads first).** `updates` split into sub-stages (log keys `input ctrl uiPtr pad padCombo padCombat
   padAim padRoom lblUpd speech`). The cost was `PSUCombat::Read` (~16 `IsValidPointer` = `VirtualQuery`, ~0.2 ms each
   in this process): `padCombo` (ComboFeedback) 3.7 ms/frame, `speech` (RefreshCommands every 0.5 s) 6 ms spikes.
   Fix: `PSUMemory::IsValidPointer` probes one byte under SEH (`ProbeReadable`) while `PSUMemory::fastPointerChecks`
   is set; synced each VR frame from setting `fastPointerChecks` (default on, Performance tab "Fast game memory
   checks"). Only difference: probing a PAGE_GUARD page clears its guard. Helps every caller (VRHands, aim hooks,
   Hook_FrameRender layer walk). **Result (user-tested town + mission, Debug):** `updates` 0.2 ms avg (was 4-5), `padCombo`
   ~0.01; game hits its 60 fps cap (16.7 ms), CPU flat ~0.4-1.0, eyes ~0.7 each (were 2.5-3.5), submit ~1.9 (gpuWait
   1.3-1.4 of it). Combat / combo buffer / JA buzz unchanged per the user. Not committed.
   **Next:** phase 1 (skip BB copy + pre-Present fence, CopySubresourceRegion, log flushing, HUD lock ranges), then
   phase 2 (mirror the left eye on the monitor). Re-take the baseline first: the old numbers predate this fix.
   **Post-fix baseline (2026-10-29, Debug, town + mission, 240 `Perf 5 s` lines; copy in the session notes folder
   `perf-baseline-post-fastptr.txt`):** 59.2 fps avg, **pinned at the game's 60 fps cap** (frame 16.7 ms) apart from load
   spikes. CPU avg ms: flat 0.54, eyes 0.61 + 0.61, passB 1.03, overlay 0.46, **capture 0.65**, present 0.29, submit 1.77
   (**gpuWait 1.25** total, endFrame 0.36, stereo 0.17, blit 0.04, hudQuads 0.07). GPU: flat 0.5, eyes 0.85 each, passB 1.2,
   frame 3.45. So phase 1 can only show up as lower CPU ms (capture, gpuWait, passB), not as fps.
   **Phase 1 built (Debug + Release) and deployed 2026-10-29 21:57 (Debug `PSUExD.dll` + `PSUExVRConfig.exe`, Release
   `PSUExVR.dll`); no settings-file changes (the new keys default on).** Four settings, Performance tab, all default on:
   - `perfSkipFlatCopy` (`VRSystem::OnPresentCapture`): in stereo without `stereoPanel`, skip `BlitBackBuffer` and the
     pre-Present fence; the fence after BeginFrame stays. Expect capture ~0.65 -> ~0.05 and gpuWait lower.
   - `perfCopyBlits` (`OpenXRRuntime::BlitTexture`): `CopySubresourceRegion` when sizes match and both formats are in the
     same 8-bit BGRA/RGBA typeless group (eyes 87 -> swapchain 90, passthrough, so byte-identical); the shader path drops
     its redundant full clear. Logs `OpenXR: blit as plain copy` once. SRV/RTV caching not done (the copy path skips them).
   - `perfHudLockRanges` (`VRHud::CaptureXyzrhwIndexed`): lock the IB range of the draw, scan min/max index, lock only that
     VB range; whole-buffer locks as fallback when a range is out of bounds. Expect passB CPU lower.
   - `perfLogBatching` (`Misc/Log.h`, `PSULog::g_batchFlush` synced each VR frame): flush at most once per second;
     `GlobalCrashHandler` calls `PSULog::Flush()` (try_lock). Unconditional: at attach a `psuex.log` over 20 MB is renamed
     `psuex.log.1` (the 150 MB log rotates on the next launch). The per-frame `SceneCB` lines were already capped
     (last session logged ~7 lines/s), so this is a small win.
   - Not done: skipping the HUD RT clears when the HUD is empty (in gameplay it never is; a missed dirty flag would leave
     a stale HUD).
   - The `Perf 5 s:` line now ends its header with `fast FCHL` (a `-` for each toggle that is off) to split A/B runs.
   **Pending test:** town + a mission, all on (~3 min), then all four off on the Performance tab (~1 min, same place),
   then on again. Compare with a summary of `tools/lastlog.sh -a -n 0 'Perf 5 s'` split by `fast FCHL` / `fast ----`.
   Headset must look the same (eyes, HUD text, map, labels, menus/2D screens).
   **Result (2026-10-29, Debug, town + mission; 67 lines on vs 19 off): all four kept.** The user saw no difference
   in the headset either way. CPU avg ms on vs off: capture 0.10 vs 0.84, gpuWait 0.65 vs 1.26, blit 0.00 vs 0.04,
   stereo 0.12 vs 0.19, hudQuads 0.04 vs 0.08, passB 0.91 vs 1.02, so about 1.4 ms less CPU per frame. Still pinned at
   the 60 fps cap. Logging has no timer of its own. The log rotated as intended: `psuex.log` was 0.36 MB after the
   run, and the old log is `psuex.log.1`. Not committed. **Next:** phase 2 (mirror the left eye on the monitor).

## Features done this stretch (all toggles in the VR settings panel)

- Combat by weapon category (`+0x2D4`/`+0x2D8`): trigger/swing = attack / tech slot 1, grip + = PA /
  slot 2 (`gripPhotonArt`); staff/rod/TCSM casting; rod slots 3/4 on the left (R1, unverified);
  bow on the left hand (aims, fires, PA); RCSM no controls; class `0x21` twin handguns (no free aim).
- Arms: Photon Art / technique animation toggle (`handsPhotonArtAnimation`, default off);
  staff/rod held still while casting (`techWeaponStill`).
- Free aim: "leash" (`aimLeash`, `aimLeashMeters` 10): reticle on the controller line, shots aimed
  from the game's firing point (player + ~1.07 m up, learned from bullets) to the reticle. Per-weapon
  gun model angle (`gunAdjust{Handgun,Bow,Rifle}{Pitch,Yaw}`) set in the headset with both grips +
  right stick while aiming. Old gun pointing (`gunPointsAtAim`) off/ignored. Works well per the user.
- Effects: world sprites from the 2D sprite drawer `0x00747248` re-projected per eye (`reprojectSprites`)
  with forced LESSEQUAL depth test; camera-locked tint sheets kept on the eye (`recenterSky`).
- Menus: dark backdrop projection layer behind 2D/menu panels (`menuBackdrop`) so SteamVR doesn't
  show its boundary; menu scanline exception (`hudOverlays`, not confirmed working).
- Left controller: Triangle = chat log window (Y in menus), Square = speech to text placeholder
  (X in menus). Chat log window (`PSUOverlay/Interface/LogDisplay.cpp`) tails `LOG\ChatLog_<date>.txt`.
- Hooks: `DrawIndexedPrimitiveUP` now attached (counters + eye skip only, no HUD capture).

## Debug tools (Debug build only)

- Ctrl+Shift+PageUp/PageDown: hide one render layer `0x02..0x3F` (toast shows it).
- Ctrl+Shift+Home: write every layer's nodes (callback, object, RTTI class) to the log.
- Ctrl+Shift+Insert/Delete: hide one eye-pass draw site at a time (toast shows address + layer).
- Ctrl+Shift+End: save the flat frame and the HUD panel as `psuex_dump_frame.png` / `psuex_dump_hud.png` in the game folder (the last run's press didn't save; check the `Dump:` log line).
- `VRMap new:` / `VRMap 5 s:` / `VRMap geo:` / `VRMap fit:`: mission map draws, screen boxes and the fit.
- Logs: `VREffect:` (screen-space draw kinds), `Layers 5 s:` (sky/blur/glare draw counts),
  `Stereo: model near the camera` / `3D near camera`, `VRBullet:`, `VRAim:`.

## Key findings (also in `docs/ADDRESS_MAP.md`)

- The game's VIEW is identity: world matrices are already camera space; eye views are built
  relative to the game camera. SetTransform wrapper `0x00749D40` (returns at `0x00749D5C`; real
  caller 4 dwords up the stack).
- `PSU_DrawLayer` `0x00560830` stores the layer being drawn at `0x009946C8`.
- Free-aim bullets start at a fixed point (player + ~(0, 10.7, 0) units), not at the muzzle.

## Ideas not started

- Speech to text: an online recognizer as a second `ISpeechEngine` in the helper; push-to-talk
  without motion controllers (gamepad / keyboard key); voice commands beyond techniques.

9. **Strafe legs + smaller attack-facing turn (built Debug 2026-10-29, untested).** `VRHands.cpp` `StrafeLegs`
   (called after the torso turn, needs `handsFollow`, a body that shows legs: set "Own body" to Headless): body yaw trails the head
   (35 deg dead-zone, follows at once when walking), travel direction (`+0x124` facing, speed from `+0x114`) against it picks
   forward / left / right / back (10 deg hysteresis); legs are yawed about the waist by a hip-twist (`strafeBodyBlend` 0.5; back flips 180 deg),
   no gait reversal. Settings `strafeBody` (default on), `strafeBodyBlend`; checkbox in Camera > First person.
   `attackFaceTolerance` (default 30 deg, was fixed 20), turn ends 10 deg early, hold 0.05 s, max 0.5 s; slider under "Turn to where I look".
   **Pending test:** Headless body, walk left / right / back (legs turn against the torso), `tools/lastlog.sh VRStrafe`;
   attacks with a small look offset turn less, `tools/lastlog.sh VRFace`. If back-walk looks like moonwalking, lower the blend or learn leg clips (PS2PI approach).
   **Rework (2026-10-29, built Debug, not deployed, game was running):** the log showed the old torso turn twisting the chest 100-180 deg
   from the legs when the head turned. With `strafeBody` on: `StrafeUpdate` (body yaw, gait, leg turn), `TurnBody` (whole skeleton to the body yaw),
   chest twist toward the head clamped to +-50 deg (`kTorsoLimit`), `TurnLegs` (extra leg yaw). Off = the old unclamped chest turn.
   Open: camera sits behind the body / clips the shoulder when walking forward (walk lean puts the head ahead of the eye;
   idea: anchor the eye to the neck bone's offset from the player position, or raise "Eye forward"). Log: `tools/lastlog.sh 'VRStrafe|torso turn'`.
   **Eye anchor (2026-10-29, built + deployed Debug):** `fpEyeFollowsNeck` (default on, checkbox "Eye follows the walking lean"): `VRHands::UpdateEyeLean`
   measures the animated neck's horizontal offset from the root minus its rest value (rest sampled while standing), eased 0.25 s, capped 0.45 m;
   `VRCamera` adds it to the first-person head anchor (`VRHands::EyeLean`). Pending test: walk forward, camera stays in front of the shoulders, no
   bobbing/nausea; if it jitters, lower the cap or turn the checkbox off.
   **Arm stretch (2026-10-29, deployed Debug):** `armStretch` (default on, "Stretch arms to reach my hands"): after the two-bone solve the visible arm stretches
   (half the gap at the elbow, half at the wrist, up to half the chain length) so the hand lands on the controller; "hands" body already shifted the hand fully.
   User reports (strafe works): shoulder still visible while running. Not fixed yet; unknown whether the eye lean engaged. Game launch from online.exe exited after Launch
   (unrelated to our DLLs: even with them and dinput8.dll moved aside); launching PSUC.exe directly works.
   **Shoulder in view when running straight (2026-10-29, deployed Debug, untested):** `UpdateEyeLean` now also tracks both shoulder bones and pushes the eye
   forward (along `VRCamera::BodyAxes` forward) until no shoulder has moved further ahead than the eye since rest. Log: `tools/lastlog.sh 'VRStrafe: eye lean'`
   (lean / goal / neck / shoulder offsets in game units, 10 = 1 m). If a shoulder still shows, send that line; next idea is folding the shoulders near the eye.

10. **Next task (requested 2026-10-29, not started): pose-less skeleton for VRChat-style IK / full-body tracking.** The user wants the game's body animations
   (attack, idle, walk, default poses) disabled so the model is a plain skeleton (rest pose) that VR drives: 3-point IK from head + hands now, standable / full-body
   trackers (hips, feet, elbows) later, locomotion layer for legs. Hook point already exists: `Hook_0074B760_BonePose.hpp` -> `VRPlayerDraw::OnBonePose` -> `VRHands::Apply`
   (28-bone skeleton, bone records 0x90 bytes with parent at +6 and inverse bind at +0x30; `av_bind` in PSP2I_Recomp_VR `port/vr/vr_body.c:688-732` poses a skeleton
   in its bind pose from the inverse binds). Plan: setting `bodyRestPose` (default off) that replaces every bone matrix with the bind pose (inverse of the record's inverse bind),
   keeping the root/placement; then the existing arm IK, torso/legs layers (`StrafeUpdate`/`TurnBody`/`TurnLegs`) pose on top; weapons keep following the hand bone
   (`techWeaponStill` etc. need rechecking since attacks no longer animate). Open questions: weapon swing visuals without animation, Photon Art effects keyed to animation
   frames (hit timing still comes from the game state, not the pose), and the full-body tracker input path (OpenXR body/`XR_HTCX_vive_tracker_interaction` or SteamVR trackers).
   Current deployed state: everything above (strafe legs, torso clamp, eye lean, arm stretch) is in the Debug DLL; all untested except strafing (user: "strafing now").

   **Rest pose (built Debug 2026-10-29, not deployed, untested):** `bodyRestPose` (default off, checkbox Camera > First person "Body in rest pose") -
   `VRHands::RestPose` runs first in `Apply` (also third person, independent of handsFollow): bone 0 keeps the game's matrix, every other bone = bind (inverse of record +0x30,
   flag 0x8 = leave animated) relative to bone 0, times the root. Arm IK / strafe / torso / eye lean then run on top; with it on, the Photon Art/attack "animation" blend
   and `techWeaponStill` are skipped (nothing animates; weapon bone is a child of the hand so it follows the arm IK). `bodyFitHeight` ("Scale body to my height"):
   scales the bones uniformly about the root so neck+6 cm = `fpEyeHeight` (log `tools/lastlog.sh 'body fit|rest pose'`; clamp 0.5-2x). Pending test: the model stands
   in rest pose (likely a T/A pose), arms follow controllers, legs follow strafe, weapon in hand; if the root isn't bone 0 or the mesh looks sheared, send the log.
   **Full-body tracker plan:** add a `VRTrackers` source (SteamVR trackers via OpenXR `XR_EXT_user_paths` roles `/user/vive_tracker_htcx/role/{waist,left_foot,right_foot,left_elbow,right_elbow}`
   with `XR_HTCX_vive_tracker_interaction`; the null-HMD test rig can emulate them). Convert each pose to model space like the controllers (`VRCamera::TrackingDirToWorld`,
   `s_world` inverse), then in `Apply` after RestPose: waist -> translate/rotate the hip subtree (`ApplyToSubtree`, bone kWaistBone) and replace `StrafeUpdate`'s inferred
   body yaw with the waist yaw; feet -> two-bone leg IK (reuse `SolveArm` on thigh/shin/foot, pole from the knee direction = foot forward); elbows -> pole vector for
   `SolveArm`. Keep strafe legs as the fallback when a tracker is absent (per-role validity flags). Calibration: stand in a T pose, record tracker-to-bone offsets (like hand calibration).
