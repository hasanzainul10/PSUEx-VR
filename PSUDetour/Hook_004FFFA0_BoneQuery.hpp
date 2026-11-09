/*
* TDivModel bone query (vtable 0x008C80E4 +0x18): multiplies one bone's world matrix into the
* matrix passed in (comes back transposed), re-running the pose build (0x0074B760) first.
* Used by the weapon / part attachment 0x005525A0 ("Head", "L_weapon", "R_weapon") and by
* VRHands::AfterPlayerDraw. Debug only (FBT plan phase 0): counts who asks for which of the
* local player's bones, split into inside the player's draw / inside VRHands' world query /
* anywhere else, logged every 5 s; dumps the player model's bone name table once per model.
* A gameplay caller ("other") that reads a hand or weapon bone would see the VR pose.
* Two generic helpers call it for most users, so for them the helper's caller is logged too
* (via=): 0x00500040 (bone by name; ESI model, EDI name, RET 0x10) and 0x00553080 (bone of
* render object model slot +0x28; pushes EDI first).
*/

#pragma once

#include "../PSUEx.h"
#include "../Misc/Log.h"
#include "../PSUMemory/Memory.hpp"
#include "../PSUVR/VRHands.h"
#include "../PSUVR/VRPlayerDraw.h"

#include <intrin.h>

namespace sub_4FFFA0
{
	typedef char( __thiscall *function_t )( void *model, float *matrix, int bone, int a, int b, int c );
	function_t pOriginal = reinterpret_cast< function_t >( 0x004FFFA0 );

	// Model vtable +0x28 -> object, +0x28 -> { int mode (0 linear / 1 sorted), int count,
	// entries* = { int bone, char *name } } (looked up by 0x00757500).
	typedef void *( __thiscall *NameOwnerFn )( void *model );

	enum Context { kDraw, kWorld, kOther, kContextCount };
	const char *const kContextNames[ kContextCount ] = { "draw", "world", "other" };

	struct Caller
	{
		uintptr_t ret;
		uintptr_t via;
		int bone;
		int context;
		unsigned count;
	};

	constexpr unsigned kMaxCallers = 48;
	Caller s_callers[ kMaxCallers ]{};
	unsigned s_callerCount = 0;
	unsigned s_dropped = 0;
	unsigned s_otherModels = 0;
	DWORD s_lastLog = 0;
	const void *s_dumpedModel = nullptr;

	bool Readable( const void *p, size_t bytes )
	{
		return p && PSUMemory::IsValidPointer( p ) &&
			PSUMemory::IsValidPointer( static_cast< const uint8_t * >( p ) + bytes - 1 );
	}

	// RTTI class name from the object's vtable (vtable[-1] = complete object locator).
	const char *ClassName( const void *object )
	{
		const uint8_t *const *vt = *static_cast< const uint8_t *const *const * >( object );
		const uint8_t *col = reinterpret_cast< const uint8_t * const * >( vt )[ -1 ];
		const uint8_t *typeDescriptor = *reinterpret_cast< const uint8_t * const * >( col + 0x0C );
		return reinterpret_cast< const char * >( typeDescriptor + 8 );
	}

	void DumpNames( void *model )
	{
		__try
		{
			void *const *vt = *static_cast< void *const *const * >( model );
			const uint8_t *owner = static_cast< const uint8_t * >( reinterpret_cast< NameOwnerFn >( vt[ 10 ] )( model ) );
			const int *table = Readable( owner, 0x2C ) ? *reinterpret_cast< const int * const * >( owner + 0x28 ) : nullptr;
			if( !Readable( table, 12 ) )
			{
				PSU_LOG( "VRPoseQuery: model %p (%s) has no bone name table (owner %p)\n", model, ClassName( model ), owner );
				return;
			}
			const int mode = table[ 0 ];
			const int count = table[ 1 ];
			const int *entries = reinterpret_cast< const int * >( table[ 2 ] );
			PSU_LOG( "VRPoseQuery: model %p (%s) bone names mode %d count %d\n", model, ClassName( model ), mode, count );
			if( count <= 0 || count > 256 || !Readable( entries, count * 8 ) )
			{
				return;
			}
			char line[ 512 ];
			int len = 0;
			for( int i = 0; i < count; ++i )
			{
				const char *name = reinterpret_cast< const char * >( entries[ i * 2 + 1 ] );
				len += sprintf_s( line + len, sizeof( line ) - len, " %d=%.24s", entries[ i * 2 ],
					Readable( name, 1 ) ? name : "?" );
				if( len > 400 || i + 1 == count )
				{
					PSU_LOG( "VRPoseQuery: names%s\n", line );
					len = 0;
				}
			}
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			PSU_LOG( "VRPoseQuery: bone name dump faulted (model %p)\n", model );
		}
	}

	// Helper return addresses -> where the helper's own return address sits above ours
	// (ours, 5 args, then the helper's pushes).
	constexpr uintptr_t kByNameRet = 0x00500081;     // 0x00500040: nothing pushed
	constexpr uintptr_t kModelSlotRet = 0x005530D5;  // 0x00553080: PUSH EDI

	uintptr_t HelperCaller( uintptr_t ret, const uintptr_t *retSlot )
	{
		const int slot = ret == kByNameRet ? 6 : ret == kModelSlotRet ? 7 : 0;
		if( !slot || !Readable( retSlot + slot, 4 ) )
		{
			return 0;
		}
		return retSlot[ slot ];
	}

	void Note( void *model, int bone, uintptr_t ret, uintptr_t via )
	{
		const void *player = PSUVR::VRPlayerDraw::PlayerModel();
		if( !player || model != player )
		{
			++s_otherModels;
		}
		else
		{
			if( model != s_dumpedModel )
			{
				s_dumpedModel = model;
				DumpNames( model );
			}
			const int context = PSUVR::VRHands::InWorldQuery() ? kWorld
				: PSUVR::VRPlayerDraw::DrawingLocalPlayer() ? kDraw : kOther;
			unsigned i = 0;
			while( i < s_callerCount && ( s_callers[ i ].ret != ret || s_callers[ i ].via != via || s_callers[ i ].bone != bone ||
				s_callers[ i ].context != context ) )
			{
				++i;
			}
			if( i < s_callerCount )
			{
				++s_callers[ i ].count;
			}
			else if( s_callerCount < kMaxCallers )
			{
				s_callers[ s_callerCount++ ] = { ret, via, bone, context, 1 };
			}
			else
			{
				++s_dropped;
			}
		}

		if( GetTickCount() - s_lastLog < 5000 )
		{
			return;
		}
		s_lastLog = GetTickCount();
		if( s_callerCount == 0 )
		{
			s_otherModels = 0;
			return;
		}
		PSU_LOG( "VRPoseQuery: %u callers, dropped %u, other models %u\n", s_callerCount, s_dropped, s_otherModels );
		// PSULog formats into 1024 bytes: several short lines.
		char line[ 960 ];
		int len = 0;
		for( unsigned c = 0; c < s_callerCount; ++c )
		{
			len += sprintf_s( line + len, sizeof( line ) - len, " ret=%08X", static_cast< unsigned >( s_callers[ c ].ret ) );
			if( s_callers[ c ].via )
			{
				len += sprintf_s( line + len, sizeof( line ) - len, "<%08X", static_cast< unsigned >( s_callers[ c ].via ) );
			}
			len += sprintf_s( line + len, sizeof( line ) - len, " bone=%d %s x%u", s_callers[ c ].bone,
				kContextNames[ s_callers[ c ].context ], s_callers[ c ].count );
			if( len > 800 || c + 1 == s_callerCount )
			{
				PSU_LOG( "VRPoseQuery: callers%s\n", line );
				len = 0;
			}
		}
		s_callerCount = 0;
		s_dropped = 0;
		s_otherModels = 0;
	}

	char __fastcall Hook_4FFFA0( void *model, void * /*edx*/, float *matrix, int bone, int a, int b, int c )
	{
		const uintptr_t *retSlot = static_cast< const uintptr_t * >( _AddressOfReturnAddress() );
		const uintptr_t ret = *retSlot;
		Note( model, bone, ret, HelperCaller( ret, retSlot ) );
		return pOriginal( model, matrix, bone, a, b, c );
	}

	void Hook()
	{
		DetourAttach( &( PVOID & )pOriginal, reinterpret_cast< PVOID >( Hook_4FFFA0 ) );
	}
}
