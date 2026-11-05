#pragma once

#include "VRRuntime.h"

#include <string>

namespace PSUVR
{
	// Body trackers (FBT Phase 3): per-role poses from the runtime, smoothed (one-euro), held for
	// vr.trackerLostHold after a loss. Read only; nothing drives the body from them yet.
	namespace VRTrackers
	{
		struct Pose
		{
			bool valid = false;       // usable: tracked now, or held after a short loss
			bool held = false;        // the last good pose, the tracker is lost right now
			bool floorValid = false;
			float position[ 3 ]{};    // tracking space (LOCAL, meters), smoothed
			float orientation[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };
			float floorHeight = 0.0f; // meters above the floor (STAGE space)
			float lostSeconds = 0.0f; // how long the tracker has been lost (0 while tracked)
		};

		// Once per frame after the runtime's input update (next to VRControllers::Update).
		void Update( IVRRuntime *runtime );

		bool Get( TrackerRole role, Pose &out );
		// Game world position of a tracker (VRCamera::BodyTrackingToWorld, so third person too).
		bool GetWorld( TrackerRole role, float world[ 3 ] );
		// Any role usable right now.
		bool Any();

		const char *RoleName( TrackerRole role );
		// "waist ok, left foot lost, ..." for the settings panel (empty with trackers off).
		std::string StatusLine();
	}
}
