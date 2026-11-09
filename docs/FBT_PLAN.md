# Full-body tracking / skeleton mode plan

Planned 2026-11-01 on `psu_vr` @ `6e596e6`. Phase 0 done 2026-11-04 (verdict below). User request: "disable body animations when attacking, also their
default poses, just make it a skeleton we can use so we can map with standable / full body tracking and vrchat-like ik".

Address tags: **[G]** checked in Ghidra (`PSUC_Uncompressed.exe`, read-only) while planning, **[R]** runtime-verified earlier
(ADDRESS_MAP / handoff), **[A]** assumed / open.

## 0. Already there (VR_HANDOFF item 10)

- `bodyRestPose` (`VRHands.cpp` `RestPose`): every bone except the root to the bind pose (inverse of record `+0x30`), scaled by
  `bodyFitHeight`, first in `VRHands::Apply`, all camera modes, attacks included. User-confirmed.
- `BodyIk`: hips follow the head offset, waist (9) / chest (10) lean up to 20 deg, `bodyProceduralLegs` (planted feet, two-bone
  legs through `SolveArm`), `bodyKeepLegAnim`, `bodyNeckDrop`, `bodyBack`. User-confirmed 2026-10-31.
- Bone map [R] (1 unit = 10 cm): 0 root, 1 pelvis, 2 hip node, legs 3/4/5 (L thigh/knee/ankle) and 6/7/8 (R), no toe; 9 waist,
  10 chest, 11 shoulder level, 12 neck, 13 head; arms 14-18 (L) / 19-23 (R) = shoulder, upper, fore, hand, weapon; 24-27 extras.
- Missing: tracker input, calibration, third-person IK, per-bone animation mask, a clear decision on which pose gameplay sees.

## Phase 0 verdict (2026-11-04, probe run 2 + Ghidra)

User: melee / JA / guns / techniques / PA fine with `bodyRestPose` on. Callers of the bone query on the local player outside
its draw (`tools/lastlog.sh 'VRPoseQuery'`), all resolved (ADDRESS_MAP, Functions):

| Caller | Bones | What |
|---|---|---|
| `0x00533B00` `Char_CacheLocatorBones` | 1 15 16 18 20 23 26 | per-frame locator cache, read by `Char_GetLocatorMatrix` |
| `0x00628160` `Char_UpdateHandAttachments` | 17 22 | hand attachments, cosmetic |
| `0x004CA6D0` `Char_GetLocatorMatrix` | 0, 22, 5 | from effect slots `0x004F3C90` and technique spawn `0x004D4B90` |
| `0x004FAA30` `WeaponMag_FollowPos` | 11 | weapon mag follows the neck |
| `0x0052D460` head forward (TPlayer vtable `+0x48`) | 13 | head direction, users unknown |

No melee hit, JA or bullet path reads bones (hits use `player+0x114` + facing constants). Only techniques take their
start point from a hand locator, so with the VR pose they leave from the VR hand, which fits VR. **Decision: no
`bodyGameplayPose` setting**; gameplay keeps seeing the VR pose. Revisit only if techniques miss in multiplayer.

## 1. Suppressing the game's animation

Pose pipeline:

| Address | What | Tag |
|---|---|---|
| `0x0049A2B0` | TDivModel animate + pose (only data xref: vtable slot `0x008C8124`) | G |
| `0x004FE760` | TModel version (callers `0x004FE830`, `0x0055AB16`) | G |
| `0x00504F50` / `0x00504E60` | motion evaluate into the local pose (anim controller `model+0x84`, `+0x4C`) | G |
| `0x0074BF10(localPose, skel)` | skeleton default local pose when no motion plays | G |
| `model+0x6C` / `model+0x70` | pre-build callback (local pose) / post-build callback via `0x004FFB30` | G |
| `0x0074B760(out = model+0x64, skel, localPose, root)` | model-space build; already hooked (`Hook_0074B760_BonePose.hpp` -> `VRPlayerDraw::OnBonePose` -> `VRHands::Apply`) | G R |
| `[0x00B9D624]` | shared local-pose scratch buffer (edit only during the player's call) | G |
| `0x008C80E4` | TDivModel vtable start (the "TAstarteCollisionDisplayer" label on `0x008C8120` in ADDRESS_MAP is a misread) | A |

Both `0x0049A2B0` and `0x004FFB30` call `0x0074B760`, so the existing hook covers either path.

Local pose, 0x30 per bone [G]: `+0x00` translation vec3, `+0x10` quaternion xyzw (`0x007456C0` = D3DXMatrixRotationQuaternion),
`+0x20` scale vec3.

Bone record, 0x90 [G]: `+0x00` flags (`0x2` no rotation, `0xF00` euler order, `0x1000` / `0x1C0000` billboard / axis locks,
`0x8000` / `0x4000` aim at bone `+0x08`, `0x1E000` skips the normal build, `0x8` no inverse bind [R]); `+0x06` parent (short);
`+0x0C` default translation; `+0x18..+0x20` default euler; `+0x24` default scale; `+0x30` inverse bind [R].

Bone names: model vtable `+0x28` -> object `+0x28` = `{int mode (0 linear / 1 sorted), int count, entries[8] = {int bone, char*
name}}`, looked up by `0x00757500` (used by the attachment code `0x005525A0` for "Head", "L_weapon", "R_weapon" at
`0x00994560..0x0099457C`). Correction [G] 2026-11-01: `+0x28` holds a pointer to `{int mode, int count, entry *entries}`; the entries are a pointer, not inline `[8]`. Layout [G], runtime [A] (Phase 0 dump). Use it to map bones by name (CASTs may differ [A]).

Gameplay:
- Attack timing comes from the animation clock: `0x004DC2C0` [G] compares `PSU_PlayerMotionEntry` (`0x005366E0`) hit frames with
  the player vtable `+0xCC` frame (`0x005532F0`) [R]. **Never stop the motion player; override only its output.**
- `0x004DC530` [G] spawns hit objects at `player+0x114` + facing-rotated constants (`0x004E4420` / `0x004E44D0` / `0x004E4580`);
  only one sub-mode reads a bone locator ("Mfire_a" via `0x004CAA60` -> `0x004F15B0`). Owning weapon class unknown [A].
- Melee `TItemWpnKnock` (vtable `0x008CC114`) `+0x68` = `0x004FC490`, `+0x6C` = `0x006098E0`: no function defined in Ghidra;
  checked 2026-11-01 from the exe bytes [G]: plain getters (`return this+0x5C` / `this+0x60`), no bone reads.
- Weapons follow the IK pose: `0x005525A0` calls the bone query (vtable `+0x18` = `0x004FFFA0`) which re-runs the pose build.
- Free-aim shots start from a fixed point [R]. Only the local TDivModel is edited (`div+0x64 == out`); others and network untouched.

Design:
1. `bodyAnimKeep` bitmask replaces `bodyKeepLegAnim` (migrate true -> bit 1): 1 legs 1-8, 2 spine 9-11, 4 head 12-13, 8 arms
   14-17 / 19-22, 16 weapon bones 18 / 23, 32 extras 24-27. Unkept bones = rest pose. Start with the existing model-space
   override; add `bodyRestLocal` (write record defaults into `localPose` before `pOriginal` in `Hook_74B760`, or call
   `0x0074BF10` into a private buffer and copy masked bones) only if mixes shear.
2. `bodyGameplayPose` "vr" (default) / "animated": IK only inside `s_drawingLocalPlayer` or `s_inWorldQuery`. Build only if
   Phase 0 finds a gameplay caller that depends on bones.
3. `bodyFlatAnimated` (off): skip `Apply` outside eye passes so the monitor shows the game's animation.
4. `bodyIkThirdPerson` (Phase 2): IK body in third person instead of a frozen rest pose.

## 2. Head + hands IK (VRChat-like), after `RestPose` in `Apply`

1. Pelvis: neck target = HMD - `bodyNeckDrop` down - `bodyBack` back (existing). Body yaw = `StrafeUpdate` yaw blended toward the
   controllers' average level yaw when both hands are below the head. Crouch: push the pelvis back by `crouch * 0.4` so knees
   bend forward (height clamp stays -0.8 / +0.15 m).
2. Spine: CCD over 9 -> 10 -> 11, weights 0.3 / 0.4 / 0.3, 2-3 iterations, per-bone `kLeanLimit`; chest twist toward head yaw
   clamped to `kTorsoLimit` 50 deg, split over the three.
3. Head: `bodyHeadFollowsHmd` rotates 13 (subtree 12) to the HMD, neck takes 30%.
4. Arms (`SolveArm`): `bodyShoulderShrug` (14 / 19 up to 20 deg when reach > 85% or target above the shoulder),
   `bodyElbowFromHand` (pole blended with the controller's back-of-hand axis); elbow tracker overrides the pole.
5. Legs: keep `bodyProceduralLegs`; knee pole from foot yaw; foot tracker replaces the planted target.
6. Third person: targets relative to the HMD (`tracker - head` in the tracking yaw frame, scaled by body fit) on the rest neck +
   root with the character yaw; lift `AfterPlayerDraw`'s `FirstPersonActive` gate on `s_world` when on.

Code: new `PSUVR/VRBodyIk.cpp/.h` (move `BodyIk` there; `VRHands.cpp` is ~1,700 lines), shared math (`SolveArm`,
`ApplyToSubtree`, `RotationAbout`, `LimitedTurn`, quaternion `ControllerAxes`) in `PSUVR/VRSkeletonMath.h`.

## 3. Trackers (OpenXR)

Today `OpenXRRuntime::CreateInstance` enables only `XR_KHR_D3D11_enable`; one `gameplay` action set (`CreateActions`); input in
`UpdateInput`.

- **`XR_HTCX_vive_tracker_interaction`** (primary; SteamVR). Profile `/interaction_profiles/htc/vive_tracker_htcx`, paths
  `/user/vive_tracker_htcx/role/{waist,chest,left_foot,right_foot,left_knee,right_knee,left_elbow,right_elbow,...}/input/grip/pose`.
  Vive / Tundra, SlimeVR and Standable all arrive this way, **but only with a role set in SteamVR > Manage Trackers**.
- `XR_MNDX_xdev_space`: Monado / WiVRn later, devices by name, no roles.
- Body-tracking extensions (`XR_FB_body_tracking`, `XR_META_body_tracking_full_body`, HTC, BD): not on SteamVR; note only.

Implementation: enumerate extensions and enable HTCX (log `OpenXR: XR_HTCX_vive_tracker_interaction available|missing`); a
`tracker_pose` action with role subaction paths + one action space per role (rejected suggestion = log, not fatal); handle
`XR_TYPE_EVENT_DATA_VIVE_TRACKER_CONNECTED_HTCX` and `xrEnumerateViveTrackerPathsHTCX`; locate at the hands' predicted time,
valid = active + POSITION|ORIENTATION valid, tracked = `_TRACKED` bits. `VRRuntime.h`: `TrackerRole` enum, `TrackerState`,
`virtual bool GetTrackerState(TrackerRole, TrackerState&)` (default false). New `PSUVR/VRTrackers.cpp/.h` (add to vcxproj +
filters), `Update` next to `VRControllers::Update(m_xr)` in `VRSystem.cpp`, world via `VRCamera::TrackingToWorld`, one-euro filter,
lost hold then 0.3 s blend, calibration offsets; `Pose(role, posWorld, axesWorld, weight)`. Every role optional, fallback to the
estimate. Foot height from the STAGE space (`m_stageSpace`) so it doesn't depend on `fpEyeHeight`.

## 4. Mapping

| Tracker | Bone | Use |
|---|---|---|
| Waist | 1 pelvis | position + orientation via `ApplyToSubtree`; its yaw replaces the `StrafeUpdate` body yaw |
| Chest | 10 | end orientation for the CCD spine |
| Feet | 5 / 8 | `SolveArm` on the leg chain; foot orientation from the tracker + offset |
| Knees / elbows | 4 / 7, 15 / 20 | pole vectors |

Proportions: uniform `s_bodyFit` (0.5-2) plus optional arm / leg ratios from calibration (`bodyLegStretch` like `armStretch`).
Guard: bone count 28 + parent checks for legs and spine (today arms only), else skip IK and log `VRBody: bone map mismatch`.

## 5. Settings and logs

Keys go in `VRSettings.h`, the X-macro list in `VRSettings.cpp`, a new "Body & trackers" panel page (move the rest-pose block),
and both game files (`psuex_vr.json`, `config.json` `vr`; `.bakN` first).

`bodyAnimKeep` int 0, `bodyRestLocal` false, `bodyGameplayPose` "vr", `bodyFlatAnimated` false, `bodyIkThirdPerson` false,
`bodyHeadFollowsHmd` true, `bodyShoulderShrug` 0.5, `bodyElbowFromHand` 0.5, `bodyLegStretch` false, `trackersEnabled` false
(restart needed), `trackerWaist|Chest|Feet|Knees|Elbows` true, `trackerSmoothing` 0.3, `trackerLostHoldMs` 300,
`trackerCal<Role>` "x y z qx qy qz qw", `trackerCalibrated`, `trackerCalibRequest` (not saved).

Logs (`tools/lastlog.sh 'VRTrackers|VRBody|VRPoseQuery'`): `OpenXR: ... HTCX available, enabled`, `OpenXR: tracker connected role
waist (...)`, `VRTrackers: waist ok ... left_foot lost 120 ms` (3 s), `VRBody: sources pelvis=tracker|estimate ...`,
`VRBody: calibrated N trackers ...`, `VRBody: bone map ok|mismatch`, `VRPoseQuery: callers ret=... bone=...` (Phase 0, Debug).

Calibration: panel button + 5 s countdown with haptics (copy the hand calibration flow); stand straight; rest skeleton under the
HMD (yaw, neck, fit from `GetHeadHeightAboveFloor` else `fpEyeHeight`); per role `offset = inverse(trackerWorld) * boneWorld`;
T-pose gives arm span; save + toast.

## 6. Phases (each buildable and testable alone)

0. **RE + probes, Debug only, no behaviour change.** Hook `0x004FFFA0` (`PSUDetour/Hook_004FFFA0_BoneQuery.hpp`, register in
   `dllmain.cpp` `DetoursHookAll`): count callers / bones for the player outside the draw and world query, log every 5 s. Dump bone
   names once. Create and read `0x004FC490` / `0x006098E0` in Ghidra. In game with `bodyRestPose` on: saber combo + JA timing,
   handgun / rifle, a technique, a PA effect position, a CAST. Expected: hits unaffected; probe shows only attachment / muzzle
   callers. If a melee path uses the weapon bone, Phase 1 builds `bodyGameplayPose = animated`.
1. **Skeleton mode cleanup** (built 2026-11-05: `bodyAnimKeep` with re-parenting, bone-map guard; `bodyGameplayPose` not needed (Phase 0), `bodyFlatAnimated` dropped (player not drawn in the flat pass)): `bodyAnimKeep`, optional `bodyRestLocal`, `bodyGameplayPose` (if needed), `bodyFlatAnimated`,
   bone-map guard. Expected: no game animation on the player in any camera / input combo, weapons stay in hands.
2. **3-point IK (`VRBodyIk.cpp`)** (2a built 2026-11-05: split, crouch, CCD spine, head, hands yaw, shrug, elbow; 2b built 2026-11-05: third person via `VRCamera::Body*` mapping; knee pole from foot yaw skipped, feet are levelled to the pelvis yaw already): pelvis + crouch, CCD spine, head, shrug, elbow from hand, third person. Risk: camera clipping
   the shoulders (tune `bodyBack`), third-person placement.
3. **Tracker input only** (built 2026-11-05: HTCX + `VRTrackers`, discs, status line; untested with Standable): HTCX, `GetTrackerState`, `VRTrackers`, debug discs (reuse the holster discs), logs; no IK change.
   Risk: trackers without roles, rejected bindings, Standable role setup.
4. **Trackers drive IK:** waist, feet, poles, chest; per-role fallback ~0.3 s, no pop. Risk: foot height on slopes (clamp to
   `groundY`, root stays the game's), stick movement with tracked feet.
5. **Calibration + proportions**, saved, "Reset calibration".
### Phase 4 + 5 plan (2026-11-05, after the first Standable run)

Run result: Standable's 8 trackers arrive through **OpenXR HTCX** now (`/devices/htc/vive_tracker<role>`, all 8 roles, `found
(OpenXR)`), so the OpenVR source stayed idle (auto). Raw poses: every tracker ~1.0 m above the floor, laid out along -Z (chest ~0,
waist -0.4, knees -0.7, feet -1.1 m): a body **lying in the air in front of you**. That is a whole-frame error in Standable's space
(rotated ~90 deg about X plus an offset), not per-tracker offsets, so calibration must fit a global transform first.

**5a. Calibration capture** (`PSUVR/VRTrackerCal.{h,cpp}`, new):
- Trigger: panel button "Calibrate trackers" (`trackerCalibRequest`, not saved) + motion chord later; 5 s countdown with
  `Countdown` haptics, `Done` on capture. Stand straight, arms hanging, look ahead.
- Expected joints at capture, in STAGE space (floor origin; survives recenter, unlike LOCAL): rest skeleton (`VRBodyIk::RestPose`
  bind pose, body fit) stood under the HMD (head yaw, neck = HMD - `bodyNeckDrop`, feet on the floor). Bones: 1 pelvis, 10 chest,
  4 / 7 knees, 5 / 8 ankles, 16 / 21 elbows.
- Trackers in STAGE: new `IVRRuntime::GetStagePose` (STAGE in LOCAL) so `VRTrackers` can convert both ways.
- **Global fit** (Umeyama: rotation + translation + uniform scale clamped 0.7..1.4) of tracker positions -> expected joints, weights
  waist / chest / feet 1, knees 0.7, elbows 0.3; needs >= 3 non-collinear roles, else yaw + translation only. Applied only if it
  cuts the RMS residual by > 5 cm vs identity (real Vive / Tundra trackers keep identity). Log `VRTrackerCal: global fit rot X deg
  scale S rms A -> B cm`.
- **Per-tracker offset**: `offset = inverse(G * tracker) * joint` (position + rotation; joint rotation = rest bone axes), so
  `joint = G * tracker * offset`.
- Saved: `trackerCalGlobal` "px py pz qx qy qz qw s" (STAGE), `trackerCal<Role>` "x y z qx qy qz qw", `trackerCalibrated`;
  "Reset calibration". Uncalibrated + trackers on: nothing drives the body (discs only).
- Discs: raw disc (dim) + calibrated joint disc (bright) per role. **Test 5a:** after calibrating, the bright discs sit on your
  hips / knees / feet / elbows and follow them; walking / turning keeps them on.

**4. Trackers drive `VRBodyIk`** (built 2026-11-06, untested; details in VR_HANDOFF "Phase 4 trackers drive the body". Deviation:
lower-body heights come from the floor (STAGE) scaled by character hip / calibrated hip, so feet meet the ground whatever fpEyeHeight is), per role, `trackerDriveBody` (true) + group toggles waist / chest / feet /
knees / elbows:
- Waist -> pelvis 1 position + orientation (replaces the head-derived hips and the `StrafeUpdate` yaw while tracked).
- Chest -> spine CCD end orientation (10 / 11), head still to the HMD.
- Feet -> leg two-bone targets + foot orientation (replaces planted feet), clamped to the ground.
- Knees -> leg poles; elbows -> arm poles (overrides `bodyElbowFromHand`).
- Per-role weight ramps 0 <-> 1 over 0.3 s on gain / loss (fallback = today's estimate, no pop). Positions through
  `VRCamera::BodyTrackingToWorld` (first + third person). The game root stays the game's (stick moves the body, trackers stay
  relative). Log `VRBody: sources pelvis=tracker|estimate ...` every 3 s.
- **Test 4:** stand / squat / kick / step sideways / sit: character's hips, knees, feet follow; cover a tracker: that limb eases
  back to the estimate; stick walking still works.

6. **Polish:** (built 2026-11-06, see VR_HANDOFF "Phase 6 polish": stricter whole-frame acceptance, residual-jump toast, foot lock `trackerFootLock`, adaptive orientation smoothing) still open: `XR_MNDX_xdev_space`, position filter tuning from logs; ADDRESS_MAP, VR_HANDOFF, VR_ROADMAP.

Files: `PSUVR/VRHands.cpp`, new `PSUVR/VRBodyIk.*`, `PSUVR/VRSkeletonMath.h`, `PSUDetour/Hook_0074B760_BonePose.hpp`,
`PSUVR/VRPlayerDraw.cpp`, `PSUVR/OpenXRRuntime.cpp`, `PSUVR/VRRuntime.h`, new `PSUVR/VRTrackers.*`, `PSUVR/VRSettings.*`,
`PSUVR/VRSettingsPanel.cpp`, `docs/ADDRESS_MAP.md`, `docs/VR_HANDOFF.md`.
