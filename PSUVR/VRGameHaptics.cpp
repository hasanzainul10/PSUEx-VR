#include "VRGameHaptics.h"

#include "VRControllers.h"
#include "VRHaptics.h"
#include "VRSyntheticPad.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"
#include "../PSUStructs/PSUCombat.h"

#include <atomic>
#include <chrono>

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

			constexpr uintptr_t kRightWeapon = 0x2E0;         // TItemWeapon* per hand (+0x2E4 left)
			constexpr uintptr_t kLeftWeapon = 0x2E4;
			constexpr uintptr_t kRightClass = 0x1F0;          // weapon class per hand (+0x1F4 left)
			constexpr uintptr_t kLeftClass = 0x1F4;
			constexpr uintptr_t kAttackStep = 0x1E4;          // TPlyCombo step: 0 idle, 0x11.. attack, 0x21.. PA
			constexpr uintptr_t kHandFlags = 0x230;           // bit 1: the left weapon is in use

			std::atomic< int > s_hits{ 0 };
			std::atomic< int > s_shots[ 2 ]{};
			std::atomic< int > s_shotScale[ 2 ]{};  // per mille, the last shot's
			std::atomic< int > s_casts{ 0 };

			// Technique charge: from the attack step starting with a technique weapon on the hand that
			// pressed, until the technique spawns, the step ends or kMaxChargeSeconds.
			constexpr double kMaxChargeSeconds = 2.0;
			// Casts charge for only 0.18..0.27 s (B3 test), so pulse fast and ramp over that time.
			constexpr double kChargePulseSeconds = 0.1;
			constexpr double kChargeRampSeconds = 0.3;
			bool s_charging = false;
			int s_chargeHand = 1;
			uint32_t s_chargeStep = 0;
			uint32_t s_spentStep = 0;  // the step that already cast / timed out: no new charge for it
			double s_chargeStart = 0.0;
			double s_nextChargePulse = 0.0;

			// HP baseline: a drop against it is a hurt; a new player / status block or a new max starts over.
			uintptr_t s_player = 0;
			uintptr_t s_status = 0;
			int s_hp = -1;
			int s_hpMax = 0;
#ifdef _DEBUG
			int s_hitLogs = 0;
			int s_shotLogs = 0;
			int s_castLogs = 0;
			bool s_loggedNoStatus = false;
#endif

			double Now()
			{
				return std::chrono::duration< double >( std::chrono::steady_clock::now().time_since_epoch() ).count();
			}

			// Heavier guns kick harder (Recoil starts at 0.7; 1.45 reaches the 1.0 cap). Classes 0x1F grenade
			// launcher / 0x20 laser cannon are inferred from the weapon list order (0x1C rifle, 0x1D shotgun,
			// 0x1E bow, ..., 0x21 twin handguns, 0x22 handgun).
			float RecoilScale( uint32_t weaponClass )
			{
				switch( weaponClass )
				{
					case 0x1C: return 1.2f;
					case 0x1D:
					case 0x1F:
					case 0x20: return 1.45f;
					default: return 1.0f;
				}
			}

			void UpdateShots()
			{
				for( int hand = 0; hand < 2; ++hand )
				{
					const int shots = s_shots[ hand ].exchange( 0 );
					if( shots > 0 && Config::Get().vr.m_hapticRecoil )
					{
						VRHaptics::Play( hand, VRHaptics::Effect::Recoil, static_cast< float >( s_shotScale[ hand ].load() ) / 1000.0f );
					}
				}
			}

			// The hand that pressed the last attack (swing, trigger, voice), or the right one.
			int AttackHand()
			{
				int hand = 1;
				bool justAttack = false, photonArt = false;
				VRSyntheticPad::LastAttack( hand, justAttack, photonArt );
				return VRControllers::Active( hand ) ? hand : 1;
			}

			// Whether the attack that just started uses a technique weapon, from the weapon in use, not
			// the hand that pressed (a left press can land on the right weapon: wand + RCSM, or a missed
			// R1 with a melee right). player+0x230 bit 1 = the left weapon is in use (the technique slot
			// hand of 0x004DE6D0); the RCSM never casts, so with it the right weapon is the one. Left in
			// use: TCSM (category B) or the rod's slots 3 / 4; right: rod (3) / wand (8).
			bool TechAttack( const PSUCombat::State &combat, uint32_t handFlags )
			{
				const bool leftInUse = ( handFlags & 2 ) != 0 && combat.leftCategory != 0xC;
				return leftInUse ? combat.leftCategory == 0xB || combat.rightCategory == 3 :
					combat.rightCategory == 3 || combat.rightCategory == 8;
			}

			void UpdateTechs()
			{
				const double now = Now();
				const int casts = s_casts.exchange( 0 );
				const uintptr_t player = PSUMemory::ReadAbsoluteMemory< uint32_t >( kLocalPlayer );
				const uint32_t step = player ? PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kAttackStep ) : 0;
				if( casts > 0 )
				{
					const int hand = s_charging ? s_chargeHand : AttackHand();
					if( Config::Get().vr.m_hapticTechs )
					{
						VRHaptics::Play( hand, VRHaptics::Effect::Cast );
					}
#ifdef _DEBUG
					PSU_LOG( "VRHaptic: technique cast x%d, %s hand, step %X, %s, +230 %08X\n", casts, hand == 0 ? "left" : "right", step,
						s_charging ? "after charging" : "no charge seen",
						player ? PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kHandFlags ) : 0u );
					if( s_charging )
					{
						PSU_LOG( "VRHaptic: charge lasted %.2f s\n", now - s_chargeStart );
					}
#endif
					s_charging = false;
					s_spentStep = step;
				}
				if( step == 0 )
				{
					s_charging = false;
					s_spentStep = 0;
					return;
				}
				if( s_charging && ( step != s_chargeStep || now - s_chargeStart > kMaxChargeSeconds ) )
				{
#ifdef _DEBUG
					PSU_LOG( "VRHaptic: charge ended without a cast (%s, %.2f s)\n", step != s_chargeStep ? "step changed" : "timeout",
						now - s_chargeStart );
#endif
					s_charging = false;
					if( step == s_chargeStep )
					{
						s_spentStep = step;
					}
				}
				if( !s_charging && step != s_spentStep )
				{
					const int hand = AttackHand();
					PSUCombat::State combat{};
					const uint32_t handFlags = PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kHandFlags );
					if( PSUCombat::Read( combat ) && TechAttack( combat, handFlags ) )
					{
						s_charging = true;
						s_chargeHand = hand;
						s_chargeStep = step;
						s_chargeStart = now;
						s_nextChargePulse = now;
#ifdef _DEBUG
						PSU_LOG( "VRHaptic: technique charge, %s hand, step %X, categories %X / %X, +230 %08X\n", hand == 0 ? "left" : "right",
							step, combat.rightCategory, combat.leftCategory, handFlags );
#endif
					}
					else
					{
						s_spentStep = step;  // not a technique: don't look again until the step changes
					}
				}
				if( s_charging && now >= s_nextChargePulse )
				{
					s_nextChargePulse = now + kChargePulseSeconds;
					if( Config::Get().vr.m_hapticTechs )
					{
						const double ramp = ( now - s_chargeStart ) / kChargeRampSeconds;
						VRHaptics::Play( s_chargeHand, VRHaptics::Effect::Charge, 1.0f + 0.45f * static_cast< float >( ramp > 1.0 ? 1.0 : ramp ) );
					}
				}
			}

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

		void NoteWeaponFire( const void *weapon, uint32_t alternate )
		{
			const uintptr_t player = PSUMemory::ReadAbsoluteMemory< uint32_t >( kLocalPlayer );
			const uintptr_t w = reinterpret_cast< uintptr_t >( weapon );
			if( !player || !w )
			{
				return;
			}
			const uintptr_t right = PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kRightWeapon );
			const uintptr_t left = PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kLeftWeapon );
			if( w != right && w != left )
			{
				return;  // another player's or an NPC's gun
			}
			const bool leftWeapon = w != right;
			const uint32_t weaponClass = PSUMemory::ReadAbsoluteMemory< uint32_t >( player + ( leftWeapon ? kLeftClass : kRightClass ) );
			// Twin handguns: the game flips arg2 between shots (one muzzle each); otherwise the hand holding it
			// (the bow is held left).
			const int hand = PSUCombat::TwinGuns( weaponClass ) ? ( alternate ? 0 : 1 ) :
				leftWeapon ? 0 : PSUCombat::GunHand( weaponClass );
			s_shotScale[ hand ].store( static_cast< int >( RecoilScale( weaponClass ) * 1000.0f ) );
			s_shots[ hand ].fetch_add( 1 );
#ifdef _DEBUG
			if( s_shotLogs < 60 )
			{
				++s_shotLogs;
				PSU_LOG( "VRHaptic: shot, weapon %p (%s object, vtable %08X) class %X arg2 %08X -> %s hand x%.2f\n", weapon,
					leftWeapon ? "left" : "right", PSUMemory::ReadAbsoluteMemory< uint32_t >( w ), weaponClass, alternate,
					hand == 0 ? "left" : "right", RecoilScale( weaponClass ) );
			}
#endif
		}

		void NoteTechSpawn( const void *weapon )
		{
			const uintptr_t player = PSUMemory::ReadAbsoluteMemory< uint32_t >( kLocalPlayer );
			const uintptr_t w = reinterpret_cast< uintptr_t >( weapon );
			if( !player || !w || ( w != PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kRightWeapon ) &&
				w != PSUMemory::ReadAbsoluteMemory< uint32_t >( player + kLeftWeapon ) ) )
			{
				return;
			}
			s_casts.fetch_add( 1 );
#ifdef _DEBUG
			if( s_castLogs < 40 )
			{
				++s_castLogs;
				PSU_LOG( "VRHaptic: our technique spawned, weapon %p (vtable %08X)\n", weapon, PSUMemory::ReadAbsoluteMemory< uint32_t >( w ) );
			}
#endif
		}

		void Update()
		{
			UpdateHits();
			UpdateHurt();
			UpdateShots();
			UpdateTechs();
		}
	}
}
