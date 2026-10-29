# PSU Client Address Map

Known addresses in the PSU Illuminus 14.30 client used by PSUEx. All are absolute (32-bit, fixed base).

## Local install

| Item | Path |
|---|---|
| Game folder | `D:\Games\PSUClementine_22072025` |
| Client exe | `D:\Games\PSUClementine_22072025\PSUC.exe` |
| RE binary | `D:\Games\PSUClementine_22072025\PSUC_Uncompressed.exe` (copy of `PSUC.exe`; this Clementine build is **not** UPX-packed) |
| Toolchain root | `D:\PSUExTools` (kept off the C: drive) |
| Ghidra project (GUI / MCP) | `D:\PSUExTools\ghidra-projects\PSUClementine` |
| Ghidra project (anchors fully created) | `D:\PSUExTools\ghidra-projects\PSUClementine_anchors` |
| Deploy Debug DLL as | `PSUEx.dll` (loader also tries `PSUExD.dll` first) |
| Loader | `dinput8.dll` proxy from `tools/dinput8_proxy` (see Load path) |

Clementine status:
- `unverified`: taken from existing PSUEx code (14.30); not yet checked against Clementine's `PSUC.exe`.
- `pe-match`: bytes at this VA look like a real function prologue in this Clementine build (static PE check).
- `ghidra-labeled`: renamed / prototyped / commented in Ghidra (live MCP and/or `PSUClementine_anchors`).
- `verified`: confirmed on Clementine by a runtime log or breakpoint.
- `differs`: Clementine uses a different address (put it in Notes).

Static PE check (2026-10-14): ImageBase `0x400000`, x86, **not UPX-packed**. All listed function VAs have plausible prologues. Globals are zeros on disk (runtime data).

Ghidra labeling (2026-10-14): Live MCP renamed `PSU_WinMain`, `PSU_InitializeDirect3D`, `PSU_InitializeDirectInput`, globals `g_pD3D` / `g_ViewMatrix` / `g_ProjectionMatrix`. Project copy `PSUClementine_anchors` created real functions for `TCameraTaskPlay_Update`, `TCameraTaskPlay_Refocus`, `PSU_UIDraw_Candidate`, `PSU_FileOpen`. Prefer opening **PSUClementine_anchors** in Ghidra for those four.

Scene↔UI hunt (2026-10-14): Live Ghidra (`PSUC_Uncompressed.exe`) still lacks `createFunction` at `0x0048DA90` / `0x00793C40` — open **PSUClementine_anchors** (or create them) before decompiling. Runtime breadcrumbs: `PSUOverlay/FrameTrace` logs the first 8 frames of `BeginScene` / `SetRenderTarget` / `SetTransform(VIEW|PROJ)` / `UIDraw(0x793C40)` / `EndScene` / `Present` with `_ReturnAddress()` into `psuex.log`.

## Load path

`PSUC.exe` imports system `dinput8.dll` and does **not** reference `PSUEx` by name. `di.dll` in the game folder is Clementine's launcher helper (`DIInit` / `DIUpdateInputState` from `PsuLauncherPC`); it does not load PSUEx.

An existing `config.json` in the game folder shows PSUEx has run before (likely via a manual injector). For reliable loading we ship `tools/dinput8_proxy`:

1. Build `dinput8.dll` (Win32) and place it next to `PSUC.exe`.
2. On process attach it `LoadLibrary`s `PSUExD.dll` / `PSUEx.dll` **only if the host is `PSUC.exe`** (`online.exe` also loads this `dinput8`; injecting there Detours PSUC VAs and AVs on char-list).
3. It forwards `DirectInput8Create` to `%SystemRoot%\SysWOW64\dinput8.dll`.

Debug: `psuex.log` in the game folder; `dinput8_proxy.log` should show `Skip PSUEx` for `online.exe` and `Loaded …` for `PSUC.exe`.

## Functions

| Address | Name | Signature | Used by | Clementine | Notes |
|---|---|---|---|---|---|
| `0x00774AD0` | PSU_WinMain | `int WINAPI (HINSTANCE, HINSTANCE, LPSTR, int)` | `Hook_EntryPoint.hpp` | ghidra-labeled | Live MCP rename + prototype |
| `0x00776590` | PSU_InitializeDirect3D | `char __cdecl (int)` | `Hook_InitializeD3D9.hpp` | verified | Runtime log 2026-10-14: hook hit; `g_pD3D` filled. VR: use `CreateDeviceEx` when Ex is active. D3D9Ex: remap `D3DPOOL_MANAGED` via `D3D9ExCompat` or game crashes (`d3d9.dll+0xF2A23`, read `0xB9197436`) |
| `0x00774F90` | PSU_InitializeDirectInput | `HRESULT __cdecl ()` | `Hook_00774F90.hpp` (disabled) | ghidra-labeled | Phase 3 input hook entry |
| `0x00787110` | PSU_CreateKeyboard | `void __cdecl ()` | — | ghidra | `CreateDevice(GUID_SysKeyboard)`, data format `0x00882694` (256 bytes), cooperative level 0x16. |
| `0x00784D70` | PSU_CreateMouse | `int* __fastcall (int*)` | — | ghidra | `CreateDevice(GUID_SysMouse)`, data format `0x00882284` (DIMOUSESTATE2, 0x14 bytes), level 6. |
| `0x007870B0` | PSU_InitGamepads | `void __cdecl ()` | `dinput8_proxy` (EnumDevices / CreateDevice / GetDeviceState hooks) | ghidra | `EnumDevices(DI8DEVCLASS_GAMECTRL, ATTACHEDONLY)` with callback `0x00787060` (max 2 pads); `0x00786E80` creates each: data format `0x0088248C` (DIJOYSTATE2, 0x110 bytes), level 5 (exclusive foreground), axis range -1000..1000 (`0x00786880`). Pad slots `0x00A8DCB0 + i*0x84`, device at `+0x2C`. The proxy merges the motion-controller virtual pad into pad 1's `GetDeviceState`. |
| `0x00870518` | g_PadButtonBits | `uint32_t[20]` | `PSUVR/VRSyntheticPad.cpp` (names only) | ghidra | Logical pad bits for the `[gamepad1]` keys (strings `0x0086EEEC..0x0086F004`): A 0x1, B 0x2, X 0x4, Y 0x8, L1 0x10, R1 0x20, SELECT 0x40, START 0x80, L2 0x100, R2 0x200, L3 0x400, R3 0x800, U 0x10000000, D 0x20000000, L 0x40000000, R 0x80000000. In menus A cancels and B confirms. |
| `0x00786C90` | PSU_PollGamepads | `void __cdecl ()` | `PSUVR/VRProbe.cpp` (watch) | ghidra | Per frame, per pad: `Poll`, `GetDeviceState(0x110)`, POV to U/R/D/L bits, DI buttons to logical bits through `0x00A8DE00`. Writes the pad state below. |
| `0x00A8DDB8` | g_PadState | `uint32_t[9]` per pad (stride 0x24) | `PSUVR/VRProbe.cpp` | ghidra | +0 held bits, +4 ~held, +8 pressed this frame, +0xC released, +0x10..+0x1C stick floats (left X/Y, right X/Y). A later injection point for motion actions (local input only). |
| `0x004BF170` | PSU_LoadKeyboardConfig | `void __cdecl (LPCSTR ini)` | — | ghidra | Reads `[keyboard]`. Action table `0x009922D0`, 40-byte entries: +0xC name, +0x10.. up to 6 input slots; writes `0x00992258[slot] = key`. Slots: `PL_ATK_A` 0x09, `PL_ATK_B` 0x0A, `PL_SBJ` 0x0B/0x0D, `PL_PALETTE0` 0x10, `PL_MOVE_F/B/L/R` 0x12-0x15 and 0x20-0x23, `PL_CAM_BACK` 0x16/0x18, `PL_VEHICLE_ACC` 0x24, `WND_AP_CP` 0x2C, `WND_MM_OPEN` 0x2F, `WND_CONVERT` 0x07/0x3C/0x41, `WND_MM_SC1..7` 0x46-0x4C, `WND_CS_UP/DOWN/DECIDE/CANCEL` 0x50-0x53, `ML_ANS` 0x55, `SND_MOVE_*` 0x5F-0x62. |
| `0x007738E0` | PSU_KeyboardSetHeldKeys | `bool __stdcall (const uint8_t keys[14], uint32_t count)` | `PSUDetour/Hook_007738E0_KeyboardKeys.hpp` (**hooked**: VR speech adds the chat key / Enter) | ghidra + disasm 2026-10-20 | Called once per keyboard poll from `0x00783B80`. Copies the held game key codes into `0x00962D64` (count `0x00962D74`, max 14); `0x00773AD0` then builds the pressed (`0x00962DF8`) / released (`0x00962E07`) bit sets that `0x007737A0(code)` (pressed) and `0x00773770(code)` (held) read. Keys come from `GetKeyboardState` (`0x00783AD0`; all keys cleared when the game window isn't foreground) mapped through the VK -> game code table `0x00870280` (VK_RETURN 0x0D -> 0x38, VK_SPACE -> 0x45, VK_BACK -> 0x34, Esc -> 0x31, 'A'..'Z' -> 0x01..0x1A, F1..F12 -> 0x25..0x30, LCtrl/RCtrl 0x39/0x67, LAlt/RAlt 0x44/0x6A). Injecting here works without focus. |
| `0x00992258` | g_KeyboardActionKeys | `uint8_t[]` game key codes | `PSUVR/VRSpeech.cpp` (reads slot 7) | ghidra 2026-10-20 | Filled by `PSU_LoadKeyboardConfig`. Defaults (exe data): [0] 0x38 Enter, [7] 0x45 Space = **open chat** ([keyboard] WND_CONVERT), [8] 0x6B Pause. The chat controller opens its input on slot 7 / 8 with input context bit 0x1000 (`0x004C2010`). |
| `0x004C9070` | PSU_TextInputWndProc | `LRESULT CALLBACK (HWND, UINT, WPARAM, LPARAM)` | — (VR speech posts WM_CHAR to it) | ghidra 2026-10-20 | Subclass of the game window set by `0x004C7CE0` (IME setup, old proc at `0x00B9D490`). The main proc `0x00776060` ignores WM_KEYDOWN / WM_CHAR; this one handles IME messages and **WM_CHAR** -> `0x004C9370` (EAX = wParam): while `0x00B9D4AB` (text box takes typing) != 0 and `0x00B9D4B0` == 1, characters 0x20..0x7E (0xA0+ only on a Japanese layout) are appended via `0x004C4070`. `0x004C9B70` starts text input (sets `0x00B9D4AC` / `0x00B9D4AD`, applied next frame by `0x004C9410`), `0x004C9C20` ends it. |
| `0x008CA35C` | `TChatCtrl` vtable | — | — | RTTI 2026-10-20 | Update `0x004C18C0` (state `+8`: 2 = field). Chat open: `0x004C2010` (open key) -> `0x004C1D80`; F1..F12 (codes 0x25..0x30) send shortcut chat (`0x004C2630` -> `0x004C2730`); `0x004C2240` handles keys in the box (0x34 / 0x7E with an empty box closes it). `TChatCtrlTask` vtable `0x008CA32C`, `TChatStack` `0x008CA380`. Globals: `0x00B9D478` chat control, `0x00B9D480` chat window, `0x00B9D488` edit. Never touches the network side: VR speech only presses keys and posts WM_CHAR. |
| weapon `+0x48` | Technique weapon slots | `struct { uint8_t id, level; } slot[4]` | `PSUStructs/PSUCombat.h` (`ReadTechSlots`) | ghidra 2026-10-20 (id -> name order **unverified**) | `TItemWpnTech` (vtable `0x008CC4EC`; weapon object `player+0x2E0` right / `+0x2E4` left). Slot = (button - 2) + hand * 2, button 2 = X, 3 = Y, hand = left weapon active (`player+0x230` bit 1): `+0x80` `0x004DE530` can-cast, `+0xC8` `0x004DE7C0` input mask, `0x004DE400` id lookup (via `+0x88` technique data). `0x004DE4D0` matches a disk item (type 6) as group = (id - 1) / 10 + 1, entry = (id - 1) % 10; `0x004DD900` clears all 4. Effect names by element in `0x009C63C0` (foie group, barta group, zonde group, ...). Staff: slots 0 / 1, rod: 0..3, TCSM: left object. |
| `0x00B9CC98` | g_InputPlayers | 4 x 0x1F8 bytes | `PSUVR/VRProbe.cpp` (watch) | ghidra | Input manager per player (init `0x004BE770`): +0 index, +4 0x75-dword slot -> pad button table (defaults copied from `0x0089D830`; values not decoded yet), then 8 state dwords. `0x0053A920` rewrites slots 0x10/0x16/0x18 by an option (camera/palette type). |
| `0x00B9CC80` | g_InputContext | `uint32_t` (+ `0x00B9CC84`, `0x00B9CC88`) | `PSUVR/VRSyntheticPad.cpp` (auto D-pad) | runtime (context probe 2026-10-16) | `0x1007` / `0x1007` / 5 while moving freely; `0x0405` / `0x0401` / 0 while a menu has the input (main menu, shops, mission counter, login screens). The weapon / item palette does not change it: that is `player+0x700` top byte (0x07 while open with A held, counts down 6..0 as it closes; runtime 2026-10-16). |
| `[0x009CA8AC]` | Local player struct fields | offsets | `PSUVR/VRProbe.cpp` | runtime (probe diffs 2026-10-15) | `+0x54..+0xBC` rotation matrices; `+0x124` facing, 16-bit angle (0x10000 = 360°), copy at `+0xD0C`; weapon switch changes `+0x1F0` (0x12 <-> 0x06, likely weapon class) and `+0x308`/`+0x30C` (0x7DA/0x7DB -> 0x7D9/-1, likely right/left equipped item ids), also `+0x1DC`, `+0x1F4`, `+0x2D4`, `+0x2D8`, `+0x854`; a Photon Art sets `+0x620..+0x628` = 0x499 (likely PA id) and `+0x61C` byte 2 = 1; `+0x20C` byte 1 changes per action (4 attack, 2 combo, 3 PA). Lock-on changed nothing in the first 0x1000 bytes except `+0x288..+0x290`. |
| `[0x009CA8AC]` | Local player melee combo state | offsets | `PSUStructs/PSUCombat.h`, `PSUVR/VRSyntheticPad.cpp` (combo buzz) | runtime (live probe 2026-10-15, saber) | `+0x710` = allowed-input mask (see `0x0053DEB0`; `0x0E` was Sevara's mask inside the window, other weapons differ: use the motion table `0x005366E0` for the window): `0x02` swinging, `0x0E` window open on Sevara, `0x0A` after the 3rd hit, `0x30E`/`0x302` returning, `0x0080030E` idle in stance. `+0x1E4` combo step 0x11/0x12/0x13 (0 idle; `+0x170` = current motion id, 0x14-0x16 for hits 1-3). `+0x700` bits 16-18 = hits done. `+0x7B4` 0x01000000 = an early press happened (cleared by the next accepted hit). `+0x00C`/`+0x010` = the swing's hit object (~0.5 s). `+0x704` stance countdown from 0x384 frames. `+0x188` bit 0 pulses for ~80 ms on each accepted press. Windows measured: hit 1 → ~500 ms, hit 2 → ~375 ms after the press. |
| `[0x009CA8AC]` | Local player weapons | offsets | `PSUStructs/PSUCombat.h` | runtime (press probe 2026-10-15) | `+0x1F0` right-hand weapon class, `+0x1F4` left-hand class (-1 none): 0x04 twin knuckles, 0x06 spear, 0x12 saber, 0x22 handgun (high digit: 0 two-handed, 1 right 1H, 2 left 1H). `+0x308`/`+0x30C` right/left item ids. `+0x1DC` = active weapon class << 8 (R1 switches it to the left weapon). `+0x2D4`/`+0x2D8` = right / left weapon **category** (0 none; force-weapon test 2026-10-19): 1 two-handed melee and twins, 2 two-handed guns (sniper, shotgun, bow `0x1E`, twin handguns `0x21`; the bow is held in the **left** hand: its arrows start ~2.8 units from the left weapon bone), 3 rod (`0x26`, 4 technique slots), 7 one-handed melee, 8 staff (`0x28`, 2 slots), 9 handgun, A machinegun, B TCSM (left `0x28`, 2 slots, cast with R1 + X / Y), C RCSM (left `0x28`). Staff, TCSM and RCSM share class `0x28` and R1 doesn't change `+0x1DC` for them or the rod: only the category tells them apart. Buttons (all three sets): X = normal attack (`+0x1E4` 0x11..), Y = Photon Art (`+0x1E4` 0x21..), R1 **held** = the left-hand weapon is the active one (`+0x1DC` 0x22xx while held, back to 0x12xx on release; `+0x710` 0x..06 during the switch); X while R1 is held fires the handgun. A/B did nothing in the field. |
| `0x008CE984` | TPlayer (local player class) | class layout | `PSUStructs/PSUCombat.h` | RTTI (2026-10-20) | RTTI `.?AVTPlayer@@` (TD `0x009D18EC`). Vtables: `0x008CE984` (+0), `0x008CE97C` (+0x50), `0x008CE974` (+0x1E0, base **TPlyCombo** `0x009D18B8`), `0x008CE96C` (+0x1E8, base **TPlyMotionSet** `0x009D18A0`); COLs `0x00937834..0x00937870`. Written in the dtor `0x00526060` (ctor around `0x00525105`). TPlyCombo holds one dword: `+0x1E4` = combo step. TPlyMotionSet starts at `+0x1E8`, so the weapon classes `+0x1F0` / `+0x1F4` are its fields. Direct calls on `player+0x1E8`: `0x0067E780` (4 sites, looks like a constructor of another object, not the motion set), `0x00582950`, `0x00517510`, `0x00547C30`, `0x004D0930`; on `+0x1E0`: `0x00778480`, `0x0062FF80` (unchecked). The Just Attack window lookup (per weapon type and step, from the motion set) is not found yet; Ghidra had only 2,025 functions analysed. |
| `0x009CBD28` | g_PlayerStateTable | 0x28-byte entries, index = `player+0x20C` (byte; `+0x20D` = previous state) | — | static init `0x00868800` (2026-10-20) | Filled at startup (zeros in the file). Entry: +0x00 flags (copied to `player+0x254` on entering the state), +0x08 allowed-input mask, +0x10 enter, +0x18 update, +0x20 exit. States 2 and 3 share enter `0x00528630` / update `0x005287F0` / exit `0x005288E0`; state 1 `0x00527BF0` / `0x00527C50` / `0x00527C90`; state 4 `0x00529240` / `0x00421180` / `0x00529C60`. State change: `FUN_00528C70` and siblings `0x00528D40..0x0052B4A0` (write `+0x20C`, `+0x1E4`, `+0x254`). |
| `0x0053DEB0` | PSU_PlayerBuildInputMask | `void __fastcall (TPlayer*)` | — | ghidra (2026-10-20) | Writes **`player+0x710` = allowed-input mask** = (state table +0x08 | `FUN_00544130` | 0x300 extras) & ~blocked (0xC000000C blocks attack/PA). So `+0x710 == 0x0E` is "attack + PA allowed", not a combo state; bit 4 = X (attack), bit 8 = Y (Photon Art), 0x40000000 / 0x80000000 = gun / tech buttons. |
| `0x00544130` | PSU_WeaponInputMask | `uint __fastcall` (EAX = `player+0x2C8`) | — | ghidra (2026-10-20) | Switch on the weapon **category** (`+0x2D4` right, `+0x2D8` left via `+0x2CC..+0x2D8`): 1 / 7 (melee) -> `FUN_005443E0`; 2 / 5 / 9 guns and 3 / 8 / 0xB techs ask the weapon object (vtable +0x80 / +0xC8). |
| `0x005443E0` | PSU_MeleeInputMask | `byte (char stateFlagBit0)` | — | ghidra (2026-10-20) | Weapon object = `player+0x2E0` (+4 for the left hand, chosen by `player+0x230` bit 1). State flag bit 0 set: `FUN_004CD8A0` (a weapon permission / restriction check, `DAT_00B9D4DC`, not timing) -> X allowed. Clear (attack states): hit index `FUN_00544BE0()` 0..1 -> X allowed, Y if weapon vtable `+0xF8`(3); 2..4 -> Y only if vtable `+0xF8`(n+1). The weapon object is a `TItemWeapon` (`0x008CB8DC`) or subclass; melee ones are `TItemWpnKnock` (vtable `0x008CC114`; `+0xF8` `0x004DBE50` and `+0x100` `0x004DB300` are Photon Art level / data checks, not timing). Others: `TItemWpnSubGun` `0x008CC364`, `TItemWpnTech` `0x008CC4EC`, `TItemWpnMagAuto` `0x008CD1EC`. |
| `0x00544BE0` | PSU_ComboHitIndex | `int (EAX = +0x1E4 step)` | — | ghidra (2026-10-20) | 0x11/0x12/0x13 -> 0/1/2, 0x21..0x24 -> 3, 0x32..0x35 -> 4, 0x43..0x46 -> 5 (Photon Art levels), else -1. |
| `0x005366E0` | PSU_PlayerMotionEntry | `float* __fastcall (TPlayer*)` | `PSUStructs/PSUCombat.h` (`ReadMotion`) | ghidra + runtime verified (2026-10-20: sword 0x02, knuckles 0x04, spear 0x06, twin daggers 0x0C, saber 0x12, dagger 0x14; buzz at the window start gave JUST ATTACK on all) | **Motion timing entry of the current motion, per weapon type.** `[0x00B9DAA4]` (needs `+4` != 0) `+0x40 + class*4` -> ptr -> `[ptr] + motion*0x40 + 0x38` -> entry; class = byte `player+0x1DD` (active weapon class, 0..0x3D), motion = `player+0x170` (vtable `+0xBC` `0x00542770`). Default entry `0x008AA3BC`. Entry floats in motion frames, -1 = unused: `+0x00` / `+0x04` checked by `0x00533CC0` / `0x00533C80` (states 1..11, frame crossed); **`+0x08` / `+0x0C` = next-input window** (`0x0053E2D0`: attack / PA allowed only while `+0x08 <= frame <= +0x0C`, used by `0x0053DEB0`) = the Just Attack window (examples: saber 13..27 / 13..27 / 14..29, spear 14..28 / 10..24 / 11..37, sword 24..38 / 20..30 / 33..51 for hits 1..3, motions 0x14..0x16); `+0x10` and `+0x1C..+0x38` (4 pairs) hit frames (`0x00528F80`, `0x00531B60`, `0x0052BDD0`, `0x004DC2C0`; `0x005312D0` copies min hit - 3 / max hit + 1 to `[player+0x6B0]+4/+8`); `+0x14` used by `0x00533740` (melee only, auto step). Loaded from the `MotTbl_*.xnr` files. |
| `0x0053E2D0` / `0x0053E350` | PSU_InNextInputWindow / PSU_PastNextInputWindow | `bool (ESI = TPlayer*)` | — | ghidra (2026-10-20) | `0x0053E2D0`: true if the entry has no window or `+0x08 <= frame <= +0x0C`; false blocks attack + PA (`0xC000000C`) in `0x0053DEB0`. `0x0053E350`: in states 2 / 3, frame >= `+0x0C` -> adds `0x300` to the input mask. So `+0x710 == 0x0E` was only Sevara's mask inside the window; other weapons add other bits. |
| `0x00528F80` | PSU_PlayerState2Update | `void (EAX = TPlayer*)` | — | ghidra (2026-10-20) | Attack states 2 / 3 per-frame update (hit frames from the motion entry -> weapon vtable `+0x68` / `+0x6C`, `0x00533740`, return to state 1). `0x005287F0` is the exit (calls weapon `+0x140`, `+0x100`(3)). `player+0x222` (ushort) = frames in the current state; `0x005349C0` = set state (EBX = state, arg1 -> `+0x1E4`). |
| `0x005532F0` | PSU_RenderMotionFrame | `float __thiscall (render)` | `PSUStructs/PSUCombat.h` (`ReadMotion`) | ghidra + runtime verified (2026-10-20) | Current animation frame of a render object (`player+0x160`; player vtable `+0xCC` `0x00542730`): mode `+0x28` 0 -> `[[+0x04]+0x84]` +0x2C, 1..4 -> `[[+0x20]+0xA8]` +0x2C (float). `0x005530E0` (vtable `+0xD4`) = "frame crossed". |
| `0x00793C40` | PSU_UIDraw_Candidate | `int __thiscall (void*, int, int, void*, int)` | `Hook_00774F90.hpp` (**hooked** for FrameTrace) | ghidra-labeled | Slot 0/3 of UI vtable at `0x00870948`. **UI draw — not the 3D scene.** Stereo must stop before this. |
| `0x0048DA90` | TCameraTaskPlay_Update | `int __thiscall (CameraTaskPlayData*)` | `Hook_TCameraTaskPlay.hpp` | verified | Slot 0 of camera vtable at `0x008C7764`. Runtime log hits when in play. Writes orbit camera; matrices still come from elsewhere. |
| `[task+0x20]` | Camera task state byte | `uint8_t` | `PSUDetour/Hook_TCameraTaskPlay.hpp` | runtime (probe 2026-10-16) | 1 follow, 2 LB held, 3 refocus, **4 free aim (R3 with a gun; the active weapon switches to the left gun, `+0x710` 0x306)**, 5 NPC talk. In VR the hook leaves state 4 alone (no rate zeroing, no first/third-person steer). LB with a gun did not lock onto a target (no target pointer found in the player struct or camera task); the player struct holds a nearby-target list around `+0x1790..+0x1880` (0x1C-byte entries: flags, counter, x/y/z; 0x111 flags while locking). |
| `0x0048E4C0` | TCameraTaskPlay_Refocus | `int __thiscall (void*)` | `Hook_TCameraTaskPlay.hpp` (disabled) | ghidra-labeled | Later slot of camera vtable `0x008C7764` |
| `0x00479E70` | PSU_MainLoopPump | `void __cdecl (void)` | — | ghidra-labeled | Message/pump loop; repeatedly calls `PSU_FrameRender` |
| `0x00479F50` | PSU_FrameRender | `unsigned char __cdecl (void)` | `Hook_FrameRender.hpp` | verified | **Per-frame orchestrator.** Order: begin → `PSU_BeginOffscreenRT` → **mode scene callback** → `FUN_00772800` → `FUN_0047a080` → `PSU_EndOffscreenToBB`. FrameTrace 2026-10-14. |
| `0x0046F080` | PSU_SceneMode1_Callback | `void __cdecl (void)` (hypothesis) | `Hook_FrameRender.hpp` | verified | Mode-1 entry from table `0x0099325C + mode*0x10`. Runtime: `callback=0046F080` (mode=1, slot `0x0099326C`). Contains UI (`UIDraw` while offscreen). **Town DIP does not return inside this callback.** |
| `0x0074E490` | PSU_DrawIndexed_Wrapper | `void __cdecl (…)` (~2 stack args; sites `add esp,8`) | FrameTrace (observe) | verified | Town: all `DrawIndexedPrimitive` returns were `0x0074E4FD` **outside** SceneCB (`DIP(out)`). Body: `call [ecx+0x148]` (D3D9 DIP vtable slot 82) on `g_pD3DDevice_ba8c40` (`0x00BA8C40`). ≥17 callers in `0x00750xxx`–`0x00763xxx`. Leaf draw helper — not a safe stereo double-call by itself. |
| `0x004FF1B0` | PSU_DrawAgg_Candidate | `int __thiscall (void*, int)` (`retn 4`; push `0xC47`) | `Hook_FrameRender.hpp` | pe-match | 12 E8 sites. Town hits vary by session; one session showed calls from `0x0065157C` only. Not confirmed as the world-pass root. |
| `0x004FD990` | PSU_RenderPassA | `void __cdecl (void)` | `Hook_FrameRender.hpp` | pe-match | Draws render layers `0x02`–`0x29` via `0x00560830`. Called from all 4 scene callbacks (`0x46F0D3`, `0x4663B3`, `0x46AD2C`, `0x474C53`). Town 3D draws are under it (stack chain). Stereo candidate. |
| `0x004FDBC0` | PSU_RenderPassB | `void __cdecl (void)` | `Hook_FrameRender.hpp` | pe-match | Draws layers `0x2B`–`0x3F`. Called right after pass A. Likely 2D/UI. |
| `0x005620A0` | PSU_FlushDeferredLists | `void __cdecl (void)` | `Hook_FrameRender.hpp` | pe-match | Called in pass A between layers `0x0C` and `0x0D`, only if `[0xB9DB88] & 1` (set at pass A entry, cleared here). Draws the lists `[0xB911A4]` and `[0xB911A8]` (`[0xB911A8]` is wrapped by callbacks `[0xB911B0]`/`[0xB911AC]`) via `0x005621F0`, which **empties** them. Zeroes `0xB9DB98`–`0xB9DBA0`. Verified (`vr.debugDoublePass`, town): both lists are empty at pass A entry and refilled during the pass (`listA4 0->1`) on both calls. The second call draws the same 165 DIPs, and animation speed is unchanged. **Pass A is repeat-safe.** |
| `0x005E3350` | PSU_ShadowMapTarget | `int __cdecl (void)` | — | pe-match | Read-only: returns the shadow-map object `[[0xB9E504]+0x88]` when shadows are on. |
| `0x005E3630` / `0x005E37B0` | PSU_ShadowBegin / PSU_ShadowEnd | `void __cdecl (void)` | — | pe-match | Bracket layer 2 at the start of pass A: render the shadow casters into the shadow RT, then composite. Eye-independent, so it could run once per frame. |
| (runtime) | Stereo screen-quad skip | — | `StereoRenderer` + `PSUOverlay` DP/DP-UP/DIP/StretchRect | verified | Pass A includes XYZRHW quads that sample RT textures filled during the **flat** pass (glare/afterimage). Re-running pass A per eye still samples that flat copy → translucent mono ghost over stereo. With `vr.stereoSkipScreenQuads` (default true), eye passes skip XYZRHW draws whose stage-0 texture is an RT **not** written this eye (SetRT / StretchRect dest). Quads that sample RTs rewritten during the eye pass (e.g. shadow composite) still draw. Log lines: `StereoQuad:` / `StereoStretch:`. |
| `0x00560830` | PSU_DrawLayer | custom: layer index in `EDI` | — | pe-match | Walks a layer's node list and calls each node's draw callback (`call [node+0x10]` @ `0x0056086A`). 57 callers. Don't Detour without a naked stub. |
| `0x0056086D` | PSU_DrawLayer callback return | code address | — | runtime (stack scan) | Seen on the stack for pass-B layer-object draws: nameplates, NPC markers (z = projected depth 0.94-0.98); the own nameplate (object fn around `0x0065FEC3`) has z = 0 at screen center. Stack scans are unreliable (stale values); use the exact layer from `Hook_PassB` instead. |
| `0x004FDBC0` | PSU_RenderPassB | `void __cdecl (void)` | `Hook_FrameRender.hpp` | disasm | Exactly `mov edi, layer; call PSU_DrawLayer` for layers `0x2B..0x3F`. `Hook_PassB` runs that loop itself and records the current layer (`VRHud::SetPassBLayer`). Don't attribute draws by scanning the stack for these return addresses: stale copies in live frames give wrong layers. |
| `0x00B9DB38` | g_DrawLayers | `DrawLayer*` (64 × 8 bytes) | `PSUVR/VRPlayerDraw.cpp` | disasm | Entry `+0` = first node, `+6` = layer-disabled byte. Node: `+0x08` next, `+0x0C` object, `+0x10` `cdecl void cb( object )`; skipped when `object+8 & 0x223`. First person swaps the local player's node callback for a thunk to know when its meshes draw. |
| `0x00560D10` | Local player draw node callback | `void __cdecl (obj)` | `PSUVR/VRPlayerDraw.cpp` (thunk) | runtime + headless decompile 2026-10-16 | Calls obj vtable `+0x0C` (update) and `+0x10` (draw). The local player draw object is a `TPlayerPad` (vtable `0x008CEF24`; update `0x00526A40`, draw `0x00526A80`). Draw: render object at `obj+0x160`, models at render `+0x04` / `+0x20` / `+0x24`; `0x005525A0` attaches items per bone from the name table `0x00994560` (Head, R_weapon, L_weapon, R_forearm, L_forearm, Navel, Spine1) via `0x00757500(skeleton names, name)` -> bone index and model vtable `+0x18` (bone matrix). Player arm bones: `L_upperarm` / `L_forearm` / `L_hand` (and `R_`). Decompile with `DecompileTree.java` (headless, PSUClementine_anchors) since the live DB lacks these functions. |
| `0x0074B760` | Bone pose build (bone-indexed) | `void __cdecl (float *out, skeleton, localPose, root)` | `PSUDetour/Hook_0074B760_BonePose.hpp` -> `PSUVR/VRHands.cpp` | runtime (hands IK 2026-10-16) | Per frame from the model pose update `0x004FFB30` (non-negative `model+0x14` byte) with `out = model+0x64`: one 4x4 per bone, **model space**, parent (record `+6`) times local T/R/S (localPose 0x30 bytes per bone). The loaded player character (`TDivModel`) takes this path. VR hands edit the arm bones right after it. |
| `0x0074D1D0` / `0x0074CFD0` | Skinned pose build / recursive skeleton walk | `cdecl (out, attachments, skeleton, localPose, root, stack, flags)` / `cdecl (int bone)` | — (hook removed) | runtime 2026-10-16 | The other branch of `0x004FFB30` (negative `model+0x14` byte, e.g. the loading placeholder `TModel`): writes skinning matrices (inverse bind x bone) by palette slot (record `+4`) into `[0x00A85BC0]`; records at `[0x00A85BA0]`. |
| `0x004FFB30` | Model pose update | register args (ESI model, EBX skeleton, EDI localPose) | — | headless disasm 2026-10-16 | Picks `0x0074D1D0` or `0x0074B760` by the sign of `model+0x14` byte 0; the latter path then calls `[model+0x70](pose)` if set. |
| `0x004FFFA0` | Model bone query (vtable `+0x18`) | `char __thiscall (model, float *inout, int bone, int, int, int)` | `PSUVR/VRHands.cpp` (model -> world) | runtime 2026-10-16 | World bone matrix, multiplied into the matrix passed in (start from identity), returned transposed (translation at [3], [7], [11]). **Re-runs the pose build**, so never call it from inside the pose hook. |
| `0x005089C0` | Render object near-camera test | `uint __fastcall (obj)` | `PSUDetour/Hook_005089C0_NearCull.hpp` | headless decompile 2026-10-16 | Nonzero = hidden. Below the distance at `0x00993368` (obj `+0x10`) it asks the object (vtable `+0x0C`). Called from the render object draw `0x005526D0`, the `TDivModel` draw `0x0049A680` and the model draw `0x004FEEA0`. |
| `0x00562070` | Model part cull test | register arg (EAX part) | `PSUDetour/Hook_00562070_PartCull.hpp` | headless decompile 2026-10-16 | With culling on (`0x00B9DB88` bit 0) tests the part against `[0x00B911A4]` / `[0x00B911A8]` (by part `+0x28` bit 2). Called from the part draw `0x0049A760`. |
| `0x0049A680` / `0x0049A760` | `TDivModel` draw / part draw (vtable `+0x34` / `+0x44`) | `__fastcall (model)` / `__thiscall (model, part, entry, mask, char)` | — | headless decompile 2026-10-16 | Draws only with `model+0x14` bit 0x10; part mask from `model+0x60` (combined with a callback at `0x00B9D64C`); parts `*model[0x31]`, entries `model+0xC8` (0x1C bytes). |
| `0x00541CD0` / `0x00541EA0` | `TPlayerPad` visibility update / body draw | `__fastcall (obj)` | `PSUVR/VRPlayerDraw.cpp` (free aim force) | headless decompile 2026-10-16 | Both skip when obj `+0x1BC` bit 0x10 and `[0x00B91140]` are set and vtable `+0x28` (returns obj `+0x30`) != -1: the update clears `+9` bit 0x04 (node drawn) but the draw returns without drawing. Otherwise the update frustum-tests the render object (`0x00552450`) into `+9` bit 0x04, which the node callback `0x00560D10` checks. Suspected free aim hide (untested). |
| `0x005526D0` | Render object draw | register arg (EAX render) | — | headless decompile 2026-10-16 | Render type `+0x28`: 0 draws the `TDivModel` (`+0x04`, needs `+0x2C` bit 0 and `0x005089C0` = 0) and 7 attachment lists at `+0x30` (node +0 model, +8 next, +0xC enabled). |
| `0x00747248` (return address) | 2D sprite draw (HUD) | — | `PSUVR/VRHud.cpp` | runtime 2026-10-16 | Free aim reticle pieces come through here on **draw layer 48** with a 256 x 256 texture at depth 0.9412: four 192 x 192 corners, a ring (~45% of the width), four ticks and four bars on the screen centre lines (2560 x 1440). Dropped while aiming with the controller. |
| `0x0074E4FD` (return address) | Mission map terrain mesh draw (DrawIndexedPrimitive) | — | `PSUVR/VRHud.cpp` | runtime 2026-10-20 (Clementine) | Pass-B layer **0x31**, FVF XYZ\|NORMAL (0x12), ~837 triangles, blend SRCALPHA / INVSRCALPHA, no depth write, game-sized RT. Replayed onto the VR HUD panel (`hudMapMeshes`). The map frame / markers are DP-UPs from `0x00747BE7` (FVF 0x42) and `0x00747248`. |
| `0x004F9F50` | Apply fire action (aim angles) | `void __thiscall (action, int)` | `PSUDetour/Hook_004F9F50_AimShot.hpp` (Debug log) | headless disasm 2026-10-17 | When action `+0x3B` == 1: copies the shot aim angles (16-bit, `0x10000` = 360 degrees; pitch `+0x34`, yaw `+0x36`) to `0x00B90E78` / `0x00B90E74`, then fires (`0x004FA400`). Called from `0x0052EBDE`. |
| `0x00B90E74` / `0x00B90E78` | Shot aim yaw / pitch | `int16` game angles | `Hook_004F9F50_AimShot.hpp` | headless 2026-10-17 | Read by the bullet direction code `0x004CB3A0` for firing type `0x1B` (instead of the facing): yaw + spread, pitch; radians = angle x `0x0092A118` (2 pi / 65536). |
| `0x004CB3A0` / `0x004E3AE0` / `0x004CACB0` | Bullet direction / `TItemBulletNormal` constructor / weapon fire | `void __stdcall (bullet, int spread, int *pitch, int *yaw, float *pitchRad, float *yawRad, void*)` (`RET 0x1C`) | `PSUDetour/Hook_004CB3A0_BulletAim.hpp` (Debug log) | headless decompile 2026-10-17 | Fire spawns bullets by type (`TItemBulletNormal` vtable `0x008CC8D0`, base `0x008CA8BC`). Start at the weapon muzzle (`Mfire_a` / `Mfire_b`) into bullet `+0x44`; direction from yaw / pitch (facing `+0x124` plus spread and a small auto-pitch toward targets within about 30 degrees, or the shot aim globals above) into velocity `+0x50`. |
| `0x008CACB0` | `TItemTechBase` vtable (techniques) | — | — | RTTI 2026-10-18 | Techniques are not bullets: `TItemTechBase` and `TItemBulletBase` are siblings under `TItemIndirectProcBase`. Delivery subclasses: Straight (`0x008CCCBC`, update `0x004EE2B0`), Chase (`0x008CC99C`), Shoot (`0x008CCC3C`), Point, Round, Front, Emit, Around, DelayPoint, MultiBeam, SteeringBehavior. Managed by `TItemTechManage` (`0x008CCF10`); rods / wands are `TItemWpnTech` (`0x008CC4EC`). None read the shot aim globals. |
| `0x004D50B0` | Technique owner check | register arg (EAX technique) | `PSUDetour/Hook_004D50B0_TechAim.hpp` (Debug log) | headless disasm 2026-10-18 | Called by every technique update each frame; resolves the caster into `+0x38` and `+0x99`. Straight techniques fly from `+0xAC` (vec3) by the angles at `+0xE0` / `+0xE4` (passed to the matrix rotate helpers `0x00787F30` / `0x00787E90`), lifetime countdown `+0xDC`. |
| `+8` of a draw object | Draw node skip flags | `uint16` | `PSUVR/VRPlayerDraw.cpp` (`BeginEyePasses`) | runtime 2026-10-18 | `PSU_DrawLayer` `0x00560830` calls a node only when its object has none of `0x223` here (and the layer byte `+6` is 0). Free aim sets `0x20` on the local player (`+8=0120`), which hides the character, hands and weapons. |
| `0x0048E9A0` | `TCameraTaskPlay` free aim handler (state 4) | `__thiscall (camera)` | `PSUDetour/Hook_TCameraTaskPlay.hpp` (writes the aim before the update) | headless disasm 2026-10-18 | State handler table `0x008C7794` (indexed by camera `+0x20`). Builds the aim view from the player (`camera+0x2C`): yaw = facing `+0x124`, pitch = int16 `+0x220` (x `0x0092A118` radians), unless player vtable `+0x270` says otherwise (then vtable `+0x350` gives them). |
| `[0x009CA8AC]+0x220` | Player free aim pitch | `int16` game angle, **positive = down** (bullet pitch = minus this) | `PSUStructs/PSUCombat.h`, `Hook_TCameraTaskPlay.hpp` | runtime 2026-10-18 | Free aim shots fly along facing (`+0x124`) and this pitch: logged bullet yaw = facing and pitch = camera pitch in every free aim shot. VR writes both directly (`vr.aimDirect`). |
| `[0x009CA8AC]+0x1F0` / `+0x1F4` | Weapon class, right / left hand | `uint32` (`0xFFFFFFFF` = none) | `PSUStructs/PSUCombat.h` | runtime 2026-10-19 (equip test) | High digit: 0 both hands, 1 right, 2 left. `0x02` two-handed sword, `0x04` twin knuckles, `0x06` spear, `0x0C` twin daggers, `0x0E` twin sabers, `0x12` saber, `0x14` dagger, `0x1C` sniper rifle (two-handed, free aim, bullet type `0x0E`), `0x1D` shotgun (two-handed, no free aim, 3 pellets of type `0x0F` at spread -3277 / 0 / +3277), `0x22` handgun (bullet type `0x14`), `0x25` machinegun (no free aim; its bullets don't pass `0x004CB3A0`), `0x28` RCSM (fires by itself at targets: bullet type `0x1B`, aimed through `0x00B90E74` / `0x00B90E78`; no controls). Bullet start (+0x44) is the muzzle only outside free aim; in free aim every shot starts at one fixed point near the player (2026-10-19 log). `+0x1DC` byte 1 = the weapon in use (left one while R1 is held). |
| `0x00749D40` | SetTransform wrapper | `void (D3DTRANSFORMSTATETYPE state, const D3DMATRIX *m)` | `PSUOverlay/PSUOverlay.cpp` (`TransformCaller`) | disasm 2026-10-19 | Calls the device's SetTransform (vtable `+0xB0`) with `[0x00BA8C40]`; returns at `0x00749D5C`. Almost every model's world matrix goes through it, so a SetTransform return address of `0x00749D5C` names the wrapper, not the object: the real caller is 4 dwords up the stack. The game's VIEW stays identity: world matrices are already in camera space. |
| render layers `0x12` / `0x1F` / `0x20` | Sky, blur, glare | layer index (`g_DrawLayers`) | `PSUVR/StereoRenderer.cpp` (sky), `PSUDetour/Hook_FrameRender.hpp` (blur / glare) | runtime layer bisect + dump 2026-10-19 | `0x12` = `TLandDisplayerHevenTask` (the area's sky / galaxy backdrop), `0x13` `TTransparent`, `0x1F` = `TBlur` (full-screen blur / glow), `0x20` = `TGlare` (full-screen glare), `0x21` = players, enemies (`TEnemyComponent`), items, `TLine3D`. `PSU_DrawLayer` `0x00560830` stores the layer it is drawing at `0x009946C8` (`mov [0x9946C8], edi`). |
| `0x009946C8` | Scene / draw mode | `int` | `PSUVR/VRPlayerDraw.cpp` (log) | headless decompile 2026-10-16 | Compared in the player draw path: 2 = a special draw (character select / preview), 0x21 another; 13 in the field. |
| `0x0046ACB0` | PSU_SceneMode5_Callback | `void __cdecl (void)` | `Hook_FrameRender.hpp` | verified | **Town render callback** (mode 5, slot `0x009932AC`). Runtime `PSU_FrameRender gameplay mode=5`. |
| `0x00466360` | PSU_SceneMode3_Callback | `void __cdecl (void)` | `Hook_FrameRender.hpp` | pe-match | Render callback for modes 3 and 9. |
| `0x00474C00` | PSU_SceneMode11_Callback | `void __cdecl (void)` | `Hook_FrameRender.hpp` | pe-match | Render callback for mode 11. |
| `0x0050B580` | PSU_WorldDraw_Candidate | `void __thiscall (void*, int, int)` | — (probe detached) | pe-match | ECX=this; two stack args. Called from mode-1 @ `0x46F215`; not the DIP choke point seen in town. |
| `0x0046F3A0` | PSU_SceneSub_Candidate | `int? __cdecl (void*)` | — (probe detached) | pe-match | Caller `0x46F240` tests EAX (`test eax,eax`). |
| `0x0049E3D0` | PSU_BeginOffscreenRT | `void __cdecl (void)` | — | verified | `SetRenderTarget` to `DAT_00b8ebe8` (OFFSCREEN 640×480), `Clear`. FrameTrace `SetRT … OFFSCREEN ret=0049E42D`. |
| `0x0049E460` | PSU_EndOffscreenToBB | `void __cdecl (void)` | — | verified | Restores BB and composites. FrameTrace `SetRT … BB ret=0049E59C`. |
| `0x0048C960` | PSU_CameraProjectionSetup | `undefined4 __cdecl (void)` | — | ghidra-labeled | Builds projection into `DAT_00b8eb20` then calls `PSU_SetProjectionMatrix`. Init path — **not** the per-frame 3D draw. |
| `0x0074AD80` | PSU_SetProjectionMatrix | `void __cdecl (float* mat)` | — | verified | Copies into `g_ProjectionMatrix` + `SetTransform(PROJ)`. FrameTrace `SetTransform PROJ ret=0074ADAA`. |
| `0x0048FA00` | TCameraTaskPlay_ApplyCamera | `void (task in EAX, new pos in EBX, push target)` | — | disasm | Copies the render camera's position/target into the task mirrors `+0xC8`/`+0xD4`, then writes new values via `0x0048B9A0` (pos → `cam+0xFC`) and `0x0048B9F0` (target → `cam+0x108`), both `cam+0x130 |= 1`. `cam = *( *(task+0x24) + 4 )`. Writing only the task mirrors does nothing (first-person test 2026-10-14). |
| `0x008C7794` | TCameraTaskPlay state handlers | `void (__thiscall*)[]` indexed by `task+0x20` | — | disasm | `0x0048DA90` dispatches on the state byte; slot 1 = `0x0048DB60`. |
| `0x00749D40` | PSU_SetTransform | `void __cdecl (D3DTRANSFORMSTATETYPE, const D3DMATRIX*)` | — | disasm | Thin wrapper over `IDirect3DDevice9::SetTransform` (vtable `+0xB0`). 24 direct callers (byte scan). CamTrace's `ret=00749D5C` is its return. |
| `0x0074E9A0` | PSU_SetWorldMatrix | `void __cdecl (const D3DMATRIX* or null)` | — | disasm | `PSU_SetTransform(0x100 WORLD, m ? m : identity@0x0086D830)`. |
| `0x00759BB0` | PSU_CopyViewMatrix | `void __cdecl (void* src)` | — | ghidra-labeled | Copies into `g_ViewMatrix`. Called from `PSU_InitViewLights` (init). |
| `0x004FC890` | PSU_InitViewLights | `undefined4 __cdecl (void)` | — | ghidra-labeled | Init/lights path; calls `PSU_CopyViewMatrix` + `FUN_0075a070`. |
| `0x00785308` | PSU_BeginScene_Wrapper | `void __cdecl (void)` | — | verified | Thin `BeginScene` on `g_pD3DDevice_a89130`. FrameTrace `BeginScene ret=0078530E`. EndScene `0x00785318`, Present `0x00785328`. |
| `0x00814040` | PSU_FileOpen | see hook | `Hook_00814040.hpp` | ghidra-labeled | Function created in anchors project |

## Globals

| Address | Name | Type | Clementine | Notes |
|---|---|---|---|---|
| `0x00A888C0` | g_pD3D | `IDirect3D9*` | verified | Runtime log 2026-10-14: non-null after `PSU_InitializeDirect3D` |
| `0x00A89130` | g_pD3DDevice_a89130 | `IDirect3DDevice9*` | ghidra-labeled | Device used by Begin/End/Present wrappers at `0x00785308`+ |
| `0x00BA8C40` | g_pD3DDevice_ba8c40 | `IDirect3DDevice9*` | ghidra-labeled | Device used by matrix/`SetRenderState` helpers (`PSU_SetProjectionMatrix`, etc.) |
| `0x00A85C30` | g_ViewMatrix | `Matrix4x4` | ghidra-labeled | **Not the per-frame camera.** Raw byte scan (2026-10-14): only 5 refs, all in `PSU_CopyViewMatrix` (init, from `PSU_InitViewLights`) and the light loop `0x0075A070` (lights → view space, `SetLight`). The orbit camera is folded into each WORLD by the callers of `0x0074E9A0`, so first person steers `TCameraTaskPlay` instead of using this matrix. |
| `0x009CA8AC` | g_pLocalPlayer (via `PSUPlayer`, `ReadMemory(0x5CA8AC)` is base-relative) | `PSUPlayer*` | code-used | `+0x114` position (`Vector3F`), `+0x124` angle. Zero during area / universe change. |
| `0x00A85B30` | g_ProjectionMatrix | `Matrix4x4` | ghidra-labeled | Stereo per-eye override target |
| `0x00B90EA8` | g_SceneModeIndex | `uint32` | verified | Index into scene callback table (`*0x10` stride) |
| `0x00B90ED0` | g_SceneCallbackSkip | `uint32` | verified | Non-zero skips the mode callback in `PSU_FrameRender` |
| `0x0099325C` | g_SceneCallbackTable | `void*[]` (stride 0x10) | verified | Slot = base + mode*0x10 holds the per-mode render callback pointer |
| `0x00B8EBE8` | g_OffscreenRT | `IDirect3DSurface9*` | verified | Bound by `PSU_BeginOffscreenRT` |
| `0x00A8E0EC` | Channel send function slot | function pointer | pe-match | Translation hook. VR must not touch it |

## Vtables (scene / UI hunt)

| Address | Role | Notes |
|---|---|---|
| `0x008C7764` | `TCameraTaskPlay` vtable | `[0]=0x0048DA90` Update, later slot `0x0048E4C0` Refocus |
| `0x00870948` | UI draw object vtable | `[0]=0x00793C40`, also `0x00791F60`, `0x00793C70`, `0x00793C60` |

## Structs

- `CameraTaskPlayData`: defined in `PSUDetour/Hook_TCameraTaskPlay.hpp`. `m_position`/`m_focusPosition` sit after `padding_9C`, and `m_playerDataPtr` leads to the player data (Phase 2 head position).

## Stereo gate (do not implement StereoRenderer yet)

| Question | Answer (2026-10-14) |
|---|---|
| Per-frame **orchestrator**? | **`PSU_FrameRender` `0x00479F50`** (verified). |
| Mode-1 body? | **`PSU_SceneMode1_Callback` `0x0046F080`** — `SceneCB ENTER/LEAVE` runtime-verified (title/lobby). |
| Where is UI? | **Inside `0x0046F080`.** Log: `UIDraw … IN_SceneCB` and `FIRST UIDraw ret=0079932C`. |
| Where is 3D DIP (town)? | Town runs **mode 5, callback `0x0046ACB0`**, so the draws were outside the *mode-1* hook. Leaf wrapper **`0x0074E490`**. Next run hooks `0x0046ACB0` to check whether town DIP happens inside it, before or after UI. |
| Exact scene-only function? | **DIP leaf named** (`0x0074E490`). Higher-level “draw whole world once” entry still TBD (walk callers `0x007502B0`…`0x00763857`). **Do not** start `StereoRenderer` on the leaf alone. |
| Safe to call twice per tick? | **`PSU_FrameRender` — NO.** **`0x0046F080` — NO** (UI). **`0x00793C40` — NO.** **`0x0074E490` — NO** (per-mesh leaf). Need a world-pass root above the `0x750xxx` callers. |
| Virtual screen | Mono quad stays live. |

### Verified order (title/lobby)

`PSU_BeginOffscreenRT` → **`0x0046F080` ENTER** → (no DIP) → **FIRST `UIDraw 0x793C40` ret=`0x0079932C`** → more UI → **LEAVE** → `PSU_EndOffscreenToBB` → Present.

### Town note

Indexed world draws go through **`0x0074E490`** while **not** inside the mode-1 callback.

**Town is scene mode 5, not mode 1** (runtime 2026-10-14: `PSU_FrameRender gameplay mode=5 callback=0046ACB0 slot=009932AC`). The mode-1 hook never saw town draws because the town frame never enters `0x0046F080`.

Render callback table `0x0099325C` (stride `0x10`, slot 0 = render callback, called as `call eax` with no args):

| Mode | Slot 0 (render) | Notes |
|---|---|---|
| 0, 4 | — | no render callback |
| 1, 2, 6, 7, 8, 10 | `0x0046F080` | title/lobby (mode 1 verified) |
| 3, 9 | `0x00466360` | |
| 5 | `0x0046ACB0` | **town** (verified) |
| 11 | `0x00474C00` | |

Live callers of `0x0074E490` in town (runtime return addresses): `0x007502B5` (fn `0x007500F0`, seen before play camera), `0x00761312` (fn `0x00761180`), `0x00750862` (fn `0x00750650`), `0x0075263A` (fn `0x007524E0`). `0x004FF1B0` did fire in this session from `0x0065157C` (`this=0F53E5A0`, `flag=0xC47`), so the earlier "zero hits" was session-dependent.

Duplicate load: `online.exe` injects its own (Release, launcher-updated) `PSUEx.dll` into PSUC, separate from our `dinput8` proxy loading `PSUExD.dll`. `dllmain.cpp` skips hooking if another PSUEx module is already present. Otherwise it Detours `LoadLibrary{A,W,ExA,ExW}` and returns its own handle for any later `PSUEx.dll` / `PSUExD.dll` load.

### Render passes (stack chains, 2026-10-14)

Town (mode 5) inside `0x0046ACB0`: ~175 DIP before the first `UIDraw` (`ret=007992F8`), then 3 UI draws. Verified call chain:

```
PSU_FrameRender 0x00479F50  --call eax-->  scene callback (0x0046ACB0 town / 0x00466360 title)
  if( byte[0x00993328] ) { 0x004FD990(); 0x004FDBC0(); }        (same pattern in all 4 callbacks)
  0x004FD990  PSU_RenderPassA  -> 0x00560830(layer in EDI) for layers 0x02..0x29 (many gated by bytes 0x009D1088+)
  0x004FDBC0  PSU_RenderPassB  -> 0x00560830 for layers 0x2B..0x3F
    0x00560830  PSU_DrawLayer  -> per-object virtual call [obj+0x10] @ 0x00560868
      town objects -> 0x00526B00 / 0x005E4040 / 0x00552800 -> 0x004FF076 -> 0x004FEC10 -> mesh wrappers -> 0x0074E490
```

Runtime (town, pass hooks, 8 frames each):

| Pass | DIP | DrawPrimitiveUP | UIDraw | SetTransform | VS consts |
|---|---|---|---|---|---|
| A `0x004FD990` | 165 | 4 | 0 | WORLD ×84, no VIEW/PROJ | 0 |
| B `0x004FDBC0` | 0 | 66 | 0 | PROJ ×2 (ret `0x0074ADAA`) | 0 |

Pass A is the fixed-function 3D world with no UI. Camera VIEW/PROJ is set before pass A, not inside it. Pass B is 2D (UP quads with its own projection). The 3 `0x793C40` UI draws happen after both passes.

Camera (CamTrace, town, 3 frames): the game never calls `SetTransform(VIEW)`, so VIEW stays identity. The camera is baked into every WORLD matrix (`0x00749D5C` sets WORLD = model×view; first sample has pitch ≈20.5°, X mirrored, translation `(0, -14.0, -5.3)`). PROJ is set once per callback at `0x0074ADAA` before pass A: right-handed, infinite far, near = 1, `cot(fovY/2)` = 2.415 (≈45°), 4:3. Pass B sets an off-center PROJ (`_31=-0.65`, `_32=-0.58`) and then restores it. No vertex-shader constants are uploaded, so it's fixed-function.

Stereo implication: at `0x004FD990` entry, set VIEW = per-eye offset (eye-space translation) and PROJ = per-eye frustum. Restore VIEW = identity and the game PROJ on exit.

`0x004FD990` / `0x004FDBC0` are `void __cdecl (void)`. `0x00560830` uses a custom convention (layer index in `EDI`), so hook its callers, not it. Hypothesis to confirm: pass A = 3D world, pass B = 2D/UI. If pass A has no UI draws and is safe to repeat, it is the stereo double-call point.

PE note: the code section `0x00401000`–`0x0086B000` is **not** flagged executable in its header (only the 8 KB `.mackt` stub is). Don't filter sections on `IMAGE_SCN_MEM_EXECUTE` when bounding game code.

### Static call graph above DIP leaf (PE, no further playtest)

```
0x0074E490  PSU_DrawIndexed_Wrapper   (leaf; ret 0x0074E4FD)
    ^ 17 mesh wrappers (0x007500F0 … 0x007634E0)
    ^ per-wrapper parents (0x007504E0, 0x00751610, …)
    ^ aggregators seen in PE:
         0x004FEC10  (thiscall-ish; 2 E8 sites)
         0x005625F0 → 0x005628E0 / 0x00562980  (draw helpers)
         0x004FF1B0  (__thiscall; 12 E8 sites — best observation candidate)
         vtable 0x008C8120 → 0x0049ABC0 / 0x0049A760 / 0x0049AA50
              (RTTI name after vtable: TAstarteCollisionDisplayer — one mesh class, not whole world)
```

`PSU_FrameRender` / `0x0046F080` do **not** `E8` directly to the DIP leaf or `0x004FF1B0`. World draw is reached via other tasks / virtual calls. Next: confirm `0x004FF1B0` at runtime (first-hit log only), then find its per-frame root.

## To find (VR roadmap)

| Target | Phase | Notes |
|---|---|---|
| Per-frame scene render entry point | 1 | Orchestrator + mode-1 found. **DIP leaf `0x0074E490` verified in town.** World-pass root above its callers still open |
| Offscreen RT → BB transition | 1 | **Found:** `PSU_BeginOffscreenRT` / `PSU_EndOffscreenToBB`; surface `g_OffscreenRT` |
| Start of the UI/HUD pass | 1 | **`0x00793C40` verified inside `0x0046F080`**; first ret `0x0079932C` |
| Player world position and head bone | 2 | Through `m_playerDataPtr` |
| Local player head and hair mesh draws | 2 | To hide them in first person |
| Movement input read (left stick) | 2 | Rotate it by HMD yaw |
| DirectInput device objects and `GetDeviceState` | 3 | Synthetic gamepad |
| Hand and weapon bone transforms | 3 | **Found:** bone pose `0x0074B760` (edited by `VRHands`), bone query `0x004FFFA0`; weapons hang off the hand bones |

## Item palette (voice items, 2026-10-29)

| Address | Name | Signature | Used by | Verified | Notes |
|---|---|---|---|---|---|
| `0x00577C40` / `0x00574A20` | TWindowActionPalette ctor / its creator caller | `__thiscall (this, owner)` | — | ghidra (2026-10-29, not verified at runtime) | Draws the weapon / item palette: owns a `TWindowCP` (`0x0057C770`, vtable `0x008D1BAC`, layout states `open_item`, `weapon_active`, `item_active`, `item_active_down` at `0x008D1C84`) with 4 slot objects (`0x0057BC80`, 0xB8 each). Display code only: **no player-side "use palette slot N" function and no readable palette slot contents were found**, so voice item use drives the virtual pad instead (`VRSyntheticPad::UseItemSlot`). Item names are therefore not available to the voice registry. |
