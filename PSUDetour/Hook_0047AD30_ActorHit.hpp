/*
* TActor vtable +0x30 (the base implementation, shared by ~100 actor classes: enemies, objects,
* TPlayer): a hit arrives at this actor. arg1 = the attacker's actor id. When it is the local
* player's id (0x009CA7D4) the game plays the hit reaction cue and adds the hit to the
* TActorDamageManager report (0x0047BB90 -> 0x0047BD00).
* VR: counts the local player's landed hits for the hit haptic (VRGameHaptics). Read only: the
* original runs unchanged first.
*/

#pragma once

#include "../PSUVR/VRGameHaptics.h"

#include <cstdint>

namespace sub_47AD30
{
	typedef int( __thiscall *function_t )( void *target, uint32_t attacker, uint32_t arg2, void *arg3, uint32_t arg4,
		uint32_t arg5, uint32_t arg6 );
	function_t pOriginal = reinterpret_cast< function_t >( 0x0047AD30 );

	int __fastcall Hook_47AD30( void *target, void * /*edx*/, uint32_t attacker, uint32_t arg2, void *arg3, uint32_t arg4,
		uint32_t arg5, uint32_t arg6 )
	{
		const int result = pOriginal( target, attacker, arg2, arg3, arg4, arg5, arg6 );
		PSUVR::VRGameHaptics::NoteActorHit( target, attacker, arg2 );
		return result;
	}

	void Hook()
	{
		DetourAttach( &( PVOID & )pOriginal, reinterpret_cast< PVOID >( Hook_47AD30 ) );
	}
}
