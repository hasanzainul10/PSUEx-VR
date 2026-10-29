#pragma once

#include <array>
#include <cstdint>
#include <string>

// VR settings shared by the game DLL (Config::vr) and the standalone configurator
// (tools/PSUExVRConfig). Serialized to the "vr" object of config.json and to
// psuex_vr.json. No game dependencies.

enum class VRAction : int
{
	Recenter = 0,
	ToggleCameraMode,
	ToggleSettings,
	TogglePause,
	ToggleSnapTurn,
	ToggleHudFollow,
	ToggleToolkitMenu,
	Count
};

constexpr int kVRActionCount = static_cast< int >( VRAction::Count );

// key: Win32 virtual-key code (0 = none), with the exact modifiers in mods. pad: XInput
// button mask pressed while Back/Select is held (0 = none).
enum VRKeyMod : uint8_t
{
	kVRModCtrl = 1,
	kVRModShift = 2,
	kVRModAlt = 4,
};

struct VRBinding
{
	int key = 0;
	uint16_t pad = 0;
	uint8_t mods = 0;
};

const char *VRActionLabel( VRAction action );
std::array< VRBinding, kVRActionCount > DefaultVRBindings();

struct VRSettings
{
	bool m_enabled = false;
	bool m_forceD3D9Ex = false;
	// Ex upgrade + CreateDeviceEx + managed-pool remap, but no OpenXR/bridge.
	bool m_d3d9exOnly = false;
	bool m_solidColorPrototype = false; // true = teal clear; false = virtual-screen blit
	std::string m_runtime = "openxr";
	std::string m_cameraMode = "firstPerson";
	std::string m_inputMode = "motion";
	// Read by the dinput8 proxy at startup: keep the gamepad (and optionally the keyboard)
	// working while the game window isn't focused. Gamepad defaults on with VR.
	bool m_backgroundInput = true;
	bool m_backgroundKeyboard = false;
	float m_worldScale = 1.0f;
	float m_ipdScale = 1.0f;
	float m_hudDistance = 2.5f;
	float m_hudSize = 1.0f;
	// Scale XYZRHW UI when capturing to VR HUD layers / nameplates (1 = native).
	float m_uiScale = 1.5f;
	// Town UI: false = everything on the visor panel; true = also use the world-locked kiosk panel.
	bool m_hudWorldPanel = false;
	// Opacity kept for near-black parts of the mission map backdrop on the visor (0 = invisible).
	float m_hudMapBlackAlpha = 0.15f;
	// Mission map: also put its vertex-buffer / indexed draws (layers 0x31 / 0x32) on the visor
	// panel; only its DP-UP pieces were captured, so the terrain was missing in the headset.
	bool m_hudMapMeshes = true;
	// Mission map on the panel: shrink and move the 3D map (terrain, markers) into its 2D frame.
	// The game sizes the 3D map to the screen but draws the frame at 640 x 480 pixel size.
	bool m_hudMapFit = true;
	// Extra zoom of the fitted map (1 = the game's 640 x 480 proportions).
	float m_hudMapFitScale = 1.0f;
	// HUD/menu panel in front of the body (lazy follow) instead of locked to the head.
	bool m_hudFollow = true;
	// Menus / 2D scenes: a dark backdrop behind the panel instead of the runtime's own
	// environment (which also shows the play-area boundary).
	bool m_menuBackdrop = true;
	// Menu overlays bigger than the screen (mission menu scanlines) go on the menu panel in their
	// draw order instead of being dropped as a background.
	bool m_hudOverlays = true;
	// Rogue's Toolkit panels and menu (drawn inside its Present hook) go on the HUD panel in the headset
	// instead of only the monitor's back buffer. They then no longer show on the flat monitor.
	bool m_toolkitOnHud = true;
	// The VR UI pointer drives the Rogue's Toolkit menu (moves and clicks the system mouse over it).
	bool m_toolkitPointer = true;
	// Degrees the head may look away before the panel glides back in front.
	float m_hudFollowAngle = 35.0f;
	// How far below eye level the panel sits (meters).
	float m_hudDrop = 0.1f;
	// First person: eye height above the player's feet and forward offset (meters, avatar scale).
	float m_fpEyeHeight = 1.6f;
	float m_fpEyeForward = 0.12f;
	bool m_fpEyeFollowsNeck = true; // the eye leans with the animated neck so walking doesn't put it behind the shoulders
	// Replace the game's animated bones (idle, walk, attack) with the skeleton's bind pose; VR layers pose on top.
	bool m_bodyRestPose = false;
	// With the rest pose: scale the whole body so its eyes sit at fpEyeHeight (your real height).
	bool m_bodyFitHeight = false;
	// With the rest pose, first person: hips follow the head (crouch), the spine leans to it.
	// With the rest pose: leave the pelvis and legs on the game's own animation (walk, idle).
	bool m_bodyKeepLegAnim = false;
	bool m_bodyIk = false;
	// With the rest pose: feet stay planted in the world and step when the hips move away (replaces the strafe legs).
	bool m_bodyProceduralLegs = false;
	// Right-stick snap turn step in degrees (snapTurn = true).
	float m_snapTurnDegrees = 30.0f;
	// Right-stick smooth turn speed in degrees per second (snapTurn = false).
	float m_fpTurnSpeed = 120.0f;
	// "head" = left stick forward goes where you look; "body" = where the body faces.
	std::string m_fpMoveDirection = "head";
	// Own character in first person: "headless" (cut above the neck), "hands" (forearms and
	// hands only, while the arms follow the controllers), "hidden", "full".
	std::string m_fpBody = "hands";
	// How far below the eyes the headless cut sits (meters).
	float m_fpNeckDrop = 0.22f;
	// How far the head may lean / crouch away from the body before the body follows (meters).
	float m_fpLeanLimit = 0.15f;
	float m_fpCrouchLimit = 0.2f;
	// First person: walking in the room walks the character (left stick toward where you
	// stepped, until it is under you again). Starts past roomWalkStart meters.
	bool m_roomWalk = true;
	float m_roomWalkStart = 0.25f;
	// Third person: camera distance behind and height above the head (meters), right-stick
	// zoom speed (m/s) and range, and the Back + right stick orbit / height steps.
	float m_tpDistance = 8.76f;
	float m_tpHeight = 1.0f;
	float m_tpZoomSpeed = 3.0f;
	float m_tpMinDistance = 1.5f;
	float m_tpMaxDistance = 10.0f;
	float m_tpOrbitDegrees = 45.0f;
	// Third person turning, separate from first person: snap steps (tpOrbitDegrees) or
	// smooth (tpTurnSpeed, deg/s). Tilt: camera pitch in degrees (negative = looking down),
	// changed with Back + right stick left/right at tpTiltSpeed deg/s.
	bool m_tpSnapTurn = true;
	float m_tpTurnSpeed = 90.0f;
	float m_tpPitch = 0.0f;
	float m_tpTiltSpeed = 45.0f;
	// What LB (refocus / lock-on) does to the VR camera. "follow": for lbWindowSeconds after
	// an LB press (or while held) the game's camera direction is adopted. "off": ignored.
	std::string m_tpLbCamera = "follow";
	std::string m_fpLbCamera = "off";
	float m_lbWindowSeconds = 2.0f;
	bool m_snapTurn = true;
	bool m_comfortVignette = false;
	// Motion controllers: draw a marker and pointer ray at each tracked controller.
	bool m_showControllers = true;
	// Motion controllers: point the right controller at PSUEx windows (VR settings) in the
	// headset; the trigger clicks, the right stick scrolls, Circle closes.
	bool m_uiPointer = true;
	// Controller label cards (what each button does) floating at the controllers: when you look
	// down at a controller (toggle: both grips + right stick click), and while a grip is held
	// still for controllerLabelsHoldSeconds. Size: card width in meters.
	bool m_controllerLabels = true;
	bool m_controllerLabelsGrip = true;
	float m_controllerLabelsHoldSeconds = 0.4f;
	float m_controllerLabelsSize = 0.14f;
	// The in-headset setup wizard (recenter, eye height, hand calibration, controls tour) has
	// been finished; false opens it on the first stereo frame (motion input).
	bool m_setupDone = false;
	// Testing without the headset on: emulated controllers (Ctrl+F9; Ctrl+numpad hotkeys) and a
	// command file in the game folder that runs whenever it changes.
	bool m_emulateControllers = false;
	std::string m_simScript = "psuex_vr_script.txt";
	// First person: the character's arms reach for the controllers (two-bone IK).
	bool m_handsFollow = true;
	bool m_armStretch = true; // visible arms stretch to reach the controllers when yours are longer
	// Arms while attacking: "controller" (your swing) or "animation" (the game's). Photon
	// Arts always play the game's animation.
	std::string m_handsAttack = "controller";
	// The same for Photon Arts and technique slot 2 (grip + swing / trigger): false = the arms stay
	// on the controllers, true = the game's animation.
	bool m_handsPhotonArtAnimation = false;
	// Hand angle calibration (controller -> hand rotation, quaternion x y z w per hand).
	// Off: estimated from the skeleton and the standard OpenXR grip pose. Defaults: a PSVR2
	// Sense calibration.
	bool m_handCalibrated = true;
	float m_handRotL0 = 0.6261195f, m_handRotL1 = -0.6223192f, m_handRotL2 = -0.3737006f, m_handRotL3 = 0.2846774f;
	float m_handRotR0 = -0.5841057f, m_handRotR1 = 0.6521613f, m_handRotR2 = -0.2755048f, m_handRotR3 = 0.3969927f;
	// Fine tune on top of the hand angle, degrees (roll mirrors between the hands).
	float m_handPitch = 0.0f;
	float m_handRoll = 0.0f;
	// Runtime only (not saved): bumped by the panel's Calibrate button.
	int m_handCalibRequest = 0;
	// Button names in the controller layout picture: "psvr2" (Sense) or "generic" (A/B/X/Y).
	std::string m_motionProfile = "psvr2";
	// Buzz the right controller when the game's combo window opens (melee).
	bool m_comboHaptic = true;
	// Motion mode: the left stick works the D-pad while a menu has the input.
	bool m_autoDpad = true;
	// Motion mode combat: swing a melee hand to attack (X), pull a gun hand's trigger to
	// shoot (R1 = left-hand weapon). Weapon per hand comes from the game.
	bool m_swingAttack = true;
	// Photon Art (Y) / technique slot 2 gestures.
	bool m_paGesture = true;
	// On: trigger click or swing = attack / slot 1, with that hand's grip held = Photon Art /
	// slot 2 (the right grip sends no R1 while the right weapon has controls). Off: the old
	// scheme, melee swings only, swing with the trigger held = Photon Art.
	bool m_gripPhotonArt = true;
	// Left Triangle in the field: the game's R2 (opens the chat log). Off: the
	// PSUEx chat log window. Y in menus either way.
	bool m_triangleChatL2 = true;  // (name kept for old settings files; it now sends R2)
	// Before the first hit of a chain, turn the character toward where you look.
	bool m_strafeBody = true;            // body trails the head, legs step sideways / back while strafing
	float m_strafeBodyBlend = 0.5f;      // how far the legs turn from the travel direction toward the body
	bool m_attackFaceLook = true;
	float m_attackFaceTolerance = 30.0f; // degrees off the look direction that still needs no turn
	// Free aim (R3 with a gun): the gun hand steers the aim.
	bool m_controllerAim = true;
	bool m_aimInvertPitch = false;
	// Free aim in first person: write the controller's angles into the game's aim (player
	// facing +0x124 and aim pitch +0x220) instead of steering it with the right stick.
	bool m_aimDirect = true;
	// Draw our reticle where the game's aim (and so the shot) points, not along the controller.
	bool m_reticleOnShot = false;
	// Free aim: turn the gun hand about the wrist so the barrel points along the shot.
	bool m_gunPointsAtAim = false;  // off: the game's pose doesn't line up reliably (bow)
	// Free aim "leash" (first person, direct aim): the reticle sits on the gun hand's aim line,
	// aimLeashMeters out, and every shot is aimed from where the game fires (above the player)
	// to that point, so shots land on the reticle. Replaces reticleOnShot / gunPointsAtAim.
	bool m_aimLeash = true;
	float m_aimLeashMeters = 20.0f;
	// Free aim: the gun hand turned by this much (degrees, in the controller's frame) so the gun
	// model lines up with the reticle. Per weapon type; set in the headset with both grips + right
	// stick while aiming.
	float m_gunAdjustHandgunPitch = 38.14338f;
	float m_gunAdjustHandgunYaw = -8.952070f;
	float m_gunAdjustBowPitch = 51.93464f;
	float m_gunAdjustBowYaw = -10.309003f;
	float m_gunAdjustRiflePitch = 26.22663f;
	float m_gunAdjustRifleYaw = 18.22735f;
	// Staff / rod: the weapon stays in the hand as held before the cast instead of following the
	// casting animation (arms following, first person). The TCSM keeps its animation.
	bool m_techWeaponStill = true;
	// Free aim hides the player; what is undone to keep the hands and gun in view (arms
	// following, first person). Each can be turned off to find what costs frame rate.
	bool m_freeAimNoNearHide = true;     // ignore the near-camera hide (0x005089C0) for the player
	bool m_freeAimNoPartCull = true;     // ignore the part culling (0x00562070) for the player
	// Free aim reticle radius in degrees of view (the game's shots spread around the centre).
	float m_reticleSize = 3.0f;
	float m_swingSpeed = 1.8f;      // m/s to count as a swing
	// A swing during the attack animation waits for the combo window.
	bool m_comboBuffer = true;
	// comboBuffer waits for the motion's Just Attack window (up to 1.5 s); off: until the swing
	// animation ends (+0x710 == 0x02, up to 0.25 s).
	bool m_comboBufferJustAttack = true;
	// Speech to text: hold left Square in the field and talk. Recognition runs offline in the
	// PSUExSpeech helper (Moonshine, English); speechModel tiny / small / medium picks the model
	// in PSUExSpeech\models. The text goes to the game chat (speechChat) or, with voiceCommands,
	// casts a technique on the equipped weapon when it names one ("Barta").
	bool m_speechToText = false;
	std::string m_speechModel = "small";
	bool m_speechChat = true;
	bool m_voiceCommands = false;
	// Voice item use ("item two"): drives the weapon / item palette through the virtual pad. The
	// script is space separated steps: hold:<button> (kept until the end), wait:open (palette up),
	// tap:<button> or tap:slot, wait:<seconds>. voiceItemButtons = the buttons of items one..four.
	// UNVERIFIED in game: the defaults are a guess, tune them from the VRVoice log lines.
	bool m_voiceItems = false;
	std::string m_voiceItemScript = "hold:B wait:open tap:slot wait:0.3";
	std::string m_voiceItemButtons = "Up,Right,Down,Left";
	// Always-on listening: the helper cuts the microphone into utterances by itself (no button).
	// voiceRoute decides where an utterance goes: "command" (match a technique, nothing else),
	// "chat" (the game chat) or "auto" (command while a weapon is out in the field, otherwise
	// ignored). The held button also falls back to chat when no command matches, except in "command".
	bool m_speechAlwaysOn = false;
	std::string m_voiceRoute = "auto";
	// 0..1 how far the vignette closes in at full motion.
	float m_vignetteStrength = 0.7f;
	// Render the 3D pass per eye (projection layer) with the game frame as a panel.
	bool m_stereo = true;
	// Scene mode that gets stereo eyes (5 = town). 0 = every mode (old behavior).
	unsigned m_stereoMode = 5;
	// During eye passes, skip XYZRHW quads that sample an RT filled only by the flat pass
	// (glare / afterimage ghosts the mono camera over stereo). Shadows rewritten per eye stay.
	bool m_stereoSkipScreenQuads = true;
	// During eye passes, world sprites the game projects itself (2D sprite draw 0x00747248 with
	// a projected depth: item / combo glows, mission overlays) are re-projected for each eye
	// instead of keeping the flat screen's pixel positions.
	bool m_reprojectSprites = true;
	// During eye passes, models the game places at its own camera (a mission's colour tint sheet)
	// stay locked to each eye, enlarged for the headset's wider view, instead of standing in the
	// world where the flat camera is.
	bool m_recenterSky = true;
	// The area's sky (render layer 0x12, TLandDisplayerHevenTask) drawn as if the eye were at the
	// game camera: infinitely far, no edges, never behind you in third person.
	bool m_skyAtEye = true;
	// The game's full-screen glow (layer 0x1F, TBlur) and glare (0x20, TGlare) in the headset. They
	// are made from the flat image and laid over everything (enemy glow over the weapon, floating
	// rectangles). Off skips their draws in the eyes; the layers still run (switching a layer off
	// crashed: its object never set itself up), and the flat window keeps them.
	bool m_eyeBlur = true;
	bool m_eyeGlare = false;
	// The area's sky (layer 0x12) in the headset. Some skies are partial backdrop boxes made for the
	// flat camera's view; in VR their unfinished sides show (galaxy panels, rectangles). Off hides
	// the sky in the headset only.
	bool m_eyeSky = true;
	// When stereo is on, also submit the flat game frame as a world-locked quad (HUD stand-in).
	// Default off — the panel reads as a dark PiP sliding over the stereo floor.
	bool m_stereoPanel = false;
	// Debug: shift the 3D pass sideways by ±this many view units, alternating each second.
	float m_debugEyeShift = 0.0f;
	// Debug: run the 3D pass twice per frame to test repeat-safety.
	bool m_debugDoublePass = false;
	// Frame timing overlay (CPU / GPU ms per pass, headset budget) and a "Perf 5 s:" log line.
	bool m_perfOverlay = false;
	// Game pointer checks (PSUMemory::IsValidPointer) read one byte under SEH instead of calling
	// VirtualQuery, which costs ~0.2 ms per call here (PSUCombat::Read alone was ~4 ms a frame).
	bool m_fastPointerChecks = true;
	// Stereo: skip the flat back-buffer copy and its pre-Present GPU wait (only the stereo panel uses it).
	bool m_perfSkipFlatCopy = true;
	// OpenXR submit: plain GPU copies instead of clear + shader blit when sizes and formats allow.
	bool m_perfCopyBlits = true;
	// HUD capture: lock only the vertex / index range a draw uses instead of the whole buffers.
	bool m_perfHudLockRanges = true;
	// psuex.log: flush about once a second instead of after every line.
	bool m_perfLogBatching = true;
	// Stereo: the monitor shows the left eye instead of a separate flat 3D pass (the HUD still draws on top).
	bool m_flatMirrorEye = true;

	// VR shortcuts (keyboard and Back+button pad chords).
	std::array< VRBinding, kVRActionCount > m_bindings = DefaultVRBindings();

	// Runtime only (never saved): stereo, first person and HUD capture stand down and the
	// headset shows the flat game on the virtual screen.
	bool m_paused = false;
};

// JSON text of a "vr" object <-> settings. Loading only overwrites keys that are present
// and well-typed, so partial files keep the other values.
bool LoadVRSettingsJson( VRSettings &settings, const std::string &jsonText );
std::string SaveVRSettingsJson( const VRSettings &settings );
