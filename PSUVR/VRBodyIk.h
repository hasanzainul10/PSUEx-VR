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

		// Body IK on the rest pose, first person. upper (headWorld set): hips follow the head (pushed back
		// by a crouch), the CCD spine leans the neck to it, the head turns to the HMD. legs: planted feet
		// that step. world = model -> game world, toModel its inverse.
		void Solve( float *out, const uint8_t *records, const D3DXMATRIX &world, const D3DXMATRIX &toModel,
			const float *headWorld, bool upper, bool legs, float dt, bool log );

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
