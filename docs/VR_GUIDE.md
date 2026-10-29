# PSUEx VR: player guide

Play Phantasy Star Universe (Clementine) in VR with motion controllers or a gamepad: stereo 3D in the game world, first or third person, and the HUD and menus on a panel in front of you.

> **Preview release.** Expect rough edges. Tested on PS VR2 Sense controllers through SteamVR.

---

## 1. What you need

- **Windows 10 or 11** and a working **Clementine** install (the game starts and you can log in normally).
- A **PC VR headset with SteamVR**. SteamVR is the tested setup. Meta Quest (Link / Air Link) and Windows Mixed Reality should work through their OpenXR runtimes but are untested.
  - **PS VR2:** install the **PlayStation VR2 App** from Steam and start it before playing. It brings up SteamVR.
- **Motion controllers** (the default; PS VR2 Sense is tested, Touch / Index should work), **or** an **Xbox-compatible gamepad** (XInput) for the VR turning and shortcuts. The game itself reads most pads; other pads (e.g. PlayStation) need an XInput wrapper such as DS4Windows for the VR stick turning and Back chords.

---

## 2. Install

1. **Download** the latest `PSUEx-VR-v….zip` from the [Releases page](https://github.com/hasanzainul10/PSUEx-VR/releases).
2. **Find your game folder**: the folder with `PSUC.exe` and `online.exe` (for example `C:\Games\PSU Clementine`).
3. **Extract everything from the zip into that folder**:

   | File | What it is |
   |---|---|
   | `dinput8.dll` | Loader: starts PSUEx with the game, and keeps your gamepad working while the game window isn't focused |
   | `PSUExVR.dll` | The mod (VR build of PSUEx) |
   | `openxr_loader.dll` | Connects the game to SteamVR (OpenXR) |
   | `PSUExVRConfig.exe` | VR settings app, works without starting the game |
   | `psuex_vr.json` | Your VR settings (VR on, first person, motion controllers) |
   | `PSUExSpeech\` | Speech to text helper and its English model (only runs when you turn speech on) |
   | `VR_GUIDE.md`, `README.txt`, `LICENSE`, `THIRD_PARTY_NOTICES.md` | Docs and licenses |

   - If Windows asks to **overwrite `dinput8.dll`** or `psuex_vr.json`, say Yes if they came from an older PSUEx. If `dinput8.dll` belongs to another mod, back it up first: only one `dinput8.dll` can be in the folder.
   - **Don't touch `di.dll`**. It belongs to Clementine.
   - **Leave Clementine's own `PSUEx.dll` alone.** The Clementine launcher re-downloads it every time it starts, which is why PSUEx VR uses its own name, `PSUExVR.dll`. The loader picks `PSUExVR.dll` over `PSUEx.dll`.
   - If a **`PSUExD.dll`** is in the folder (a developer build), delete it: the loader prefers it over `PSUExVR.dll`.

---

## 3. Recommended game settings

Set these in Clementine's game options (the options tool that comes with Clementine). They're stored in `%LOCALAPPDATA%\SEGA\PHANTASY STAR UNIVERSE Illuminus\PsuIlluminus.ini`.

| Setting | Recommended | Why |
|---|---|---|
| Screen mode | **Windowed** | Easier to switch to SteamVR or other apps; VR doesn't need fullscreen. |
| Resolution | **16:9**, e.g. **1920 x 1080** or **2560 x 1440** | The HUD panel in the headset uses the game resolution: higher = sharper text, but more work for the PC. |
| VSync | **Off** | The headset sets the pace; VSync on the monitor only adds lag. |
| Shadows | **Low** if your frame rate struggles | The 3D world is drawn twice (once per eye). |

---

## 4. First launch

1. **Start SteamVR** (PS VR2: start the **PlayStation VR2 App** on Steam first).
   - If you have another VR app installed (Oculus, WMR), make SteamVR the OpenXR runtime: SteamVR → Settings → OpenXR → **Set SteamVR as OpenXR Runtime**.
2. **Put on the headset.**
3. **Start the game with `online.exe`** as usual (not `PSUC.exe` directly).
4. What you'll see:
   - **Login, menus, loading:** the game on a big floating screen.
   - **In the game world:** stereo 3D. In first person you're in your character's head; the HUD and menus sit on a panel in front of you.
5. **Recenter** whenever "forward" feels off: **Ctrl + F9** on the keyboard, or hold **Back/Select** and press **X** on the pad.
6. **Setup wizard** (motion controllers): the first time the game shows stereo 3D, a **VR Setup** panel opens. Point at it with the right controller and click with the trigger. Steps:
   1. **Welcome:** how the pointer works (trigger clicks, right stick scrolls, right grip moves the panel).
   2. **Play position:** stand or sit where you'll play, look ahead, press **Recenter**.
   3. **Eye height:** stand straight and press **Measure**: your head height above the floor (from the headset's floor level) becomes the first person eye height (1.2-2.0 m). Fine tune with the slider, or **Keep current**. Headsets without a floor level show only the slider.
   4. **Hands:** **Calibrate** the hand angle (arms straight out, fists, palms facing each other, keep still 5 s), or **Keep the PS VR2 defaults**. Needs first person with the hands following the controllers.
   5. **Controls:** the controller layout and how to see the label cards.

   **Finish** or **Skip** saves `setupDone` so it doesn't open again. Run it again from **VR Settings → General → Run VR setup again** (that button also works with a gamepad, using the mouse). Untick **VR setup done** in `PSUExVRConfig.exe` to get it on the next start.

---

## 5. VR settings outside the game

Run **`PSUExVRConfig.exe`** from the game folder.

- It edits `psuex_vr.json` in its folder. If the file isn't found, pick your game folder with **Browse...**.
- Change anything, then press **Save**.
- **Close the game first**: the game rewrites `psuex_vr.json` when it saves its settings.

Sections: General (VR on/off, camera mode, world scale), First person, Third person, Turning, HUD and menus, Comfort, Controls.

---

## 6. VR settings in the game

Open the **VR Settings** window:

- press **Ctrl + F10**, or hold **Back/Select** and press **Start**, or
- press **Insert** for the PSUEx menu → Window → **VR Settings**.

In the headset the window shows on the HUD panel, with a mouse pointer drawn for you. Changes apply immediately. **Save** keeps them; **Reload from file** picks up changes made in `PSUExVRConfig.exe`.

- **Pause VR** (Ctrl + F8): instantly switch to normal flat play. The headset shows the game on a big screen. Press again to resume. Not saved.
- **VR enabled**: turns VR on or off completely. Takes effect the **next time you start the game**.

---

## 7. Controls

### VR shortcuts

Pad shortcuts are chords: **hold Back/Select, then press the button**. Keyboard shortcuts work while the game window is focused.

| Action | Keyboard | Gamepad |
|---|---|---|
| Recenter | Ctrl + F9 | Back + X |
| Switch first / third person | Ctrl + F7 | Back + Y |
| Show / hide VR settings | Ctrl + F10 | Back + Start |
| Pause VR | Ctrl + F8 | — |
| Snap / smooth turn | — | — |
| Body-locked HUD on / off | — | — |

Change them under **Controls** in either settings window (click a cell, press the new key or button; hold Ctrl/Shift/Alt with the key if you want). The game also sees the Back and button presses, so pick chords you don't need at that moment.

> Upgrading from an earlier PSUEx build? Press **Reset controls** once to get the Ctrl + F defaults.

### First person

| Input | Effect |
|---|---|
| Left stick | Walk where you look (or where your body faces: *Left stick forward* setting) |
| Right stick left / right | Turn: 30° steps by default, or smooth (*Turning* settings) |
| Your head | Look around freely. Lean or duck a little; walk in your room and your character walks with you (motion controllers). |
| LB | Lock on (your view stays yours) |

Your own head is cut away so you don't see inside it; your hands, body and legs stay visible.

### Motion controllers (PS VR2 Sense, Touch, Index...)

**Play with motion controllers** is on by default (turn it off in the VR settings to use a gamepad). The controllers act as a gamepad, and in first person your character's arms follow them.

| Control | Left hand | Right hand |
|---|---|---|
| Stick | Move | Turn |
| Stick click | Free aim mode on / off | START (menu) |
| Square / Cross (X / A) | X in menus; in the field **hold to talk**: speech to text (see *Talking* below) | B (confirm) |
| Triangle / Circle (Y / B) | Y in menus; in the field R2: opens the game chat log (*Left Triangle = game chat log (R2)* off: the PSUEx chat log window) | A (cancel) |
| Grip | L1 | R1 |
| Trigger | L2, or that hand's weapon (see below) | R2, or that hand's weapon |

**Hold both grips** for VR shortcuts: + Cross recenter, + Circle first / third person, + Square VR settings, + Triangle pause VR, + left stick click switches the left stick to the D-pad (it switches by itself in menus, shops and the weapon palette), + right stick click R3, + **left stick pushed up: Rogue's Toolkit menu** (with the toolkit installed; also Ctrl+F6, Back + A, or the VR Settings button), + **both triggers: hand calibration**. Left grip + left stick click is L3.

**Fighting.** Each hand uses the weapon the game puts in it:

- **Melee:** swing the hand or pull its trigger to attack. Hold that hand's grip while you swing / pull for the Photon Art. A short buzz marks the Just Attack timing of each hit: swing on the buzz for a Just Attack (every melee weapon, timed from the game's own motion data). A swing made too early in a hit is held until the buzz and then sent, so it lands as a Just Attack (*Hold early swings for the combo window*; the first hit of a chain goes out at once). Turn off *Hold until the Just Attack timing* to send it as soon as the swing animation ends instead (faster, not timed for a Just Attack). Two-handed and twin weapons work from either hand.
- **Techniques:** staff or rod (right hand): swing / trigger casts slot 1, with the right grip held slot 2. TCSM (left hand): the same with the left hand. Rod: the left hand casts slots 3 / 4. The staff / rod stays in your hand while casting (*Staff / rod stays in the hand while casting*); the TCSM keeps its animation.
- **Guns:** handgun, machinegun and bow (left hand), sniper rifle and shotgun (right hand): pull the trigger to shoot; hold it to keep firing. Grip + trigger is the gun's Photon Art. The bow aims with the left controller. The RCSM fires by itself.
- While the right weapon has controls the right grip is its Photon Art button, not R1. *Grip = Photon Art* off (Motion controllers) brings back the old scheme: melee by swing only, swing with the trigger held = Photon Art.
- **Free aim:** click the left stick to turn free aim mode on. Raise the gun into view and the game's free aim follows where the gun points (our reticle shows it); lower it, or walk / turn with the sticks, and you're back to normal. Right stick click is the scan while aiming.

**Your arms** (first person) reach for the controllers. Settings in **Motion controllers**:

- *Arms follow the controllers*: on / off.
- *Play the game's attack animation*: off = your swing is the swing; on = attacks hand over to the game's animation.
- *Play the game's Photon Art / technique animation*: the same for grip + swing / trigger (off by default: arms stay on the controllers).
- **Hand calibration:** the defaults come with a calibration made on PS VR2 Sense controllers; *Back to automatic* estimates the hand angle from the skeleton instead. If it looks off, hold both grips + both triggers, then within 5 seconds hold your arms straight forward, fists around the controllers, palms facing each other, and keep still until the long buzz. It's saved. *Hand pitch / roll* fine-tune it; *Back to automatic* undoes it.
- **Own body → Hands only** (First person settings): only the hands and weapons, right on the controllers, so the camera never clips your shoulders.

**Talking** (*Voice* settings, off by default). Turn on *Speech to text (hold left Square)*, then in the field hold **left Square**, talk, let go. What you said goes into the game chat as if typed (the chat box opens, the text goes in, Enter sends it), and a toast shows it. Recognition runs on your PC, offline and English only, in `PSUExSpeech\PSUExSpeech.exe` (Moonshine), which starts with the game while the setting is on; the microphone stays open while it runs.

- *Speech model*: Small comes with the mod. Tiny (faster, more mistakes) and Medium (best, more CPU) can be fetched with *Download* (needs internet once).
- *Send what I say to the game chat* off: the text is only shown.
- *Voice commands*: with a rod, staff or TCSM, say the name of a technique set on the weapon ("Barta") to cast it from its slot, the same as a swing (slot 1) or grip + swing (slot 2); "slot one" .. "slot four" work too. Only techniques on the weapon in your hands count; anything else you say goes to the chat. Needs *Motion combat*.

**Walking in your room** walks your character: step away and it follows until it's under you again (*Walk in the room moves the character*, First person settings). Your stick still works and takes over.

### Third person

| Input | Effect |
|---|---|
| Right stick left / right | Turn the camera around your character (45° steps, or smooth) |
| Right stick up / down | Zoom in / out |
| Hold Back + right stick left / right | Tilt the camera |
| Hold Back + right stick up / down | Raise / lower the camera |
| LB | Camera behind your character, or lock-on framing with a target (as in the normal game) |

The camera never turns by itself; it only moves when you tell it to.

---

## 8. Comfort tips

- **First person with snap turn** (the defaults) is the most comfortable.
- The view never moves on its own: only your stick and your head move it.
- Feeling queasy with smooth turning? Turn on **Comfort → Vignette**: it darkens the edges of your view while you turn or move.
- Recenter (Ctrl + F9 / Back + X) whenever forward feels off.
- The HUD panel stays in front of your body; turn your head to read its corners.

---

## 9. Play without VR / uninstall

- **Without VR for now:** untick **VR enabled** (settings app or in-game window), then restart the game. Everything else in PSUEx keeps working.
- **Uninstall:** delete `PSUExVR.dll`, `dinput8.dll`, `openxr_loader.dll`, `PSUExVRConfig.exe` and `psuex_vr.json` from the game folder. (Keep Clementine's `PSUEx.dll`.)

---

## 10. Troubleshooting

| Problem | Try |
|---|---|
| Game starts flat, nothing in the headset | Start SteamVR before the game; make SteamVR the OpenXR runtime; check **VR enabled** in `PSUExVRConfig.exe`. |
| PSUEx VR doesn't seem to load | `dinput8.dll` and `PSUExVR.dll` must be next to `PSUC.exe`. Open `dinput8_proxy.log` in the game folder: it should say `Loaded PSUExVR.dll` (not `PSUEx.dll`). |
| Gamepad does nothing at all | The loader lists your controllers in `dinput8_proxy.log` (`#0` is the one the game uses). VR motion controllers are moved to the end automatically; unplug extra pads or sticks if the wrong one is `#0`. |
| Gamepad does nothing while the game window isn't focused | Settings → General → *Gamepad works when the game isn't focused* must be on (restart the game after changing it). |
| HUD text is blurry | Use a higher game resolution (2560 x 1440), or raise **HUD and menus → Size**. |
| Choppy | Lower shadows, lower the game resolution, close other apps. |
| Something else | Send `psuex.log` and `dinput8_proxy.log` from the game folder with your report. |

---

## 11. Testing without the headset

For trying things over remote desktop with no headset on: SteamVR's built-in fake headset (the null driver) keeps the VR session running, and PSUEx emulates the motion controllers.

### Turn on the fake headset

1. Close SteamVR.
2. Open `C:\Program Files (x86)\Steam\config\steamvr.vrsettings` in a text editor and add (or merge into the existing sections):

   ```json
   "steamvr" : {
      "forcedDriver" : "null",
      "activateMultipleDrivers" : true
   },
   "driver_null" : {
      "enable" : true,
      "renderWidth" : 2016,
      "renderHeight" : 2240,
      "windowWidth" : 1920,
      "windowHeight" : 1080
   }
   ```

3. Start SteamVR. A window shows what the fake headset sees.

To go back to the real headset, close SteamVR and remove `forcedDriver` (or set `driver_null` `enable` to `false`).

### Emulated controllers

Turn them on with **Ctrl+Shift+F9** (or *Emulate controllers* in the VR settings). Keep **Num Lock** on. Hotkeys (all with **Ctrl**):

| Keys | Does |
|---|---|
| Num7 / Num8 / Num9 | Keys act on the left / both / right hands |
| Num1–Num5 | Pose: rest, forward, gun (up in view), raised, calibration |
| Num0 | Trigger (held) |
| Num . | Grip (held) |
| Num / | Cross / A |
| Num + | Circle / B |
| Num − | Stick click |
| Num6 | Swing |
| Num * | Both grips (the VR chord) |
| T / G / F / H | Stick up / down / left / right |

### Command file

`psuex_vr_script.txt` in the game folder runs from the top every time the file changes (one command per line, `#` for comments). Each line is written to `psuex.log` as `VRSim: > ...`.

| Command | Does |
|---|---|
| `emulate on` / `emulate off` | Turn the emulated controllers on / off |
| `wait <seconds>` | Pause the script |
| `pose <left\|right\|both> <rest\|forward\|gun\|raised\|calib>` | Move the hands (eased) |
| `press <hand> <trigger\|grip\|primary\|secondary\|stick> [seconds]` | Press a control (default 0.15 s) |
| `hold <hand> <control> on\|off` | Hold or release a control |
| `stick <hand> <x> <y> [seconds]` | Push the stick (−1 to 1) |
| `swing <hand>` | A fast swing (attack) |
| `chord <hand> <control>` | Both grips, then that control (e.g. `chord both trigger` = hand calibration) |
| `log <text>` | A marker in the log |

Example: free aim with the left gun, shoot, then put the gun down.

```
emulate on
pose left gun
wait 2
press left trigger
wait 3
pose left rest
```

Full settings reference: [INSTALL.md](INSTALL.md).
