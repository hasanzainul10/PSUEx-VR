#pragma once

#include "../PSUMemory/Memory.hpp"

#include <cstdint>
#include <cstring>

// Read-only view of the local player's melee combo state (offsets from probe runs,
// 2026-10-15; see docs/ADDRESS_MAP.md "Local player struct fields").
namespace PSUCombat
{
	// What a hand holds, from the weapon class (+0x1F0 right, +0x1F4 left). The high digit is
	// 0 = both hands (two-handed or twin), 1 = right hand, 2 = left hand. Seen so far:
	// 0x02 two-handed sword, 0x04 twin knuckles, 0x06 spear, 0x0C twin daggers, 0x0E twin sabers,
	// 0x12 saber, 0x14 dagger, 0x1C sniper rifle, 0x1D shotgun, 0x1E bow, 0x21 twin handguns (two-handed guns despite
	// the high digit), 0x22 handgun, 0x25 machinegun, 0x26 rod, 0x28 staff / TCSM / RCSM (the category
	// below tells those apart).
	// Gun1H: left-hand gun (fired with R1 held, which makes it the active weapon). Gun2H:
	// two-handed gun held in both hands (the active weapon, fired with the attack button).
	// Auto: fires by itself (RCSM), nothing for the hand to do.
	enum class WeaponKind { None, Melee1H, Melee2H, Twin, Gun1H, Gun2H, Auto, Unknown };

	inline WeaponKind ClassifyWeapon( uint32_t weaponClass )
	{
		if( weaponClass == 0xFFFFFFFFu )
		{
			return WeaponKind::None;
		}
		switch( weaponClass )
		{
			case 0x04:
			case 0x0C:
			case 0x0E: return WeaponKind::Twin;
			case 0x06: return WeaponKind::Melee2H;
			case 0x12: return WeaponKind::Melee1H;
			case 0x1C:
			case 0x1D:
			case 0x1E:
			case 0x21: return WeaponKind::Gun2H;
			case 0x22: return WeaponKind::Gun1H;
			case 0x28: return WeaponKind::Auto;
			default: break;
		}
		switch( weaponClass >> 4 )
		{
			case 0: return WeaponKind::Melee2H;  // rifles / rods also land here until mapped
			case 1: return WeaponKind::Melee1H;
			case 2: return WeaponKind::Gun1H;
			default: return WeaponKind::Unknown;
		}
	}

	// Guns the game's free aim (R3) works with. The machinegun and the shotgun have none; the twin
	// handguns (0x21) are untested.
	inline bool FreeAimCapable( uint32_t weaponClass )
	{
		const WeaponKind kind = ClassifyWeapon( weaponClass );
		return ( kind == WeaponKind::Gun1H || kind == WeaponKind::Gun2H ) && weaponClass != 0x25 && weaponClass != 0x1D &&
			weaponClass != 0x21;
	}

	// The hand a two-handed gun is held (and so aimed and fired) with: the bow sits in the left
	// hand (its arrows start at the left weapon bone), the others in the right.
	inline int GunHand( uint32_t weaponClass )
	{
		return weaponClass == 0x1E ? 0 : 1;
	}

	inline const char *WeaponKindName( WeaponKind kind )
	{
		switch( kind )
		{
			case WeaponKind::None: return "none";
			case WeaponKind::Melee1H: return "melee 1H";
			case WeaponKind::Melee2H: return "melee 2H";
			case WeaponKind::Twin: return "twin";
			case WeaponKind::Gun1H: return "gun 1H";
			case WeaponKind::Gun2H: return "gun 2H";
			case WeaponKind::Auto: return "auto";
			default: return "unknown";
		}
	}

	struct State
	{
		uint32_t attackState = 0;  // +0x710
		int comboStep = 0;         // 1..3 while attacking, 0 idle (+0x1E4 low digit)
		bool photonArt = false;    // the chain is a Photon Art (+0x1E4 0x2n)
		bool earlyPress = false;   // a press landed before the window (+0x7B4)
		bool hitObject = false;    // the swing's hit object exists (+0x00C / +0x010)
		uint32_t weaponClass = 0;  // +0x1F0 right hand (0xFFFFFFFF = none)
		uint32_t leftClass = 0;    // +0x1F4 left hand
		uint32_t activeClass = 0;  // +0x1DC >> 8: the weapon being used (switches to the left one on R1)
		// Weapon category (+0x2D4 right, +0x2D8 left; 0 = none): 1 two-handed melee / twin,
		// 2 two-handed gun, 3 rod, 7 one-handed melee, 8 staff, 9 handgun, A machinegun, B TCSM, C RCSM.
		uint32_t rightCategory = 0;
		uint32_t leftCategory = 0;
		uint16_t facing = 0;       // +0x124, 0x10000 = 360 degrees
		float position[ 3 ]{};     // +0x114, game world
		int16_t aimPitch = 0;      // +0x220, free aim pitch (same units, positive = DOWN); shots fly at -aimPitch
		bool paletteOpen = false;  // weapon / item palette shown (+0x700 top byte: 7 open, counts down closing)
		// Just Attack window of the current motion, in motion frames (ReadMotion; see ADDRESS_MAP
		// 0x005366E0). motionValid is false when the table or the animation can't be read.
		bool motionValid = false;
		int motionClass = -1;      // +0x1DD: active weapon class, picks the weapon type's motion table
		int motionId = -1;         // +0x170: current motion (0x14.. while attacking)
		float motionFrame = 0.0f;  // current animation frame (0x005532F0)
		float jaStart = -1.0f;     // entry +0x08 (-1 = none)
		float jaEnd = -1.0f;       // entry +0x0C
		uintptr_t motionEntry = 0; // the motion's timing entry (Debug logs)
	};

	namespace detail
	{
		inline bool Readable( uintptr_t address )
		{
			return address >= 0x10000 && PSUMemory::IsValidPointer( reinterpret_cast< const void * >( address ) );
		}

		inline uint32_t Dword( uintptr_t address )
		{
			return *reinterpret_cast< const uint32_t * >( address );
		}
	}

	// The current motion's timing entry from the weapon type's motion table, the way 0x005366E0
	// looks it up: [0x00B9DAA4] +0x40 + class * 4 -> [ptr] + motion * 0x40 -> +0x38. Entry floats
	// (frames, -1 = unused): +0x08 / +0x0C the window in which the game accepts the next attack
	// or Photon Art (0x0053E2D0, allowed-input mask) = the Just Attack window; +0x10 and the pairs
	// at +0x1C..+0x38 hit frames. The frame comes from the player's animation as 0x005532F0
	// reads it (render object +0x160).
	inline void ReadMotion( uintptr_t player, State &out )
	{
		using detail::Dword;
		using detail::Readable;
		out.motionClass = *reinterpret_cast< const int8_t * >( player + 0x1DD );
		out.motionId = static_cast< int >( Dword( player + 0x170 ) );

		const uintptr_t render = Dword( player + 0x160 );
		if( !Readable( render ) || !Readable( render + 0x28 ) )
		{
			return;
		}
		const uint32_t mode = Dword( render + 0x28 );
		uintptr_t holder = 0;
		if( mode == 0 )
		{
			const uintptr_t model = Dword( render + 0x04 );
			holder = Readable( model ) ? Dword( model + 0x84 ) : 0;
		}
		else if( mode <= 4 )
		{
			const uintptr_t blend = Dword( render + 0x20 );
			holder = Readable( blend ) ? Dword( blend + 0xA8 ) : 0;
		}
		const uintptr_t unit = Readable( holder ) ? Dword( holder ) : 0;
		if( !Readable( unit ) || !Readable( unit + 0x2C ) )
		{
			return;
		}
		out.motionFrame = *reinterpret_cast< const float * >( unit + 0x2C );

		const uintptr_t tables = PSUMemory::ReadAbsoluteMemory< uint32_t >( 0x00B9DAA4 );
		if( !Readable( tables ) || !Dword( tables + 4 ) || out.motionClass < 0 || out.motionClass > 0x3D ||
			out.motionId < 0 || out.motionId > 0x400 )
		{
			return;
		}
		const uintptr_t slot = Dword( tables + 0x40 + out.motionClass * 4 );
		const uintptr_t base = Readable( slot ) ? Dword( slot ) : 0;
		if( !Readable( base ) || !Readable( base + out.motionId * 0x40 + 0x38 ) )
		{
			return;
		}
		const uintptr_t entry = Dword( base + out.motionId * 0x40 + 0x38 );
		if( !Readable( entry ) || !Readable( entry + 0x3C ) )
		{
			return;
		}
		out.motionEntry = entry;
		out.jaStart = *reinterpret_cast< const float * >( entry + 0x08 );
		out.jaEnd = *reinterpret_cast< const float * >( entry + 0x0C );
		out.motionValid = true;
	}

	// False when there is no local player (login, area change).
	inline bool Read( State &out )
	{
		out = {};
		const uintptr_t table = 0x009CA8AC;
		if( !PSUMemory::IsValidPointer( reinterpret_cast< const void * >( table ) ) )
		{
			return false;
		}
		const uintptr_t player = *reinterpret_cast< const uintptr_t * >( table );
		if( !player || !PSUMemory::IsValidPointer( reinterpret_cast< const void * >( player ) ) ||
			!PSUMemory::IsValidPointer( reinterpret_cast< const void * >( player + 0x800 ) ) )
		{
			return false;
		}
		const auto dword = [ player ]( uintptr_t offset ) { return *reinterpret_cast< const uint32_t * >( player + offset ); };
		out.attackState = dword( 0x710 );
		const uint32_t step = dword( 0x1E4 );
		// 0x11..0x13 normal attack chain, 0x21..0x23 Photon Art chain.
		const uint32_t chain = step >> 4;
		const uint32_t hit = step & 0xF;
		out.comboStep = ( chain == 1 || chain == 2 ) && hit >= 1 && hit <= 3 ? static_cast< int >( hit ) : 0;
		out.photonArt = chain == 2 && out.comboStep > 0;
		out.earlyPress = ( dword( 0x7B4 ) & 0x01000000 ) != 0;
		out.hitObject = dword( 0x010 ) != 0;
		out.weaponClass = dword( 0x1F0 );
		out.leftClass = dword( 0x1F4 );
		out.activeClass = ( dword( 0x1DC ) >> 8 ) & 0xFF;  // upper half can be 0xFFFF
		out.rightCategory = dword( 0x2D4 ) & 0xFF;
		out.leftCategory = dword( 0x2D8 ) & 0xFF;
		out.facing = static_cast< uint16_t >( dword( 0x124 ) & 0xFFFF );
		for( int i = 0; i < 3; ++i )
		{
			out.position[ i ] = *reinterpret_cast< const float * >( player + 0x114 + i * 4 );
		}
		out.aimPitch = *reinterpret_cast< const int16_t * >( player + 0x220 );
		out.paletteOpen = ( dword( 0x700 ) & 0xFF000000u ) != 0;
		ReadMotion( player, out );
		return true;
	}

	// The current motion has a Just Attack window (both ends set). False: fall back to ComboWindowOpen.
	inline bool HasJustAttackWindow( const State &state )
	{
		return state.motionValid && state.jaStart >= 0.0f && state.jaEnd >= state.jaStart;
	}

	// Mid-combo and the animation is inside the motion's Just Attack window (the same frame range
	// 0x0053E2D0 allows the next attack in).
	inline bool JustAttackWindowOpen( const State &state )
	{
		return state.comboStep > 0 && HasJustAttackWindow( state ) && state.motionFrame >= state.jaStart &&
			state.motionFrame <= state.jaEnd;
	}

	// Mid-combo and the next press would be accepted (+0x710 == 0x0E; idle is 0x0080030E).
	inline bool ComboWindowOpen( const State &state )
	{
		return state.comboStep > 0 && ( state.attackState & 0xFFFF ) == 0x000E;
	}

	// Swinging: presses now are early (+0x710 == 0x02).
	inline bool Swinging( const State &state )
	{
		return state.comboStep > 0 && ( state.attackState & 0xFFFF ) == 0x0002;
	}

	// Techniques on the equipped technique weapons, by cast slot: 0 = X, 1 = Y (right weapon),
	// 2 = R1 + X, 3 = R1 + Y (rod slots 3 / 4, or the left-hand TCSM). The weapon object
	// (player +0x2E0 right, +0x2E4 left) keeps 4 slots at +0x48 + slot * 2: byte 0 technique id
	// (0 = empty), byte 1 level. TItemWpnTech (vtable 0x008CC4EC) picks the slot as
	// (button - 2) + hand * 2 (0x004DE400 / 0x004DE7C0). The id matches a technique disk as
	// group (id - 1) / 10 + 1, entry (id - 1) % 10 (0x004DE4D0).
	struct TechSlots
	{
		uint8_t id[ 4 ]{};
		uint8_t level[ 4 ]{};
		uint8_t raw[ 2 ][ 8 ]{};    // +0x48..+0x4F of the right / left weapon (Debug logs)
		uint32_t vtable[ 2 ]{};     // right / left weapon object vtables
	};

	// The action palette's TWindowCP (docs/ADDRESS_MAP.md, "Item palette"): [0x00B9DBF0] is the
	// TWindowPlayerCtrl, +0x4C the TWindowActionPalette, +0x90 its TWindowCP. In the CP window
	// +0xD0 is the row (0 weapons, 1 items), +0xC8 the item cursor, +0xDC the item count and
	// +0x134 + i * 4 the packed item ids (byte 0 = category, 0 = empty slot; byte 1 / 2 =
	// kind / index). Layout read from the palette code, check the Debug log on first use.
	struct ItemPalette
	{
		int state = -1;            // TWindowActionPalette +0xA8: 0 closed, 1 open while Cancel is held (0x00578020)
		bool itemRow = false;      // the item row is the one selected
		int cursor = 0;
		int count = 0;
		uint32_t entry[ 7 ]{};
	};

	inline bool ReadItemPalette( ItemPalette &out )
	{
		using detail::Dword;
		using detail::Readable;
		out = {};
		if( !Readable( 0x00B9DBF0 ) )
		{
			return false;
		}
		const uintptr_t control = Dword( 0x00B9DBF0 );
		if( !Readable( control ) || !Readable( control + 0x50 ) )
		{
			return false;
		}
		const uintptr_t palette = Dword( control + 0x4C );
		if( !Readable( palette ) || !Readable( palette + 0xAC ) )
		{
			return false;
		}
		const uintptr_t window = Dword( palette + 0x90 );
		if( !Readable( window ) || !Readable( window + 0x14C ) )
		{
			return false;
		}
		const uint32_t row = Dword( window + 0xD0 );
		const uint32_t cursor = Dword( window + 0xC8 );
		const uint32_t count = Dword( window + 0xDC );
		if( row > 1 || count < 1 || count > 7 || cursor >= count )
		{
			return false;
		}
		out.state = static_cast< int >( Dword( palette + 0xA8 ) );
		out.itemRow = row == 1;
		out.cursor = static_cast< int >( cursor );
		out.count = static_cast< int >( count );
		for( uint32_t i = 0; i < count; ++i )
		{
			out.entry[ i ] = Dword( window + 0x134 + i * 4 );
		}
		return true;
	}

	inline bool ReadTechSlots( const State &state, TechSlots &out )
	{
		using detail::Dword;
		using detail::Readable;
		out = {};
		if( !Readable( 0x009CA8AC ) )
		{
			return false;
		}
		const uintptr_t player = Dword( 0x009CA8AC );
		if( !Readable( player ) || !Readable( player + 0x2E8 ) )
		{
			return false;
		}
		bool any = false;
		for( int hand = 0; hand < 2; ++hand )
		{
			const uintptr_t weapon = Dword( player + 0x2E0 + hand * 4 );
			if( !Readable( weapon ) || !Readable( weapon + 0x50 ) )
			{
				continue;
			}
			out.vtable[ hand ] = Dword( weapon );
			memcpy( out.raw[ hand ], reinterpret_cast< const void * >( weapon + 0x48 ), 8 );
			any = true;
		}
		const auto take = [ &out ]( int slot, int hand, int index )
		{
			out.id[ slot ] = out.raw[ hand ][ index * 2 ];
			out.level[ slot ] = out.raw[ hand ][ index * 2 + 1 ];
		};
		if( state.rightCategory == 3 )
		{
			// Rod: all four slots on the one weapon.
			for( int slot = 0; slot < 4; ++slot )
			{
				take( slot, 0, slot );
			}
		}
		else if( state.rightCategory == 8 )
		{
			take( 0, 0, 0 );
			take( 1, 0, 1 );
		}
		if( state.leftCategory == 0xB )
		{
			// TCSM: its own slots 2 / 3 (R1 + X / Y), or 0 / 1 if those are empty.
			const bool high = out.raw[ 1 ][ 4 ] || out.raw[ 1 ][ 6 ];
			take( 2, 1, high ? 2 : 0 );
			take( 3, 1, high ? 3 : 1 );
		}
		return any;
	}
}
