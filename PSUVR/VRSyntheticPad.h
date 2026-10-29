#pragma once

#include <cstdint>

namespace PSUVR
{
	// Motion controllers as a gamepad (vr.inputMode "motion"). Controller input becomes a
	// logical PSU pad (the button names of PsuIlluminus.ini [gamepad1]), which the dinput8
	// loader merges into the joystick state the game reads (PSUEx_MergeJoystickState).
	//
	// The layout (plain presses, both-grip chords, the D-pad toggle) is VRMotionLayout::kLayout.
	namespace VRSyntheticPad
	{
		// Once per frame, after VRControllers::Update.
		void Update();
		// True while the controllers drive the pad (motion mode, a controller active).
		bool Active();
		// Free aim mode with the gun raised and the game in free aim: the hand whose ray aims
		// (0 left, 1 right). Our reticle is drawn there and the game's centre reticle hidden.
		bool GunAiming( int &hand );
		// While GunAiming: the gun angle adjustment (degrees) for the weapon being aimed.
		bool GunAdjust( float &pitchDegrees, float &yawDegrees );
		// VRAction bits fired by grip chords since the last call.
		unsigned TakeActions();

		// What each control of one hand does now (for the controller label cards). Index by
		// VRMotionLayout::Control; nullptr = nothing. swing: what a swing of that hand does.
		struct HandLabels
		{
			const char *control[ 6 ]{};
			const char *swing = nullptr;
		};
		// gripMask: the grips held for the context (1 left, 2 right, 3 both = the VR chords).
		// Uses the same weapon roles, menu / palette state and modes as Update.
		void DescribeControls( int gripMask, HandLabels out[ 2 ] );
		// Free aim with the controller: the game yaw / pitch (radians) the aim is being steered
		// to this frame. False when the controller isn't aiming.
		bool LastFreeAim( float &yaw, float &pitch );
		// Direct free aim (vr.aimDirect, first person): the game yaw / pitch (radians) to write
		// into the player's aim this update. False = leave the game's aim alone.
		bool DirectAim( float &yaw, float &pitch );

		// Voice command: casts the technique in cast slot 0..3 (PSUCombat::TechSlots) the way the
		// gestures do (X / Y on the right weapon, R1 + X / Y for slots 3 / 4). False when motion
		// combat can't press it now (menu, palette, no technique weapon).
		bool CastTechSlot( int slot );

		// Voice command: uses palette item slot 0..3 by running vr.voiceItemScript on the virtual pad
		// (experimental, see VRSettings.h). False when busy, in a menu, or the palette is already open.
		bool UseItemSlot( int slot );

		// state: DIJOYSTATE2. axisMin / axisMax: the device range of lX, lY, lZ, lRx, lRy,
		// lRz, slider 0, slider 1. replace: drop the device's own input first (the device is
		// a VR controller seen through DirectInput). Returns false when nothing was merged.
		bool MergeJoystickState( void *state, unsigned size, const long axisMin[ 8 ], const long axisMax[ 8 ], bool replace );
	}
}
