#include "VRBodyIk.h"

#include "VRCamera.h"
#include "VRGameHaptics.h"
#include "VRRuntime.h"
#include "VRTrackers.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
			if( !s_restValid[ kHeadBone ] || !VRCamera::BodyHeadAxes( forwardWorld, upWorld ) )
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

		// ---- trackers driving the body (FBT Phase 4)
		constexpr float kTrackerRamp = 0.3f;              // seconds for a role to ease in / out
		constexpr float kChestLimit = 70.0f * ( kPi / 180.0f );
		constexpr float kFootLimit = 120.0f * ( kPi / 180.0f );
		struct Driven
		{
			float weight = 0.0f;
			bool have = false;      // pose holds a calibrated pose (live, or the last one while easing out)
			bool live = false;
			VRTrackers::Pose pose;  // LOCAL, re-mapped every frame so the stick moving the body carries it
		};
		Driven s_drive[ kTrackerRoleCount ];
		float s_legScale = 1.0f;     // character hip height over the user's (calibration), last Solve

		float DriveWeight( TrackerRole role )
		{
			const Driven &d = s_drive[ static_cast< int >( role ) ];
			return d.have ? d.weight : 0.0f;
		}

		// The joint in the game world (the same mapping as the hands: VRCamera::BodyTrackingToWorld).
		bool JointWorld( TrackerRole role, D3DXVECTOR3 &p )
		{
			const Driven &d = s_drive[ static_cast< int >( role ) ];
			float w[ 3 ];
			if( !d.have || !VRCamera::BodyTrackingToWorld( d.pose.joint, w ) )
			{
				return false;
			}
			p = D3DXVECTOR3( w[ 0 ], w[ 1 ], w[ 2 ] );
			return true;
		}

		// The joint's frame in the game world: forward (-Z of the calibrated orientation) and up.
		bool JointAxes( TrackerRole role, D3DXVECTOR3 &forward, D3DXVECTOR3 &up )
		{
			const Driven &d = s_drive[ static_cast< int >( role ) ];
			if( !d.have )
			{
				return false;
			}
			const D3DXVECTOR3 f = RotateByQuat( d.pose.jointOrientation, D3DXVECTOR3( 0.0f, 0.0f, -1.0f ) );
			const D3DXVECTOR3 u = RotateByQuat( d.pose.jointOrientation, D3DXVECTOR3( 0.0f, 1.0f, 0.0f ) );
			const float fIn[ 3 ] = { f.x, f.y, f.z }, uIn[ 3 ] = { u.x, u.y, u.z };
			float fw[ 3 ], uw[ 3 ];
			if( !VRCamera::BodyTrackingDirToWorld( fIn, fw ) || !VRCamera::BodyTrackingDirToWorld( uIn, uw ) )
			{
				return false;
			}
			forward = D3DXVECTOR3( fw[ 0 ], fw[ 1 ], fw[ 2 ] );
			up = D3DXVECTOR3( uw[ 0 ], uw[ 1 ], uw[ 2 ] );
			return true;
		}

		// Game units per real meter of the body mapping (first person: world scale; third: the character's size).
		float MappingUnits()
		{
			const float a[ 3 ] = { 0.0f, 0.0f, 0.0f }, b[ 3 ] = { 0.0f, 1.0f, 0.0f };
			float wa[ 3 ], wb[ 3 ];
			if( !VRCamera::BodyTrackingToWorld( a, wa ) || !VRCamera::BodyTrackingToWorld( b, wb ) )
			{
				return VRCamera::kGameUnitsPerMeter;
			}
			const float d = std::sqrt( ( wb[ 0 ] - wa[ 0 ] ) * ( wb[ 0 ] - wa[ 0 ] ) + ( wb[ 1 ] - wa[ 1 ] ) * ( wb[ 1 ] - wa[ 1 ] ) +
				( wb[ 2 ] - wa[ 2 ] ) * ( wb[ 2 ] - wa[ 2 ] ) );
			return d > 0.1f ? d : VRCamera::kGameUnitsPerMeter;
		}

		// The axes bone should have: its rest axes on the root, turned the way the tracker's frame
		// (model space) is turned from the root's level frame. A calibrated tracker reads the root's
		// frame when you stand straight, so standing straight gives the rest pose.
		D3DXMATRIX TrackedAxes( const float *out, int bone, const D3DXVECTOR3 &forwardModel, const D3DXVECTOR3 &upModel )
		{
			const D3DXMATRIX rootAxes = Axes( *reinterpret_cast< const D3DXMATRIX * >( out ) );
			D3DXVECTOR3 rootForward, rootUp;
			const D3DXVECTOR3 up( 0.0f, 1.0f, 0.0f );
			D3DXVec3TransformNormal( &rootForward, &kModelForward, &rootAxes );
			D3DXVec3TransformNormal( &rootUp, &up, &rootAxes );
			const D3DXMATRIX turn = FrameRotation( rootForward, rootUp, forwardModel, upModel );
			const D3DXMATRIX restAxes = Axes( s_rest[ bone ] );
			D3DXMATRIX restModel, desired;
			D3DXMatrixMultiply( &restModel, &restAxes, &rootAxes );
			D3DXMatrixMultiply( &desired, &restModel, &turn );
			return desired;
		}

		// Rotation (no pivot) taking the bone's current axes onto desired.
		D3DXMATRIX TurnTo( const float *out, int bone, const D3DXMATRIX &desired )
		{
			D3DXMATRIX currentT, turn;
			const D3DXMATRIX current = Axes( *reinterpret_cast< const D3DXMATRIX * >( out + bone * 16 ) );
			D3DXMatrixTranspose( &currentT, &current );
			D3DXMatrixMultiply( &turn, &currentT, &desired );
			return turn;
		}

		// A pole (model space) for a two-bone chain from root toward target that bends toward joint (a
		// tracked knee / elbow), blended into fallback by weight. Near a straight chain the joint says
		// little about the bend, so the fallback keeps a say there.
		D3DXVECTOR3 PoleToward( const D3DXVECTOR3 &joint, const D3DXVECTOR3 &root, const D3DXVECTOR3 &target,
			const D3DXVECTOR3 &fallback, float weight )
		{
			const D3DXVECTOR3 base = Normalized( fallback, D3DXVECTOR3( 0.0f, 0.0f, 1.0f ) );
			const D3DXVECTOR3 dir = Normalized( target - root, D3DXVECTOR3( 0.0f, -1.0f, 0.0f ) );
			const D3DXVECTOR3 toJoint = joint - root;
			const float length = D3DXVec3Length( &toJoint );
			if( length < 1.0e-3f || weight <= 0.0f )
			{
				return fallback;
			}
			const D3DXVECTOR3 across = ( toJoint - dir * D3DXVec3Dot( &toJoint, &dir ) ) / length;
			const D3DXVECTOR3 tracked = Normalized( across + base * 0.1f, base );
			return Normalized( base + ( tracked - base ) * weight, base );
		}

		const char *SourceName( TrackerRole role, char *buffer, size_t size )
		{
			const float w = DriveWeight( role );
			if( w >= 0.999f )
			{
				return "tracker";
			}
			if( w <= 0.0f )
			{
				return "estimate";
			}
			sprintf_s( buffer, size, "blend %.2f", w );
			return buffer;
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

		bool RestNeck( const float *out, const uint8_t *records, D3DXVECTOR3 &neck )
		{
			if( records != s_restRecords || !s_restValid[ kNeckBone ] )
			{
				return false;
			}
			D3DXMATRIX fitScale, fitRoot, m;
			D3DXMatrixScaling( &fitScale, s_bodyFit, s_bodyFit, s_bodyFit );
			D3DXMatrixMultiply( &fitRoot, &fitScale, reinterpret_cast< const D3DXMATRIX * >( out ) );
			D3DXMatrixMultiply( &m, &s_rest[ kNeckBone ], &fitRoot );
			neck = D3DXVECTOR3( m._41, m._42, m._43 );
			return true;
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
		// hips move away from it. Tracked roles (UpdateTrackers) take over by their weight.
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
			const auto modelPoint = [ & ]( const D3DXVECTOR3 &v ) {
				D3DXVECTOR3 r;
				D3DXVec3TransformCoord( &r, &v, &toModel );
				return r;
			};
			// Body forward (level) in the world, from the pelvis.
			// Same as the torso turn: the thigh line (left is +X) squared to the facing.
			D3DXVECTOR3 forwardModel, forward, left;
			const auto bodyFrame = [ & ]() {
				const D3DXVECTOR3 acrossModel = Position( out, kLegs[ 0 ].upper ) - Position( out, kLegs[ 1 ].upper );
				forwardModel = Normalized( D3DXVECTOR3( -acrossModel.z, 0.0f, acrossModel.x ), kModelForward );
				D3DXVECTOR3 leftWorld;
				D3DXVec3TransformNormal( &forward, &forwardModel, &world );
				D3DXVec3TransformNormal( &leftWorld, &acrossModel, &world );
				forward.y = 0.0f;
				leftWorld.y = 0.0f;
				forward = Normalized( forward, D3DXVECTOR3( 0.0f, 0.0f, 1.0f ) );
				left = Normalized( leftWorld, D3DXVECTOR3( forward.z, 0.0f, -forward.x ) );
			};
			bodyFrame();

			// Trackers: lower-body joints take their height from the floor (STAGE), scaled from your hip
			// height to the character's, so the feet meet the ground whatever your height is against
			// fpEyeHeight; across the floor they use the same mapping as the hands.
			const float groundY = ToWorld( world, Position( out, 0 ) ).y;
			const float units = MappingUnits();
			{
				const float hip = VRTrackers::CalibratedJointHeight( TrackerRole::Waist );
				const float restHip = s_restValid[ kLegs[ 0 ].shoulder ] ? s_rest[ kLegs[ 0 ].shoulder ]._42 * fit : 0.0f;
				s_legScale = hip > 0.3f && restHip > 0.0f ? std::clamp( restHip / ( hip * units ), 0.6f, 1.6f ) : 1.0f;
			}
			const float legScale = s_legScale;
			// above: world units added to the ground; calHeight: the joint's height at calibration (meters)
			// counts as "on the ground", lower is clamped to it.
			const auto lowerJoint = [ & ]( TrackerRole role, float above, float calHeight, D3DXVECTOR3 &p ) {
				if( !JointWorld( role, p ) )
				{
					return false;
				}
				const float h = s_drive[ static_cast< int >( role ) ].pose.jointFloorHeight;
				p.y = groundY + above + ( std::max )( h - calHeight, 0.0f ) * units * legScale;
				return true;
			};
			const float waistW = DriveWeight( TrackerRole::Waist );

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
				D3DXVECTOR3 shift = D3DXVECTOR3( d.x, dy, d.z ) - forward * ( crouch * kCrouchBack );
				// Waist tracker: the hips (bone 2, the legs' root) where it says.
				D3DXVECTOR3 hip;
				if( waistW > 0.0f && lowerJoint( TrackerRole::Waist, 0.0f, 0.0f, hip ) )
				{
					const D3DXVECTOR3 tracked = hip - ToWorld( world, Position( out, kLegs[ 0 ].shoulder ) );
					shift += ( tracked - shift ) * waistW;
				}
				D3DXMATRIX move;
				const D3DXVECTOR3 shiftModel = model( shift );
				D3DXMatrixTranslation( &move, shiftModel.x, shiftModel.y, shiftModel.z );
				ApplyToSubtree( out, records, kPelvisBone, move );
				// Waist tracker yaw: the hips and legs turn to it (the spine keeps its turn to the look).
				float pelvisYaw = 0.0f, pelvisW = 0.0f;
				if( TrackedPelvisYaw( pelvisYaw, pelvisW ) )
				{
					const D3DXVECTOR3 wantWorld( std::sin( pelvisYaw ), 0.0f, std::cos( pelvisYaw ) );
					const D3DXVECTOR3 want = model( wantWorld );
					const float turn = WrapPi( std::atan2( want.x, want.z ) - std::atan2( forwardModel.x, forwardModel.z ) ) * pelvisW;
					ApplyToSubtree( out, records, kLegs[ 0 ].shoulder, YawAbout( turn, Position( out, kLegs[ 0 ].shoulder ) ) );
					bodyFrame();
				}
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
				// Chest tracker: the chest (and everything above it) turns and leans as it does, up to
				// kChestLimit; the head still turns to the HMD after.
				const float chestW = DriveWeight( TrackerRole::Chest );
				D3DXVECTOR3 chestForward, chestUp;
				float chestTurn = 0.0f;
				if( chestW > 0.0f && s_restValid[ kChestBone ] && JointAxes( TrackerRole::Chest, chestForward, chestUp ) )
				{
					const D3DXMATRIX turn = TurnTo( out, kChestBone, TrackedAxes( out, kChestBone, model( chestForward ), model( chestUp ) ) );
					chestTurn = RotationAngle( turn );
					const float t = chestTurn > kChestLimit ? kChestLimit / chestTurn : 1.0f;
					ApplyToSubtree( out, records, kChestBone, PartialAbout( turn, chestW * t, Position( out, kChestBone ) ) );
				}
				if( log )
				{
#ifdef _DEBUG
					const D3DXVECTOR3 after = target - ToWorld( world, Position( out, kNeckBone ) );
					PSU_LOG( "VRBody: hips shift %.2f %.2f %.2f (crouch %.2f, waist tracker %.2f), neck error %.2f -> %.2f (after %.2f %.2f %.2f), "
						"spine %.0f %.0f %.0f deg, chest tracker %.0f deg, forward %.2f %.2f\n", shift.x, shift.y, shift.z, crouch, waistW,
						D3DXVec3Length( &d ), D3DXVec3Length( &after ), after.x, after.y, after.z, used[ 0 ] * 57.29578f, used[ 1 ] * 57.29578f,
						used[ 2 ] * 57.29578f, chestTurn * 57.29578f, forward.x, forward.z );
#endif
				}
				if( vr.m_bodyHeadFollowsHmd )
				{
					HeadToHmd( out, records, toModel, log );
				}
			}

			const TrackerRole footRole[ 2 ] = { TrackerRole::LeftFoot, TrackerRole::RightFoot };
			const TrackerRole kneeRole[ 2 ] = { TrackerRole::LeftKnee, TrackerRole::RightKnee };
			const float footW[ 2 ] = { DriveWeight( footRole[ 0 ] ), DriveWeight( footRole[ 1 ] ) };
			const float kneeW[ 2 ] = { DriveWeight( kneeRole[ 0 ] ), DriveWeight( kneeRole[ 1 ] ) };
			if( !legs && footW[ 0 ] <= 0.0f && footW[ 1 ] <= 0.0f && kneeW[ 0 ] <= 0.0f && kneeW[ 1 ] <= 0.0f )
			{
				return;
			}
			const D3DXVECTOR3 pelvis = ToWorld( world, Position( out, kPelvisBone ) );
			const float ankleHeight = s_rest[ kLegs[ 0 ].hand ]._42 * fit;
			const float side = std::fabs( s_rest[ kLegs[ 0 ].hand ]._41 ) * fit;
			const float ahead = s_rest[ kLegs[ 0 ].hand ]._43 * fit;
			const float legLength = ( s_rest[ kLegs[ 0 ].upper ]._42 - s_rest[ kLegs[ 0 ].hand ]._42 ) * fit;
			const DWORD now = GetTickCount();
			const float elapsed = s_feetTick ? static_cast< float >( now - s_feetTick ) / 1000.0f : 0.0f;
			const D3DXVECTOR3 pelvisGround( pelvis.x, groundY, pelvis.z );
			// Planted feet (vr.bodyProceduralLegs): the estimate the feet trackers blend from.
			if( legs )
			{
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
			}
			for( int i = 0; i < 2; ++i )
			{
				const Arm &leg = kLegs[ i ];
				D3DXVECTOR3 target = Position( out, leg.hand ); // without planted feet: where the rest pose has it
				if( legs )
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
					target = modelPoint( D3DXVECTOR3( p.x, groundY + ankleHeight + lift, p.z ) );
				}
				// Foot tracker: the ankle where it is, never below its standing height above the ground.
				D3DXVECTOR3 ankle;
				if( footW[ i ] > 0.0f && lowerJoint( footRole[ i ], ankleHeight, VRTrackers::CalibratedJointHeight( footRole[ i ] ), ankle ) )
				{
					target += ( modelPoint( ankle ) - target ) * footW[ i ];
				}
				D3DXVECTOR3 pole = Normalized( model( forward + left * ( i == 0 ? 0.2f : -0.2f ) ), kModelForward );
				// Knee tracker: the knee bends toward it.
				D3DXVECTOR3 knee;
				if( kneeW[ i ] > 0.0f && lowerJoint( kneeRole[ i ], 0.0f, 0.0f, knee ) )
				{
					pole = PoleToward( modelPoint( knee ), Position( out, leg.upper ), target, pole, kneeW[ i ] );
				}
				SolveArm( out, records, leg, target, pole );

				// Foot orientation: planted feet level it on the hips' turn; a foot tracker turns it as the foot is.
				const D3DXMATRIX current = Axes( *reinterpret_cast< const D3DXMATRIX * >( out + leg.hand * 16 ) );
				D3DXMATRIX desired = current;
				bool turnFoot = false;
				D3DXMATRIX restHipsInverse;
				if( legs && s_restValid[ leg.shoulder ] && D3DXMatrixInverse( &restHipsInverse, nullptr, &s_rest[ leg.shoulder ] ) )
				{
					D3DXMatrixMultiply( &desired, &s_rest[ leg.hand ], &restHipsInverse );
					D3DXMatrixMultiply( &desired, &desired, reinterpret_cast< const D3DXMATRIX * >( out + leg.shoulder * 16 ) );
					desired = Axes( desired );
					turnFoot = true;
				}
				D3DXVECTOR3 footForward, footUp;
				if( footW[ i ] > 0.0f && JointAxes( footRole[ i ], footForward, footUp ) )
				{
					const D3DXMATRIX tracked = TrackedAxes( out, leg.hand, model( footForward ), model( footUp ) );
					D3DXQUATERNION a, b, q;
					D3DXQuaternionRotationMatrix( &a, &desired );
					D3DXQuaternionRotationMatrix( &b, &tracked );
					D3DXQuaternionSlerp( &q, &a, &b, footW[ i ] );
					D3DXMatrixRotationQuaternion( &desired, &q );
					turnFoot = true;
				}
				if( turnFoot )
				{
					D3DXMATRIX currentT, turn;
					D3DXMatrixTranspose( &currentT, &current );
					D3DXMatrixMultiply( &turn, &currentT, &desired );
					// A tracker briefly upside down (Standable guessing) shouldn't fold the foot over.
					const float angle = RotationAngle( turn );
					if( angle > kFootLimit )
					{
						turn = PartialAbout( turn, kFootLimit / angle, D3DXVECTOR3( 0.0f, 0.0f, 0.0f ) );
					}
					SetPivot( turn, Position( out, leg.hand ) );
					ApplyToSubtree( out, records, leg.hand, turn );
				}
			}
			if( log )
			{
#ifdef _DEBUG
				PSU_LOG( "VRBody: feet planted %.1f,%.1f / %.1f,%.1f  stepping %d %d  pelvis speed %.1f, feet trackers %.2f %.2f knees %.2f %.2f, "
					"%.1f units/m\n", s_feet[ 0 ].current.x, s_feet[ 0 ].current.z, s_feet[ 1 ].current.x, s_feet[ 1 ].current.z,
					s_feet[ 0 ].stepping, s_feet[ 1 ].stepping, D3DXVec3Length( &s_pelvisVel ), footW[ 0 ], footW[ 1 ], kneeW[ 0 ],
					kneeW[ 1 ], units );
#endif
			}
		}

		void UpdateTrackers( float dt, bool log )
		{
			const auto &vr = Config::Get().vr;
			const bool on = vr.m_trackersEnabled && vr.m_trackerDriveBody && VRTrackers::Calibrated();
			const bool group[ kTrackerRoleCount ] = { vr.m_trackerDriveWaist, vr.m_trackerDriveChest, vr.m_trackerDriveFeet,
				vr.m_trackerDriveFeet, vr.m_trackerDriveKnees, vr.m_trackerDriveKnees, vr.m_trackerDriveElbows, vr.m_trackerDriveElbows };
					int landed = 0;
			const float step = ( std::max )( dt, 0.0f ) / kTrackerRamp;
			for( int role = 0; role < kTrackerRoleCount; ++role )
			{
				Driven &d = s_drive[ role ];
				VRTrackers::Pose pose;
				d.live = on && group[ role ] && VRTrackers::Get( static_cast< TrackerRole >( role ), pose ) && pose.jointValid;
				if( d.live )
				{
					d.pose = pose;
					d.have = true;
				}
				d.weight = std::clamp( d.weight + ( d.live ? step : -step ), 0.0f, 1.0f );
				if( d.weight <= 0.0f && !d.live )
				{
								landed |= 1 << i; // footfall haptic (vr.hapticSteps)
					d.have = false;
				}
			}
			if( log && on )
			{
				char b[ 8 ][ 16 ];
				PSU_LOG( "VRBody: sources pelvis=%s chest=%s feet=%s/%s knees=%s/%s elbows=%s/%s (legs x%.2f)\n",
					SourceName( TrackerRole::Waist, b[ 0 ], 16 ), SourceName( TrackerRole::Chest, b[ 1 ], 16 ),
					SourceName( TrackerRole::LeftFoot, b[ 2 ], 16 ), SourceName( TrackerRole::RightFoot, b[ 3 ], 16 ),
					SourceName( TrackerRole::LeftKnee, b[ 4 ], 16 ), SourceName( TrackerRole::RightKnee, b[ 5 ], 16 ),
					SourceName( TrackerRole::LeftElbow, b[ 6 ], 16 ), SourceName( TrackerRole::RightElbow, b[ 7 ], 16 ), s_legScale );
			}
		}

		bool TrackersDriving()
		{
			for( int role = 0; role < kTrackerRoleCount; ++role )
			{
				if( DriveWeight( static_cast< TrackerRole >( role ) ) > 0.0f )
				{
					return true;
				}
			}
			return false;
		}

		bool TrackedPelvisYaw( float &yaw, float &weight )
					VRGameHaptics::NoteFootPlant( landed );
		{
			weight = DriveWeight( TrackerRole::Waist );
			D3DXVECTOR3 forward, up;
			if( weight <= 0.0f || !JointAxes( TrackerRole::Waist, forward, up ) )
			{
				return false;
			}
			// Bent far forward the forward axis points at the floor: the up axis leans the same way.
			D3DXVECTOR3 level( forward.x, 0.0f, forward.z );
			if( D3DXVec3Length( &level ) < 0.3f )
			{
				level = D3DXVECTOR3( up.x, 0.0f, up.z );
			}
			if( D3DXVec3Length( &level ) < 0.1f )
			{
				return false;
			}
			yaw = std::atan2( level.x, level.z );
			return true;
		}

		D3DXVECTOR3 TrackedElbowPole( int arm, const float *out, const D3DXMATRIX &toModel, const D3DXVECTOR3 &target,
			const D3DXVECTOR3 &pole )
		{
			const TrackerRole role = arm == 0 ? TrackerRole::LeftElbow : TrackerRole::RightElbow;
			const float weight = DriveWeight( role );
			D3DXVECTOR3 elbow;
			if( weight <= 0.0f || !JointWorld( role, elbow ) )
			{
				return pole;
			}
			D3DXVECTOR3 elbowModel;
			D3DXVec3TransformCoord( &elbowModel, &elbow, &toModel );
			return PoleToward( elbowModel, Position( out, kArms[ arm & 1 ].upper ), target, pole, weight );
		}

		float BodyYawGoal( float lookYaw, const ControllerState *const pads[ 2 ] )
		{
			float head[ 3 ];
			if( !VRCamera::BodyHeadWorld( head ) )
			{
				return lookYaw;
			}
			D3DXVECTOR3 sum( 0.0f, 0.0f, 0.0f );
			for( int hand = 0; hand < 2; ++hand )
			{
				float w[ 3 ];
				if( !pads[ hand ]->gripValid || !VRCamera::BodyTrackingToWorld( pads[ hand ]->gripPosition, w ) || w[ 1 ] > head[ 1 ] )
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
