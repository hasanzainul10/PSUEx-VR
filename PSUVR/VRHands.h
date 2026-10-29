#pragma once

#include <cstdint>

namespace PSUVR
{
	// First person: the player's arms reach for the motion controllers. The loaded
	// character's bone pose (model space, one 4x4 per bone, from 0x0074B760) is edited
	// right after the game builds it: the upper arm and forearm subtrees are rotated
	// (two-bone IK) so the wrist lands on the controller grip.
	namespace VRHands
	{
		// model = the player's TDivModel, out = its bone matrices (model +0x64),
		// skeleton = its skeleton (+0x28 bone count, +0x30 bone records).
		void Apply( void *model, float *out, const uint8_t *skeleton );
		// After the local player's draw callback: measures the model's world matrix through
		// the game's bone query (which re-runs the pose build, so not from inside Apply).
		void AfterPlayerDraw( void *model );
		// The pose has the head folded away (vr.fpBody "headless"): no neck clip plane needed.
		bool HeadHidden();
		// How far the animated neck leans from rest while walking (game world units, horizontal),
		// eased; false when unknown (not first person, setting off). Moves the camera's head anchor.
		bool EyeLean( float &dx, float &dz );

		// The hand angle calibration that vr.handCalibRequest asked for (setup wizard).
		struct CalibStatus
		{
			enum Phase
			{
				Idle,      // nothing asked since the game started
				Waiting,   // asked; the first person hands haven't run yet (no player / not first person)
				Counting,  // countdown, secondsLeft until the capture
				Done,      // captured (hands = how many)
				Missed,    // the capture frame had no tracked hand
			} phase = Idle;
			float secondsLeft = 0.0f;
			int hands = 0;
		};
		CalibStatus CalibrationStatus();
	}
}
