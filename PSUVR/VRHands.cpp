#include "VRHands.h"

#include "VRCamera.h"
#include "VRBindings.h"
#include "VRControllers.h"
#include "VRSyntheticPad.h"
#include "../PSUStructs/PSUCombat.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"

#include <Windows.h>
#include <d3dx9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace PSUVR
{
	namespace
	{
		// PSU human skeleton (28 bones; bone records 0x90 bytes, parent at +6). Controller
		// hand 0 drives the character's left arm.
		constexpr int kBoneCount = 28;
		constexpr size_t kRecordSize = 0x90;
		constexpr size_t kRecordParent = 0x06;
		struct Arm
		{
			int shoulder, upper, fore, hand;
		};
		constexpr Arm kArms[ 2 ] = { { 14, 15, 16, 17 }, { 19, 20, 21, 22 } };
		// Waist (above the hips): its subtree is the upper body. Neck: its subtree is the head.
		constexpr int kWaistBone = 9;
		constexpr int kNeckBone = 12;
		// The model faces +Z (left side is +X, up +Y).
		const D3DXVECTOR3 kModelForward( 0.0f, 0.0f, 1.0f );
		DWORD s_headHiddenTick = 0;

		// Free aim gun pointing: the weapon bone is the bone after the hand (18 / 23). Free aim
		// bullets start at a fixed point, not at the muzzle, so the barrel can't be learned from
		// them; the game's own free aim pose holds the gun along the shot instead.

		// Staff / rod held still while casting: each weapon bone relative to its hand bone, taken
		// while not attacking, and put back over the casting animation.
		D3DXMATRIX s_weaponRest[ 2 ];
		bool s_haveWeaponRest[ 2 ]{};
		uint32_t s_weaponRestClass = 0;
		DWORD s_castTick = 0;
		// After a cast, the animation returns the weapon: keep holding it this long.
		constexpr DWORD kCastSettleMs = 400;
		const std::string kHeadlessBody = "headless";

		// How much the arms follow the controllers (0 = the game's animation), eased.
		float s_weight = 0.0f;
		DWORD s_weightTick = 0;
		constexpr float kBlendSeconds = 0.2f;

		// Hand angle calibration: countdown after the panel's button.
		int s_seenCalibRequest = 0;
		DWORD s_calibStart = 0;
		bool s_calibPending = false;
		int s_calibBuzz = 0;
		constexpr DWORD kCalibDelayMs = 5000;
		// The request whose capture ran and how many hands it stored.
		int s_calibCapturedRequest = 0;
		int s_calibCapturedHands = 0;

		// Automatic hand angle (no calibration), per hand, for the skeleton it was made from.
		D3DXMATRIX s_autoK[ 2 ];
		const uint8_t *s_autoRecords[ 2 ] = { nullptr, nullptr };

		// The wrist sits this far behind the controller's grip point (grip +Z points back).
		constexpr float kWristBackMeters = 0.06f;

		// Model vtable +0x18: bone matrix in game world, multiplied into the matrix passed in
		// (comes back transposed: translation at [3], [7], [11]).
		typedef char( __thiscall *GetBoneMatrixFn )( void *model, float *matrix, int bone, int, int, int );

		DWORD s_lastLog = 0;
		bool s_loggedWorld = false;

		// Model -> world, measured after the player's last draw. The game's bone query
		// re-runs the pose build (and so Apply), so it never runs inside Apply.
		D3DXMATRIX s_world;
		DWORD s_worldTick = 0;
		bool s_haveWorld = false;
		bool s_inWorldQuery = false;
		constexpr DWORD kWorldStaleMs = 500;

		// Why Apply returned early (logged every 3 s while diagnosing).
		enum Skip
		{
			kSkipSetting, kSkipFirstPerson, kSkipControllers, kSkipSkeleton, kSkipLayout, kSkipWorld,
			kSkipInverse, kSkipBody, kSkipTracking, kSolved, kSkipCount
		};
		const char *const kSkipNames[ kSkipCount ] = { "setting", "firstPerson", "controllers", "skeleton", "layout",
			"world", "inverse", "body", "tracking", "solved" };
		unsigned s_skips[ kSkipCount ]{};
		DWORD s_lastSkipLog = 0;

		void Note( Skip why )
		{
			++s_skips[ why ];
			if( GetTickCount() - s_lastSkipLog >= 3000 )
			{
				s_lastSkipLog = GetTickCount();
				char line[ 256 ];
				int len = 0;
				for( int i = 0; i < kSkipCount && len < 200; ++i )
				{
					len += sprintf_s( line + len, sizeof( line ) - len, " %s=%u", kSkipNames[ i ], s_skips[ i ] );
				}
#ifdef _DEBUG
				PSU_LOG( "VRHands: player poses:%s\n", line );
#endif
			}
		}

		bool Readable( const void *p, size_t bytes )
		{
			return p && PSUMemory::IsValidPointer( p ) &&
				PSUMemory::IsValidPointer( static_cast< const uint8_t * >( p ) + bytes - 1 );
		}

		D3DXVECTOR3 Position( const float *out, int bone )
		{
			const float *m = out + bone * 16;
			return { m[ 12 ], m[ 13 ], m[ 14 ] };
		}

		int Parent( const uint8_t *records, int bone )
		{
			return *reinterpret_cast< const short * >( records + bone * kRecordSize + kRecordParent );
		}

		bool Below( const uint8_t *records, int bone, int root )
		{
			for( int guard = 0; bone >= 0 && guard < kBoneCount; ++guard )
			{
				if( bone == root )
				{
					return true;
				}
				bone = Parent( records, bone );
			}
			return false;
		}

		D3DXVECTOR3 RotateByQuat( const float q[ 4 ], const D3DXVECTOR3 &v )
		{
			// v + 2w(u x v) + 2(u x (u x v)), u = q.xyz
			const D3DXVECTOR3 u( q[ 0 ], q[ 1 ], q[ 2 ] );
			D3DXVECTOR3 t;
			D3DXVec3Cross( &t, &u, &v );
			t *= 2.0f;
			D3DXVECTOR3 c;
			D3DXVec3Cross( &c, &u, &t );
			return v + t * q[ 3 ] + c;
		}

		// Row-vector matrix rotating unit a onto unit b about pivot (v' = (v - p) R + p).
		D3DXMATRIX RotationAbout( const D3DXVECTOR3 &a, const D3DXVECTOR3 &b, const D3DXVECTOR3 &pivot )
		{
			D3DXVECTOR3 k;
			D3DXVec3Cross( &k, &a, &b );
			const float c = D3DXVec3Dot( &a, &b );
			// Column form R = I + K + K^2 / (1 + c), K = skew(a x b); valid away from c = -1.
			float r[ 3 ][ 3 ] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			if( c > -0.999f )
			{
				const float K[ 3 ][ 3 ] = { { 0, -k.z, k.y }, { k.z, 0, -k.x }, { -k.y, k.x, 0 } };
				const float f = 1.0f / ( 1.0f + c );
				for( int i = 0; i < 3; ++i )
				{
					for( int j = 0; j < 3; ++j )
					{
						float k2 = 0.0f;
						for( int n = 0; n < 3; ++n )
						{
							k2 += K[ i ][ n ] * K[ n ][ j ];
						}
						r[ i ][ j ] += K[ i ][ j ] + k2 * f;
					}
				}
			}
			// Row-vector form is the transpose.
			D3DXMATRIX m;
			D3DXMatrixIdentity( &m );
			for( int i = 0; i < 3; ++i )
			{
				for( int j = 0; j < 3; ++j )
				{
					m( i, j ) = r[ j ][ i ];
				}
			}
			D3DXVECTOR3 moved;
			D3DXVec3TransformNormal( &moved, &pivot, &m );
			m( 3, 0 ) = pivot.x - moved.x;
			m( 3, 1 ) = pivot.y - moved.y;
			m( 3, 2 ) = pivot.z - moved.z;
			return m;
		}

		// 3x3 with unit rows (axes), no translation.
		D3DXMATRIX Axes( const D3DXMATRIX &m )
		{
			D3DXMATRIX r;
			D3DXMatrixIdentity( &r );
			for( int i = 0; i < 3; ++i )
			{
				D3DXVECTOR3 row( m( i, 0 ), m( i, 1 ), m( i, 2 ) );
				const float len = D3DXVec3Length( &row );
				row = len > 1.0e-6f ? row / len : row;
				r( i, 0 ) = row.x;
				r( i, 1 ) = row.y;
				r( i, 2 ) = row.z;
			}
			return r;
		}

		// Controller grip axes in game world (rows X, Y, Z).
		bool ControllerAxes( const ControllerState &c, D3DXMATRIX &axes )
		{
			D3DXMatrixIdentity( &axes );
			const D3DXVECTOR3 local[ 3 ] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
			for( int i = 0; i < 3; ++i )
			{
				const D3DXVECTOR3 t = RotateByQuat( c.gripOrientation, local[ i ] );
				const float in[ 3 ] = { t.x, t.y, t.z };
				float w[ 3 ];
				if( !VRCamera::TrackingDirToWorld( in, w ) )
				{
					return false;
				}
				axes( i, 0 ) = w[ 0 ];
				axes( i, 1 ) = w[ 1 ];
				axes( i, 2 ) = w[ 2 ];
			}
			return true;
		}

		D3DXQUATERNION CalibQuat( int hand )
		{
			const auto &vr = Config::Get().vr;
			D3DXQUATERNION q = hand == 0 ? D3DXQUATERNION( vr.m_handRotL0, vr.m_handRotL1, vr.m_handRotL2, vr.m_handRotL3 )
				: D3DXQUATERNION( vr.m_handRotR0, vr.m_handRotR1, vr.m_handRotR2, vr.m_handRotR3 );
			D3DXQuaternionNormalize( &q, &q );
			return q;
		}

		void StoreCalib( int hand, const D3DXQUATERNION &q )
		{
			auto &vr = Config::Get().vr;
			float *dst[ 4 ] = { &vr.m_handRotL0, &vr.m_handRotL1, &vr.m_handRotL2, &vr.m_handRotL3 };
			if( hand == 1 )
			{
				dst[ 0 ] = &vr.m_handRotR0;
				dst[ 1 ] = &vr.m_handRotR1;
				dst[ 2 ] = &vr.m_handRotR2;
				dst[ 3 ] = &vr.m_handRotR3;
			}
			*dst[ 0 ] = q.x;
			*dst[ 1 ] = q.y;
			*dst[ 2 ] = q.z;
			*dst[ 3 ] = q.w;
		}

		// Scale a bone to nothing about a point (its vertices collapse there).
		void FoldBone( float *out, int bone, const D3DXVECTOR3 &point )
		{
			constexpr float kTiny = 0.001f;
			D3DXMATRIX fold;
			D3DXMatrixScaling( &fold, kTiny, kTiny, kTiny );
			fold._41 = point.x * ( 1.0f - kTiny );
			fold._42 = point.y * ( 1.0f - kTiny );
			fold._43 = point.z * ( 1.0f - kTiny );
			D3DXMATRIX *m = reinterpret_cast< D3DXMATRIX * >( out + bone * 16 );
			D3DXMatrixMultiply( m, m, &fold );
		}

		// "headless": the neck and head fold into the top of the chest. "hands": everything but
		// the forearms and hands folds away (upper arms into the elbows, the rest into one
		// point, so blended vertices stay on a line and draw nothing). Last, after the IK.
		void FoldBody( float *out, const uint8_t *records, const std::string &mode )
		{
			if( mode == "headless" )
			{
				const D3DXVECTOR3 neck = Position( out, kNeckBone );
				for( int b = 0; b < kBoneCount; ++b )
				{
					if( Below( records, b, kNeckBone ) )
					{
						FoldBone( out, b, neck );
					}
				}
				s_headHiddenTick = GetTickCount();
			}
			else if( mode == "hands" )
			{
				// Hands only (no tracking for the forearm): the arms fold into the wrists.
				const D3DXVECTOR3 wrists[ 2 ] = { Position( out, kArms[ 0 ].hand ), Position( out, kArms[ 1 ].hand ) };
				const D3DXVECTOR3 body = Position( out, kWaistBone );
				for( int b = 0; b < kBoneCount; ++b )
				{
					// Roots (no parent) stay: the model->world measurement reads the root bone.
					if( Parent( records, b ) < 0 || Below( records, b, kArms[ 0 ].hand ) || Below( records, b, kArms[ 1 ].hand ) )
					{
						continue;
					}
					const bool left = b == kArms[ 0 ].upper || b == kArms[ 0 ].fore;
					const bool right = b == kArms[ 1 ].upper || b == kArms[ 1 ].fore;
					FoldBone( out, b, left ? wrists[ 0 ] : ( right ? wrists[ 1 ] : body ) );
				}
				s_headHiddenTick = GetTickCount();
			}
		}

		D3DXVECTOR3 Normalized( const D3DXVECTOR3 &v, const D3DXVECTOR3 &fallback );

		// Calibration pose: arms forward, fists, palms facing each other. The hand's axes in that
		// pose (game world rows) from its bind pose: knuckles (wrist -> hand tip bone) point
		// forward, the palm faces the other hand. False if the bind data is unusable.
		bool CalibrationHandAxes( const uint8_t *records, const Arm &arm, int hand, const D3DXVECTOR3 &forward,
			const D3DXVECTOR3 &right, D3DXMATRIX &axes, bool log = true )
		{
			const auto inverseBind = [ records ]( int bone ) {
				return *reinterpret_cast< const D3DXMATRIX * >( records + bone * kRecordSize + 0x30 );
			};
			const D3DXMATRIX handInverse = inverseBind( arm.hand );
			const D3DXMATRIX tipInverse = inverseBind( arm.hand + 1 );
			const D3DXMATRIX foreInverse = inverseBind( arm.fore );
			D3DXMATRIX handBind, tipBind, foreBind;
			if( !D3DXMatrixInverse( &handBind, nullptr, &handInverse ) || !D3DXMatrixInverse( &tipBind, nullptr, &tipInverse ) ||
				!D3DXMatrixInverse( &foreBind, nullptr, &foreInverse ) )
			{
				return false;
			}
			const D3DXVECTOR3 handPos( handBind._41, handBind._42, handBind._43 );
			const D3DXVECTOR3 tipPos( tipBind._41, tipBind._42, tipBind._43 );
			const D3DXVECTOR3 forePos( foreBind._41, foreBind._42, foreBind._43 );
			// Bind pose palm: arms hanging down -> palms face the body (left side is +X);
			// arms out -> palms face down.
			D3DXVECTOR3 arm3 = handPos - forePos;
			arm3 = Normalized( arm3, { 0.0f, -1.0f, 0.0f } );
			const bool hanging = std::fabs( arm3.y ) > 0.7f;
			const D3DXVECTOR3 palmModel = hanging ? D3DXVECTOR3( hand == 0 ? -1.0f : 1.0f, 0.0f, 0.0f ) : D3DXVECTOR3( 0.0f, -1.0f, 0.0f );
			// Into the hand's own axes.
			D3DXVECTOR3 knuckleLocal, palmLocal;
			D3DXVec3TransformCoord( &knuckleLocal, &tipPos, &handInverse );
			D3DXVec3TransformNormal( &palmLocal, &palmModel, &handInverse );
			knuckleLocal = Normalized( knuckleLocal, { 0.0f, 1.0f, 0.0f } );
			palmLocal = palmLocal - knuckleLocal * D3DXVec3Dot( &palmLocal, &knuckleLocal );
			palmLocal = Normalized( palmLocal, { 1.0f, 0.0f, 0.0f } );
			D3DXVECTOR3 thirdLocal;
			D3DXVec3Cross( &thirdLocal, &knuckleLocal, &palmLocal );
			// The same three in the calibration pose (world): knuckles forward, palm inward.
			const D3DXVECTOR3 palmWorld = hand == 0 ? right : -right;
			D3DXVECTOR3 thirdWorld;
			D3DXVec3Cross( &thirdWorld, &forward, &palmWorld );
			// local basis L (rows) -> world basis B (rows): R = L^T x B.
			D3DXMATRIX l, b, lT;
			D3DXMatrixIdentity( &l );
			D3DXMatrixIdentity( &b );
			const D3DXVECTOR3 lRows[ 3 ] = { knuckleLocal, palmLocal, thirdLocal };
			const D3DXVECTOR3 bRows[ 3 ] = { forward, palmWorld, thirdWorld };
			for( int i = 0; i < 3; ++i )
			{
				l( i, 0 ) = lRows[ i ].x;
				l( i, 1 ) = lRows[ i ].y;
				l( i, 2 ) = lRows[ i ].z;
				b( i, 0 ) = bRows[ i ].x;
				b( i, 1 ) = bRows[ i ].y;
				b( i, 2 ) = bRows[ i ].z;
			}
			D3DXMatrixTranspose( &lT, &l );
			D3DXMatrixMultiply( &axes, &lT, &b );
			if( log )
			PSU_LOG( "VRHands: calibration %s hand: bind arm (%.2f %.2f %.2f) %s, knuckle local (%.2f %.2f %.2f) palm local (%.2f %.2f %.2f)\n",
				hand == 0 ? "left" : "right", arm3.x, arm3.y, arm3.z, hanging ? "hanging" : "out", knuckleLocal.x, knuckleLocal.y,
				knuckleLocal.z, palmLocal.x, palmLocal.y, palmLocal.z );
			return true;
		}

		// Without calibration: in the calibration pose an OpenXR grip has +X right (away from the
		// left palm, into the right one), +Y up and -Z forward along the pointing finger, so
		// K = H_pose x C_pose^T needs no measurement. Any forward gives the same K.
		bool AutoCalibration( const uint8_t *records, int hand, D3DXMATRIX &k )
		{
			if( s_autoRecords[ hand ] == records )
			{
				k = s_autoK[ hand ];
				return true;
			}
			const D3DXVECTOR3 forward( 0.0f, 0.0f, 1.0f );
			const D3DXVECTOR3 right( -forward.z, 0.0f, forward.x );
			D3DXMATRIX pose;
			if( !CalibrationHandAxes( records, kArms[ hand ], hand, forward, right, pose, true ) )
			{
				return false;
			}
			D3DXMATRIX controller;
			D3DXMatrixIdentity( &controller );
			const D3DXVECTOR3 rows[ 3 ] = { right, { 0.0f, 1.0f, 0.0f }, -forward };
			for( int i = 0; i < 3; ++i )
			{
				controller( i, 0 ) = rows[ i ].x;
				controller( i, 1 ) = rows[ i ].y;
				controller( i, 2 ) = rows[ i ].z;
			}
			D3DXMATRIX controllerT;
			D3DXMatrixTranspose( &controllerT, &controller );
			D3DXMatrixMultiply( &k, &pose, &controllerT );
			s_autoK[ hand ] = k;
			s_autoRecords[ hand ] = records;
			return true;
		}

		void ApplyToSubtree( float *out, const uint8_t *records, int root, const D3DXMATRIX &delta )
		{
			for( int b = 0; b < kBoneCount; ++b )
			{
				if( Below( records, b, root ) )
				{
					D3DXMATRIX *m = reinterpret_cast< D3DXMATRIX * >( out + b * 16 );
					D3DXMatrixMultiply( m, m, &delta );
				}
			}
		}

		D3DXVECTOR3 Normalized( const D3DXVECTOR3 &v, const D3DXVECTOR3 &fallback )
		{
			const float len = D3DXVec3Length( &v );
			return len > 1.0e-5f ? v / len : fallback;
		}

		// Model -> game world, from the game's own bone query: world(root) = out(root) * W.
		bool ModelToWorld( void *model, const float *out, D3DXMATRIX &world )
		{
			void *const *vt = *static_cast< void *const *const * >( model );
			const auto getBone = reinterpret_cast< GetBoneMatrixFn >( vt[ 6 ] );
			float g[ 32 ]{};
			g[ 0 ] = g[ 5 ] = g[ 10 ] = g[ 15 ] = 1.0f;
			if( !getBone( model, g, 0, 0, 0, 1 ) )
			{
				return false;
			}
			D3DXMATRIX rootWorld;
			for( int i = 0; i < 4; ++i )
			{
				for( int j = 0; j < 4; ++j )
				{
					rootWorld( i, j ) = g[ j * 4 + i ];
				}
			}
			D3DXMATRIX rootInverse;
			if( !D3DXMatrixInverse( &rootInverse, nullptr, reinterpret_cast< const D3DXMATRIX * >( out ) ) )
			{
				return false;
			}
			D3DXMatrixMultiply( &world, &rootInverse, &rootWorld );
			return std::isfinite( world._41 ) && std::isfinite( world._42 ) && std::isfinite( world._43 );
		}

		void SolveArm( float *out, const uint8_t *records, const Arm &arm, const D3DXVECTOR3 &target,
			const D3DXVECTOR3 &pole )
		{
			const D3DXVECTOR3 s = Position( out, arm.upper );
			const D3DXVECTOR3 e = Position( out, arm.fore );
			const D3DXVECTOR3 h = Position( out, arm.hand );
			const D3DXVECTOR3 se = e - s;
			const D3DXVECTOR3 eh = h - e;
			const float a = D3DXVec3Length( &se );
			const float b = D3DXVec3Length( &eh );
			if( a < 1.0e-3f || b < 1.0e-3f )
			{
				return;
			}
			D3DXVECTOR3 toTarget = target - s;
			float dist = D3DXVec3Length( &toTarget );
			const D3DXVECTOR3 dir = Normalized( toTarget, Normalized( h - s, { 0.0f, -1.0f, 0.0f } ) );
			const float minReach = std::fabs( a - b ) + 1.0e-3f;
			const float maxReach = ( a + b ) * 0.999f;
			dist = dist < minReach ? minReach : ( dist > maxReach ? maxReach : dist );

			// Elbow in the plane of the reach and the pole.
			D3DXVECTOR3 bend = pole - dir * D3DXVec3Dot( &pole, &dir );
			bend = Normalized( bend, Normalized( se - dir * D3DXVec3Dot( &se, &dir ), { 0.0f, -1.0f, 0.0f } ) );
			const float cosA = ( a * a + dist * dist - b * b ) / ( 2.0f * a * dist );
			const float clamped = cosA < -1.0f ? -1.0f : ( cosA > 1.0f ? 1.0f : cosA );
			const float sinA = std::sqrt( 1.0f - clamped * clamped );
			const D3DXVECTOR3 elbow = s + dir * ( a * clamped ) + bend * ( a * sinA );
			const D3DXVECTOR3 wrist = s + dir * dist;

			// Upper arm: shoulder -> elbow; then the forearm: elbow -> wrist.
			ApplyToSubtree( out, records, arm.upper,
				RotationAbout( Normalized( se, dir ), Normalized( elbow - s, dir ), s ) );
			const D3DXVECTOR3 e1 = Position( out, arm.fore );
			const D3DXVECTOR3 h1 = Position( out, arm.hand );
			ApplyToSubtree( out, records, arm.fore,
				RotationAbout( Normalized( h1 - e1, dir ), Normalized( wrist - e1, dir ), e1 ) );
		}
	}

	namespace
	{
		// Strafe legs (VRChat-style locomotion on top of the head / hand IK): the body yaw trails the
		// head with a dead-zone, and the legs are posed against the direction the game moves the
		// character (it faces its travel direction) relative to that body yaw.
		enum Gait { kGaitForward, kGaitLeft, kGaitRight, kGaitBack };
		float s_bodyYaw = 0.0f;
		bool s_haveBodyYaw = false;
		float s_legTurn = 0.0f;           // eased yaw of the legs away from the game's facing
		Gait s_gait = kGaitForward;
		float s_lastPos[ 3 ]{};
		DWORD s_lastPosTick = 0;
		float s_speed = 0.0f;             // game units per second
		constexpr float kPi = 3.14159265f;
		constexpr float kBodyDeadZone = 35.0f * ( kPi / 180.0f );
		constexpr float kBodyFollowRate = 6.0f;
		constexpr float kGaitHysteresis = 10.0f * ( kPi / 180.0f );
		constexpr float kMovingSpeed = 8.0f;
		constexpr float kLegEaseSeconds = 0.12f;
		constexpr float kTorsoLimit = 50.0f * ( kPi / 180.0f );

		float WrapPi( float a )
		{
			while( a > kPi ) a -= 2.0f * kPi;
			while( a < -kPi ) a += 2.0f * kPi;
			return a;
		}

		D3DXMATRIX YawAbout( float angle, const D3DXVECTOR3 &pivot )
		{
			D3DXMATRIX t0, r, t1, m;
			D3DXMatrixTranslation( &t0, -pivot.x, -pivot.y, -pivot.z );
			D3DXMatrixRotationY( &r, angle );
			D3DXMatrixTranslation( &t1, pivot.x, pivot.y, pivot.z );
			D3DXMatrixMultiply( &m, &t0, &r );
			D3DXMatrixMultiply( &m, &m, &t1 );
			return m;
		}

		// Speed from the position, sampled at least 40 ms apart (Apply runs more than once a frame).
		void SampleSpeed( const PSUCombat::State &combat )
		{
			const DWORD now = GetTickCount();
			if( s_lastPosTick == 0 || now - s_lastPosTick > 500 )
			{
				std::memcpy( s_lastPos, combat.position, sizeof( s_lastPos ) );
				s_lastPosTick = now;
				s_speed = 0.0f;
			}
			else if( now - s_lastPosTick >= 40 )
			{
				const float dx = combat.position[ 0 ] - s_lastPos[ 0 ];
				const float dz = combat.position[ 2 ] - s_lastPos[ 2 ];
				s_speed = std::sqrt( dx * dx + dz * dz ) / ( ( now - s_lastPosTick ) / 1000.0f );
				std::memcpy( s_lastPos, combat.position, sizeof( s_lastPos ) );
				s_lastPosTick = now;
			}
		}

		// Eye anchor: how far the animated neck leans from where it stands at rest (game world,
		// horizontal), eased. Walking leans the head ahead of the player position the camera uses,
		// which put the eye behind the shoulders. The eye also stays at least as far ahead of each
		// shoulder (along the body's forward) as it is at rest, since a running twist swings a
		// shoulder into view.
		constexpr int kLeanPoints = 3; // neck, left shoulder, right shoulder
		float s_leanRest[ kLeanPoints ][ 2 ]{};
		bool s_haveLeanRest = false;
		float s_lean[ 2 ]{};
		DWORD s_leanTick = 0;
		constexpr float kLeanEaseSeconds = 0.25f;
		constexpr float kLeanMaxMeters = 0.45f;

		void UpdateEyeLean( const float *out, const D3DXMATRIX &world, const PSUCombat::State &combat, const float forward[ 3 ],
			float dt, bool log )
		{
			const int bones[ kLeanPoints ] = { kNeckBone, kArms[ 0 ].shoulder, kArms[ 1 ].shoulder };
			D3DXVECTOR3 root = Position( out, 0 );
			D3DXVec3TransformCoord( &root, &root, &world );
			float now[ kLeanPoints ][ 2 ];
			for( int i = 0; i < kLeanPoints; ++i )
			{
				D3DXVECTOR3 point = Position( out, bones[ i ] );
				D3DXVec3TransformCoord( &point, &point, &world );
				now[ i ][ 0 ] = point.x - root.x;
				now[ i ][ 1 ] = point.z - root.z;
			}
			SampleSpeed( combat );
			float goal[ 2 ] = { 0.0f, 0.0f };
			if( !s_haveLeanRest )
			{
				std::memcpy( s_leanRest, now, sizeof( now ) );
				s_haveLeanRest = true;
			}
			if( s_speed <= kMovingSpeed )
			{
				const float k = dt * 3.0f > 1.0f ? 1.0f : dt * 3.0f;
				for( int i = 0; i < kLeanPoints; ++i )
				{
					s_leanRest[ i ][ 0 ] += ( now[ i ][ 0 ] - s_leanRest[ i ][ 0 ] ) * k;
					s_leanRest[ i ][ 1 ] += ( now[ i ][ 1 ] - s_leanRest[ i ][ 1 ] ) * k;
				}
			}
			else
			{
				goal[ 0 ] = now[ 0 ][ 0 ] - s_leanRest[ 0 ][ 0 ];
				goal[ 1 ] = now[ 0 ][ 1 ] - s_leanRest[ 0 ][ 1 ];
				// Ahead of the shoulders: push along the forward axis until no shoulder moved further
				// forward than the eye did.
				const float fl = std::sqrt( forward[ 0 ] * forward[ 0 ] + forward[ 2 ] * forward[ 2 ] );
				if( fl > 1.0e-4f )
				{
					const float fx = forward[ 0 ] / fl, fz = forward[ 2 ] / fl;
					float eyeAhead = goal[ 0 ] * fx + goal[ 1 ] * fz;
					float shoulderAhead = eyeAhead;
					for( int i = 1; i < kLeanPoints; ++i )
					{
						const float ahead = ( now[ i ][ 0 ] - s_leanRest[ i ][ 0 ] ) * fx + ( now[ i ][ 1 ] - s_leanRest[ i ][ 1 ] ) * fz;
						shoulderAhead = ahead > shoulderAhead ? ahead : shoulderAhead;
					}
					goal[ 0 ] += fx * ( shoulderAhead - eyeAhead );
					goal[ 1 ] += fz * ( shoulderAhead - eyeAhead );
				}
				const float limit = kLeanMaxMeters * 10.0f;
				const float len = std::sqrt( goal[ 0 ] * goal[ 0 ] + goal[ 1 ] * goal[ 1 ] );
				if( len > limit )
				{
					goal[ 0 ] *= limit / len;
					goal[ 1 ] *= limit / len;
				}
			}
			const float k = dt / kLeanEaseSeconds > 1.0f ? 1.0f : dt / kLeanEaseSeconds;
			s_lean[ 0 ] += ( goal[ 0 ] - s_lean[ 0 ] ) * k;
			s_lean[ 1 ] += ( goal[ 1 ] - s_lean[ 1 ] ) * k;
			s_leanTick = GetTickCount();
#ifdef _DEBUG
			if( log )
			{
				PSU_LOG( "VRStrafe: eye lean (%.2f %.2f) goal (%.2f %.2f) neck (%.2f %.2f) shoulders L (%.2f %.2f) R (%.2f %.2f) speed %.1f\n",
					s_lean[ 0 ], s_lean[ 1 ], goal[ 0 ], goal[ 1 ], now[ 0 ][ 0 ] - s_leanRest[ 0 ][ 0 ], now[ 0 ][ 1 ] - s_leanRest[ 0 ][ 1 ],
					now[ 1 ][ 0 ] - s_leanRest[ 1 ][ 0 ], now[ 1 ][ 1 ] - s_leanRest[ 1 ][ 1 ],
					now[ 2 ][ 0 ] - s_leanRest[ 2 ][ 0 ], now[ 2 ][ 1 ] - s_leanRest[ 2 ][ 1 ], s_speed );
			}
#endif
		}

		void StrafeUpdate( const PSUCombat::State &combat, float lookYaw, float dt )
		{
			const auto &vr = Config::Get().vr;
			const float facing = static_cast< float >( combat.facing ) * ( 2.0f * kPi / 65536.0f );

			SampleSpeed( combat );
			const bool moving = s_speed > kMovingSpeed;

			// Body yaw: stays while the head turns inside the dead-zone, then follows; follows at once
			// while walking so the body faces where you look.
			if( !s_haveBodyYaw )
			{
				s_bodyYaw = lookYaw;
				s_haveBodyYaw = true;
			}
			const float headOff = WrapPi( lookYaw - s_bodyYaw );
			if( moving || std::fabs( headOff ) > kBodyDeadZone )
			{
				const float k = dt * kBodyFollowRate > 1.0f ? 1.0f : dt * kBodyFollowRate;
				s_bodyYaw = WrapPi( s_bodyYaw + headOff * k );
			}

			float target = 0.0f; // wanted leg yaw minus the game's facing
			if( moving )
			{
				const float d = WrapPi( facing - s_bodyYaw ); // travel direction against the body
				const float ad = std::fabs( d );
				// Sector changes need to pass the edge by the hysteresis.
				Gait gait = s_gait;
				const float edgeFwd = kPi * 0.25f, edgeBack = kPi * 0.75f;
				const float h = kGaitHysteresis;
				switch( s_gait )
				{
					case kGaitForward:
						if( ad > edgeFwd + h ) gait = ad > edgeBack ? kGaitBack : ( d > 0 ? kGaitRight : kGaitLeft );
						break;
					case kGaitBack:
						if( ad < edgeBack - h ) gait = d > 0 ? kGaitRight : kGaitLeft;
						break;
					default:
						if( ad < edgeFwd - h ) gait = kGaitForward;
						else if( ad > edgeBack + h ) gait = kGaitBack;
						else gait = d > 0 ? kGaitRight : kGaitLeft;
						break;
				}
				s_gait = gait;
				// Hip twist: the legs keep part of the way toward the body's facing.
				const float ref = gait == kGaitBack ? facing + kPi : facing;
				target = WrapPi( ref + vr.m_strafeBodyBlend * WrapPi( s_bodyYaw - ref ) - facing );
			}
			else
			{
				s_gait = kGaitForward;
				target = WrapPi( s_bodyYaw - facing );
				target = target > 1.75f ? 1.75f : ( target < -1.75f ? -1.75f : target );
			}
			const float ease = dt / kLegEaseSeconds > 1.0f ? 1.0f : dt / kLegEaseSeconds;
			s_legTurn = WrapPi( s_legTurn + WrapPi( target - s_legTurn ) * ease );
		}

		// The whole skeleton below the root turns about the vertical through the waist.
		void TurnBody( float *out, const uint8_t *records, float angle )
		{
			if( std::fabs( angle ) < 0.001f )
			{
				return;
			}
			const D3DXMATRIX delta = YawAbout( angle, Position( out, kWaistBone ) );
			for( int b = 0; b < kBoneCount; ++b )
			{
				if( Parent( records, b ) >= 0 )
				{
					D3DXMATRIX *m = reinterpret_cast< D3DXMATRIX * >( out + b * 16 );
					D3DXMatrixMultiply( m, m, &delta );
				}
			}
		}

		// Extra yaw for the legs only (subtrees hanging off the root or the hips, not the waist's).
		void TurnLegs( float *out, const uint8_t *records, float angle, bool log )
		{
			if( std::fabs( angle ) < 0.001f )
			{
				return;
			}
			// Rotate every leg subtree (children of the root, or of a bone above the waist, that
			// are not the waist's own) about the vertical through the waist.
			const D3DXVECTOR3 pivot = Position( out, kWaistBone );
			const D3DXMATRIX delta = YawAbout( angle, pivot );
			for( int b = 0; b < kBoneCount; ++b )
			{
				const int parent = Parent( records, b );
				if( parent < 0 || Below( records, b, kWaistBone ) || Below( records, kWaistBone, b ) )
				{
					continue;
				}
				if( Parent( records, parent ) < 0 || Below( records, kWaistBone, parent ) )
				{
					ApplyToSubtree( out, records, b, delta );
				}
			}
#ifdef _DEBUG
			if( log )
			{
				PSU_LOG( "VRStrafe: speed %.1f gait %d body %.0f leg extra %.0f deg\n", s_speed,
					static_cast< int >( s_gait ), s_bodyYaw * 57.29578f, angle * 57.29578f );
			}
#endif
		}
	}

	namespace VRHands
	{
		bool EyeLean( float &dx, float &dz )
		{
			if( !Config::Get().vr.m_fpEyeFollowsNeck || GetTickCount() - s_leanTick > 300 )
			{
				return false;
			}
			dx = s_lean[ 0 ];
			dz = s_lean[ 1 ];
			return true;
		}

		void AfterPlayerDraw( void *model )
		{
			if( !Config::Get().vr.m_handsFollow || !VRCamera::FirstPersonActive() || s_inWorldQuery )
			{
				return;
			}
			s_inWorldQuery = true;
			__try
			{
				const float *out = *reinterpret_cast< const float * const * >( static_cast< const uint8_t * >( model ) + 0x64 );
				D3DXMATRIX world;
				if( Readable( out, 64 ) && ModelToWorld( model, out, world ) )
				{
					s_world = world;
					s_worldTick = GetTickCount();
					s_haveWorld = true;
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				s_haveWorld = false;
			}
			s_inWorldQuery = false;
		}

		bool HeadHidden()
		{
			return s_headHiddenTick != 0 && GetTickCount() - s_headHiddenTick < 300;
		}

		CalibStatus CalibrationStatus()
		{
			CalibStatus status;
			const int request = Config::Get().vr.m_handCalibRequest;
			if( request == 0 )
			{
				return status;
			}
			if( request != s_seenCalibRequest )
			{
				status.phase = CalibStatus::Waiting;
			}
			else if( s_calibPending )
			{
				status.phase = CalibStatus::Counting;
				const DWORD elapsed = GetTickCount() - s_calibStart;
				status.secondsLeft = elapsed < kCalibDelayMs ? ( kCalibDelayMs - elapsed ) / 1000.0f : 0.0f;
			}
			else if( s_calibCapturedRequest == request )
			{
				status.hands = s_calibCapturedHands;
				status.phase = s_calibCapturedHands > 0 ? CalibStatus::Done : CalibStatus::Missed;
			}
			else
			{
				status.phase = CalibStatus::Waiting;
			}
			return status;
		}

		// The skeleton's bind pose relative to the root bone (inverse of each record's inverse bind,
		// flag 0x8 = no inverse bind), cached per record array.
		const uint8_t *s_restRecords = nullptr;
		D3DXMATRIX s_rest[ kBoneCount ];
		bool s_restValid[ kBoneCount ]{};
		float s_bodyFit = 1.0f;

		void RestPose( float *out, const uint8_t *skeleton )
		{
			__try
			{
				const int count = *reinterpret_cast< const int * >( skeleton + 0x28 );
				const uint8_t *records = *reinterpret_cast< const uint8_t * const * >( skeleton + 0x30 );
				if( count != kBoneCount || !Readable( records, kBoneCount * kRecordSize ) || !Readable( out, kBoneCount * 64 ) )
				{
					return;
				}
				if( s_restRecords != records )
				{
					D3DXMATRIX bind[ kBoneCount ];
					bool ok[ kBoneCount ]{};
					for( int b = 0; b < kBoneCount; ++b )
					{
						const uint8_t *r = records + b * kRecordSize;
						const D3DXMATRIX inverseBind = *reinterpret_cast< const D3DXMATRIX * >( r + 0x30 );
						// No inverse bind (flag 0x8) means the identity, as in the reference skeleton.
						ok[ b ] = true;
						if( ( *reinterpret_cast< const uint32_t * >( r ) & 0x8u ) || !D3DXMatrixInverse( &bind[ b ], nullptr, &inverseBind ) ||
							!std::isfinite( bind[ b ]._41 ) || !std::isfinite( bind[ b ]._42 ) || !std::isfinite( bind[ b ]._43 ) )
						{
							D3DXMatrixIdentity( &bind[ b ] );
						}
					}
					D3DXMATRIX rootInverse;
					const bool haveRoot = ok[ 0 ] && D3DXMatrixInverse( &rootInverse, nullptr, &bind[ 0 ] );
					for( int b = 0; b < kBoneCount; ++b )
					{
						s_restValid[ b ] = haveRoot && ok[ b ];
						if( s_restValid[ b ] )
						{
							D3DXMatrixMultiply( &s_rest[ b ], &bind[ b ], &rootInverse );
						}
					}
					s_restRecords = records;
#ifdef _DEBUG
					for( int b = 0; b < kBoneCount; ++b )
					{
						PSU_LOG( "VRBones: %2d parent %2d rest %7.2f %7.2f %7.2f%s\n", b, Parent( records, b ), s_rest[ b ]._41, s_rest[ b ]._42,
							s_rest[ b ]._43, s_restValid[ b ] ? "" : " (no bind)" );
					}
#endif
					PSU_LOG( "VRHands: rest pose cached (%d bones, root %s)\n", kBoneCount, haveRoot ? "ok" : "no bind" );
				}
				// The root keeps the game's placement; every other bone sits at its bind pose on it.
				const D3DXMATRIX root = *reinterpret_cast< const D3DXMATRIX * >( out );
				// Optionally scale the whole body about the root so the eyes (neck + ~6 cm) sit at fpEyeHeight.
				float fit = 1.0f;
				if( Config::Get().vr.m_bodyFitHeight && s_restValid[ kNeckBone ] )
				{
					const float eyes = s_rest[ kNeckBone ]._42 + 0.6f;
					fit = eyes > 1.0f ? Config::Get().vr.m_fpEyeHeight * VRCamera::kGameUnitsPerMeter / eyes : 1.0f;
					fit = std::clamp( fit, 0.5f, 2.0f );
					static float s_loggedFit = 0.0f;
					if( s_loggedFit != fit )
					{
						s_loggedFit = fit;
						PSU_LOG( "VRHands: body fit %.3f (rest eyes %.2f)\n", fit, eyes );
					}
				}
				s_bodyFit = fit;
				const bool keepLegs = Config::Get().vr.m_bodyKeepLegAnim;
				for( int b = 1; b < kBoneCount; ++b )
				{
					// Bones 1-8: pelvis, hip node and both legs.
					if( s_restValid[ b ] && !( keepLegs && b <= 8 ) )
					{
						D3DXMATRIX scaled = s_rest[ b ];
						for( int i = 0; i < 4; ++i )
						{
							for( int j = 0; j < 3; ++j )
							{
								scaled.m[ i ][ j ] *= fit;
							}
						}
						D3DXMatrixMultiply( reinterpret_cast< D3DXMATRIX * >( out + b * 16 ), &scaled, &root );
					}
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				PSU_LOG( "VRHands: rest pose exception\n" );
			}
		}

		// Body IK on the rest pose (needs the rest pose): the hips follow the head's height and
		// part of its offset, the spine leans the rest of the way to the head, and each foot stays
		// planted in the world, stepping to a new spot when the hips move away from it.
		constexpr int kPelvisBone = 1;
		constexpr Arm kLegs[ 2 ] = { { 2, 3, 4, 5 }, { 2, 6, 7, 8 } };
		constexpr int kChestBone = 10;
		constexpr float kStepSeconds = 0.28f;
		constexpr float kStepLift = 0.9f;
		constexpr float kStepDistance = 2.5f;
		// The body sits this far behind the eye (game units) so the camera isn't inside the neck and shoulders.
		constexpr float kEyeAheadOfNeck = 2.0f;
		constexpr float kLeanLimit = 40.0f * ( kPi / 180.0f );
		struct Foot
		{
			D3DXVECTOR3 planted, from, to, current;
			float t = 0.0f;
			float duration = 0.28f;
			bool stepping = false;
			bool settled = false; // already re-centred since the last time the body moved
		};
		Foot s_feet[ 2 ];
		bool s_feetInit = false;
		DWORD s_feetTick = 0;
		D3DXVECTOR3 s_pelvisLast( 0.0f, 0.0f, 0.0f ), s_pelvisVel( 0.0f, 0.0f, 0.0f );
		float s_idleSeconds = 0.0f;

		D3DXVECTOR3 ToWorld( const D3DXMATRIX &world, const D3DXVECTOR3 &v )
		{
			D3DXVECTOR3 r;
			D3DXVec3TransformCoord( &r, &v, &world );
			return r;
		}

		D3DXMATRIX RotationPart( const D3DXMATRIX &m )
		{
			D3DXMATRIX r;
			D3DXMatrixIdentity( &r );
			for( int i = 0; i < 3; ++i )
			{
				D3DXVECTOR3 row( m( i, 0 ), m( i, 1 ), m( i, 2 ) );
				row = Normalized( row, D3DXVECTOR3( i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f ) );
				r( i, 0 ) = row.x;
				r( i, 1 ) = row.y;
				r( i, 2 ) = row.z;
			}
			return r;
		}

		// Rotate unit a toward unit b by at most limit, about pivot.
		D3DXMATRIX LimitedTurn( const D3DXVECTOR3 &a, const D3DXVECTOR3 &b, float limit, const D3DXVECTOR3 &pivot )
		{
			const float c = std::clamp( D3DXVec3Dot( &a, &b ), -1.0f, 1.0f );
			const float angle = std::acos( c );
			D3DXVECTOR3 to = b;
			if( angle > limit && angle > 1.0e-4f )
			{
				const float t = limit / angle;
				to = Normalized( a * ( 1.0f - t ) + b * t, b );
			}
			return RotationAbout( a, to, pivot );
		}

		void BodyIk( float *out, const uint8_t *records, const D3DXMATRIX &world, const D3DXMATRIX &toModel,
			const float *headWorld, bool upper, bool legs, float dt, bool log )
		{
			const float fit = s_bodyFit;
			const auto model = [ & ]( const D3DXVECTOR3 &v ) {
				D3DXVECTOR3 r;
				D3DXVec3TransformNormal( &r, &v, &toModel );
				return r;
			};
			// Body forward (level) in the world, from the pelvis.
			// Same as the torso turn: the thigh line (left is +X) squared to the facing.
			const D3DXVECTOR3 acrossModel = Position( out, kLegs[ 0 ].upper ) - Position( out, kLegs[ 1 ].upper );
			const D3DXVECTOR3 forwardModel = Normalized( D3DXVECTOR3( -acrossModel.z, 0.0f, acrossModel.x ), kModelForward );
			D3DXVECTOR3 forward, leftWorld;
			D3DXVec3TransformNormal( &forward, &forwardModel, &world );
			D3DXVec3TransformNormal( &leftWorld, &acrossModel, &world );
			forward.y = 0.0f;
			leftWorld.y = 0.0f;
			forward = Normalized( forward, D3DXVECTOR3( 0.0f, 0.0f, 1.0f ) );
			const D3DXVECTOR3 left = Normalized( leftWorld, D3DXVECTOR3( forward.z, 0.0f, -forward.x ) );

			if( upper && headWorld )
			{
				const D3DXVECTOR3 target = D3DXVECTOR3( headWorld[ 0 ], headWorld[ 1 ], headWorld[ 2 ] ) -
					D3DXVECTOR3( 0.0f, Config::Get().vr.m_fpNeckDrop * VRCamera::kGameUnitsPerMeter, 0.0f ) - forward * kEyeAheadOfNeck;
				const D3DXVECTOR3 neck = ToWorld( world, Position( out, kNeckBone ) );
				const D3DXVECTOR3 d = target - neck;
				// Hips: the neck goes straight under the head; the spine only takes up what is left.
				D3DXVECTOR3 shift( d.x, std::clamp( d.y, -8.0f, 1.5f ), d.z );
				D3DXMATRIX move;
				const D3DXVECTOR3 shiftModel = model( shift );
				D3DXMatrixTranslation( &move, shiftModel.x, shiftModel.y, shiftModel.z );
				ApplyToSubtree( out, records, kPelvisBone, move );
				// Spine: the waist, then the chest, turn the neck toward the head (shared, each limited).
				for( int step = 0; step < 2; ++step )
				{
					const int bone = step == 0 ? kWaistBone : kChestBone;
					const D3DXVECTOR3 pivotModel = Position( out, bone );
					const D3DXVECTOR3 pivot = ToWorld( world, pivotModel );
					const D3DXVECTOR3 now = ToWorld( world, Position( out, kNeckBone ) ) - pivot;
					const D3DXVECTOR3 want = target - pivot;
					if( D3DXVec3Length( &now ) < 1.0e-3f || D3DXVec3Length( &want ) < 1.0e-3f )
					{
						continue;
					}
					const D3DXVECTOR3 a = Normalized( model( Normalized( now, forward ) ), kModelForward );
					const D3DXVECTOR3 b = Normalized( model( Normalized( want, forward ) ), kModelForward );
					ApplyToSubtree( out, records, bone, LimitedTurn( a, b, kLeanLimit * ( step == 0 ? 0.5f : 0.5f ), pivotModel ) );
				}
				if( log )
				{
#ifdef _DEBUG
					const D3DXVECTOR3 after = target - ToWorld( world, Position( out, kNeckBone ) );
					PSU_LOG( "VRBody: hips shift %.2f %.2f %.2f, neck error %.2f -> %.2f (after %.2f %.2f %.2f), forward %.2f %.2f\n", shift.x, shift.y,
						shift.z, D3DXVec3Length( &d ), D3DXVec3Length( &after ), after.x, after.y, after.z, forward.x, forward.z );
#endif
				}
			}

			if( !legs )
			{
				return;
			}
			const D3DXVECTOR3 pelvis = ToWorld( world, Position( out, kPelvisBone ) );
			const float groundY = ToWorld( world, Position( out, 0 ) ).y;
			const float ankleHeight = s_rest[ kLegs[ 0 ].hand ]._42 * fit;
			const float side = std::fabs( s_rest[ kLegs[ 0 ].hand ]._41 ) * fit;
			const float ahead = s_rest[ kLegs[ 0 ].hand ]._43 * fit;
			const float legLength = ( s_rest[ kLegs[ 0 ].upper ]._42 - s_rest[ kLegs[ 0 ].hand ]._42 ) * fit;
			const DWORD now = GetTickCount();
			const float elapsed = s_feetTick ? static_cast< float >( now - s_feetTick ) / 1000.0f : 0.0f;
			const D3DXVECTOR3 pelvisGround( pelvis.x, groundY, pelvis.z );
			D3DXVECTOR3 home[ 2 ];
			for( int i = 0; i < 2; ++i )
			{
				home[ i ] = pelvisGround + left * ( i == 0 ? side : -side ) + forward * ahead;
			}
			if( !s_feetInit || s_feetTick == 0 || now - s_feetTick > 500 || ( s_pelvisLast.x - pelvisGround.x ) * ( s_pelvisLast.x - pelvisGround.x ) + ( s_pelvisLast.z - pelvisGround.z ) * ( s_pelvisLast.z - pelvisGround.z ) > 225.0f )
			{
				for( int i = 0; i < 2; ++i )
				{
					s_feet[ i ].planted = s_feet[ i ].current = home[ i ];
					s_feet[ i ].stepping = false;
				}
				s_pelvisVel = D3DXVECTOR3( 0.0f, 0.0f, 0.0f );
				s_pelvisLast = pelvisGround;
				s_feetTick = now;
				s_feetInit = true;
			}
			// Update once per game frame: Apply also runs for the second eye and the world query.
			if( elapsed >= 0.012f )
			{
				const D3DXVECTOR3 vel = ( pelvisGround - s_pelvisLast ) / elapsed;
				s_pelvisVel += ( vel - s_pelvisVel ) * ( std::min )( 1.0f, elapsed * 8.0f );
				s_pelvisLast = pelvisGround;
				s_feetTick = now;
				s_idleSeconds = D3DXVec3Length( &s_pelvisVel ) < 3.0f ? s_idleSeconds + elapsed : 0.0f;
				if( s_idleSeconds == 0.0f )
				{
					s_feet[ 0 ].settled = s_feet[ 1 ].settled = false;
				}
				for( int i = 0; i < 2; ++i )
				{
					Foot &f = s_feet[ i ];
					// Running: the feet alternate, so the next one may start once the other is half way.
					const Foot &other = s_feet[ 1 - i ];
					const bool fast = D3DXVec3Length( &s_pelvisVel ) > 20.0f;
					const bool otherStepping = other.stepping && !( fast && other.t > 0.5f );
					if( f.stepping )
					{
						f.t += elapsed / f.duration;
						if( f.t >= 1.0f )
						{
							f.planted = f.to;
							f.stepping = false;
						}
						continue;
					}
					D3DXVECTOR3 miss = home[ i ] - f.planted;
					miss.y = 0.0f;
					const D3DXVECTOR3 reach = pelvis - D3DXVECTOR3( f.planted.x, groundY + ankleHeight, f.planted.z );
					const bool tooFar = D3DXVec3Length( &reach ) > legLength * 0.97f && !otherStepping;
					// Standing still: settle back to shoulder width instead of staying wherever the last step ended.
					const bool settle = s_idleSeconds > 0.5f && !f.settled && D3DXVec3Length( &miss ) > 1.5f * fit;
					if( ( ( D3DXVec3Length( &miss ) > kStepDistance * fit || settle ) && !otherStepping ) || tooFar )
					{
						D3DXVECTOR3 lead = s_pelvisVel;
						lead.y = 0.0f;
						const float speed = D3DXVec3Length( &lead );
						// Quicker steps when moving fast; land where the hips will be when the foot arrives,
						// plus half a step so the stride is centred on them.
						f.duration = std::clamp( 0.32f - speed * 0.0025f, 0.16f, 0.30f );
						lead = speed > 1.0f ? lead * ( ( std::min )( speed * f.duration + 0.5f * kStepDistance * fit, 14.0f * fit ) / speed )
							: D3DXVECTOR3( 0.0f, 0.0f, 0.0f );
						f.settled = s_idleSeconds > 0.5f;
						f.from = f.planted;
						f.to = home[ i ] + lead;
						f.t = 0.0f;
						f.stepping = true;
					}
				}
			}
			for( int i = 0; i < 2; ++i )
			{
				Foot &f = s_feet[ i ];
				D3DXVECTOR3 p = f.planted;
				float lift = 0.0f;
				if( f.stepping )
				{
					const float t = std::clamp( f.t, 0.0f, 1.0f );
					const float e = t * t * ( 3.0f - 2.0f * t );
					p = f.from + ( f.to - f.from ) * e;
					lift = std::sin( t * kPi ) * kStepLift * fit;
				}
				// Never cross the legs: each foot stays on its own side of the body.
				const D3DXVECTOR3 fromPelvis = p - pelvisGround;
				const float lateral = D3DXVec3Dot( &fromPelvis, &left ) * ( i == 0 ? 1.0f : -1.0f );
				if( lateral < side * 0.5f )
				{
					p += left * ( ( side * 0.5f - lateral ) * ( i == 0 ? 1.0f : -1.0f ) );
				}
				f.current = p;
				const D3DXVECTOR3 ankle( p.x, groundY + ankleHeight + lift, p.z );
				const D3DXVECTOR3 targetModel = [ & ]() {
					D3DXVECTOR3 r;
					D3DXVec3TransformCoord( &r, &ankle, &toModel );
					return r;
				}();
				SolveArm( out, records, kLegs[ i ], targetModel, model( forward + left * ( i == 0 ? 0.2f : -0.2f ) ) );
				// Level the foot: the rest orientation of the foot on the pelvis' current orientation.
				const D3DXMATRIX *restFoot = &s_rest[ kLegs[ i ].hand ];
				D3DXMATRIX restPelvisInverse;
				if( D3DXMatrixInverse( &restPelvisInverse, nullptr, &s_rest[ kPelvisBone ] ) )
				{
					D3DXMATRIX desired, current, currentInverse, turn;
					D3DXMatrixMultiply( &desired, restFoot, &restPelvisInverse );
					D3DXMatrixMultiply( &desired, &desired, reinterpret_cast< const D3DXMATRIX * >( out + kPelvisBone * 16 ) );
					desired = RotationPart( desired );
					current = RotationPart( *reinterpret_cast< const D3DXMATRIX * >( out + kLegs[ i ].hand * 16 ) );
					D3DXMatrixTranspose( &currentInverse, &current );
					D3DXMatrixMultiply( &turn, &currentInverse, &desired );
					const D3DXVECTOR3 pivot = Position( out, kLegs[ i ].hand );
					turn._41 = pivot.x - ( pivot.x * turn._11 + pivot.y * turn._21 + pivot.z * turn._31 );
					turn._42 = pivot.y - ( pivot.x * turn._12 + pivot.y * turn._22 + pivot.z * turn._32 );
					turn._43 = pivot.z - ( pivot.x * turn._13 + pivot.y * turn._23 + pivot.z * turn._33 );
					ApplyToSubtree( out, records, kLegs[ i ].hand, turn );
				}
			}
			if( log )
			{
#ifdef _DEBUG
				PSU_LOG( "VRBody: feet planted %.1f,%.1f / %.1f,%.1f  stepping %d %d  pelvis speed %.1f\n", s_feet[ 0 ].current.x, s_feet[ 0 ].current.z,
					s_feet[ 1 ].current.x, s_feet[ 1 ].current.z, s_feet[ 0 ].stepping, s_feet[ 1 ].stepping, D3DXVec3Length( &s_pelvisVel ) );
#endif
			}
		}

		void Apply( void *model, float *out, const uint8_t *skeleton )
		{
			const bool restPose = Config::Get().vr.m_bodyRestPose;
			if( restPose )
			{
				RestPose( out, skeleton );
			}
			// Also runs inside the world query's pose rebuild, so the second eye keeps the arms.
			if( !Config::Get().vr.m_handsFollow )
			{
				Note( kSkipSetting );
				return;
			}
			if( !VRCamera::FirstPersonActive() )
			{
				Note( kSkipFirstPerson );
				return;
			}
			const ControllerState *pads[ 2 ] = { &VRControllers::State( 0 ), &VRControllers::State( 1 ) };
			__try
			{
				const int count = *reinterpret_cast< const int * >( skeleton + 0x28 );
				const uint8_t *records = *reinterpret_cast< const uint8_t * const * >( skeleton + 0x30 );
				if( count != kBoneCount || !Readable( records, kBoneCount * kRecordSize ) ||
					!Readable( out, kBoneCount * 64 ) )
				{
					Note( kSkipSkeleton );
					return;
				}
				for( const Arm &arm : kArms )
				{
					if( Parent( records, arm.upper ) != arm.shoulder || Parent( records, arm.fore ) != arm.upper ||
						Parent( records, arm.hand ) != arm.fore )
					{
						Note( kSkipLayout );
						return;
					}
				}
				// From here on, an early exit still hides what first person hides (hands only /
				// headless), so losing tracking for a moment doesn't show the whole body.
				if( !( pads[ 0 ]->gripValid || pads[ 1 ]->gripValid ) )
				{
					// Tracking lost for a moment: keep the body mode. No controller at all: "hands"
					// falls back to headless (nothing would put the hands anywhere).
					const std::string &body = Config::Get().vr.m_fpBody;
					const bool anyActive = pads[ 0 ]->active || pads[ 1 ]->active;
					FoldBody( out, records, anyActive || body != "hands" ? body : kHeadlessBody );
					Note( kSkipControllers );
					return;
				}
				if( !s_haveWorld || GetTickCount() - s_worldTick > kWorldStaleMs )
				{
					FoldBody( out, records, Config::Get().vr.m_fpBody );
					Note( kSkipWorld );
					return;
				}
				const D3DXMATRIX world = s_world;
				D3DXMATRIX toModel;
				if( !D3DXMatrixInverse( &toModel, nullptr, &world ) )
				{
					FoldBody( out, records, Config::Get().vr.m_fpBody );
					Note( kSkipInverse );
					return;
				}
				float forward[ 3 ], right[ 3 ];
				if( !VRCamera::BodyAxes( forward, right ) )
				{
					FoldBody( out, records, Config::Get().vr.m_fpBody );
					Note( kSkipBody );
					return;
				}
				if( !s_loggedWorld )
				{
					s_loggedWorld = true;
					const uint8_t *node = *reinterpret_cast< const uint8_t * const * >( static_cast< const uint8_t * >( model ) + 0x10 );
					const float *nm = Readable( node, 0x44 ) ? reinterpret_cast< const float * >( node + 4 ) : nullptr;
					PSU_LOG( "VRHands: model->world [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f]\n",
						world._11, world._12, world._13, world._21, world._22, world._23, world._31, world._32, world._33,
						world._41, world._42, world._43 );
					if( nm )
					{
						PSU_LOG( "VRHands: node+4 [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]\n",
							nm[ 0 ], nm[ 1 ], nm[ 2 ], nm[ 3 ], nm[ 4 ], nm[ 5 ], nm[ 6 ], nm[ 7 ], nm[ 8 ], nm[ 9 ], nm[ 10 ], nm[ 11 ],
							nm[ 12 ], nm[ 13 ], nm[ 14 ], nm[ 15 ] );
					}
				}

				const bool log = GetTickCount() - s_lastLog >= 3000;
				if( log )
				{
					s_lastLog = GetTickCount();
				}

				// Ease toward the game's animation during Photon Arts (and attacks, if chosen).
				const DWORD now = GetTickCount();
				float dt = s_weightTick ? static_cast< float >( now - s_weightTick ) / 1000.0f : 0.0f;
				dt = dt > 0.1f ? 0.1f : dt;
				s_weightTick = now;
				PSUCombat::State combat{};
				const bool haveCombat = PSUCombat::Read( combat );
				const bool attacking = haveCombat && ( combat.comboStep > 0 || PSUCombat::Swinging( combat ) );
				const bool animation = haveCombat && !restPose && ( combat.photonArt ? Config::Get().vr.m_handsPhotonArtAnimation
					: ( attacking && Config::Get().vr.m_handsAttack == "animation" ) );
				const float goal = animation ? 0.0f : 1.0f;
				const float step = dt / kBlendSeconds;
				s_weight += goal > s_weight ? ( goal - s_weight < step ? goal - s_weight : step )
					: ( s_weight - goal < step ? goal - s_weight : -step );
				const float w = s_weight;

				// Calibration countdown (buzz each second; taken at the end).
				auto &vr = Config::Get().vr;
				if( vr.m_handCalibRequest != s_seenCalibRequest )
				{
					s_seenCalibRequest = vr.m_handCalibRequest;
					s_calibPending = true;
					s_calibStart = now;
					s_calibBuzz = 0;
				}
				bool captureNow = false;
				if( s_calibPending )
				{
					const int second = static_cast< int >( ( now - s_calibStart ) / 1000 );
					if( now - s_calibStart >= kCalibDelayMs )
					{
						captureNow = true;
						s_calibPending = false;
						s_calibCapturedRequest = s_seenCalibRequest;
						s_calibCapturedHands = 0;
					}
					else if( second >= s_calibBuzz )
					{
						s_calibBuzz = second + 1;
						VRControllers::Haptic( 0, 0.3f, 0.03f );
						VRControllers::Haptic( 1, 0.3f, 0.03f );
					}
				}

				// Free aim gun pointing, part 1: in the game's own pose (before anything here moves
				// the arms) the gun points along the shot, so the shot direction in the animated
				// weapon bone's axes is the barrel. Taken fresh each frame: nothing accumulates.
				int aimHand = 0;
				const bool pointGun = vr.m_gunPointsAtAim && !vr.m_aimLeash && haveCombat && w > 0.0f && VRSyntheticPad::GunAiming( aimHand ) &&
					pads[ aimHand & 1 ]->gripValid;
				aimHand &= 1;
				D3DXVECTOR3 shot( 0.0f, 0.0f, 1.0f ), barrelLocal( 1.0f, 0.0f, 0.0f );
				if( pointGun )
				{
					constexpr float kGameAngle = 6.2831853f / 65536.0f;
					const float yaw = static_cast< int16_t >( combat.facing ) * kGameAngle;
					const float pitch = -combat.aimPitch * kGameAngle;
					// Game forward is ( sin, 0, cos ) of the yaw; +0x220 is positive down.
					shot = D3DXVECTOR3( std::sin( yaw ) * std::cos( pitch ), std::sin( pitch ), std::cos( yaw ) * std::cos( pitch ) );
					D3DXMATRIX attachWorld, axesT;
					D3DXMatrixMultiply( &attachWorld, reinterpret_cast< const D3DXMATRIX * >( out + ( kArms[ aimHand ].hand + 1 ) * 16 ),
						&world );
					const D3DXMATRIX axes = Axes( attachWorld );
					D3DXMatrixTranspose( &axesT, &axes );
					D3DXVec3TransformNormal( &barrelLocal, &shot, &axesT );
					barrelLocal = Normalized( barrelLocal, D3DXVECTOR3( 1.0f, 0.0f, 0.0f ) );
				}

				// The game turns the character only when it moves. With strafeBody the whole body
				// trails the head (dead-zone), the chest twists toward the head within a limit and
				// the legs step against the travel direction; without it the chest follows the head.
				float lookYaw = 0.0f;
				if( w > 0.0f && VRCamera::LookYaw( lookYaw ) )
				{
					const bool strafe = vr.m_strafeBody && haveCombat;
					float torsoYaw = lookYaw, legExtra = 0.0f;
					if( strafe )
					{
						StrafeUpdate( combat, lookYaw, dt );
						const float facing = static_cast< float >( combat.facing ) * ( 2.0f * kPi / 65536.0f );
						const float whole = WrapPi( s_bodyYaw - facing ) * w;
						TurnBody( out, records, whole );
						legExtra = WrapPi( s_legTurn * w - whole );
						torsoYaw = s_bodyYaw + std::clamp( WrapPi( lookYaw - s_bodyYaw ), -kTorsoLimit, kTorsoLimit );
					}
					const D3DXVECTOR3 lookWorld( std::sin( torsoYaw ), 0.0f, std::cos( torsoYaw ) );
					D3DXVECTOR3 lookModel;
					D3DXVec3TransformNormal( &lookModel, &lookWorld, &toModel );
					// Where the chest faces now: square to the shoulder line (left side is +X, so
					// left - right = +X means facing +Z). Animations often twist the torso.
					const D3DXVECTOR3 across = Position( out, kArms[ 0 ].upper ) - Position( out, kArms[ 1 ].upper );
					const D3DXVECTOR3 chest = Normalized( D3DXVECTOR3( -across.z, 0.0f, across.x ), kModelForward );
					const float chestYaw = std::atan2( chest.x, chest.z );
					const float lookYawModel = std::atan2( lookModel.x, lookModel.z );
					float delta = lookYawModel - chestYaw;
					delta = delta > 3.14159265f ? delta - 6.2831853f : ( delta < -3.14159265f ? delta + 6.2831853f : delta );
					const float angle = chestYaw + delta * w;
					const D3DXVECTOR3 turned( std::sin( angle ), 0.0f, std::cos( angle ) );
					ApplyToSubtree( out, records, kWaistBone, RotationAbout( chest, turned, Position( out, kWaistBone ) ) );
					if( strafe && !( restPose && vr.m_bodyProceduralLegs ) )
					{
						TurnLegs( out, records, legExtra, log );
					}
					if( log )
					{
#ifdef _DEBUG
						PSU_LOG( "VRHands: torso turn %.0f deg (chest was %.0f), look %.0f body %.0f, weight %.2f\n", delta * w * 57.29578f,
							chestYaw * 57.29578f, lookYaw * 57.29578f, s_bodyYaw * 57.29578f, w );
#endif
					}
				}

				if( restPose && w > 0.0f && ( vr.m_bodyIk || vr.m_bodyProceduralLegs ) )
				{
					float head[ 3 ];
					const bool haveHead = vr.m_bodyIk && VRCamera::HeadWorld( head );
					BodyIk( out, records, world, toModel, haveHead ? head : nullptr, haveHead, vr.m_bodyProceduralLegs, dt, log );
				}

				if( haveCombat && vr.m_fpEyeFollowsNeck && !( restPose && vr.m_bodyIk ) )
				{
					UpdateEyeLean( out, world, combat, forward, dt, log );
				}

				if( w <= 0.0f && !captureNow )
				{
					// The game's animation; only hide what first person hides. (The clip plane,
					// which also cut raised hands, isn't needed once the head is folded.)
					FoldBody( out, records, vr.m_fpBody );
					Note( kSolved );
					return;
				}

				bool tracked = true;
				for( int hand = 0; hand < 2; ++hand )
				{
					const ControllerState &c = *pads[ hand ];
					if( !c.gripValid )
					{
						continue;
					}
					const D3DXVECTOR3 grip( c.gripPosition[ 0 ], c.gripPosition[ 1 ], c.gripPosition[ 2 ] );
					const D3DXVECTOR3 wristTracking = grip + RotateByQuat( c.gripOrientation, { 0.0f, 0.0f, kWristBackMeters } );
					const float tracking[ 3 ] = { wristTracking.x, wristTracking.y, wristTracking.z };
					float targetWorld[ 3 ];
					if( !VRCamera::TrackingToWorld( tracking, targetWorld ) )
					{
						tracked = false;
						break;
					}
					// Elbows hang down, a little back and out to the side.
					const float side = hand == 0 ? -1.0f : 1.0f;
					const D3DXVECTOR3 poleWorld( -forward[ 0 ] * 0.5f + right[ 0 ] * side * 0.4f, -1.0f,
						-forward[ 2 ] * 0.5f + right[ 2 ] * side * 0.4f );
					D3DXVECTOR3 target, pole;
					const D3DXVECTOR3 tw( targetWorld[ 0 ], targetWorld[ 1 ], targetWorld[ 2 ] );
					D3DXVec3TransformCoord( &target, &tw, &toModel );
					D3DXVec3TransformNormal( &pole, &poleWorld, &toModel );
					pole = Normalized( pole, { 0.0f, -1.0f, 0.0f } );

					const Arm &arm = kArms[ hand ];
					// Blend: reach part of the way from the animated wrist.
					const D3DXVECTOR3 animated = Position( out, arm.hand );
					const D3DXVECTOR3 reach = animated + ( target - animated ) * w;
					if( log )
					{
						const D3DXVECTOR3 s = Position( out, arm.upper );
						const D3DXVECTOR3 d = target - s;
#ifdef _DEBUG
						PSU_LOG( "VRHands: %s target model (%.2f %.2f %.2f) shoulder (%.2f %.2f %.2f) reach %.2f\n",
							hand == 0 ? "L" : "R", target.x, target.y, target.z, s.x, s.y, s.z, D3DXVec3Length( &d ) );
#endif
					}
					SolveArm( out, records, arm, reach, pole );
					{
						// The character's arm is shorter than yours: the solved arm stops at its own
						// length. "hands" has no arm to show, so the hand goes the rest of the way; with a
						// visible arm (armStretch) it stretches, half the gap at the elbow and half at the
						// wrist, up to half its length.
						D3DXVECTOR3 gap = reach - Position( out, arm.hand );
						if( vr.m_fpBody != "hands" )
						{
							if( !vr.m_armStretch )
							{
								gap = D3DXVECTOR3( 0.0f, 0.0f, 0.0f );
							}
							else
							{
								const D3DXVECTOR3 upperArm = Position( out, arm.fore ) - Position( out, arm.upper );
								const D3DXVECTOR3 foreArm = Position( out, arm.hand ) - Position( out, arm.fore );
								const float chain = D3DXVec3Length( &upperArm ) + D3DXVec3Length( &foreArm );
								const float gl = D3DXVec3Length( &gap );
								if( gl > chain * 0.5f && gl > 1.0e-4f )
								{
									gap *= chain * 0.5f / gl;
								}
							}
						}
						if( D3DXVec3LengthSq( &gap ) > 1.0e-8f )
						{
							const D3DXVECTOR3 half = gap * 0.5f;
							D3DXMATRIX shift;
							if( vr.m_fpBody != "hands" )
							{
								D3DXMatrixTranslation( &shift, half.x, half.y, half.z );
								ApplyToSubtree( out, records, arm.fore, shift );
								D3DXMatrixTranslation( &shift, gap.x - half.x, gap.y - half.y, gap.z - half.z );
							}
							else
							{
								D3DXMatrixTranslation( &shift, gap.x, gap.y, gap.z );
							}
							ApplyToSubtree( out, records, arm.hand, shift );
						}
					}

					// Hand angle: hand axes = calibration x controller axes (both in world).
					D3DXMATRIX controller;
					if( !ControllerAxes( c, controller ) )
					{
						continue;
					}
					D3DXMATRIX handWorld;
					D3DXMatrixMultiply( &handWorld, reinterpret_cast< const D3DXMATRIX * >( out + arm.hand * 16 ), &world );
					const D3DXMATRIX handAxes = Axes( handWorld );
					if( captureNow )
					{
						// K = H x C^T (rows are axes, so C^-1 = C^T), H = the hand in the calibration pose.
						// Forward = where both controllers point (level), not the body: you may stand
						// turned away from the character's facing.
						D3DXMATRIX poseAxes;
						D3DXVECTOR3 forwardWorld( forward[ 0 ], forward[ 1 ], forward[ 2 ] );
						D3DXVECTOR3 aimSum( 0.0f, 0.0f, 0.0f );
						for( int h = 0; h < 2; ++h )
						{
							if( !pads[ h ]->aimValid )
							{
								continue;
							}
							const D3DXVECTOR3 aim = RotateByQuat( pads[ h ]->aimOrientation, { 0.0f, 0.0f, -1.0f } );
							const float in[ 3 ] = { aim.x, aim.y, aim.z };
							float aimWorld[ 3 ];
							if( VRCamera::TrackingDirToWorld( in, aimWorld ) )
							{
								aimSum += D3DXVECTOR3( aimWorld[ 0 ], 0.0f, aimWorld[ 2 ] );
							}
						}
						forwardWorld = Normalized( aimSum, forwardWorld );
						// Game right = ( -forward.z, 0, forward.x ).
						const D3DXVECTOR3 rightWorld( -forwardWorld.z, 0.0f, forwardWorld.x );
						if( !CalibrationHandAxes( records, arm, hand, forwardWorld, rightWorld, poseAxes ) )
						{
							continue;
						}
						D3DXMATRIX controllerT, k;
						D3DXMatrixTranspose( &controllerT, &controller );
						D3DXMatrixMultiply( &k, &poseAxes, &controllerT );
						D3DXQUATERNION q;
						D3DXQuaternionRotationMatrix( &q, &k );
						StoreCalib( hand, q );
						vr.m_handCalibrated = true;
						++s_calibCapturedHands;
						VRControllers::Haptic( hand, 0.8f, 0.15f );
						PSU_LOG( "VRHands: calibrated %s hand (%.3f %.3f %.3f %.3f)\n", hand == 0 ? "left" : "right", q.x, q.y, q.z, q.w );
						if( hand == 1 || !pads[ 1 ]->gripValid )
						{
							Config::Get().SaveToFile();
							VRBindings::ShowToast( "Hand calibration saved" );
						}
						continue;
					}
					if( w <= 0.0f )
					{
						continue;
					}
					D3DXMATRIX k, tweak, tweakRoll, desired, handT, delta;
					if( vr.m_handCalibrated )
					{
						const D3DXQUATERNION q = CalibQuat( hand );
						D3DXMatrixRotationQuaternion( &k, &q );
					}
					else if( !AutoCalibration( records, hand, k ) )
					{
						continue;
					}
					// Fine tune in the controller's frame: pitch about its X, roll about its Z.
					constexpr float kDeg = 3.14159265f / 180.0f;
					D3DXMatrixRotationX( &tweak, vr.m_handPitch * kDeg );
					D3DXMatrixRotationZ( &tweakRoll, ( hand == 0 ? -vr.m_handRoll : vr.m_handRoll ) * kDeg );
					D3DXMatrixMultiply( &tweak, &tweak, &tweakRoll );
					// Free aim (leash): the gun hand's own angle adjustment, so the gun model lines up
					// with the reticle on the controller's line. Yaw about the controller's up (Y).
					int gunHand = 0;
					float adjustPitch = 0.0f, adjustYaw = 0.0f;
					if( vr.m_aimLeash && VRSyntheticPad::GunAiming( gunHand ) && gunHand == hand &&
						VRSyntheticPad::GunAdjust( adjustPitch, adjustYaw ) )
					{
						D3DXMATRIX adjust, adjustYawM;
						D3DXMatrixRotationX( &adjust, adjustPitch * kDeg );
						D3DXMatrixRotationY( &adjustYawM, adjustYaw * kDeg );
						D3DXMatrixMultiply( &adjust, &adjust, &adjustYawM );
						D3DXMatrixMultiply( &tweak, &tweak, &adjust );
					}
					D3DXMatrixMultiply( &k, &k, &tweak );
					D3DXMatrixMultiply( &desired, &k, &controller );
					D3DXMatrixTranspose( &handT, &handAxes );
					D3DXMatrixMultiply( &delta, &handT, &desired );  // H x delta = desired
					if( w < 1.0f )
					{
						D3DXQUATERNION dq, partial;
						const D3DXQUATERNION identity( 0.0f, 0.0f, 0.0f, 1.0f );
						D3DXQuaternionRotationMatrix( &dq, &delta );
						D3DXQuaternionSlerp( &partial, &identity, &dq, w );
						D3DXMatrixRotationQuaternion( &delta, &partial );
					}
					// About the wrist, in world; then into model space: W x A x W^-1.
					const D3DXVECTOR3 wrist( handWorld._41, handWorld._42, handWorld._43 );
					D3DXVECTOR3 moved;
					D3DXVec3TransformNormal( &moved, &wrist, &delta );
					delta._41 = wrist.x - moved.x;
					delta._42 = wrist.y - moved.y;
					delta._43 = wrist.z - moved.z;
					D3DXMATRIX modelDelta;
					D3DXMatrixMultiply( &modelDelta, &world, &delta );
					D3DXMatrixMultiply( &modelDelta, &modelDelta, &toModel );
					ApplyToSubtree( out, records, arm.hand, modelDelta );
				}
				// Staff (category 8) / rod (3): keep each weapon bone where it sits in the hand outside
				// attacks, so casting doesn't swing the weapon about. Not the TCSM (left category B).
				const bool techWeapon = haveCombat && ( combat.rightCategory == 3 || combat.rightCategory == 8 );
				if( !techWeapon || combat.weaponClass != s_weaponRestClass )
				{
					s_haveWeaponRest[ 0 ] = s_haveWeaponRest[ 1 ] = false;
					s_weaponRestClass = haveCombat ? combat.weaponClass : 0;
				}
				if( techWeapon && vr.m_techWeaponStill && w > 0.0f && !restPose )
				{
					if( combat.comboStep > 0 || PSUCombat::Swinging( combat ) )
					{
						s_castTick = now;
					}
					const bool settled = now - s_castTick > kCastSettleMs;
					for( int hand = 0; hand < 2; ++hand )
					{
						const int handBone = kArms[ hand ].hand;
						const int weaponBone = handBone + 1;
						if( ( hand == 0 && combat.leftCategory == 0xB ) || Parent( records, weaponBone ) != handBone )
						{
							continue;
						}
						const D3DXMATRIX &handM = *reinterpret_cast< const D3DXMATRIX * >( out + handBone * 16 );
						const D3DXMATRIX &weaponM = *reinterpret_cast< const D3DXMATRIX * >( out + weaponBone * 16 );
						D3DXMATRIX handInverse, weaponInverse;
						if( !D3DXMatrixInverse( &handInverse, nullptr, &handM ) )
						{
							continue;
						}
						if( settled )
						{
							// weapon = rest x hand (row vectors).
							D3DXMatrixMultiply( &s_weaponRest[ hand ], &weaponM, &handInverse );
							s_haveWeaponRest[ hand ] = true;
						}
						else if( s_haveWeaponRest[ hand ] && D3DXMatrixInverse( &weaponInverse, nullptr, &weaponM ) )
						{
							D3DXMATRIX desired, delta;
							D3DXMatrixMultiply( &desired, &s_weaponRest[ hand ], &handM );
							D3DXMatrixMultiply( &delta, &weaponInverse, &desired );
							ApplyToSubtree( out, records, weaponBone, delta );
						}
					}
				}
				// Free aim gun pointing, part 2: turn the gun hand about the wrist so that barrel
				// points along the shot (the player's facing and aim pitch, which bullets fly along).
				if( pointGun )
				{
					const Arm &arm = kArms[ aimHand ];
					D3DXMATRIX handWorld, attachWorld;
					D3DXMatrixMultiply( &handWorld, reinterpret_cast< const D3DXMATRIX * >( out + arm.hand * 16 ), &world );
					D3DXMatrixMultiply( &attachWorld, reinterpret_cast< const D3DXMATRIX * >( out + ( arm.hand + 1 ) * 16 ), &world );
					const D3DXVECTOR3 wrist( handWorld._41, handWorld._42, handWorld._43 );
					const D3DXMATRIX axes = Axes( attachWorld );
					D3DXVECTOR3 barrel;
					D3DXVec3TransformNormal( &barrel, &barrelLocal, &axes );
					barrel = Normalized( barrel, shot );
					const D3DXMATRIX turn = RotationAbout( barrel, shot, wrist );
					D3DXMATRIX modelDelta;
					D3DXMatrixMultiply( &modelDelta, &world, &turn );
					D3DXMatrixMultiply( &modelDelta, &modelDelta, &toModel );
					ApplyToSubtree( out, records, arm.hand, modelDelta );
				}
				FoldBody( out, records, vr.m_fpBody );
				Note( tracked ? kSolved : kSkipTracking );
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				PSU_LOG( "VRHands: exception\n" );
			}
		}
	}
}
