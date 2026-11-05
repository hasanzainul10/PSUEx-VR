#pragma once

#include "VRRuntime.h"

#include <d3d9.h>

namespace PSUVR
{
	// The one place OpenXR poses become D3D matrices.
	//
	// The game leaves D3D VIEW at identity and bakes its camera into WORLD, so
	// game camera space (right-handed, +Y up, looking down -Z) is the space the
	// 3D pass draws in. An eye's VIEW is the inverse of its pose relative to the
	// recenter reference, which puts the HMD on top of the game camera.
	namespace VRCamera
	{
		// Game world units per real meter (before vr.worldScale). The follow camera sits
		// 40-70 units behind the player and focuses 15 units up: ~4-7 m and ~1.5 m.
		constexpr float kGameUnitsPerMeter = 10.0f;

		// Reference = head position and yaw now; the game camera sits there.
		void Recenter( const EyePose views[ 2 ] );
		bool HasReference();

		// Camera hook, every update: where the render camera object's position (+0xFC)
		// and target (+0x108) live. Captured right before the eye passes.
		void NoteRenderCamera( const void *cameraPosition, const void *cameraTarget );
		void CaptureRenderCamera();

		// Third person: HMD on top of the game camera, with the camera's pitch removed
		// (level horizon). gameProj supplies the depth mapping; FOV comes from the eye.
		void BuildEye( const EyePose &eye, float unitsPerMeter, const D3DMATRIX &gameProj,
			D3DMATRIX &view, D3DMATRIX &proj );

		// First person: the orbit camera is steered level behind the player's head,
		// looking along body yaw (right stick) plus HMD yaw when vr.fpMoveDirection is
		// "head" (default). With "body" the HMD looks around without steering.
		bool FirstPersonMode();
		// Camera task hook, after the game's update. Measures right-stick turn, then
		// rewrites the camera position/focus (game world units). cameraState is the task
		// state byte: 1/3 = normal play; others (5 = NPC talk) get no head steering or
		// turning. False = not applied.
		bool SteerFirstPerson( float cameraPosition[ 3 ], float focusPosition[ 3 ],
			const float playerPosition[ 3 ], int cameraState );
		// Third person in VR: our own level orbit camera (no auto-follow). Right stick zooms;
		// hold Back + right stick to orbit (vr.tpOrbitDegrees steps) or step the height.
		bool ThirdPersonMode();
		bool SteerThirdPerson( float cameraPosition[ 3 ], float focusPosition[ 3 ],
			const float playerPosition[ 3 ], int cameraState );
		// leftThirdPerson: the mode was left (restart the orbit next time); false = a
		// one-tick miss that keeps the current orbit yaw.
		void ClearThirdPerson( bool leftThirdPerson );
		// True once a steer happened for the current camera; eyes then use BuildEyeFirstPerson.
		bool FirstPersonActive();
		// Free aim in first person: keep the first-person view (head on the body) without
		// writing the game camera, so the game's aim camera can turn underneath. False when
		// first person isn't primed.
		bool HoldFirstPerson( const float playerPosition[ 3 ] );
		// A tracking-space direction (OpenXR yaw, pitch) as a game-world yaw / pitch, using the
		// first-person body frame. False outside first person.
		bool WorldAimFromTracking( float trackingYaw, float trackingPitch, float &yaw, float &pitch );
		// The reverse: a game-world yaw / pitch as an OpenXR yaw / pitch. False outside first person.
		bool TrackingAimFromWorld( float yaw, float pitch, float &trackingYaw, float &trackingPitch );
		// First person: a tracking-space point as a game world position, mapped like the eyes
		// (body frame at the head, eye-forward offset included). False outside first person.
		bool TrackingToWorld( const float tracking[ 3 ], float world[ 3 ] );
		// The inverse: a game world point in tracking space (first person).
		bool WorldToTracking( const float world[ 3 ], float tracking[ 3 ] );
		// Same for a direction (no scale or offset).
		bool TrackingDirToWorld( const float tracking[ 3 ], float world[ 3 ] );
		// First-person head (eye anchor) in game world. False outside first person.
		bool HeadWorld( float head[ 3 ] );
		// First-person HMD forward and up in game world (unit). False outside first person.
		bool HeadAxes( float forward[ 3 ], float up[ 3 ] );
		// First-person body forward and right in game world (unit, level). False outside it.
		bool BodyAxes( float forward[ 3 ], float right[ 3 ] );
		void ClearFirstPerson();

		// Body IK (VRHands / VRBodyIk). First person: the plain functions above. Third person with
		// vr.bodyIkThirdPerson: the headset's offset from the recenter reference (sideways clamped) on
		// the eye anchor VRHands sets each pose, in the character's facing; tracked points are placed
		// relative to the headset. All false when neither applies.
		// The headset's tracking pose, every frame before the eye passes.
		void NoteHmd( const EyePose views[ 2 ] );
		// Third person, setting on, our orbit camera, a reference, not the dressing room.
		bool ThirdPersonBodyWanted();
		// eyeWorld = where the head is when you stand at the reference; facingYaw in the game's
		// convention; unitsPerMeter = the character's size over yours.
		void SetThirdPersonBody( const float eyeWorld[ 3 ], float facingYaw, float unitsPerMeter );
		bool BodyTrackingToWorld( const float tracking[ 3 ], float world[ 3 ] );
		bool BodyTrackingDirToWorld( const float tracking[ 3 ], float world[ 3 ] );
		bool BodyHeadWorld( float head[ 3 ] );
		bool BodyHeadAxes( float forward[ 3 ], float up[ 3 ] );
		bool BodyFrameAxes( float forward[ 3 ], float right[ 3 ] );
		bool BodyLookYaw( float &yaw );
		// The right stick is read through XInput (false = turning falls back to the game's orbit).
		bool TurnFromStick();
		// Game units from the steered first-person camera to the head.
		float FirstPersonCameraDistance();
		// First-person body yaw (game convention, radians); changes only by stick turns.
		float BodyYaw();
		// Where the player looks, in the game's yaw convention (atan2(dx, dz) of the camera's
		// ground forward): body + head yaw in first person, the render camera otherwise.
		bool LookYaw( float &yaw );
		// Camera task state byte from the camera hook (1 follow, 2 LB held, 3 refocus,
		// 4 free aim, 5 NPC talk).
		void NoteCameraState( int state );
		// True once after the camera entered state 5; the stereo renderer then recenters.
		bool ConsumeRecenterRequest();
		// The dressing room (camera state 5 with its background tiles, which NPC talk lacks):
		// the game's own camera is shown, like third person, whatever vr.cameraMode says.
		void NoteDressingRoom();
		bool DressingRoom();
		int CameraState();
		// The render camera's yaw (game convention) and pitch (positive = up).
		bool RenderCameraYawPitch( float &yaw, float &pitch );
		// Leash the first-person head to the body: physical lean/crouch beyond
		// vr.fpLeanLimit / vr.fpCrouchLimit drags the anchor instead of the view.
		void UpdateFirstPersonAnchor( const EyePose views[ 2 ] );
		// Room walk (vr.roomWalk): the left stick that walks the character toward where the
		// head stepped in the room. userStickIdle = the player isn't using the stick (it takes
		// over). False = nothing to do; the caller leaves the stick alone.
		bool RoomWalkStick( bool userStickIdle, float &x, float &y );
		// Eye at the head, expressed in the camera the game actually renders from, so any
		// game re-aim (NPC talk camera, smoothing) is cancelled out.
		void BuildEyeFirstPerson( const EyePose &eye, float unitsPerMeter, const D3DMATRIX &gameProj,
			D3DMATRIX &view, D3DMATRIX &proj );

		// LOCAL-space pose of the body-locked HUD/menu panel (vr.hudFollow): in front of
		// the body, lazily re-centred when the head looks away past vr.hudFollowAngle.
		void HudPanelPose( const EyePose views[ 2 ], float distanceMeters, float position[ 3 ], float orientation[ 4 ] );

		// LOCAL-space pose for a panel in front of the reference, dropped by dropMeters.
		void PanelPose( float distanceMeters, float dropMeters, float position[ 3 ], float orientation[ 4 ] );
	}
}
