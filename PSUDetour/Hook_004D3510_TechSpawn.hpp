/*
* Technique spawn: creates the technique object for cast slot arg1 (0..3) of the technique
* weapon in EDI (TItemTechStraight / Chase / Shoot / ... by the technique's delivery type) and
* returns 1 when it spawned. __cdecl with 4 stack args plus EDI. Called only from TItemWpnTech
* vtable +0x12C (0x004DDE80) and +0x130 (0x004DDE30), slot = button - 2 + hand * 2.
* VR: cast haptic for the local player's techniques (VRGameHaptics). Read only: the original
* runs unchanged first, and EDI / the return value are kept.
*/

#pragma once

#include "../PSUVR/VRGameHaptics.h"

#include <windows.h>

namespace sub_4D3510
{
	PVOID pOriginal = reinterpret_cast< PVOID >( 0x004D3510 );

	void __cdecl NoteSpawn( const void *weapon, int spawned )
	{
		if( spawned & 0xFF )
		{
			PSUVR::VRGameHaptics::NoteTechSpawn( weapon );
		}
	}

	__declspec( naked ) void Hook_4D3510()
	{
		__asm
		{
			// Copy the 4 stack args (each push moves ESP by 4, so [esp + 16] walks arg4 .. arg1).
			push dword ptr [ esp + 16 ]
			push dword ptr [ esp + 16 ]
			push dword ptr [ esp + 16 ]
			push dword ptr [ esp + 16 ]
			call dword ptr [ pOriginal ]
			add esp, 16
			push eax
			push eax
			push edi
			call NoteSpawn
			add esp, 8
			pop eax
			ret
		}
	}

	void Hook()
	{
		DetourAttach( &pOriginal, reinterpret_cast< PVOID >( Hook_4D3510 ) );
	}
}
