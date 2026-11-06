/*
* Weapon fire (TItemWeapon, one shot): spawns the shot's bullets by the weapon's bullet type
* (shotgun pellets, multi-bullet shots: all in one call) and returns true when they spawned.
* Called by 0x004FA400 (fire N shots; arg2 is flipped between shots, the twin handguns' muzzle)
* and by TItemWpnSubGun vtable +0x130 (0x004DD250). The weapon's owner actor id is at +0x24.
* VR: recoil haptic for the local player's shots (VRGameHaptics). Read only: the original runs
* unchanged first.
*/

#pragma once

#include "../PSUVR/VRGameHaptics.h"

#include <cstdint>

namespace sub_4CACB0
{
	typedef bool( __cdecl *function_t )( void *weapon, uint32_t alternate );
	function_t pOriginal = reinterpret_cast< function_t >( 0x004CACB0 );

	bool __cdecl Hook_4CACB0( void *weapon, uint32_t alternate )
	{
		const bool fired = pOriginal( weapon, alternate );
		if( fired )
		{
			PSUVR::VRGameHaptics::NoteWeaponFire( weapon, alternate );
		}
		return fired;
	}

	void Hook()
	{
		DetourAttach( &( PVOID & )pOriginal, reinterpret_cast< PVOID >( Hook_4CACB0 ) );
	}
}
