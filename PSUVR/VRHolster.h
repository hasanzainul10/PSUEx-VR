#pragma once

namespace PSUVR
{
	// Holster gestures (vr.holsterEnabled): zones around the player's body in the HMD's yaw frame.
	// A grip press with the hand inside a zone is the zone's action (VRSyntheticPad does the
	// dispatch): back of the head = weapon up, back of the waist = weapon down, mouth = use item.
	namespace VRHolster
	{
		enum class Zone { None, Head, Waist, Mouth };

		// A zone is an ellipsoid: centre (tracking / LOCAL space, meters), three unit axes (right, up,
		// forward; the mouth's follow the head) and half extents along them, for the next views, scaled
		// by the head height above the floor and the zone's radius setting (as a multiplier of its default).
		// False without views.
		bool ZoneShape( Zone zone, float center[ 3 ], float axes[ 3 ][ 3 ], float half[ 3 ] );
		// The zone the hand's grip is in (smallest relative distance wins), None outside all.
		Zone ZoneAtHand( int hand );
		const char *Name( Zone zone );
	}
}
