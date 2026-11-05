#pragma once

#include <cstdint>

namespace PSUVR
{
	// Haptics from game events (immersion Phase B):
	//   vr.hapticHits: the local player's attack lands (TActor hit 0x0047AD30 with our actor id as the
	//     attacker) -> Hit on the hand that attacked, HeavyHit for a Just Attack or a Photon Art.
	//   vr.hapticHurt: the local player's HP drops (status [[0x009CA8AC]+0x48] +0x20 now / +0x24 max)
	//     -> Hurt on both hands, stronger for a larger share of max HP.
	namespace VRGameHaptics
	{
		// From the 0x0047AD30 hook (game thread): a hit arrived at `target` from actor id `attacker`.
		void NoteActorHit( const void *target, uint32_t attacker, uint32_t arg2 );
		// Once per frame after VRSyntheticPad::Update, before VRHaptics::Update.
		void Update();
	}
}
