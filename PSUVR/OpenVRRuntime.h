#pragma once

#include "VRRuntime.h"

namespace PSUVR
{
	// Pose-only OpenVR side connection for body trackers (FBT): SteamVR's OpenXR tracker extension
	// doesn't list every tracker (Standable), OpenVR does. Runs next to the OpenXR session in the
	// same process, through SteamVR's own vrclient.dll (already loaded by the OpenXR runtime), as a
	// Background app. Poses are in OpenVR's STANDING universe (floor origin, meters); the caller
	// converts them to its own space.
	namespace OpenVRTrackerSource
	{
		// Connects once (later calls return the first result). Logs what happened.
		bool Connect();
		void Disconnect();
		bool Connected();

		// Once per frame: device scan every 2 s (roles), poses predicted extraSeconds past the
		// next vsync-to-photons time.
		void Poll( float extraSeconds );

		// Tracker by role in STANDING space; floorHeight = standing y. False without a device.
		bool Get( TrackerRole role, TrackerState &out );
		// HMD pose in STANDING space (for aligning to another space).
		bool GetHmd( float position[ 3 ], float orientation[ 4 ] );
		// Number of generic trackers seen in the last scan (with or without a role).
		int TrackerCount();
	}
}
