#include "VRBodyIk.h"

#include "VRCamera.h"
#include "VRRuntime.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PSUVR
{
	using namespace Skeleton;

	namespace
	{
		bool Readable( const void *p, size_t bytes )
		{
			return p && PSUMemory::IsValidPointer( p ) &&
				PSUMemory::IsValidPointer( static_cast< const uint8_t * >( p ) + bytes - 1 );
		}

		const uint8_t *s_mapRecords = nullptr;
		bool s_mapOk = false;

		// The VRBodyAnimKeep group a bone belongs to.
		unsigned KeepGroup( int bone )
		{
			if( bone <= 8 )
				return kVRKeepLegs;
			if( bone <= 11 )
				return kVRKeepSpine;
			if( bone <= 13 )
				return kVRKeepHead;
			if( bone == 18 || bone == 23 )
				return kVRKeepWeapons;
			if( bone <= 22 )
				return kVRKeepArms;
			return kVRKeepExtras;
		}

		// The skeleton's bind pose relative to the root bone (inverse of each record's inverse bind,
		// flag 0x8 = no inverse bind), cached per record array.
		const uint8_t *s_restRecords = nullptr;
		D3DXMATRIX s_rest[ kBoneCount ];
		bool s_restValid[ kBoneCount ]{};
		float s_bodyFit = 1.0f;

		// Spine: CCD from the waist up. Each bone takes its share of the turn still needed (the last
		// takes the rest), within its part of kSpineLean.
		constexpr int kSpine[ 3 ] = { kWaistBone, kChestBone, kShoulderLevelBone };
		constexpr float kSpineWeights[ 3 ] = { 0.3f, 0.4f, 0.3f };
		constexpr float kSpineShare[ 3 ] = { 0.3f, 0.4f / 0.7f, 1.0f };
		constexpr int kSpineIterations = 3;
		constexpr float kSpineLean = 60.0f * ( kPi / 180.0f );
		// Crouching pushes the hips back by this much of the drop, so the knees bend forward.
		constexpr float kCrouchBack = 0.4f;
		// Head to the HMD: the neck takes this share; the whole turn is limited (the body yaw
		// catches up past it).
		constexpr float kNeckShare = 0.3f;
		constexpr float kHeadLimit = 80.0f * ( kPi / 180.0f );
		// Body yaw toward the hands: how far, and from how far in front of the head (meters) the
		// hands count fully.
		constexpr float kHandsYawBlend = 0.5f;
		constexpr float kHandsYawNear = 0.05f, kHandsYawFull = 0.25f;
		constexpr float kHandsYawMaxOff = 90.0f * ( kPi / 180.0f );
		// Shoulder shrug: at bodyShoulderShrug 0.5, up to 20 deg once the reach passes kShrugFrom.
		constexpr float kShrugPerStrength = 40.0f * ( kPi / 180.0f );
		constexpr float kShrugFrom = 0.85f;

		// Planted feet.
		constexpr float kStepLift = 0.9f;
		constexpr float kStepDistance = 2.5f;
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

		// The head (subtree of the neck) turns to the HMD: the neck takes kNeckShare, the head the
		// rest, together at most kHeadLimit.
		void HeadToHmd( float *out, const uint8_t *records, const D3DXMATRIX &toModel, bool log )
		{
			float forwardWorld[ 3 ], upWorld[ 3 ];
			if( !s_restValid[ kHeadBone ] || !VRCamera::HeadAxes( forwardWorld, upWorld ) )
			{
				return;
			}
			D3DXVECTOR3 wantForward, wantUp;
			const D3DXVECTOR3 fw( forwardWorld[ 0 ], forwardWorld[ 1 ], forwardWorld[ 2 ] );
			const D3DXVECTOR3 uw( upWorld[ 0 ], upWorld[ 1 ], upWorld[ 2 ] );
			D3DXVec3TransformNormal( &wantForward, &fw, &toModel );
			D3DXVec3TransformNormal( &wantUp, &uw, &toModel );
			// The head's forward and up in its own axes, from the bind pose (faces the model's +Z).
			D3DXMATRIX restAxesT;
			const D3DXMATRIX restAxes = Axes( s_rest[ kHeadBone ] );
			D3DXMatrixTranspose( &restAxesT, &restAxes );
			const D3DXVECTOR3 up( 0.0f, 1.0f, 0.0f );
			D3DXVECTOR3 localForward, localUp;
			D3DXVec3TransformNormal( &localForward, &kModelForward, &restAxesT );
			D3DXVec3TransformNormal( &localUp, &up, &restAxesT );
			const auto turnToHmd = [ & ]() {
				const D3DXMATRIX axes = Axes( *reinterpret_cast< const D3DXMATRIX * >( out + kHeadBone * 16 ) );
				D3DXVECTOR3 forward, headUp;
				D3DXVec3TransformNormal( &forward, &localForward, &axes );
				D3DXVec3TransformNormal( &headUp, &localUp, &axes );
				return FrameRotation( forward, headUp, wantForward, wantUp );
			};
			const D3DXMATRIX full = turnToHmd();
			const float angle = RotationAngle( full );
			if( angle < 1.0e-3f )
			{
				return;
			}
			const float t = ( std::min )( 1.0f, kHeadLimit / angle );
			ApplyToSubtree( out, records, kNeckBone, PartialAbout( full, kNeckShare * t, Position( out, kNeckBone ) ) );
			// What is left of the same turn, measured again from the new pose.
			const float rest = ( t - kNeckShare * t ) / ( 1.0f - kNeckShare * t );
			ApplyToSubtree( out, records, kHeadBone, PartialAbout( turnToHmd(), rest, Position( out, kHeadBone ) ) );
#ifdef _DEBUG
			if( log )
			{
				PSU_LOG( "VRBody: head to HMD %.0f deg (limited to %.0f)\n", angle * 57.29578f, angle * t * 57.29578f );
			}
#endif
		}
	}

	namespace VRBodyIk
	{
		bool BoneMapOk( const uint8_t *records )
		{
			if( records == s_mapRecords )
			{
				return s_mapOk;
			}
			s_mapRecords = records;
			s_mapOk = true;
			for( int b = 0; b < kBoneCount; ++b )
			{
				const int parent = Parent( records, b );
				if( parent != kBoneParents[ b ] )
				{
					PSU_LOG( "VRBody: bone map mismatch (bone %d parent %d, expected %d): rest pose and body IK off for this model\n",
						b, parent, kBoneParents[ b ] );
					s_mapOk = false;
					return false;
				}
			}
			PSU_LOG( "VRBody: bone map ok (records %p)\n", records );
			return true;
		}

		float BodyFit()
		{
			return s_bodyFit;
		}

		void RestPose( float *out, const uint8_t *skeleton )
		{
			__try
			{
				const int count = *reinterpret_cast< const int * >( skeleton + 0x28 );
				const uint8_t *records = *reinterpret_cast< const uint8_t * const * >( skeleton + 0x30 );
				if( count != kBoneCount || !Readable( records, kBoneCount * kRecordSize ) || !Readable( out, kBoneCount * 64 ) ||
					!BoneMapOk( records ) )
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
				// Kept bones (bodyAnimKeep) keep their animated pose relative to their parent and
				// follow the parent's new pose; parents come before children (BoneMapOk).
				const unsigned keep = Config::Get().vr.m_bodyAnimKeep;
				D3DXMATRIX anim[ kBoneCount ];
				memcpy( anim, out, sizeof( anim ) );
				D3DXMATRIX fitScale;
				D3DXMatrixScaling( &fitScale, fit, fit, fit );
				D3DXMATRIX fitRoot;
				D3DXMatrixMultiply( &fitRoot, &fitScale, &root );
				bool moved[ kBoneCount ]{};
				moved[ 0 ] = fit != 1.0f;
				for( int b = 1; b < kBoneCount; ++b )
				{
					D3DXMATRIX *target = reinterpret_cast< D3DXMATRIX * >( out + b * 16 );
					const int parent = kBoneParents[ b ];
					if( s_restValid[ b ] && !( keep & KeepGroup( b ) ) )
					{
						D3DXMatrixMultiply( target, &s_rest[ b ], &fitRoot );
						moved[ b ] = true;
					}
					else if( moved[ parent ] )
					{
						D3DXMATRIX parentInverse, local;
						if( D3DXMatrixInverse( &parentInverse, nullptr, &anim[ parent ] ) )
						{
							D3DXMatrixMultiply( &local, &anim[ b ], &parentInverse );
							const D3DXMATRIX *parentNow = parent == 0 ? &fitRoot : reinterpret_cast< const D3DXMATRIX * >( out + parent * 16 );
							D3DXMatrixMultiply( target, &local, parentNow );
							moved[ b ] = true;
						}
					}
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				PSU_LOG( "VRHands: rest pose exception\n" );
			}
		}

		// The hips follow the head's height and part of its offset, the spine leans the rest of the
		// way to the head, and each foot stays planted in the world, stepping to a new spot when the
		// hips move away from it.
		void Solve( float *out, const uint8_t *records, const D3DXMATRIX &world, const D3DXMATRIX &toModel,
			const float *headWorld, bool upper, bool legs, float dt, bool log )
		{
			const auto &vr = Config::Get().vr;
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
					D3DXVECTOR3( 0.0f, vr.m_bodyNeckDrop * VRCamera::kGameUnitsPerMeter, 0.0f ) -
					forward * ( vr.m_bodyBack * VRCamera::kGameUnitsPerMeter );
				const D3DXVECTOR3 neck = ToWorld( world, Position( out, kNeckBone ) );
				const D3DXVECTOR3 d = target - neck;
				// Hips: the neck goes straight under the head; a crouch pushes them back (the knees bend
				// forward) and the spine leans the rest of the way.
				const float dy = std::clamp( d.y, -8.0f, 1.5f );
				const float crouch = dy < 0.0f ? -dy : 0.0f;
				const D3DXVECTOR3 shift = D3DXVECTOR3( d.x, dy, d.z ) - forward * ( crouch * kCrouchBack );
				D3DXMATRIX move;
				const D3DXVECTOR3 shiftModel = model( shift );
				D3DXMatrixTranslation( &move, shiftModel.x, shiftModel.y, shiftModel.z );
				ApplyToSubtree( out, records, kPelvisBone, move );
				// Spine (CCD): waist, chest, shoulder level each turn the neck toward the target.
				float used[ 3 ]{};
				for( int iteration = 0; iteration < kSpineIterations; ++iteration )
				{
					for( int i = 0; i < 3; ++i )
					{
						const int bone = kSpine[ i ];
						const float budget = kSpineLean * kSpineWeights[ i ] - used[ i ];
						const D3DXVECTOR3 pivotModel = Position( out, bone );
						const D3DXVECTOR3 pivot = ToWorld( world, pivotModel );
						const D3DXVECTOR3 now = ToWorld( world, Position( out, kNeckBone ) ) - pivot;
						const D3DXVECTOR3 want = target - pivot;
						if( budget < 1.0e-4f || D3DXVec3Length( &now ) < 1.0e-3f || D3DXVec3Length( &want ) < 1.0e-3f )
						{
							continue;
						}
						const D3DXVECTOR3 a = Normalized( model( Normalized( now, forward ) ), kModelForward );
						const D3DXVECTOR3 b = Normalized( model( Normalized( want, forward ) ), kModelForward );
						const float angle = std::acos( std::clamp( D3DXVec3Dot( &a, &b ), -1.0f, 1.0f ) );
						const float step = ( std::min )( angle * kSpineShare[ i ], budget );
						if( step < 1.0e-4f )
						{
							continue;
						}
						ApplyToSubtree( out, records, bone, LimitedTurn( a, b, step, pivotModel ) );
						used[ i ] += step;
					}
				}
				if( log )
				{
#ifdef _DEBUG
					const D3DXVECTOR3 after = target - ToWorld( world, Position( out, kNeckBone ) );
					PSU_LOG( "VRBody: hips shift %.2f %.2f %.2f (crouch %.2f), neck error %.2f -> %.2f (after %.2f %.2f %.2f), "
						"spine %.0f %.0f %.0f deg, forward %.2f %.2f\n", shift.x, shift.y, shift.z, crouch, D3DXVec3Length( &d ),
						D3DXVec3Length( &after ), after.x, after.y, after.z, used[ 0 ] * 57.29578f, used[ 1 ] * 57.29578f,
						used[ 2 ] * 57.29578f, forward.x, forward.z );
#endif
				}
				if( vr.m_bodyHeadFollowsHmd )
				{
					HeadToHmd( out, records, toModel, log );
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
					// Stretched leg: measured from the hip joint, and only worth a step if it lands somewhere else.
					const D3DXVECTOR3 reach = ToWorld( world, Position( out, kLegs[ i ].upper ) ) - D3DXVECTOR3( f.planted.x, groundY + ankleHeight, f.planted.z );
					const bool tooFar = D3DXVec3Length( &reach ) > legLength * 1.02f && D3DXVec3Length( &miss ) > 0.6f * fit && !otherStepping;
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
					desired = Axes( desired );
					current = Axes( *reinterpret_cast< const D3DXMATRIX * >( out + kLegs[ i ].hand * 16 ) );
					D3DXMatrixTranspose( &currentInverse, &current );
					D3DXMatrixMultiply( &turn, &currentInverse, &desired );
					SetPivot( turn, Position( out, kLegs[ i ].hand ) );
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

		float BodyYawGoal( float lookYaw, const ControllerState *const pads[ 2 ] )
		{
			float head[ 3 ];
			if( !VRCamera::HeadWorld( head ) )
			{
				return lookYaw;
			}
			D3DXVECTOR3 sum( 0.0f, 0.0f, 0.0f );
			for( int hand = 0; hand < 2; ++hand )
			{
				float w[ 3 ];
				if( !pads[ hand ]->gripValid || !VRCamera::TrackingToWorld( pads[ hand ]->gripPosition, w ) || w[ 1 ] > head[ 1 ] )
				{
					return lookYaw;
				}
				sum += D3DXVECTOR3( w[ 0 ], 0.0f, w[ 2 ] );
			}
			const D3DXVECTOR3 mid = sum * 0.5f - D3DXVECTOR3( head[ 0 ], 0.0f, head[ 2 ] );
			const float dist = D3DXVec3Length( &mid ) / VRCamera::kGameUnitsPerMeter;
			const float weight = std::clamp( ( dist - kHandsYawNear ) / ( kHandsYawFull - kHandsYawNear ), 0.0f, 1.0f );
			const float off = WrapPi( std::atan2( mid.x, mid.z ) - lookYaw );
			if( weight <= 0.0f || std::fabs( off ) > kHandsYawMaxOff )
			{
				return lookYaw;
			}
			return WrapPi( lookYaw + off * kHandsYawBlend * weight );
		}

		void TwistSpine( float *out, const uint8_t *records, float angle )
		{
			if( std::fabs( angle ) < 0.001f )
			{
				return;
			}
			for( int i = 0; i < 3; ++i )
			{
				ApplyToSubtree( out, records, kSpine[ i ], YawAbout( angle * kSpineWeights[ i ], Position( out, kSpine[ i ] ) ) );
			}
		}

		void Shrug( float *out, const uint8_t *records, const Arm &arm, const D3DXVECTOR3 &target )
		{
			const float strength = Config::Get().vr.m_bodyShoulderShrug;
			if( strength <= 0.0f )
			{
				return;
			}
			const D3DXVECTOR3 base = Position( out, arm.shoulder );
			const D3DXVECTOR3 s = Position( out, arm.upper );
			const D3DXVECTOR3 upperArm = Position( out, arm.fore ) - s;
			const D3DXVECTOR3 foreArm = Position( out, arm.hand ) - Position( out, arm.fore );
			const float chain = D3DXVec3Length( &upperArm ) + D3DXVec3Length( &foreArm );
			const D3DXVECTOR3 toTarget = target - s;
			if( chain < 1.0e-3f )
			{
				return;
			}
			// Far (past kShrugFrom of the arm's length) or above the shoulder (model +Y is up).
			const float distant = ( D3DXVec3Length( &toTarget ) / chain - kShrugFrom ) / ( 1.0f - kShrugFrom );
			const float high = toTarget.y / chain;
			const float amount = std::clamp( ( std::max )( distant, high ), 0.0f, 1.0f );
			if( amount <= 0.0f )
			{
				return;
			}
			const D3DXVECTOR3 a = Normalized( s - base, kModelForward );
			const D3DXVECTOR3 b = Normalized( target - base, a );
			ApplyToSubtree( out, records, arm.shoulder, LimitedTurn( a, b, kShrugPerStrength * strength * amount, base ) );
		}

		D3DXVECTOR3 ElbowPole( const float *out, const Arm &arm, const D3DXVECTOR3 &target, const D3DXVECTOR3 &pole,
			const D3DXVECTOR3 &forearmBack )
		{
			const float k = std::clamp( Config::Get().vr.m_bodyElbowFromHand, 0.0f, 1.0f );
			if( k <= 0.0f )
			{
				return pole;
			}
			const D3DXVECTOR3 foreArm = Position( out, arm.hand ) - Position( out, arm.fore );
			const D3DXVECTOR3 elbow = target + Normalized( forearmBack, D3DXVECTOR3( 0.0f, 0.0f, 0.0f ) ) * D3DXVec3Length( &foreArm );
			const D3DXVECTOR3 fromHand = Normalized( elbow - Position( out, arm.upper ), pole );
			return Normalized( Normalized( pole, fromHand ) * ( 1.0f - k ) + fromHand * k, pole );
		}
	}
}
