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
   **Phase 2 built (Debug + Release) and deployed 2026-10-29 22:22 (Debug `PSUExD.dll` + `PSUExVRConfig.exe`, Release
   `PSUExVR.dll`); no settings-file changes (new key defaults on).** Setting `flatMirrorEye` (Performance tab, "Monitor
   shows the left eye"). `Hook_PassA` skips the flat `RunPassA()` when VR is on, the setting is on, `stereoPanel` is off,
   `debugDoublePass` is off and `StereoRenderer::CanRenderEyes` (SceneAllowsStereo + bridge + NextViews). After
   `RenderEyes` (now returns whether both eyes drew), `StereoRenderer::MirrorLeftEye` StretchRects the left eye RT,
   centre-cropped to the game viewport's aspect, onto the game RT's viewport, then clears the game depth (+stencil).
   If the eyes didn't draw or the StretchRect fails, the flat pass runs after all. The `flat` perf stage (CPU + GPU)
   now times the mirror copy in those frames. `Perf 5 s:` header: `fast FCHLM` (`M` = mirror on). Log once:
   `Stereo: monitor mirrors the left eye, src [...] -> viewport ...`.
   Dependency check (code reading, not yet confirmed in game): the eyes don't read flat-pass output. Flat-only RT
   quads (glare / afterimage) are already skipped in the eyes (`stereoSkipScreenQuads`, written-texture set is per eye),
   `MarkSurfaceTexture` state is cleared per eye, the deferred lists start empty each pass A, shadows run inside each
   pass A, the device VIEW / PROJECTION aren't set by pass A (the eye passes would clobber them otherwise), and pass B
   (HUD capture, minimap, nameplate remap) still runs as before on the game RT. Known monitor-only differences:
   pass B nameplates sit at flat-camera positions over the left-eye picture, and they draw over geometry (depth cleared).
   **Pending test:** town (stereoMode 5) and a field mission, toggle on (~2 min), then off (~1 min, same place).
   Expect the monitor to show the left eye + HUD, the headset unchanged, and `flat` CPU/GPU near 0 with `fast FCHLM`.
   Summarize `tools/lastlog.sh -a -n 0 'Perf 5 s'` split by `FCHLM` / `FCHL-` against the post-phase-1 numbers
   (CPU flat 0.5, eyes 0.6 each, passB 0.9, submit 1.6; GPU flat 0.5, eyes 0.8 each, passB 1.2, frame 3.3).
   **First run (2026-10-29, Debug, 136 `Perf 5 s` lines, all `FCHLM`, no off run):** the mirror logged once,
   `src [0 602 2632 2082] of 2632x2684 -> viewport 0,0 2560x1440`, `hr 0`; no errors in the session. Stereo-100% lines
   (106, 31.6k frames): 59.6 fps; CPU flat **0.00** (was 0.5), eyes 0.83 + 0.73, passB 1.17, capture 0.03, submit 1.75
   (gpuWait 0.73); GPU flat **0.10** (the mirror StretchRect; was 0.5), eyes 0.90 + 0.88, passB 1.41, frame 3.55.
   So the flat pass is gone (~0.5 ms CPU, ~0.4 ms GPU). Eyes / passB / frame read higher than the phase-1 baseline,
   most likely heavier scenes (more mission time), but there's no same-place off run to confirm.
   **A/B run (2026-10-29, Debug, same spot, stereo-100% lines: 8 on `FCHLM` vs 32 off `FCHL-`): kept.**
   CPU ms on vs off: flat 0.00 vs 0.64, eyes 0.71 + 0.60 vs 0.68 + 0.68, passB 0.66 vs 0.91, submit 1.69 vs 1.67.
   GPU ms: flat 0.10 vs 0.52, eyes 0.66 + 0.76 vs 0.80 + 0.84, passB 0.87 vs 1.24, **frame 2.69 vs 3.66**.
   So about 0.9 ms CPU and 1 ms GPU less per frame; fps unchanged (60 cap). The user confirmed the monitor shows the
   left eye ("a bit buggy, doesn't matter"; confirmed: the flat-placed nameplates noted above, left as is) and reported nothing wrong
   in the headset. **Monitor nameplates fixed (built Debug + Release 2026-10-29, not yet deployed: PSUC was running):**
   in mirror frames `PSUOverlay::Hook_DrawPrimitiveUP` runs `VRHud::CapturePassBDraw` *before* the game's draw; a plate
   routed to the eyes is also drawn by `DrawNameplateToEyes` on the game RT at its left-eye place mapped through the
   mirror crop (`m_mirrorSrc` -> `m_mirrorDst`), and the game's flat-placed draw is skipped (`VRHud::NameplateOnMonitor`).
   Toggle off / no mirror: the old order (draw, then capture). **Pending test:** deploy, then town with nameplates in view:
   on the monitor they sit over the right people; the headset is unchanged. Committed with phase 2.
   **Next:** phase 3 (eye resolution scale, LOCAL-space layer, quality presets).
   **Phase 3 built (Debug + Release, 2026-10-30); Debug deployed by the user; no settings-file changes by us.** Performance tab:
   - `stereoLayerSpace` ("view" default / "local"), combo *Headset image space*: `OpenXRRuntime::EndFrame` submits the stereo
     projection layer in LOCAL space with the poses the eyes were rendered with (`m_submittedPoses`, from `LocateNextViews`
     one period ahead), so SteamVR reprojection corrects head rotation at 90 Hz while the game runs at 60. Logs
     `OpenXR: stereo layer in LOCAL|VIEW space` on each change. Risk the old comment names: motion smoothing may ghost sky / water.
   - `eyeResolutionScale` (0.5-1.5, default 1.0), slider *Eye resolution*: `OpenXRRuntime::SetEyeResolutionScale` (called by
     `VRSystem` before `BeginFrame`) recreates the eye swapchains at recommended x scale (even sizes, clamped to max) once the
     value is steady 0.5 s; on failure it goes back to the old size and won't retry that scale. The D3D9 eye RTs follow via
     `EyeSwapchainSize` -> `EnsureEyeTargets`, so sizes match and the plain-copy blit still applies; `MirrorLeftEye` and the
     nameplate mapping use `m_lastEyeW/H` (set in `RenderEyes`), so they follow too. Log `OpenXR: eye resolution scale a -> b`,
     then `OpenXR: eye swapchains WxH` and `D3D11Bridge: eye targets WxH`.
   - *Quality* combo (panel only, no stored key): Low = scale 0.7, `eyeBlur` / `eyeGlare` / `reprojectSprites` off; Medium = 0.85,
     blur / glare off, sprites on; High = defaults (1.0, blur on, glare off, sprites on); anything else shows Custom.
   - `Perf 5 s:` header now ends `fast FCHLM, layer view|local, res 1.00`.
   **Pending tests (deploy first: Debug `PSUExD.dll` + `PSUExVRConfig.exe`, Release `PSUExVR.dll`), `perfOverlay` on, town:**
   1. Judder A/B: *Headset image space* View ~1 min turning the head, then World (local) ~1 min, same spot. User judges judder
      and any sky / water smear (try with SteamVR Motion Smoothing on and off).
   2. Scale: Quality Low (0.7) ~1 min, then High ~1 min. Expect GPU eyes / frame ms lower at 0.7, softer image, no black /
      stretched eyes, monitor mirror still cropped right. Also try 1.5 once (does the bigger shared RT create?).
   Compare with `tools/lastlog.sh -a -n 0 'Perf 5 s'` split by `layer` / `res`, and `tools/lastlog.sh 'eye resolution|eye swapchains|eye targets|stereo layer'`.
   **First run (2026-10-30, Debug deployed 03:25, Release `PSUExVR.dll` in the game folder is still the phase-2 build; Rogue's
   Toolkit loaded): "seems to work".** Whole session in LOCAL (no VIEW run, so no judder A/B in the log yet). Scale changes
   0.5 / 0.7 / 1.03 / 1.05 / 1.48 / 1.5 all recreated cleanly (1.5 = 3948x4026 eyes, shared D3D9 RT fine), no failures.
   Stereo lines, GPU eyes L+R ms: 0.5 -> 1.09+1.12, 1.0 -> 1.19+1.18, 1.5 -> 1.46+1.36: on this GPU the eye passes are not
   fill-bound, so the scale barely moves our GPU time (it still cuts SteamVR's compositor / reprojection work, not measured
   here; it matters on weak cards). Few windows at non-1.0 values. GPU frame ~9-10 ms and passB 4-5 ms GPU / 3.5-4.3 CPU
   are much higher than phase 2 (2.7 / 0.9): the toolkit was loaded this time (its BeginScene / draws), not a phase-3
   effect. "Over budget" stays ~47% (60 fps game vs 90 Hz) whatever the layer: LOCAL hides it via reprojection, it can't
   lower it. **Smear test (2026-10-30):** technique effects looked more pixelated with Local; the user found it the same in
   View / Local and Motion Smoothing on / off, and sharp again with the toolkit disabled -> not phase 3, it is the toolkit
   (see item 7). **Judder (2026-10-30): the user confirmed Local has less judder -> `stereoLayerSpace` default is now
   "local"** (built Debug + Release; the game folder's settings files already say "local"). Eye resolution scale and
   presets work; the scale barely changes GPU time on this PC (not fill-bound). Phase 3 done; not committed.
   **Next:** phase 4 (separate plan): shadows once per frame, D3D9Ex MANAGED->DYNAMIC remap, the game's frame cap.

7. **Rogue's Toolkit 0.7.2 alongside PSUEx (UPDATED 2026-10-30, see "Status 2026-10-30" at the end of this item; the chain described next is superseded).** Plan:
   `C:\Users\hasan\.claude\plans\plan-to-make-our-giggly-sifakis.md`. The toolkit is a Rust `dinput8.dll` proxy
   (hooks D3D9 Present / Reset / DP-UP / SetRenderTarget, DirectInput devices, Get(Async)KeyState, game text draw,
   archive loader, network (theirs only)). Game folder now: toolkit `dinput8.dll` + `RoguesToolkit\`;
   **our proxy was renamed `psuex_dinput8.dll`** (original backed up in `BACKUP_pre_toolkit\dinput8.dll`);
   `roguestoolkit.toml`: `chain_dll = "psuex_dinput8.dll"`, `trigger_fix = false`, `settings_pad = ""` (Back+Start is
   our VR settings chord). Chain: PSUC -> toolkit -> psuex_dinput8 -> system dinput8. To undo: delete toolkit
   `dinput8.dll` + `RoguesToolkit\`, rename `psuex_dinput8.dll` back to `dinput8.dll`.
   **Shortcut to the toolkit menu (built, untested):** new rebindable action `ToggleToolkitMenu` (default Ctrl+F6, pad
   Back+A; `VRBindings::OpenToolkitMenu` sends a real F6 with `SendInput` once Ctrl/Shift/Alt are up, log
   `VRToolkit: sent F6`); button "Open Rogue's Toolkit menu" in VR Settings > General. Not done: a motion-controller
   gesture (all face buttons in the both-grips chord are taken), a `toolkitMenuShortcut` on/off setting.
   **Pending tests (in order):** A flat, VR off: both load (`RoguesToolkit\logs\roguestoolkit.log`, `dinput8_proxy.log`
   says "Loaded PSUExD.dll"; the toolkit may stop on "unknown PSUC.exe build" because our PSUC.exe is modified);
   B VR on: are the toolkit panels (damage meter, drop panel, HP bars) in the headset or only on the monitor;
   C shortcut: Ctrl+F6 / Back+A / panel button open the toolkit menu; pad and motion input still work.

   **Status 2026-10-30.**
   - **Defender** removed the toolkit's `dinput8.dll` once (`Behavior:Win32/DefenseEvasion.A!ml`, heuristic on an
     unsigned DLL-proxy). The user restored it. The pristine copy is `D:\Games\rogues-toolkit-v0.7.2\dinput8.dll`
     (5,214,208 bytes, hash starts `EC01FCCA`). Changing Defender is the user's call, never ours.
   - **The toolkit-first chain failed:** with the toolkit as `dinput8.dll` and `chain_dll` = our proxy, the toolkit loads
     our proxy ~1 s late (at the first DirectInput8Create), after the launcher's `PSUEx.dll` is in, so `PSUExD.dll`
     logs "Skip Detours/VR - another PSUEx is already loaded" and nothing hooks. (Flat "worked" only because
     Clementine's own PSUEx.dll ran.) The toolkit's build check says "this is PSUC.exe (Clementine)" for the game
     process and "unknown build" for `online.exe` (expected).
   - **Fix, built + deployed:** reversed chain. Game folder: `dinput8.dll` = OUR proxy (new build, 135,680 bytes), which
     now forwards DirectInput to `toolkit_dinput8.dll` if present (`dinput8_proxy.cpp` `LoadRealDInput8`, log "Forwarding
     DirectInput to toolkit_dinput8.dll"), else the system DLL; `toolkit_dinput8.dll` = the toolkit's DLL, its
     `roguestoolkit.toml` has `chain_dll = ""`, `trigger_fix = false`, `settings_pad = ""`. Build the proxy with
     `tools/dinput8_proxy/build.ps1 -GameDir NONE` (the default GameDir copies over the game's dinput8.dll; the
     "vswhere" line is harmless). Old proxy backup: `BACKUP_pre_toolkit\dinput8.dll`.
   - **Result:** VR works and the toolkit loads. **Open bug:** with the toolkit loaded, technique effects render
     wrongly in VR ("like our first release"). **Test in progress:** rename `toolkit_dinput8.dll` ->
     `toolkit_dinput8.dll.off` (game closed) and see if techniques render right in VR. If yes: the toolkit's D3D9 hooks
     (SetViewport / SetRenderTarget / DrawPrimitiveUP / Present) clash with our eye passes: follow plan Step 2
     **Result (2026-10-30, during perf phase 3):** with `toolkit_dinput8.dll` disabled, technique effects in VR are back at full
     resolution (with it loaded they look low-res / pixelated). So the toolkit is the cause; follow plan Step 2.
     **Pixelation fix (deployed 04:21 2026-10-30: Debug `PSUExD.dll` + `PSUExVRConfig.exe`, Release `PSUExVR.dll`;
     `toolkit_dinput8.dll` enabled again):** suspect = toolkit `ui_sharpness = "sharp"` (on in its toml,
     with `ui_scale` 2.0): it point-filters the game's 2D sprite draws (it takes them for UI), and the eyes magnify
     technique sprites. `Hook_DrawPrimitiveUP` now forces stage-0 MIN/MAG filter POINT -> LINEAR around reprojected eye
     sprites (site 0x747248, eye passes only, restored after; flat untouched). Log `VRToolkit: eye sprite had point
     filtering` (first 4) confirms the cause. Test: cast techniques in VR: sharp as without the toolkit. If the log line is missing and effects stay pixelated: try toolkit `ui_sharpness = "smooth"`; if that
     fixes it, the toolkit sets filtering some other way (e.g. a low-res UI target), look there.
     (hook order, who is outermost) and look for a toolkit setting to reduce what it hooks. If no: it is our own
     effect handling (layers / glare, see item 3). Undo = rename back.
   - **Technique effects bug FIXED (2026-10-30):** not the toolkit's drawing, `ui_scale`, or its archive/minimap patches
     (all bisected, no effect). The toolkit's DLL calls `DrawPrimitiveUP` for the game, so `_ReturnAddress()` in our draw
     hooks was `toolkit_dinput8.dll`, not the game site `0x00747248`, and `ReprojectSpriteIfNeeded` (exact ret match)
     never fired. `PSUOverlay.cpp` now uses `GAME_RET()` (`GameRet`): if the caller is outside the exe, scan the stack for
     the first call site in the exe (`E8` or `FF /2`). User confirmed effects fine. Toolkit toml is back to the original
     (backups `.pre_uiscale`, `.pre_nodraw`, `.pre_nopatch`, `.bisect` in `RoguesToolkit\`).
     Note: the `StereoRenderer`/`VRHud` ret checks for other sites go through the same hooks, so they benefit too.
     The 'stereo submit miss' log is just the headset going inactive (OpenXR session state 3), not a bug.
   - **Toolkit panels on the HUD (plan Step 3, built + deployed 2026-10-30, UNTESTED):** the toolkit draws inside its own
     Present hook, which sits below ours, so our back-buffer capture never saw it. `Hook_Present` (`PSUOverlay.cpp`) now
     points the render target at the HUD user surface (`HudUserSurface`), depth off, full-surface viewport, around the real
     `Present` call; draws that arrive meanwhile are counted (`NOTE_TOOLKIT_DRAW`) and, if any, `MarkUserContent()` is set
     (log once: `VRToolkit: N draws inside Present went to the HUD panel`). Setting `toolkitOnHud` (default on; checkbox
     "Toolkit panels on the VR HUD" in VR Settings > General). Side effect: toolkit panels leave the flat monitor while on.
     Guess to verify: that the toolkit does not rebind the back buffer itself (its SetRenderTarget hook); if panels still
     only show on the monitor, the log line is missing and its draws bypass these hooks (e.g. a different vtable/thunk).
   - **Toolkit menu + VR pointer (built, UNTESTED):** the menu's open state cannot be observed, so `VRBindings::Poll` assumes
     it toggles on a plain F6 (ours sent, or the real key) and closes on Esc (logs `VRToolkit: menu assumed open/closed`).
     While assumed open and `toolkitPointer` is on (default, checkbox "Pointer drives the toolkit menu"), `VRUiPointer`
     (`SetToolkitMenuOpen`) is wanted and `DriveSystemMouse()` moves the system cursor (panel pixel -> client pixel) and sends
     left down/up from the trigger and wheel from the right stick, so the toolkit sees a normal mouse. Needs the game window
     foreground. If the menu is closed by its own button the assumption is stale: press F6 twice or Esc to resync.
   - **Update (user confirmed panels work in the headset):** the toolkit does not draw inside the real Present; it opens its own
     scene (`BeginScene`/`EndScene` called from `toolkit_dinput8.dll`, text via `d3dx9_43.dll` DIP, DP-UP). `Hook_BeginScene` /
     `Hook_EndScene` now detect a caller outside the game exe and this DLL (`CallerIsOther`) and redirect that scene to the HUD
     user surface (`BeginToolkitRedirect`/`EndToolkitRedirect`), marking the panel; the Present-window path remains as fallback.
     Log `VRToolkit: <call> from <module>` (first 24). Pointer fix (built, deploy + test pending): menu open state is now toggled
     when WE send F6 (previously only focused physical F6 was seen, so VR never switched the pointer on); before sending F6 and
     for the mouse, the game window is found with `VRBindings::GameWindow()` and brought to the foreground.
   - **Fixes after the first VR test (built + deployed, untested):** (1) the free-aim reticle (and other draws of our own, e.g.
     beams) vanished with the toolkit: `GameRet` scanned the stack for a game call site even when the caller was our own DLL
     (`VRControllers::DrawReticle` through the device), so our draws were taken for game draws; `GameRet` now keeps the return
     address when it is in the exe or in our DLL (`InOwnModule`). (2) The pointer could not click: `DriveSystemMouse` only
     worked with the game window in the foreground (VR leaves it behind); now it uses `VRBindings::GameWindow()`, real mouse
     input when that window is in front, else `PostMessage` WM_MOUSEMOVE/LBUTTON to it. Log `VRToolkit: pointer real
     input|posted messages hit= click=` (first 12). If clicks still do nothing with "posted messages", the toolkit reads the
     cursor itself (GetCursorPos / raw input) and needs the window in front.
   - **Second round (built + deployed, untested):** user: reticle still missing, shortcut often did not open the menu, clicks
     moved the cursor but did not register (log said `posted messages`: the game window was behind). Changes:
     `VRBindings::BringGameToFront()` (AttachThreadInput + SetForegroundWindow) before sending F6 (waits up to 0.8 s for focus)
     and, throttled to 0.4 s, from `DriveSystemMouse`, so real `mouse_event` input is used (the toolkit reads the button
     state, not messages). Reticle hypothesis: the toolkit's `DrawPrimitiveUP` hook (UI scaling, ui_scale 2.0) moves our
     screen-space marker verts, so `VRControllers::DrawMarkers` now draws through a one-call dynamic VB + `DrawPrimitive`
     (`DrawVerts`). If the reticle is still missing, test toolkit `ui_scale = 1.0` to confirm the cause.
   - **Round 2 result:** clicks and the shortcut work, our reticle is back. **Open:** a white "+" shaped flash after each free-aim
     shot, not there without the toolkit. Hypothesis: the game's reticle pieces (layer 48 crosslines/ticks, `IsGameReticle` in
     `VRHud.cpp`) no longer match its size thresholds because the toolkit's `ui_scale` 2.0 enlarges the game's UI. A log
     `VRHud reticle: layer 48 piece box=…` (first 80, built, NOT yet deployed because the game was running) will show their sizes;
     quick check: toolkit `ui_scale = 1.0` (then the flash should be gone) and tune the thresholds from the log.
   - **"+" flash result:** persists with toolkit `ui_scale` 1.0, so not the scale. The log shows the reticle's tick sprites
     (66x66 at y 357 / 1083, 88x50 at x 796 / 1764) sitting just outside `IsGameReticle`'s bounds (dy 363 > 0.25 h); they
     spread further out right after a shot, so they escape the matcher for a moment. Widened in `VRHud.cpp`: square sprite
     vertical 0.25 -> 0.4 of height, layer-48 central horizontal 0.3 -> 0.45 of width (built, NOT deployed - game was
     running). Test: fire in manual aim; the "+" should be gone. Check nothing else got hidden while aiming.
   - **"+" flash, correction (2026-10-30):** the widened `IsGameReticle` bounds (deployed) did NOT fix it, and the user clarifies it is
     not a UI element: a sudden cross-shaped flash spanning the area where the shot lands, for a split second (looks like a
     world impact sprite / glare star in the eyes). Not `ui_scale` (same at 1.0). Glare layer counters are 0 and `eyeGlare` is off,
     so not the TGlare blend. Suspects: an impact/muzzle sprite whose draw site is no longer recognized because the toolkit sits
     between the game and our hooks (`GAME_RET` stack scan may pick a wrong or stale game call site), or a draw path the toolkit
     reroutes. Next: (1) confirm by renaming `toolkit_dinput8.dll` -> `.off` (cross should vanish); (2) find the draw with the
     Debug draw-site bisect (Ctrl+Shift+Insert / Delete) while shooting; (3) check its return address against what `GAME_RET()`
     reports (log both); `VREffect: new #N other ret=…` lines show every new site.
     **Session notes (2026-10-30):** strongest lead: `ReprojectSpriteIfNeeded` only treats a draw as the game's world sprite when
     `ret == 0x00747248` exactly, so any other/wrong `GAME_RET()` leaves an impact sprite as a flat screen quad in each eye.
     Added Debug-only logs (built, NOT deployed - game was running): `VRToolkit: eye UP draw direct ret=… scanned game ret=…`
     (eye-pass UP draws whose direct caller is not the game) and `Stereo: eye XYZRHW draw with projected z from ret=…`
     (XYZRHW draws with z in (0,1) from a site other than 0x747248). Test: deploy `Debug\PSUExD.dll`, fire a few manual-aim shots,
     `tools/lastlog.sh "VRToolkit: eye UP|eye XYZRHW"`; if the flash site shows up there, route it through the sprite path.
     Still to do: step (1), rename `toolkit_dinput8.dll` -> `.off` to confirm the cause.
     **Result (2026-10-30):** NOT the toolkit (flash also with `toolkit_dinput8.dll.off`); the user's layer bisect says layer 0x0F
     (enemies, `TEnemyComponent`). `VRLayer0F:` log: one draw, `DrawIndexedPrimitive` ret 0x0074E4FD, 2 prims, fvf 0x102, 32x32 tex,
     blend ONE/ONE, z on / zwrite off = the hit-spark billboard. Fix (built, NOT deployed - game running):
     `StereoRenderer::SkipEyeHitSpark` (eye passes only, layer 0x0F, 2 prims, additive, tex <= 32x32) called from
     `Hook_DrawIndexedPrimitive`. Test: shoot enemies in manual aim, no cross; check enemy status effects/other additive bits
     still look right (restore `toolkit_dinput8.dll.off` -> `.dll` for toolkit use). `NoteLayerDraw` stays as Debug logging.
   - **Motion-controller shortcut (built + deployed, untested):** both grips + left stick pushed up (>0.8, once per push) sends
     `VRAction::ToggleToolkitMenu` (`VRSyntheticPad.cpp` before the layout loop; `VRMotionLayout` entry so the label cards and
     diagram show it; `VR_GUIDE.md`).
   - **Pending tests (Step 3 / Test E):** VR on, toolkit enabled: damage meter / drop panel / timers / HP bars visible on
     the HUD panel and readable; Ctrl+F6, pad Back+A and the General-tab button open the menu; menu visible in the headset
     and operable with the right controller (hover, click, scroll); `tools/lastlog.sh VRToolkit`. Toolkit `ui_scale` 2.0
     and `panel_x/y` are in game pixels: check placement on the panel. Not done: motion-controller gesture.

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

   **Body IK, phases 0-3 (built + deployed Debug 2026-10-29, untested):** rest-pose confirmed by the user (arms follow, weapon stays, height fit OK; `rest pose cached ... root ok`, `body fit 1.028`).
   Bone map from the `VRBones` dump (1 unit = 10 cm): 0 root, 1 pelvis, 2 hip node, **left leg 3 thigh / 4 knee / 5 ankle, right 6 / 7 / 8** (no toe bone), 9 waist, 10 chest, 11 shoulder level,
   12 neck, 13 head, 14-18 left arm, 19-23 right arm, 24/25 chest extras, 26 waist extra, 27 extra. New `VRHands.cpp` `BodyIk` (after the torso block, before the eye lean, needs `bodyRestPose`):
   `bodyIk` (first person; `VRCamera::HeadWorld`): hips take the head's height (down to -0.8 m, up 0.15 m) and 60% of its sideways offset, waist + chest each lean the neck toward the head (limited to 20 deg each);
   eye-lean is skipped while it is on. `bodyProceduralLegs`: ankle targets stay planted in the world; a foot steps (0.28 s, 9 cm lift, led by the hip velocity) when it is more than 35 cm from its home spot
   (under the pelvis, shoulder width) or the leg would over-stretch; one foot at a time; feet re-leveled to the pelvis' orientation; replaces `TurnLegs` (strafe legs). Both reuse `SolveArm` for the legs.
   Checkboxes under "Body in rest pose" (Camera > First person), settings in `psuex_vr.json` / `config.json`. Log: `tools/lastlog.sh 'VRBody'`.
   Pending test: crouch (hips drop, no mesh shear), lean forward, walk (feet plant and step, no sliding/crossing), stand still (feet planted shoulder width), turn on the spot (feet step), strafe.
   Not started: phase 4-6 (Standable / Vive tracker input via OpenXR, tracker -> IK, calibration).

   **Body IK update (2026-10-30, deployed Debug):** user confirms the body/camera position is right after the fixes (hips take the full head offset, neck = eye - `fpNeckDrop` and 20 cm back,
   body facing from the thigh line, real HMD position via `VRCamera::HeadWorld`). `bodyKeepLegAnim` ("Keep the game's leg animation"): rest pose skips bones 1-8, so the strafe layer's game leg
   animation stays (user: works like before). Planted-feet tuning (untested): step at 25 cm (was 35), step time 0.16-0.30 s by speed, lead 0.2 s of velocity, settle back to shoulder width after 0.35 s idle,
   legs never cross (each foot >= half the stance on its own side), knee pole 20% outward. Open: decide between planted feet and the game leg animation after the next test; tracker phases 4-6 not started.
