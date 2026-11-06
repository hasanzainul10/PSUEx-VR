#pragma once

#include <cstdint>

namespace PSUVR
{
	// Haptics from game events (immersion Phase B):
	//   vr.hapticHits: the local player's attack lands (TActor hit 0x0047AD30 with our actor id as the
	//     attacker) -> Hit on the hand that attacked, HeavyHit for a Just Attack or a Photon Art.
	//   vr.hapticHurt: the local player's HP drops (status [[0x009CA8AC]+0x48] +0x20 now / +0x24 max)
	//     -> Hurt on both hands, stronger for a larger share of max HP.
	//   vr.hapticRecoil: our gun fired a shot (weapon fire 0x004CACB0 with one of the local player's
	//     weapon objects) -> Recoil on the shooting hand, scaled by weapon class; twin handguns
	//     alternate hands with the game's muzzle flag.
	//   vr.hapticTechs: a technique weapon is attacking (+0x1E4 step) and hasn't cast yet -> Charge,
	//     replayed with a rising scale; our technique spawned (0x004D3510) -> Cast.
	//   vr.hapticSteps: footfalls while the character walks / runs -> Step on both hands (from the motion
	//     loop: frame wrap + half way; IK planted feet until the loop is known), stronger when running.
	namespace VRGameHaptics
	{
		// From the 0x0047AD30 hook (game thread): a hit arrived at `target` from actor id `attacker`.
		void NoteActorHit( const void *target, uint32_t attacker, uint32_t arg2 );
		// From the 0x004CACB0 hook (game thread): `weapon` fired a shot; `alternate` is its arg2.
		void NoteWeaponFire( const void *weapon, uint32_t alternate );
		// From the 0x004D3510 hook (game thread): `weapon` spawned a technique.
		void NoteTechSpawn( const void *weapon );
		// From VRBodyIk's planted feet (vr.bodyProceduralLegs), once per foot update: `landed` bit 0 left /
		// bit 1 right = that foot finished a step this update. The footfalls come from the walk / run motion's
		// loop (vr.hapticSteps); these only stand in, rate-limited, until the loop length is known.
		void NoteFootPlant( int landed );
		// Once per frame after VRSyntheticPad::Update, before VRHaptics::Update.
		void Update();
	}
}
