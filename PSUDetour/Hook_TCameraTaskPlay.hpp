#pragma once

#include "../PSUEx.h"
#include "../Misc/Log.h"
#include "../PSUOverlay/FrameTrace.h"
#include "../PSUStructs/PSUPlayerData.h"
#include "../PSUVR/VRCamera.h"
#include "../PSUVR/StereoRenderer.h"
#include "../PSUVR/VRProbe.h"
#include "../PSUVR/VRSyntheticPad.h"

#include <cmath>

namespace TCameraTaskPlay
{
	struct CameraTaskPlayData {
		void *ptr_a;               // Possibly vtable
		void *ptr_b;               // Possibly vtable

		uint32_t m_bitflags;         // Flags related to camera states or modes. Changing the first byte to 01 causes the camera to stop following.
		void *selfPtr;             // Pointer to self

		uint32_t unknown_a;        // Possibly an ID or reference index
		void *m_ptr_d;               // Another Pointer (Potentially related to task management)
		char padding_18[ 0x08 ];

		uint16_t m_state;            // Camera State (Bitflag or Enum)
		uint16_t unknown_b;        // Counter or mode indicator (Increases after first-person view)

		void *m_ptr_e;               // Unknown Pointer
		void *m_ptr_f;               // Unknown Pointer
		void *m_playerDataPtr;       // Pointer to player data

		int32_t m_playerState;       // Player's current state

		float m_leadOffset;          // Camera Lead Offset
		float m_focusHeight;         // Height above player where camera focuses
		float m_offsetAboveFocus;    // Offset above focus point
		float m_nearDeadZone;        // Minimum distance from player to focus point

		float m_followDistanceNear;           // Minimum follow distance from the player
		float m_followDistanceFar;            // Maximum follow distance from the player
		float m_followSmoothing;		// Smoothness factor for following

		float m_refocusSpeed;         // Speed of refocus adjustment
		float rotationSpeed;        // Camera rotation speed when refocusing (higher = slower);
		float fpsViewSpeed;			// Transition speed to First Person
		float unknown_c;            // Unknown float

		float m_updateDist;			// Update Distance ( 0 for super smoothing )

		float m_refocusHeight;        // Height adjustment when refocusing
		float m_refocusDistanceNear;  // COULD BE WRONG
		float m_refocusDistanceFar;   // Distance adjustment for refocusing

		float m_upperViewAngle;		// Vertical view angle
		float m_lowerViewAngle;		// Lower view angle

		float unknown_d;            // Something with refocusing behind you
		float unknown_e;            // Unknown float

		float m_rotateSpeedH;    // Horizontal refocus turning speed
		float m_rotateSpeedV;    // Vertical refocus turning speed

		float unknown_f;            // Unknown float
		float unknown_g;            // Unknown float

		int32_t unknown_h;          // Unknown int	(Is 1)
		int32_t unknown_i;          // Unknown. Raises and lowers as things appear, sometimes.

		float m_turnAcceleration;     // How fast the camera reaches maximum turning speed (1 for instant, 0.01 for slow)

		char padding_9C[ 44 ];

		Vector3F m_position;          // Actual camera position (X, Y, Z)
		Vector3F m_focusPosition;     // Focus position the camera is trying to look at (X, Y, Z)
	};

	typedef int( __thiscall *function48E4C0 )( int thisPtr );
	function48E4C0 pOriginal48E4C0 = reinterpret_cast< function48E4C0 >( 0x0048E4C0 );

	typedef int( __thiscall *function48DA90 )( int thisPtr );
	function48DA90 pOriginal48DA90 = reinterpret_cast< function48DA90 >( 0x0048DA90 );

	// Calls onCamera Rotation/Refocus
	int __fastcall Hook_48E4C0( int thisPtr )
	{
		// thisPtr may be the same as the global camera pointer
		// haven't checked it.
		
		// Call the original function
		return pOriginal48E4C0( thisPtr );
	}

	// The camera object the view is built from: *( *( task + 0x24 ) + 4 ).
	// 0x0048FA00 copies its position (+0xFC) and target (+0x108) into m_position /
	// m_focusPosition, then writes new values back through 0x0048B9A0 / 0x0048B9F0,
	// which set bit 0 of the flags at +0x130 (view dirty).
	constexpr uintptr_t kRenderCameraPosition = 0xFC;
	constexpr uintptr_t kRenderCameraTarget = 0x108;
	constexpr uintptr_t kRenderCameraFlags = 0x130;

	uint8_t *RenderCameraObject( const CameraTaskPlayData *camera )
	{
		auto *holder = static_cast< uint8_t * >( camera->m_ptr_e );
		if( !holder || !PSUMemory::IsValidPointer( holder + 4 ) )
		{
			return nullptr;
		}
		auto *object = *reinterpret_cast< uint8_t ** >( holder + 4 );
		if( !object || !PSUMemory::IsValidPointer( object + kRenderCameraFlags ) )
		{
			return nullptr;
		}
		return object;
	}

	constexpr uint8_t kCameraStateFreeAim = 4;

	// Local player aim angles (16-bit game angles, 0x10000 = 360 degrees, 2 pi / 65536 per unit).
	constexpr uintptr_t kPlayerTable = 0x009CA8AC;
	constexpr uintptr_t kPlayerFacing = 0x124;     // int, low 16 bits are the angle
	constexpr uintptr_t kPlayerAimPitch = 0x220;   // int16, positive = down (shots fly at -this)
	constexpr float kRadiansToGameAngle = 65536.0f / 6.2831853f;
	constexpr int kMaxAimPitch = 0x2AAA;           // 60 degrees

	void WriteFreeAim( float yaw, float pitch )
	{
		const uintptr_t player = PSUMemory::ReadAbsoluteMemory< uintptr_t >( kPlayerTable );
		if( !player )
		{
			return;
		}
		// Facing: move by the shortest way to the wanted angle, keeping any whole turns.
		const int facing = PSUMemory::ReadAbsoluteMemory< int >( player + kPlayerFacing );
		const int wantFacing = static_cast< int >( std::lround( yaw * kRadiansToGameAngle ) );
		const int delta = static_cast< int16_t >( static_cast< uint16_t >( wantFacing - facing ) );
		PSUMemory::WriteAbsoluteMemory< int >( player + kPlayerFacing, facing + delta );
		// Our pitch is positive up; the game's aim pitch is positive down.
		int wantPitch = -static_cast< int >( std::lround( pitch * kRadiansToGameAngle ) );
		wantPitch = wantPitch > kMaxAimPitch ? kMaxAimPitch : ( wantPitch < -kMaxAimPitch ? -kMaxAimPitch : wantPitch );
		PSUMemory::WriteAbsoluteMemory< int16_t >( player + kPlayerAimPitch, static_cast< int16_t >( wantPitch ) );
	}

	// Calls any time the camera updates.
	int __fastcall Hook_48DA90( int thisPtr )
	{
		CameraTaskPlayData *camera = reinterpret_cast< CameraTaskPlayData * >( thisPtr );

		static bool s_logged = false;
		if( !s_logged )
		{
			s_logged = true;
			PSU_LOG( "TCameraTaskPlay_Update @ 0x0048DA90 first hit (this=%p)\n", ( void * )( uintptr_t )thisPtr );
			FrameTrace::NotifyGameplay();
		}

		Global::cameraPosition = camera->m_position;
		Global::cameraFocus = camera->m_focusPosition;

		auto viewMatrix = PSUMemory::ReadAbsoluteMemory< Matrix4x4 >( 0x00A85C30 );
		auto projectionMatrix = PSUMemory::ReadAbsoluteMemory< Matrix4x4 >( 0x00A85B30 );
		Global::viewProjectionMatrix = viewMatrix * projectionMatrix;

		Global::cameraForward = Global::cameraFocus - Global::cameraPosition;
		Global::cameraForward.Normalize();

		// Update the camera data with our own settings
		auto &config = Config::Get().camera;

		camera->m_followDistanceNear = Constant::Camera::FOLLOW_NEAR * config.m_followDistance;
		camera->m_followDistanceFar = Constant::Camera::FOLLOW_FAR * config.m_followDistance;
		camera->m_followSmoothing = config.m_smoothing;

		camera->m_refocusDistanceNear = Constant::Camera::REFOCUS_DISTANCE_NEAR * config.m_followDistance;
		camera->m_refocusDistanceFar = Constant::Camera::REFOCUS_DISTANCE_FAR * config.m_followDistance;

		camera->m_focusHeight = config.m_followHeight;
		camera->m_refocusHeight = config.m_followHeight;

		camera->m_refocusSpeed = config.m_refocusSpeed;
		camera->m_rotateSpeedH = Constant::Camera::ROTATION_SPEED_H * config.m_rotationSpeedH;
		camera->m_rotateSpeedV = Constant::Camera::ROTATION_SPEED_V * config.m_rotationSpeedV;

		if( camera->m_turnAcceleration < config.m_turnAcceleration )
		{
			camera->m_turnAcceleration = config.m_turnAcceleration;
		}

		camera->m_updateDist = Constant::Camera::UPDATE_DISTANCE;

		// SteamVR often bleeds controller/mouse look into the flat game while an
		// OpenXR session is active. Kill orbit rates until VR input is owned by us.
		// First person keeps them only when XInput is missing and the orbit is read back as turning.
		const bool firstPerson = PSUVR::VRCamera::FirstPersonMode();
		// Camera state 4 = free aim (R3 with a gun): the right stick aims, so its rates stay.
		const bool freeAim = *reinterpret_cast< uint8_t * >( reinterpret_cast< uint8_t * >( camera ) + 0x20 ) == kCameraStateFreeAim;
		if( Config::Get().vr.m_enabled && !Config::Get().vr.m_paused && !freeAim &&
			( !firstPerson || PSUVR::VRCamera::TurnFromStick() ) )
		{
			static bool s_loggedVrCam = false;
			if( !s_loggedVrCam )
			{
				s_loggedVrCam = true;
				PSU_LOG( "TCameraTaskPlay: VR enabled — zeroing rotate speeds (SteamVR look bleed)\n" );
			}
			camera->m_rotateSpeedH = 0.0f;
			camera->m_rotateSpeedV = 0.0f;
			camera->m_turnAcceleration = 0.0f;
		}

		// Free aim, first person: the aim camera (handler 0x0048E9A0) and the bullets aim along the
		// player's facing (+0x124) and aim pitch (+0x220), 16-bit game angles. Write the gun
		// hand's angles there before the update, so the aim matches the hand with no lag.
		float aimYaw = 0.0f, aimPitch = 0.0f;
		if( freeAim && PSUVR::VRSyntheticPad::DirectAim( aimYaw, aimPitch ) )
		{
			WriteFreeAim( aimYaw, aimPitch );
		}

		const int result = pOriginal48DA90( thisPtr );
		PSUVR::VRProbe::NoteCameraTask( camera );
		// A live game task: its parent chain leads to the task tree root (flat shop / mission menus).
		PSUVR::StereoRenderer::NoteGameTask( camera );

		// Camera state byte (+0x20): 1 = follow, 2 = LB held (lock-on), 3 = refocus,
		// 4 = free aim, 5 = NPC talk.
		const int cameraState = *reinterpret_cast< uint8_t * >( reinterpret_cast< uint8_t * >( camera ) + 0x20 );
		PSUVR::VRCamera::NoteCameraState( cameraState );
		// Free aim: the game's aim camera stays as it is (the headset looks around on top).
		// The dressing room shows the game's framed shot (no first / third person steer).
		const bool steerAllowed = cameraState != kCameraStateFreeAim && !PSUVR::VRCamera::DressingRoom();
		uint8_t *renderCamera = RenderCameraObject( camera );
		// The eyes are built from what the game actually renders (talk camera included).
		PSUVR::VRCamera::NoteRenderCamera(
			renderCamera ? renderCamera + kRenderCameraPosition : nullptr,
			renderCamera ? renderCamera + kRenderCameraTarget : nullptr );

		// First person: steer the orbit camera level behind the head so the game's
		// camera-relative movement and culling follow the player.
		bool steered = false;
		if( firstPerson && steerAllowed )
		{
			// Get() refreshes the player pointer; it returns a zeroed dummy while not ready.
			const PSUPlayer &player = PSUPlayer::Get();
			if( PSUPlayer::IsReady() && renderCamera )
			{
				const float playerPos[ 3 ] = { player.position.x, player.position.y, player.position.z };
				steered = PSUVR::VRCamera::SteerFirstPerson(
					&camera->m_position.x, &camera->m_focusPosition.x, playerPos, cameraState );
				if( steered )
				{
					// m_position / m_focusPosition are mirrors; the view is built from the camera object.
					std::memcpy( renderCamera + kRenderCameraPosition, &camera->m_position.x, sizeof( float ) * 3 );
					std::memcpy( renderCamera + kRenderCameraTarget, &camera->m_focusPosition.x, sizeof( float ) * 3 );
					*reinterpret_cast< uint32_t * >( renderCamera + kRenderCameraFlags ) |= 1u;
				}
			}
		}
		// Free aim in first person: the game turns its aim camera; the view stays on the head.
		if( !steered && firstPerson && cameraState == kCameraStateFreeAim )
		{
			const PSUPlayer &player = PSUPlayer::Get();
			if( PSUPlayer::IsReady() )
			{
				const float playerPos[ 3 ] = { player.position.x, player.position.y, player.position.z };
				steered = PSUVR::VRCamera::HoldFirstPerson( playerPos );
			}
		}
		if( !steered )
		{
			PSUVR::VRCamera::ClearFirstPerson();
		}

		// Third person: our own orbit camera (right stick zoom, Back + stick orbit steps).
		bool steeredThird = false;
		if( !firstPerson && steerAllowed && PSUVR::VRCamera::ThirdPersonMode() && renderCamera )
		{
			const PSUPlayer &player = PSUPlayer::Get();
			if( PSUPlayer::IsReady() )
			{
				const float playerPos[ 3 ] = { player.position.x, player.position.y, player.position.z };
				steeredThird = PSUVR::VRCamera::SteerThirdPerson(
					&camera->m_position.x, &camera->m_focusPosition.x, playerPos, cameraState );
				if( steeredThird )
				{
					std::memcpy( renderCamera + kRenderCameraPosition, &camera->m_position.x, sizeof( float ) * 3 );
					std::memcpy( renderCamera + kRenderCameraTarget, &camera->m_focusPosition.x, sizeof( float ) * 3 );
					*reinterpret_cast< uint32_t * >( renderCamera + kRenderCameraFlags ) |= 1u;
				}
			}
		}
		if( !steeredThird )
		{
			PSUVR::VRCamera::ClearThirdPerson( firstPerson || !PSUVR::VRCamera::ThirdPersonMode() );
		}
		return result;
	}

	void Hook()
	{
		//DetourAttach( &( PVOID & )pOriginal48E4C0, reinterpret_cast< PVOID >( Hook_48E4C0 ) );
		DetourAttach( &( PVOID & )pOriginal48DA90, reinterpret_cast< PVOID >( Hook_48DA90 ) );
	}
}