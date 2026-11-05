#include "VRGameHaptics.h"

#include "VRControllers.h"
#include "VRHaptics.h"
#include "VRSyntheticPad.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"

#include <atomic>

namespace PSUVR
{
	namespace VRGameHaptics
	{
		namespace
		{
			constexpr uintptr_t kLocalPlayer = 0x009CA8AC;    // TPlayer*
			constexpr uintptr_t kLocalActorId = 0x009CA7D4;   // the local player's actor id (actor +0x30)
			constexpr uintptr_t kActorStatus = 0x48;          // actor -> status block (0x0047B120 / 0x0047B180)
			constexpr uintptr_t kStatusHpNow = 0x20;          // int, index 0 of { now, max } pairs (8 bytes apart)
			constexpr uintptr_t kStatusHpMax = 0x24;

			std::atomic< int > s_hits{ 0 };

			// HP baseline: a drop against it is a hurt; a new player / status block or a new max starts over.
			uintptr_t s_player = 0;
			uintptr_t s_status = 0;
			int s_hp = -1;
			int s_hpMax = 0;
#ifdef _DEBUG
			int s_hitLogs = 0;
			bool s_loggedNoStatus = false;
#endif

			void UpdateHits()
			{
				const int hits = s_hits.exchange( 0 );
				if( hits <= 0 || !Config::Get().vr.m_hapticHits )
				{
					return;
				}
				int hand = 1;
				bool justAttack = false, photonArt = false;
				VRSyntheticPad::LastAttack( hand, justAttack, photonArt );
				if( !VRControllers::Active( hand ) )
				{
					hand = 1;
				}
				const bool heavy = justAttack || photonArt;
				VRHaptics::Play( hand, heavy ? VRHaptics::Effect::HeavyHit : VRHaptics::Effect::Hit );
#ifdef _DEBUG
				PSU_LOG( "VRHaptic: hit landed x%d (%s%s)\n", hits, justAttack ? "Just Attack" : photonArt ? "Photon Art" : "normal",
					hand == 0 ? ", left" : "" );
#endif
			}

			void UpdateHurt()
			{
				const uintptr_t player = PSUMemory::ReadAbsoluteMemory< uint32_t >( kLocalPlayer );
				const uintptr_t status = player ? PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kActorStatus ) : 0;
				if( !status )
				{
#ifdef _DEBUG
					if( player && !s_loggedNoStatus )
					{
						s_loggedNoStatus = true;
						PSU_LOG( "VRHaptic: player %08X has no status block at +0x48 (hurt buzz off)\n", static_cast< unsigned >( player ) );
					}
#endif
					s_player = 0;
					s_status = 0;
					s_hp = -1;
					return;
				}
				const int hp = PSUMemory::ReadAbsoluteMemory< int >( status + kStatusHpNow );
				const int hpMax = PSUMemory::ReadAbsoluteMemory< int >( status + kStatusHpMax );
				if( hpMax <= 0 || hpMax > 1000000 || hp < 0 || hp > hpMax )
				{
					s_hp = -1;
					return;
				}
				if( player == s_player && status == s_status && hpMax == s_hpMax && s_hp >= 0 && hp < s_hp )
				{
					const float lost = static_cast< float >( s_hp - hp ) / static_cast< float >( hpMax );
					// 0.75 for a scratch, full strength from ~20% of max HP in one hit.
					float scale = 0.75f + lost * 2.5f;
					scale = scale > 1.6f ? 1.6f : scale;
					if( Config::Get().vr.m_hapticHurt )
					{
						VRHaptics::PlayBoth( VRHaptics::Effect::Hurt, scale );
					}
#ifdef _DEBUG
					PSU_LOG( "VRHaptic: hurt %d -> %d of %d (%.0f%%)%s\n", s_hp, hp, hpMax, lost * 100.0f,
						Config::Get().vr.m_hapticHurt ? "" : " (setting off)" );
#endif
				}
				s_player = player;
				s_status = status;
				s_hp = hp;
				s_hpMax = hpMax;
			}
		}

		void NoteActorHit( const void *target, uint32_t attacker, uint32_t arg2 )
		{
			if( !target || attacker != PSUMemory::ReadAbsoluteMemory< uint32_t >( kLocalActorId ) )
			{
				return;
			}
			// Not a hit on ourselves.
			if( reinterpret_cast< uintptr_t >( target ) == PSUMemory::ReadAbsoluteMemory< uint32_t >( kLocalPlayer ) )
			{
				return;
			}
			s_hits.fetch_add( 1 );
#ifdef _DEBUG
			if( s_hitLogs < 40 )
			{
				++s_hitLogs;
				PSU_LOG( "VRHaptic: our hit on %p (vtable %08X, id %08X) arg2 %08X\n", target,
					PSUMemory::ReadAbsoluteMemory< uint32_t >( reinterpret_cast< uintptr_t >( target ) ),
					PSUMemory::ReadAbsoluteMemory< uint32_t >( reinterpret_cast< uintptr_t >( target ) + 0x30 ), arg2 );
			}
#endif
		}

		void Update()
		{
			UpdateHits();
			UpdateHurt();
		}
	}
}
