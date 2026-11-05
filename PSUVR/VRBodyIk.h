#pragma once

#include "VRSkeletonMath.h"

#include <cstdint>

namespace PSUVR
{
	struct ControllerState;

	// The player's body as a skeleton for VR (FBT_PLAN.md): the rest pose that replaces the game's
	// animation, and the 3-point IK (head + hands) on top of it. VRHands calls these on the pose the
	// game just built (model space, Skeleton:: conventions).
	namespace VRBodyIk
	{
		// The 28-bone map (kBoneParents) matches the model's records; logs "VRBody: bone map ok|mismatch"
		// once per record array.
		bool BoneMapOk( const uint8_t *records );
		// vr.bodyRestPose: every bone not kept by vr.bodyAnimKeep to its bind pose on the game's root,
		// scaled by the body fit. skeleton = +0x28 bone count, +0x30 bone records.
		void RestPose( float *out, const uint8_t *skeleton );
		// The body fit scale RestPose used last (1 = off).
		float BodyFit();
		// The neck of the rest pose RestPose last built for these records, on the current root (model
		// space, body fit included). False without a rest pose for them.
		bool RestNeck( const float *out, const uint8_t *records, D3DXVECTOR3 &neck );

		// Body IK on the rest pose (first person, or third person through VRCamera's Body* mapping). upper (headWorld set): hips follow the head (pushed back
		// by a crouch), the CCD spine leans the neck to it, the head turns to the HMD. legs: planted feet
		// that step. Tracked roles (UpdateTrackers) take over by their weight: waist -> hips position + leg
		// yaw, chest -> chest turn, feet -> ankle targets + foot turn, knees -> leg poles.
		// world = model -> game world, toModel its inverse.
		void Solve( float *out, const uint8_t *records, const D3DXMATRIX &world, const D3DXMATRIX &toModel,
			const float *headWorld, bool upper, bool legs, float dt, bool log );

		// vr.trackerDriveBody: each calibrated tracker role eases in (0.3 s) while usable and out after a
		// loss, its last pose kept for the ease-out. Once per Apply, after VRCamera's body mapping is set
		// (third person). Logs "VRBody: sources pelvis=tracker|estimate ..." with log.
		void UpdateTrackers( float dt, bool log );
		// Some tracker role has weight: Solve has something to do even without bodyIk / planted feet.
		bool TrackersDriving();
		// The waist tracker's facing (game yaw) and its weight; false while it has none.
		bool TrackedPelvisYaw( float &yaw, float &weight );
		// The arm's elbow tracker (arm 0 left, 1 right) blended into pole (model space; target = the wrist
		// the arm reaches for). pole unchanged without one.
		D3DXVECTOR3 TrackedElbowPole( int arm, const float *out, const D3DXMATRIX &toModel, const D3DXVECTOR3 &target,
			const D3DXVECTOR3 &pole );

		// Where the body should face (game yaw): the look yaw pulled toward the hands when both are in
		// front, below the head. Returns lookYaw when the hands don't say anything.
		float BodyYawGoal( float lookYaw, const ControllerState *const pads[ 2 ] );
		// Yaw twist of the upper body by angle, split over the waist, chest and shoulder level.
		void TwistSpine( float *out, const uint8_t *records, float angle );
		// vr.bodyShoulderShrug: the shoulder (clavicle) turns toward a target that is far or high.
		void Shrug( float *out, const uint8_t *records, const Skeleton::Arm &arm, const D3DXVECTOR3 &target );
		// vr.bodyElbowFromHand: pole (model space) blended toward the elbow the controller implies
		// (forearmBack = the grip's +Z, model space, pointing from the wrist toward the elbow).
		D3DXVECTOR3 ElbowPole( const float *out, const Skeleton::Arm &arm, const D3DXVECTOR3 &target, const D3DXVECTOR3 &pole,
			const D3DXVECTOR3 &forearmBack );
	}
}
