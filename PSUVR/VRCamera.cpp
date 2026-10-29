#include "VRCamera.h"

#include "StereoRenderer.h"
#include "VRControllers.h"
#include "VRHands.h"
#include "VRGamepad.h"
#include "VRSyntheticPad.h"
#include "VRSystem.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <Windows.h>

#include <cmath>

namespace PSUVR
{
	namespace
	{
		struct Quat
		{
			float x, y, z, w;
		};

		struct Vec3
		{
			float x, y, z;
		};

		constexpr float kPi = 3.14159265f;

		Quat Multiply( const Quat &a, const Quat &b )
		{
			return {
				a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
				a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
				a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
				a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
		}

		Vec3 Rotate( const Quat &q, const Vec3 &v )
		{
			const Quat p{ v.x, v.y, v.z, 0.0f };
			const Quat conj{ -q.x, -q.y, -q.z, q.w };
			const Quat r = Multiply( Multiply( q, p ), conj );
			return { r.x, r.y, r.z };
		}

		Quat YawQuat( float yaw )
		{
			return { 0.0f, std::sin( yaw * 0.5f ), 0.0f, std::cos( yaw * 0.5f ) };
		}

		// Rotation about +X; positive pitches -Z (forward) toward +Y (up).
		Quat PitchQuat( float pitch )
		{
			return { std::sin( pitch * 0.5f ), 0.0f, 0.0f, std::cos( pitch * 0.5f ) };
		}

		Quat ToQuat( const float o[ 4 ] ) { return { o[ 0 ], o[ 1 ], o[ 2 ], o[ 3 ] }; }
		Vec3 ToVec( const float p[ 3 ] ) { return { p[ 0 ], p[ 1 ], p[ 2 ] }; }
		Vec3 Add( const Vec3 &a, const Vec3 &b ) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		Vec3 Scale( const Vec3 &a, float s ) { return { a.x * s, a.y * s, a.z * s }; }

		float WrapAngle( float a )
		{
			while( a > kPi ) a -= 2.0f * kPi;
			while( a < -kPi ) a += 2.0f * kPi;
			return a;
		}

		// Yaw of an OpenXR orientation around +Y (0 = looking down -Z).
		float PoseYaw( const float orientation[ 4 ] )
		{
			const Vec3 forward = Rotate( ToQuat( orientation ), { 0.0f, 0.0f, -1.0f } );
			return std::atan2( -forward.x, -forward.z );
		}

		bool s_haveReference = false;
		Vec3 s_refPosition{};
		float s_refYaw = 0.0f;
		// First person: tracking-space point the head is leashed to (follows the head
		// once it leans further than fpLeanLimit / fpCrouchLimit).
		Vec3 s_fpAnchor{};

		// Body-locked HUD panel: tracking-space yaw it faces, eased toward the head
		// only after the head has looked away by more than vr.hudFollowAngle.
		float s_hudYaw = 0.0f;
		bool s_hudFollowing = false;
		LARGE_INTEGER s_hudLastTime{};

		// First person (game world: forward = ( sin, 0, cos ), right = ( -cos, 0, sin ),
		// turning left increases the yaw). The game camera is steered behind the head;
		// the eyes are built from wherever the game actually renders from.
		bool s_fpPrimed = false;
		bool s_fpActive = false;
		bool s_turnArmed = true;
		bool s_turnFromStick = false;

		// Right stick for turning: the XInput pad's or the right motion controller's,
		// whichever is pushed further (both may be connected).
		bool TurnStick( float &x, float &y )
		{
			const bool pad = VRGamepad::RightStick( x, y );
			if( !VRControllers::Active( 1 ) )
			{
				return pad;
			}
			constexpr float kDeadzone = 0.25f;
			const float *stick = VRControllers::State( 1 ).stick;
			float motion[ 2 ]{};
			for( int i = 0; i < 2; ++i )
			{
				const float magnitude = std::fabs( stick[ i ] );
				const float scaled = magnitude <= kDeadzone ? 0.0f : ( magnitude - kDeadzone ) / ( 1.0f - kDeadzone );
				motion[ i ] = ( stick[ i ] < 0.0f ? -1.0f : 1.0f ) * ( scaled > 1.0f ? 1.0f : scaled );
			}
			if( !pad || motion[ 0 ] * motion[ 0 ] + motion[ 1 ] * motion[ 1 ] > x * x + y * y )
			{
				static bool s_logged = false;
				if( !s_logged && ( motion[ 0 ] != 0.0f || motion[ 1 ] != 0.0f ) )
				{
					s_logged = true;
					PSU_LOG( "VRCamera: turning from the right motion controller stick (%.2f, %.2f)\n", stick[ 0 ], stick[ 1 ] );
				}
				x = motion[ 0 ];
				y = motion[ 1 ];
			}
			return true;
		}
		// Camera-delta fallback: head yaw last frame, and when the head last turned.
		float s_prevHeadYaw = 0.0f;
		DWORD s_headTurnTick = 0;
		bool s_loggedTurnSource = false;
		float s_bodyYaw = 0.0f;
		float s_lastWrittenYaw = 0.0f;
		float s_camDistance = 0.0f;  // game units from steered camera to head
		float s_steerHmdYaw = 0.0f;  // HMD yaw (relative to the reference) the camera was steered to
		Vec3 s_eyeLook{ 0.0f, 0.0f, -1.0f };  // body-frame level look at the last eye build
		Vec3 s_headWorld{};          // game world head position at the last steer
		LARGE_INTEGER s_lastSteerTime{};
		// The play camera task can stop updating; first person must not keep stale state.
		DWORD s_lastSteerTick = 0;
		constexpr DWORD kSteerStaleMs = 250;

		// Render camera object fields (position +0xFC, target +0x108), noted by the camera
		// hook, and the values captured right before the eye passes.
		const float *s_renderCamPos = nullptr;
		const float *s_renderCamTarget = nullptr;
		bool s_haveRenderCam = false;
		Vec3 s_renderPos{};
		Vec3 s_renderTarget{};

		unsigned s_turnLogs = 0;

		// Third person (VR): our own orbit camera. Yaw only changes by Back + right stick
		// steps, distance by right stick zoom; no auto-follow rotation.
		bool s_tpPrimed = false;
		float s_tpYaw = 0.0f;
		bool s_tpOrbitArmed = true;
		bool s_tpHeightArmed = true;
		bool s_tpActive = false;   // our camera (and its user-chosen tilt) is what renders
		LARGE_INTEGER s_tpLastTime{};

		// LB (refocus / lock-on): the game's camera direction is followed only in a window
		// opened by the player's press, never for the camera's own automatic refocus.
		bool s_lbWasDown = false;
		DWORD s_lbPressTick = 0;
		int s_lastCameraState = -1;
		unsigned s_cameraStateLogs = 0;

		bool LbWindowOpen( int cameraState )
		{
			unsigned short buttons = 0;
			VRGamepad::Buttons( buttons );
			const bool lbDown = ( buttons & 0x0100 ) != 0;  // XINPUT_GAMEPAD_LEFT_SHOULDER
			if( lbDown && !s_lbWasDown )
			{
				s_lbPressTick = GetTickCount();
			}
			s_lbWasDown = lbDown;
			if( cameraState != s_lastCameraState && s_cameraStateLogs < 60 )
			{
				++s_cameraStateLogs;
				PSU_LOG( "VRCamera: cam state %d -> %d (LB %s)\n", s_lastCameraState, cameraState, lbDown ? "held" : "up" );
			}
			s_lastCameraState = cameraState;
			if( cameraState == 5 )  // NPC talk
			{
				return false;
			}
			if( lbDown )
			{
				return true;
			}
			const DWORD windowMs = static_cast< DWORD >( Config::Get().vr.m_lbWindowSeconds * 1000.0f );
			return s_lbPressTick != 0 && cameraState != 1 && GetTickCount() - s_lbPressTick < windowMs;
		}

		// Steered camera distance behind the head. Further back widens what the
		// game's frustum culling keeps around the head.
		constexpr float kSteerDistanceUnits = 25.0f;
		// Fallback (no XInput pad): per-tick game camera yaw change that counts as a
		// right-stick turn. High enough to ignore the follow camera's own drift.
		constexpr float kCameraDeltaDeadband = 1.5f * ( kPi / 180.0f );
		// Snap turn on the stick: fire past kSnapPress, re-arm under kSnapRelease.
		constexpr float kSnapPress = 0.6f;
		constexpr float kSnapRelease = 0.3f;

		// Captured render camera yaw (game convention) and pitch (positive = looking up).
		bool RenderCameraAngles( float &yaw, float &pitch )
		{
			if( !s_haveRenderCam )
			{
				return false;
			}
			const float dx = s_renderTarget.x - s_renderPos.x;
			const float dy = s_renderTarget.y - s_renderPos.y;
			const float dz = s_renderTarget.z - s_renderPos.z;
			const float horizontal = std::sqrt( dx * dx + dz * dz );
			if( horizontal < 1.0e-4f && std::fabs( dy ) < 1.0e-4f )
			{
				return false;
			}
			yaw = std::atan2( dx, dz );
			pitch = std::atan2( dy, horizontal );
			return true;
		}

		// View/projection for an eye at p with orientation q, both in game camera space
		// (right-handed, +Y up, looking down -Z, game units).
		void BuildViewProj( const EyePose &eye, const D3DMATRIX &gameProj, const Vec3 &p, const Quat &q,
			D3DMATRIX &view, D3DMATRIX &proj )
		{
			// Column-vector rotation of the eye in camera space. D3D row vectors use
			// v_eye = ( v_cam - p ) * R, i.e. VIEW = T(-p) * R.
			const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
			const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
			const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
			const float r[ 3 ][ 3 ] = {
				{ 1.0f - 2.0f * ( yy + zz ), 2.0f * ( xy - wz ), 2.0f * ( xz + wy ) },
				{ 2.0f * ( xy + wz ), 1.0f - 2.0f * ( xx + zz ), 2.0f * ( yz - wx ) },
				{ 2.0f * ( xz - wy ), 2.0f * ( yz + wx ), 1.0f - 2.0f * ( xx + yy ) } };

			view = {};
			view._11 = r[ 0 ][ 0 ]; view._12 = r[ 0 ][ 1 ]; view._13 = r[ 0 ][ 2 ];
			view._21 = r[ 1 ][ 0 ]; view._22 = r[ 1 ][ 1 ]; view._23 = r[ 1 ][ 2 ];
			view._31 = r[ 2 ][ 0 ]; view._32 = r[ 2 ][ 1 ]; view._33 = r[ 2 ][ 2 ];
			view._41 = -( p.x * r[ 0 ][ 0 ] + p.y * r[ 1 ][ 0 ] + p.z * r[ 2 ][ 0 ] );
			view._42 = -( p.x * r[ 0 ][ 1 ] + p.y * r[ 1 ][ 1 ] + p.z * r[ 2 ][ 1 ] );
			view._43 = -( p.x * r[ 0 ][ 2 ] + p.y * r[ 1 ][ 2 ] + p.z * r[ 2 ][ 2 ] );
			view._44 = 1.0f;

			// Right-handed off-center projection (camera looks down -Z, w = -z).
			const float left = std::tan( eye.fovAngleLeft );
			const float right = std::tan( eye.fovAngleRight );
			const float up = std::tan( eye.fovAngleUp );
			const float down = std::tan( eye.fovAngleDown );
			proj = gameProj;
			proj._11 = 2.0f / ( right - left );
			proj._12 = 0.0f;
			proj._21 = 0.0f;
			proj._22 = 2.0f / ( up - down );
			proj._31 = ( right + left ) / ( right - left );
			proj._32 = ( up + down ) / ( up - down );
		}

		void LogTurn( float delta )
		{
			if( s_turnLogs < 12 )
			{
				++s_turnLogs;
				PSU_LOG( "VRCamera FP: turn %.2f deg -> body=%.1f deg (%s, %s)\n",
					delta * ( 180.0f / kPi ), s_bodyYaw * ( 180.0f / kPi ),
					Config::Get().vr.m_snapTurn ? "snap" : "smooth", s_turnFromStick ? "stick" : "camera-delta" );
			}
		}

		// Right stick X (-1..1): stick right turns right, i.e. lowers the game yaw.
		void ApplyStickTurn( float stickX, float dt )
		{
			const auto &vr = Config::Get().vr;
			const float magnitude = std::fabs( stickX );
			if( vr.m_snapTurn )
			{
				if( magnitude < kSnapRelease )
				{
					s_turnArmed = true;
					return;
				}
				if( magnitude < kSnapPress || !s_turnArmed )
				{
					return;
				}
				s_turnArmed = false;
				const float step = vr.m_snapTurnDegrees * ( kPi / 180.0f );
				const float delta = stickX > 0.0f ? -step : step;
				s_bodyYaw = WrapAngle( s_bodyYaw + delta );
				LogTurn( delta );
				return;
			}
			if( magnitude <= 0.0f )
			{
				return;
			}
			const float delta = -stickX * vr.m_fpTurnSpeed * ( kPi / 180.0f ) * dt;
			s_bodyYaw = WrapAngle( s_bodyYaw + delta );
			LogTurn( delta );
		}

		// Fallback without XInput: the game's own right-stick orbit, read back as a yaw change.
		void ApplyCameraDeltaTurn( float delta )
		{
			const auto &vr = Config::Get().vr;
			if( std::fabs( delta ) < kCameraDeltaDeadband )
			{
				s_turnArmed = true;
				return;
			}
			if( vr.m_snapTurn )
			{
				if( !s_turnArmed )
				{
					return;
				}
				s_turnArmed = false;
				const float step = vr.m_snapTurnDegrees * ( kPi / 180.0f );
				delta = delta > 0.0f ? step : -step;
			}
			s_bodyYaw = WrapAngle( s_bodyYaw + delta );
			LogTurn( delta );
		}

		float SteerDeltaSeconds()
		{
			LARGE_INTEGER now{};
			LARGE_INTEGER frequency{};
			QueryPerformanceCounter( &now );
			QueryPerformanceFrequency( &frequency );
			float dt = 0.0f;
			if( s_lastSteerTime.QuadPart != 0 && frequency.QuadPart > 0 )
			{
				dt = static_cast< float >( now.QuadPart - s_lastSteerTime.QuadPart ) /
					static_cast< float >( frequency.QuadPart );
			}
			s_lastSteerTime = now;
			return dt < 0.0f ? 0.0f : ( dt > 0.1f ? 0.1f : dt );
		}
	}

	namespace VRCamera
	{
		void Recenter( const EyePose views[ 2 ] )
		{
			s_refPosition = {
				( views[ 0 ].position[ 0 ] + views[ 1 ].position[ 0 ] ) * 0.5f,
				( views[ 0 ].position[ 1 ] + views[ 1 ].position[ 1 ] ) * 0.5f,
				( views[ 0 ].position[ 2 ] + views[ 1 ].position[ 2 ] ) * 0.5f };
			// Yaw only, so the game camera's horizon stays level with the real one.
			s_refYaw = PoseYaw( views[ 0 ].orientation );
			s_haveReference = true;
			s_fpAnchor = s_refPosition;
			s_hudYaw = s_refYaw;
			s_hudFollowing = false;
			// First person re-derives body yaw from the camera on its next steer; third
			// person restarts behind wherever the camera looks.
			s_fpPrimed = false;
			s_tpPrimed = false;
			PSU_LOG( "VRCamera: recentered at (%.3f, %.3f, %.3f) yaw=%.1f deg\n",
				s_refPosition.x, s_refPosition.y, s_refPosition.z, s_refYaw * 57.29578f );
		}

		bool HasReference()
		{
			return s_haveReference;
		}

		void NoteRenderCamera( const void *cameraPosition, const void *cameraTarget )
		{
			s_renderCamPos = static_cast< const float * >( cameraPosition );
			s_renderCamTarget = static_cast< const float * >( cameraTarget );
		}

		void CaptureRenderCamera()
		{
			s_haveRenderCam = false;
			if( !s_renderCamPos || !s_renderCamTarget )
			{
				return;
			}
			__try
			{
				s_renderPos = { s_renderCamPos[ 0 ], s_renderCamPos[ 1 ], s_renderCamPos[ 2 ] };
				s_renderTarget = { s_renderCamTarget[ 0 ], s_renderCamTarget[ 1 ], s_renderCamTarget[ 2 ] };
				s_haveRenderCam = true;
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				s_haveRenderCam = false;
			}
		}

		void BuildEye( const EyePose &eye, float unitsPerMeter, const D3DMATRIX &gameProj,
			D3DMATRIX &view, D3DMATRIX &proj )
		{
			// Third person: HMD on top of the game's orbit camera. The camera's own pitch
			// is removed so the horizon stays level with the real one.
			// Our own third-person camera's pitch is the user's tilt (tpPitch): keep it.
			float camYaw = 0.0f, camPitch = 0.0f;
			const bool level = !s_tpActive && RenderCameraAngles( camYaw, camPitch );
			const Quat unpitch = level ? PitchQuat( -camPitch ) : Quat{ 0.0f, 0.0f, 0.0f, 1.0f };
			const Quat bodyInverse = YawQuat( -s_refYaw );
			const Vec3 offset = ToVec( eye.position );
			const Vec3 local = Rotate( bodyInverse,
				{ offset.x - s_refPosition.x, offset.y - s_refPosition.y, offset.z - s_refPosition.z } );
			const Vec3 p = Rotate( unpitch, Scale( local, unitsPerMeter ) );
			const Quat q = Multiply( unpitch, Multiply( bodyInverse, ToQuat( eye.orientation ) ) );
			BuildViewProj( eye, gameProj, p, q, view, proj );
		}

		bool FirstPersonMode()
		{
			// Stereo scenes only; elsewhere the game camera stays on the flat virtual screen.
			return Config::Get().vr.m_cameraMode == "firstPerson" && StereoRenderer::SceneAllowsStereo();
		}

		bool SteerFirstPerson( float cameraPosition[ 3 ], float focusPosition[ 3 ],
			const float playerPosition[ 3 ], int cameraState )
		{
			EyePose views[ 2 ];
			EyePose viewSpace[ 2 ];
			if( !FirstPersonMode() || !s_haveReference || !System::Get().NextViews( views, viewSpace ) )
			{
				s_fpActive = false;
				return false;
			}
			const auto &vr = Config::Get().vr;
			const float dt = SteerDeltaSeconds();
			// States 1 (follow) and 3 (refocus) are normal play. Others (5 = NPC talk) are
			// game-driven: no head steering and no turning, so nothing moves by itself.
			const bool playState = cameraState == 1 || cameraState == 3;

			// Yaw the game left the camera at after its own update.
			const float dx = focusPosition[ 0 ] - cameraPosition[ 0 ];
			const float dz = focusPosition[ 2 ] - cameraPosition[ 2 ];
			const bool haveGameYaw = ( dx * dx + dz * dz ) > 1.0e-6f;
			const float gameYaw = haveGameYaw ? std::atan2( dx, dz ) : s_lastWrittenYaw;
			float stickX = 0.0f;
			float stickY = 0.0f;
			s_turnFromStick = TurnStick( stickX, stickY );
			if( !s_loggedTurnSource )
			{
				s_loggedTurnSource = true;
				PSU_LOG( "VRCamera FP: turn source = %s\n", s_turnFromStick ? "stick" : "camera-delta" );
			}
			// The game eases its camera toward the yaw we write, so while (and shortly after)
			// the head turns, the camera lags it: that lag is not a stick turn.
			const float headYaw = PoseYaw( views[ 0 ].orientation );
			if( std::fabs( WrapAngle( headYaw - s_prevHeadYaw ) ) > 0.3f * ( kPi / 180.0f ) )
			{
				s_headTurnTick = GetTickCount();
			}
			s_prevHeadYaw = headYaw;
			const bool headSettled = GetTickCount() - s_headTurnTick > 400;
			if( !s_fpPrimed )
			{
				// Body forward = where the camera points now (after recenter: where you look).
				s_bodyYaw = gameYaw;
				s_fpPrimed = true;
				s_turnArmed = true;
				PSU_LOG( "VRCamera FP: primed body=%.1f deg, player=(%.2f, %.2f, %.2f)\n",
					s_bodyYaw * ( 180.0f / kPi ), playerPosition[ 0 ], playerPosition[ 1 ], playerPosition[ 2 ] );
			}
			else if( playState && s_turnFromStick )
			{
				ApplyStickTurn( stickX, dt );
			}
			else if( playState && haveGameYaw && headSettled )
			{
				ApplyCameraDeltaTurn( WrapAngle( gameYaw - s_lastWrittenYaw ) );
			}

			// "head": forward = where you look (stick turns add on top). "body": FPS-style.
			const bool headMove = playState && vr.m_fpMoveDirection != "body";
			const float hmdYaw = headMove ? WrapAngle( PoseYaw( views[ 0 ].orientation ) - s_refYaw ) : 0.0f;
			// Optional (fpLbCamera "follow"): during the LB window keep the game's lock-on
			// direction by solving the body yaw for it. Off by default (rotates the world).
			const bool lbWindow = LbWindowOpen( cameraState );
			if( vr.m_fpLbCamera == "follow" && lbWindow && haveGameYaw )
			{
				s_bodyYaw = WrapAngle( gameYaw - hmdYaw );
			}
			const float yaw = WrapAngle( s_bodyYaw + hmdYaw );
			const float fx = std::sin( yaw );
			const float fz = std::cos( yaw );
			float lean[ 2 ]{};
			VRHands::EyeLean( lean[ 0 ], lean[ 1 ] );
			const float head[ 3 ] = {
				playerPosition[ 0 ] + lean[ 0 ],
				playerPosition[ 1 ] + vr.m_fpEyeHeight * kGameUnitsPerMeter,
				playerPosition[ 2 ] + lean[ 1 ] };
			focusPosition[ 0 ] = head[ 0 ];
			focusPosition[ 1 ] = head[ 1 ];
			focusPosition[ 2 ] = head[ 2 ];
			cameraPosition[ 0 ] = head[ 0 ] - fx * kSteerDistanceUnits;
			cameraPosition[ 1 ] = head[ 1 ];
			cameraPosition[ 2 ] = head[ 2 ] - fz * kSteerDistanceUnits;

			s_headWorld = { head[ 0 ], head[ 1 ], head[ 2 ] };
			s_lastWrittenYaw = yaw;
			s_lastSteerTick = GetTickCount();
			s_steerHmdYaw = hmdYaw;
			s_camDistance = kSteerDistanceUnits;
			s_fpActive = true;
			return true;
		}

		bool HoldFirstPerson( const float playerPosition[ 3 ] )
		{
			if( !FirstPersonMode() || !s_fpPrimed || !s_haveReference )
			{
				return false;
			}
			// The head follows the body; the eyes keep cancelling whatever the game camera does
			// (BuildEyeFirstPerson), so the view doesn't follow the game's aim camera.
			const auto &vr = Config::Get().vr;
			float lean[ 2 ]{};
			VRHands::EyeLean( lean[ 0 ], lean[ 1 ] );
			s_headWorld = { playerPosition[ 0 ] + lean[ 0 ], playerPosition[ 1 ] + vr.m_fpEyeHeight * kGameUnitsPerMeter,
				playerPosition[ 2 ] + lean[ 1 ] };
			s_lastSteerTick = GetTickCount();
			s_fpActive = true;
			return true;
		}

		bool WorldAimFromTracking( float trackingYaw, float trackingPitch, float &yaw, float &pitch )
		{
			if( !FirstPersonActive() )
			{
				return false;
			}
			// Same mapping as the head: game yaw = body yaw + (tracking yaw - recenter yaw).
			yaw = WrapAngle( s_bodyYaw + WrapAngle( trackingYaw - s_refYaw ) );
			pitch = trackingPitch;
			return true;
		}

		float s_hmdTracking[ 3 ]{};

		bool TrackingToWorld( const float tracking[ 3 ], float world[ 3 ] )
		{
			if( !FirstPersonActive() || !s_haveReference )
			{
				return false;
			}
			const auto &vr = Config::Get().vr;
			const float unitsPerMeter = kGameUnitsPerMeter * ( vr.m_worldScale > 0.01f ? vr.m_worldScale : 1.0f );
			// Body frame (as BuildEyeFirstPerson): yaw-free, origin at the anchor, looking down -Z.
			Vec3 local = Scale( Rotate( YawQuat( -s_refYaw ),
				{ tracking[ 0 ] - s_fpAnchor.x, tracking[ 1 ] - s_fpAnchor.y, tracking[ 2 ] - s_fpAnchor.z } ), unitsPerMeter );
			local = Add( local, Scale( s_eyeLook, vr.m_fpEyeForward * kGameUnitsPerMeter ) );
			// Game forward = ( sin, 0, cos ) of the body yaw, game right = ( -cos, 0, sin ).
			const float fx = std::sin( s_bodyYaw );
			const float fz = std::cos( s_bodyYaw );
			world[ 0 ] = s_headWorld.x - fz * local.x - fx * local.z;
			world[ 1 ] = s_headWorld.y + local.y;
			world[ 2 ] = s_headWorld.z + fx * local.x - fz * local.z;
			return true;
		}

		bool HeadWorld( float head[ 3 ] )
		{
			// The real head (not the leashed anchor), so crouching and stepping show.
			return TrackingToWorld( s_hmdTracking, head );
		}

		bool WorldToTracking( const float world[ 3 ], float tracking[ 3 ] )
		{
			if( !FirstPersonActive() || !s_haveReference )
			{
				return false;
			}
			const auto &vr = Config::Get().vr;
			const float unitsPerMeter = kGameUnitsPerMeter * ( vr.m_worldScale > 0.01f ? vr.m_worldScale : 1.0f );
			// Undo TrackingToWorld step by step.
			const float fx = std::sin( s_bodyYaw );
			const float fz = std::cos( s_bodyYaw );
			const float dx = world[ 0 ] - s_headWorld.x;
			const float dy = world[ 1 ] - s_headWorld.y;
			const float dz = world[ 2 ] - s_headWorld.z;
			Vec3 local{ -fz * dx + fx * dz, dy, -fx * dx - fz * dz };
			local = Add( local, Scale( s_eyeLook, -vr.m_fpEyeForward * kGameUnitsPerMeter ) );
			const Vec3 t = Rotate( YawQuat( s_refYaw ), Scale( local, 1.0f / unitsPerMeter ) );
			tracking[ 0 ] = t.x + s_fpAnchor.x;
			tracking[ 1 ] = t.y + s_fpAnchor.y;
			tracking[ 2 ] = t.z + s_fpAnchor.z;
			return true;
		}

		bool TrackingDirToWorld( const float tracking[ 3 ], float world[ 3 ] )
		{
			if( !FirstPersonActive() || !s_haveReference )
			{
				return false;
			}
			const Vec3 local = Rotate( YawQuat( -s_refYaw ), { tracking[ 0 ], tracking[ 1 ], tracking[ 2 ] } );
			const float fx = std::sin( s_bodyYaw );
			const float fz = std::cos( s_bodyYaw );
			world[ 0 ] = -fz * local.x - fx * local.z;
			world[ 1 ] = local.y;
			world[ 2 ] = fx * local.x - fz * local.z;
			return true;
		}

		bool BodyAxes( float forward[ 3 ], float right[ 3 ] )
		{
			if( !FirstPersonActive() )
			{
				return false;
			}
			const float fx = std::sin( s_bodyYaw );
			const float fz = std::cos( s_bodyYaw );
			forward[ 0 ] = fx;
			forward[ 1 ] = 0.0f;
			forward[ 2 ] = fz;
			right[ 0 ] = -fz;
			right[ 1 ] = 0.0f;
			right[ 2 ] = fx;
			return true;
		}

		bool TrackingAimFromWorld( float yaw, float pitch, float &trackingYaw, float &trackingPitch )
		{
			if( !FirstPersonActive() )
			{
				return false;
			}
			trackingYaw = WrapAngle( s_refYaw + WrapAngle( yaw - s_bodyYaw ) );
			trackingPitch = pitch;
			return true;
		}

		bool ThirdPersonMode()
		{
			return Config::Get().vr.m_cameraMode != "firstPerson" && StereoRenderer::SceneAllowsStereo();
		}

		bool SteerThirdPerson( float cameraPosition[ 3 ], float focusPosition[ 3 ],
			const float playerPosition[ 3 ], int cameraState )
		{
			if( !ThirdPersonMode() )
			{
				s_tpPrimed = false;
				return false;
			}
			auto &vr = Config::Get().vr;

			LARGE_INTEGER now{};
			LARGE_INTEGER frequency{};
			QueryPerformanceCounter( &now );
			QueryPerformanceFrequency( &frequency );
			float dt = 0.0f;
			if( s_tpLastTime.QuadPart != 0 && frequency.QuadPart > 0 )
			{
				dt = static_cast< float >( now.QuadPart - s_tpLastTime.QuadPart ) /
					static_cast< float >( frequency.QuadPart );
			}
			s_tpLastTime = now;
			dt = dt < 0.0f ? 0.0f : ( dt > 0.1f ? 0.1f : dt );

			if( !s_tpPrimed )
			{
				// Start behind wherever the game camera was looking (keep the last yaw if
				// the game camera has no usable direction this tick).
				const float dx = focusPosition[ 0 ] - cameraPosition[ 0 ];
				const float dz = focusPosition[ 2 ] - cameraPosition[ 2 ];
				if( ( dx * dx + dz * dz ) > 1.0e-6f )
				{
					s_tpYaw = std::atan2( dx, dz );
				}
				s_tpPrimed = true;
				s_tpOrbitArmed = true;
				s_tpHeightArmed = true;
				PSU_LOG( "VRCamera TP: orbit camera yaw=%.1f deg distance=%.1f m\n",
					s_tpYaw * ( 180.0f / kPi ), vr.m_tpDistance );
			}

			// LB refocus / lock-on: adopt the yaw the game just produced, so its own swing
			// behind the character or its target framing plays out from our camera.
			if( vr.m_tpLbCamera == "follow" && LbWindowOpen( cameraState ) )
			{
				const float dx = focusPosition[ 0 ] - cameraPosition[ 0 ];
				const float dz = focusPosition[ 2 ] - cameraPosition[ 2 ];
				if( ( dx * dx + dz * dz ) > 1.0e-6f )
				{
					s_tpYaw = std::atan2( dx, dz );
				}
			}

			// Only in normal play (1 follow / 3 refocus); NPC talk etc. stays still.
			const bool playState = cameraState == 1 || cameraState == 3;
			float stickX = 0.0f, stickY = 0.0f;
			unsigned short buttons = 0;
			VRGamepad::Buttons( buttons );
			const bool havePad = TurnStick( stickX, stickY );
			if( playState && havePad )
			{
				constexpr unsigned short kBack = 0x0020;  // XINPUT_GAMEPAD_BACK
				if( std::fabs( stickX ) < kSnapRelease ) s_tpOrbitArmed = true;
				if( std::fabs( stickY ) < kSnapRelease ) s_tpHeightArmed = true;
				if( buttons & kBack )
				{
					// Back + left/right: tilt (stick right looks further down).
					vr.m_tpPitch -= stickX * vr.m_tpTiltSpeed * dt;
					vr.m_tpPitch = vr.m_tpPitch < -60.0f ? -60.0f : ( vr.m_tpPitch > 30.0f ? 30.0f : vr.m_tpPitch );
					// Back + up/down: height in 0.5 m steps.
					if( s_tpHeightArmed && std::fabs( stickY ) > kSnapPress )
					{
						s_tpHeightArmed = false;
						vr.m_tpHeight += stickY > 0.0f ? 0.5f : -0.5f;
						vr.m_tpHeight = vr.m_tpHeight < -1.0f ? -1.0f : ( vr.m_tpHeight > 4.0f ? 4.0f : vr.m_tpHeight );
					}
				}
				else
				{
					// Left/right turns: snap steps or smooth (own setting, separate from first person).
					// Stick right turns the view right, i.e. lowers the game yaw.
					if( vr.m_tpSnapTurn )
					{
						if( s_tpOrbitArmed && std::fabs( stickX ) > kSnapPress )
						{
							s_tpOrbitArmed = false;
							const float step = vr.m_tpOrbitDegrees * ( kPi / 180.0f );
							s_tpYaw = WrapAngle( s_tpYaw + ( stickX > 0.0f ? -step : step ) );
						}
					}
					else
					{
						s_tpYaw = WrapAngle( s_tpYaw - stickX * vr.m_tpTurnSpeed * ( kPi / 180.0f ) * dt );
					}
					// Up/down zooms (up = closer).
					vr.m_tpDistance -= stickY * vr.m_tpZoomSpeed * dt;
				}
			}
			const float minDistance = vr.m_tpMinDistance > 0.3f ? vr.m_tpMinDistance : 0.3f;
			const float maxDistance = vr.m_tpMaxDistance > minDistance ? vr.m_tpMaxDistance : minDistance;
			vr.m_tpDistance = vr.m_tpDistance < minDistance ? minDistance :
				( vr.m_tpDistance > maxDistance ? maxDistance : vr.m_tpDistance );

			// Camera behind and above the head, looking ahead tilted by the user's tpPitch
			// (0 = level horizon, the character sits below the centre of view).
			const float fx = std::sin( s_tpYaw );
			const float fz = std::cos( s_tpYaw );
			const float pitch = vr.m_tpPitch * ( kPi / 180.0f );
			const float head[ 3 ] = {
				playerPosition[ 0 ],
				playerPosition[ 1 ] + vr.m_fpEyeHeight * kGameUnitsPerMeter,
				playerPosition[ 2 ] };
			const float back = vr.m_tpDistance * kGameUnitsPerMeter;
			const float up = vr.m_tpHeight * kGameUnitsPerMeter;
			cameraPosition[ 0 ] = head[ 0 ] - fx * back;
			cameraPosition[ 1 ] = head[ 1 ] + up;
			cameraPosition[ 2 ] = head[ 2 ] - fz * back;
			const float reach = back > 1.0f ? back : 1.0f;
			focusPosition[ 0 ] = cameraPosition[ 0 ] + fx * std::cos( pitch ) * reach;
			focusPosition[ 1 ] = cameraPosition[ 1 ] + std::sin( pitch ) * reach;
			focusPosition[ 2 ] = cameraPosition[ 2 ] + fz * std::cos( pitch ) * reach;
			s_tpActive = true;
			return true;
		}

		void ClearThirdPerson( bool leftThirdPerson )
		{
			// A one-tick miss (player data briefly unavailable) keeps the orbit; only
			// leaving third person (mode switch, pause, scene change) starts it over.
			if( leftThirdPerson )
			{
				s_tpPrimed = false;
			}
			s_tpActive = false;
		}

		bool FirstPersonActive()
		{
			return s_fpActive && FirstPersonMode() && GetTickCount() - s_lastSteerTick < kSteerStaleMs;
		}

		void ClearFirstPerson()
		{
			s_fpActive = false;
		}

		bool TurnFromStick()
		{
			return s_turnFromStick;
		}

		float FirstPersonCameraDistance()
		{
			return s_camDistance;
		}

		float BodyYaw()
		{
			return s_bodyYaw;
		}

		namespace
		{
			int s_cameraState = 0;
			bool s_recenterRequest = false;
			DWORD s_dressingRoomTick = 0;
		}

		void NoteDressingRoom()
		{
			s_dressingRoomTick = GetTickCount();
		}

		bool DressingRoom()
		{
			return s_cameraState == 5 && s_dressingRoomTick != 0 && GetTickCount() - s_dressingRoomTick < 500;
		}

		bool ConsumeRecenterRequest()
		{
			const bool request = s_recenterRequest;
			s_recenterRequest = false;
			return request;
		}

		void NoteCameraState( int state )
		{
			static unsigned s_logs = 0;
			if( state != s_cameraState && s_logs < 200 )
			{
				++s_logs;
				PSU_LOG( "VRCamera: camera state %d -> %d\n", s_cameraState, state );
			}
			// Entering the game's framed shots (dressing room, NPC talk): recenter so the
			// shot is in front of the player.
			if( state == 5 && s_cameraState != 5 )
			{
				s_recenterRequest = true;
			}
			s_cameraState = state;
		}

		int CameraState()
		{
			return s_cameraState;
		}

		bool RenderCameraYawPitch( float &yaw, float &pitch )
		{
			return RenderCameraAngles( yaw, pitch );
		}

		bool LookYaw( float &yaw )
		{
			if( FirstPersonActive() )
			{
				yaw = WrapAngle( s_bodyYaw + s_steerHmdYaw );
				return true;
			}
			float pitch = 0.0f;
			return RenderCameraAngles( yaw, pitch );
		}

		namespace
		{
			// Room walk: the head's offset from the anchor (body frame, meters) and whether the
			// stick is walking the character toward it.
			constexpr float kRoomWalkStop = 0.08f;     // meters
			constexpr float kRoomMaxOffset = 1.0f;     // the character is stuck: the view stops drifting
			constexpr float kRoomTeleportUnits = 20.0f;
			bool s_roomWanted = false;
			bool s_roomDriving = false;
			float s_roomStick[ 2 ]{};
			Vec3 s_prevHeadWorld{};
			bool s_havePrevHeadWorld = false;
			DWORD s_roomLogTick = 0;

			// Needs the motion-controller pad (motion input, a controller tracked): it is what
			// walks the character. Without it the normal lean leash applies.
			bool RoomWalkOn()
			{
				return Config::Get().vr.m_roomWalk && FirstPersonActive() && VRSyntheticPad::Active();
			}

			// The character moved this frame while the stick walked it: move the anchor along
			// the head's offset by as much (never past the head), so the view stays where the
			// head is and the offset shrinks as the character catches up.
			void FollowCharacter( const Vec3 &head )
			{
				const Vec3 moved{ s_headWorld.x - s_prevHeadWorld.x, 0.0f, s_headWorld.z - s_prevHeadWorld.z };
				const bool valid = s_havePrevHeadWorld;
				s_prevHeadWorld = s_headWorld;
				s_havePrevHeadWorld = true;
				if( !valid || !s_roomDriving )
				{
					return;
				}
				const float movedLength = std::sqrt( moved.x * moved.x + moved.z * moved.z );
				if( movedLength < 1.0e-5f || movedLength > kRoomTeleportUnits )
				{
					return;
				}
				const auto &vr = Config::Get().vr;
				const float unitsPerMeter = kGameUnitsPerMeter * ( vr.m_worldScale > 0.01f ? vr.m_worldScale : 1.0f );
				// World -> body frame (x right, z back): game forward ( sin, 0, cos ), right ( -cos, 0, sin ).
				const float fx = std::sin( s_bodyYaw ), fz = std::cos( s_bodyYaw );
				const float forward = ( moved.x * fx + moved.z * fz ) / unitsPerMeter;
				const float right = ( -moved.x * fz + moved.z * fx ) / unitsPerMeter;
				const Vec3 offset = Rotate( YawQuat( -s_refYaw ), { head.x - s_fpAnchor.x, 0.0f, head.z - s_fpAnchor.z } );
				const float offsetLength = std::sqrt( offset.x * offset.x + offset.z * offset.z );
				if( offsetLength < 1.0e-4f )
				{
					return;
				}
				const Vec3 dir{ offset.x / offsetLength, 0.0f, offset.z / offsetLength };
				float t = right * dir.x + ( -forward ) * dir.z;
				t = t < 0.0f ? 0.0f : ( t > offsetLength ? offsetLength : t );
				const Vec3 step = Rotate( YawQuat( s_refYaw ), Scale( dir, t ) );
				s_fpAnchor.x += step.x;
				s_fpAnchor.z += step.z;
			}

			// Stick toward the head's offset, relative to the camera (the game moves the left
			// stick relative to where the camera looks).
			void UpdateRoomStick( const Vec3 &head )
			{
				const auto &vr = Config::Get().vr;
				const Vec3 offset = Rotate( YawQuat( -s_refYaw ), { head.x - s_fpAnchor.x, 0.0f, head.z - s_fpAnchor.z } );
				const float distance = std::sqrt( offset.x * offset.x + offset.z * offset.z );
				const float start = vr.m_roomWalkStart > kRoomWalkStop ? vr.m_roomWalkStart : kRoomWalkStop + 0.02f;
				const bool wanted = s_roomWanted ? distance > kRoomWalkStop : distance > start;
				if( wanted != s_roomWanted )
				{
					s_roomWanted = wanted;
					PSU_LOG( "VRRoomWalk: %s (offset %.2f m)\n", wanted ? "walking" : "stopped", distance );
				}
				if( !wanted )
				{
					s_roomStick[ 0 ] = s_roomStick[ 1 ] = 0.0f;
					return;
				}
				// Body frame -> world direction.
				const float bx = std::sin( s_bodyYaw ), bz = std::cos( s_bodyYaw );
				const float wx = -bz * offset.x - bx * offset.z;
				const float wz = bx * offset.x - bz * offset.z;
				// World -> camera stick (forward = camera's ground forward, x = its right).
				const float cx = std::sin( s_lastWrittenYaw ), cz = std::cos( s_lastWrittenYaw );
				float sy = wx * cx + wz * cz;
				float sx = -wx * cz + wz * cx;
				const float length = std::sqrt( sx * sx + sy * sy );
				if( length < 1.0e-5f )
				{
					s_roomStick[ 0 ] = s_roomStick[ 1 ] = 0.0f;
					return;
				}
				// Walk slowly for small steps, run for big ones.
				float magnitude = 0.35f + ( distance - kRoomWalkStop ) * 1.3f;
				magnitude = magnitude > 1.0f ? 1.0f : magnitude;
				s_roomStick[ 0 ] = sx / length * magnitude;
				s_roomStick[ 1 ] = sy / length * magnitude;
				if( GetTickCount() - s_roomLogTick >= 2000 )
				{
					s_roomLogTick = GetTickCount();
#ifdef _DEBUG
					PSU_LOG( "VRRoomWalk: offset %.2f m stick (%.2f, %.2f)%s\n", distance, s_roomStick[ 0 ], s_roomStick[ 1 ],
						s_roomDriving ? "" : " (not driving)" );
#endif
				}
			}
		}

		bool RoomWalkStick( bool userStickIdle, float &x, float &y )
		{
			const int state = CameraState();
			const bool playState = state == 1 || state == 3;
			s_roomDriving = RoomWalkOn() && playState && userStickIdle && s_roomWanted;
			if( !s_roomDriving )
			{
				return false;
			}
			x = s_roomStick[ 0 ];
			y = s_roomStick[ 1 ];
			return true;
		}

		void UpdateFirstPersonAnchor( const EyePose views[ 2 ] )
		{
			const auto &vr = Config::Get().vr;
			const Vec3 head{
				( views[ 0 ].position[ 0 ] + views[ 1 ].position[ 0 ] ) * 0.5f,
				( views[ 0 ].position[ 1 ] + views[ 1 ].position[ 1 ] ) * 0.5f,
				( views[ 0 ].position[ 2 ] + views[ 1 ].position[ 2 ] ) * 0.5f };
			s_hmdTracking[ 0 ] = head.x;
			s_hmdTracking[ 1 ] = head.y;
			s_hmdTracking[ 2 ] = head.z;
			const bool roomWalk = RoomWalkOn();
			if( roomWalk )
			{
				FollowCharacter( head );
				UpdateRoomStick( head );
			}
			else
			{
				s_roomWanted = false;
				s_roomDriving = false;
				s_havePrevHeadWorld = false;
			}
			// Horizontal leash: lean up to fpLeanLimit, then the anchor follows the head. With
			// room walk the character walks to the head instead; the leash only stops the view
			// drifting off when it can't (a wall).
			float lean = vr.m_fpLeanLimit < 0.0f ? 0.0f : vr.m_fpLeanLimit;
			if( roomWalk && lean < kRoomMaxOffset )
			{
				lean = kRoomMaxOffset;
			}
			const float dx = head.x - s_fpAnchor.x;
			const float dz = head.z - s_fpAnchor.z;
			const float horizontal = std::sqrt( dx * dx + dz * dz );
			if( horizontal > lean && horizontal > 1.0e-6f )
			{
				const float pull = ( horizontal - lean ) / horizontal;
				s_fpAnchor.x += dx * pull;
				s_fpAnchor.z += dz * pull;
			}
			// Vertical leash (ducking, standing up).
			const float crouch = vr.m_fpCrouchLimit < 0.0f ? 0.0f : vr.m_fpCrouchLimit;
			const float dy = head.y - s_fpAnchor.y;
			if( dy > crouch )
			{
				s_fpAnchor.y += dy - crouch;
			}
			else if( dy < -crouch )
			{
				s_fpAnchor.y += dy + crouch;
			}
		}

		void BuildEyeFirstPerson( const EyePose &eye, float unitsPerMeter, const D3DMATRIX &gameProj,
			D3DMATRIX &view, D3DMATRIX &proj )
		{
			// Body frame: VR yaw s_refYaw = game yaw s_bodyYaw, level, origin at the head.
			const Quat bodyInverse = YawQuat( -s_refYaw );
			const Quat bodyEye = Multiply( bodyInverse, ToQuat( eye.orientation ) );
			const Vec3 eyePos = ToVec( eye.position );
			Vec3 offset = Scale( Rotate( bodyInverse,
				{ eyePos.x - s_fpAnchor.x, eyePos.y - s_fpAnchor.y, eyePos.z - s_fpAnchor.z } ), unitsPerMeter );
			// The face offset follows where the head looks, so the eyes stay in front of the face.
			Vec3 look = Rotate( bodyEye, { 0.0f, 0.0f, -1.0f } );
			const float flat = std::sqrt( look.x * look.x + look.z * look.z );
			look = flat > 1.0e-4f ? Vec3{ look.x / flat, 0.0f, look.z / flat } : Vec3{ 0.0f, 0.0f, -1.0f };
			offset = Add( offset, Scale( look, Config::Get().vr.m_fpEyeForward * kGameUnitsPerMeter ) );
			s_eyeLook = look;

			// Camera the game actually renders from (our steer, or the NPC talk camera).
			// Place the eye at the head in that camera's space, so any game re-aim or
			// smoothing is cancelled: the world only moves by stick input and the head.
			float camYaw = 0.0f, camPitch = 0.0f;
			Vec3 headCam{ 0.0f, 0.0f, -s_camDistance };
			float yawDelta = s_steerHmdYaw;
			if( RenderCameraAngles( camYaw, camPitch ) )
			{
				const float dhx = s_headWorld.x - s_renderPos.x;
				const float dhy = s_headWorld.y - s_renderPos.y;
				const float dhz = s_headWorld.z - s_renderPos.z;
				const float sy = std::sin( camYaw ), cy = std::cos( camYaw );
				const float sp = std::sin( camPitch ), cp = std::cos( camPitch );
				const float ahead = dhx * sy + dhz * cy;   // along the camera's ground forward
				const float right = -dhx * cy + dhz * sy;  // game right = ( -cos, 0, sin )
				headCam = { right, -ahead * sp + dhy * cp, -( ahead * cp + dhy * sp ) };
				yawDelta = WrapAngle( camYaw - s_bodyYaw );
			}
			else
			{
				camPitch = 0.0f;
			}
			// Body -> camera: undo the camera's yaw relative to the body, then its pitch.
			const Quat bodyToCam = Multiply( PitchQuat( -camPitch ), YawQuat( -yawDelta ) );
			const Vec3 p = Add( headCam, Rotate( bodyToCam, offset ) );
			const Quat q = Multiply( bodyToCam, bodyEye );
			BuildViewProj( eye, gameProj, p, q, view, proj );
		}

		void HudPanelPose( const EyePose views[ 2 ], float distanceMeters, float position[ 3 ], float orientation[ 4 ] )
		{
			const auto &vr = Config::Get().vr;
			LARGE_INTEGER now{};
			LARGE_INTEGER frequency{};
			QueryPerformanceCounter( &now );
			QueryPerformanceFrequency( &frequency );
			float dt = 0.0f;
			if( s_hudLastTime.QuadPart != 0 && frequency.QuadPart > 0 )
			{
				dt = static_cast< float >( now.QuadPart - s_hudLastTime.QuadPart ) /
					static_cast< float >( frequency.QuadPart );
			}
			s_hudLastTime = now;
			dt = dt < 0.0f ? 0.0f : ( dt > 0.1f ? 0.1f : dt );

			// Lazy follow: stay put while the head looks around, glide back once it has
			// looked away further than hudFollowAngle, stop when nearly centred.
			const float headYaw = PoseYaw( views[ 0 ].orientation );
			const float delta = WrapAngle( headYaw - s_hudYaw );
			const float followAngle = vr.m_hudFollowAngle * ( kPi / 180.0f );
			constexpr float kSettle = 5.0f * ( kPi / 180.0f );
			constexpr float kFollowRate = 4.0f;  // 1/s, exponential ease
			if( std::fabs( delta ) > followAngle )
			{
				s_hudFollowing = true;
			}
			if( s_hudFollowing )
			{
				const float step = 1.0f - std::exp( -kFollowRate * dt );
				s_hudYaw = WrapAngle( s_hudYaw + delta * step );
				if( std::fabs( WrapAngle( headYaw - s_hudYaw ) ) < kSettle )
				{
					s_hudFollowing = false;
				}
			}

			// First person anchors at the body leash point so leaning doesn't move it.
			const Vec3 anchor = FirstPersonActive() ? s_fpAnchor : s_refPosition;
			const float drop = vr.m_hudDrop;
			const Quat yaw = YawQuat( s_hudYaw );
			const Vec3 ahead = Rotate( yaw, { 0.0f, -drop, -distanceMeters } );
			position[ 0 ] = anchor.x + ahead.x;
			position[ 1 ] = anchor.y + ahead.y;
			position[ 2 ] = anchor.z + ahead.z;
			// Tilt the top back slightly so the panel faces the eyes from below eye level
			// (negative rotation about +X turns the quad's +Z normal upward).
			const float tilt = std::atan2( drop, distanceMeters );
			const Quat q = Multiply( yaw, PitchQuat( -tilt ) );
			orientation[ 0 ] = q.x;
			orientation[ 1 ] = q.y;
			orientation[ 2 ] = q.z;
			orientation[ 3 ] = q.w;
		}

		void PanelPose( float distanceMeters, float dropMeters, float position[ 3 ], float orientation[ 4 ] )
		{
			const Quat yaw = YawQuat( s_refYaw );
			const Vec3 ahead = Rotate( yaw, { 0.0f, 0.0f, -distanceMeters } );
			position[ 0 ] = s_refPosition.x + ahead.x;
			position[ 1 ] = s_refPosition.y + ahead.y - dropMeters;
			position[ 2 ] = s_refPosition.z + ahead.z;
			orientation[ 0 ] = yaw.x;
			orientation[ 1 ] = yaw.y;
			orientation[ 2 ] = yaw.z;
			orientation[ 3 ] = yaw.w;
		}
	}
}
