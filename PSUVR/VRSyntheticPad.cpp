#include "VRSyntheticPad.h"

#include "VRBindings.h"
#include "VRCamera.h"
#include "VRControllers.h"
#include "VRSystem.h"
#include "VRMotionLayout.h"
#include "VRPerf.h"
#include "VRSpeech.h"
#include "VRUiPointer.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUStructs/PSUCombat.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace PSUVR
{
	namespace
	{
		// Logical PSU pad buttons, in PsuIlluminus.ini [gamepad1] order.
		enum PadButton : int
		{
			kPadA, kPadB, kPadX, kPadY, kPadL1, kPadR1, kPadL2, kPadR2, kPadL3, kPadR3, kPadSelect, kPadStart,
			kPadUp, kPadRight, kPadDown, kPadLeft,
			kPadButtonCount
		};
		const char *const kIniKeys[ kPadButtonCount ] = {
			"BUTTON_A", "BUTTON_B", "BUTTON_X", "BUTTON_Y", "BUTTON_L1", "BUTTON_R1", "BUTTON_L2", "BUTTON_R2",
			"BUTTON_L3", "BUTTON_R3", "BUTTON_SELECT", "BUTTON_START", "BUTTON_U", "BUTTON_R", "BUTTON_D", "BUTTON_L" };

		constexpr uint32_t Bit( int button ) { return 1u << button; }

		struct PadState
		{
			uint32_t buttons = 0;
			float lx = 0.0f, ly = 0.0f;  // -1..1, +y = stick pushed up
			float rx = 0.0f, ry = 0.0f;
		};

		// [gamepad1]: DirectInput button index per logical button (-1 = none; the D-pad
		// uses the POV hat when its entries are 0), and the right stick's axes.
		struct PadMap
		{
			int button[ kPadButtonCount ];
			int rightX = 3;  // lRx
			int rightY = 4;  // lRy
		};

		std::mutex s_mutex;
		PadState s_out;           // read by MergeJoystickState (game input thread)
		bool s_active = false;
		DWORD s_updateTick = 0;
		PadMap s_map{};
		bool s_mapLoaded = false;
		FILETIME s_iniTime{};
		DWORD s_iniCheckTick = 0;
		unsigned s_actions = 0;
		bool s_loggedMerge = false;

		LARGE_INTEGER s_lastTime{};

		// Grip handling: a press waits before it is sent, so both-grip chords don't also
		// press L1 / R1 in the game. Consumed: the grip was part of another combination
		// (left grip + stick click) and sends nothing until released.
		enum class Grip { Idle, Pending, Held, Modifier, Consumed };
		Grip s_grip[ 2 ] = { Grip::Idle, Grip::Idle };
		float s_gripTime[ 2 ]{};
		float s_pulse[ 2 ]{};     // short L1 / R1 press left after a quick tap
		bool s_triggerDown[ 2 ]{};
		bool s_comboWindow = false;       // last frame's Just Attack / combo window
		int s_comboStep = 0;
		int s_comboMotion = -1;           // last frame's motion (+0x170); the buzz fires once per motion
		bool s_comboBuzzed = false;
		bool s_combatValid = false;
		PSUCombat::State s_combat;
		int s_lastAttackHand = 1;     // the combo buzz goes to the hand that attacked

		bool s_aimModeOn = false;         // free aim mode (left stick click)
		bool s_gunInView = false;         // the gun hand is raised into view
		bool s_gunAdjusting = false;      // both grips + right stick are turning the gun model
		bool s_gunAdjustDirty = false;

		// The settings pair for the gun being aimed: bow, other two-handed guns, or handguns.
		bool GunAdjustFields( float *&pitch, float *&yaw )
		{
			auto &vr = Config::Get().vr;
			if( !s_combatValid )
			{
				return false;
			}
			const bool twoHanded = s_combat.rightCategory == 2 ||
				PSUCombat::ClassifyWeapon( s_combat.weaponClass ) == PSUCombat::WeaponKind::Gun2H;
			if( twoHanded && s_combat.weaponClass == 0x1E )
			{
				pitch = &vr.m_gunAdjustBowPitch;
				yaw = &vr.m_gunAdjustBowYaw;
			}
			else if( twoHanded )
			{
				pitch = &vr.m_gunAdjustRiflePitch;
				yaw = &vr.m_gunAdjustRifleYaw;
			}
			else
			{
				pitch = &vr.m_gunAdjustHandgunPitch;
				yaw = &vr.m_gunAdjustHandgunYaw;
			}
			return true;
		}
		bool s_dpadMode = false;          // manual flip of the D-pad mode (both grips + left stick click)
		bool s_ui = false;                // a menu has the input (UiContext)

		// The input manager's context (0x00B9CC80): 0x1007 while moving freely, 0x0405 while a
		// menu has the input (main menu, shops, mission counter, login screens).
		bool UiContext()
		{
			return ( PSUMemory::ReadAbsoluteMemory< uint32_t >( 0x00B9CC80 ) & 0xFFFF ) == 0x0405;
		}

		// Left stick = D-pad: automatically in menus (vr.autoDpad), flipped by the manual toggle
		// until the context changes.
		bool DpadActive()
		{
			// The palette (held A) is picked with the D-pad too.
			const bool palette = s_combatValid && s_combat.paletteOpen;
			return ( Config::Get().vr.m_autoDpad && ( s_ui || palette ) ) != s_dpadMode;
		}
		bool s_leftClickConsumed = false; // the stick click was a grip combination: not the layout's output
		bool s_leftClickL3 = false;       // left grip + left stick click: L3 while the click is held

		constexpr float kGripDelay = 0.15f;
		constexpr float kTapPulse = 0.06f;
		constexpr float kDpadThreshold = 0.5f;

		void IniPath( char *out, size_t size )
		{
			out[ 0 ] = '\0';
			char base[ MAX_PATH ]{};
			if( GetEnvironmentVariableA( "LOCALAPPDATA", base, MAX_PATH ) == 0 )
			{
				return;
			}
			sprintf_s( out, size, "%s\\SEGA\\PHANTASY STAR UNIVERSE Illuminus\\PsuIlluminus.ini", base );
		}

		void LoadMap()
		{
			char path[ MAX_PATH * 2 ]{};
			IniPath( path, sizeof( path ) );
			WIN32_FILE_ATTRIBUTE_DATA attributes{};
			const bool exists = path[ 0 ] && GetFileAttributesExA( path, GetFileExInfoStandard, &attributes );
			if( s_mapLoaded && exists && CompareFileTime( &attributes.ftLastWriteTime, &s_iniTime ) == 0 )
			{
				return;
			}
			s_iniTime = exists ? attributes.ftLastWriteTime : FILETIME{};
			s_mapLoaded = true;

			// The game numbers buttons from 1 (0 = unassigned / POV for the D-pad).
			static const int kDefaults[ kPadButtonCount ] = { 2, 1, 3, 4, 5, 6, 11, 12, 7, 10, 0, 8, 0, 0, 0, 0 };
			PadMap map{};
			char line[ 256 ];
			int length = 0;
			length += sprintf_s( line + length, sizeof( line ) - length, "VRSyntheticPad: [gamepad1]%s", exists ? "" : " (ini not found, defaults)" );
			for( int i = 0; i < kPadButtonCount; ++i )
			{
				const int value = exists ? static_cast< int >( GetPrivateProfileIntA( "gamepad1", kIniKeys[ i ], kDefaults[ i ], path ) ) : kDefaults[ i ];
				map.button[ i ] = value > 0 && value <= 128 ? value - 1 : -1;
				if( length < static_cast< int >( sizeof( line ) ) - 16 )
				{
					length += sprintf_s( line + length, sizeof( line ) - length, " %s=%d", kIniKeys[ i ] + 7, value );
				}
			}
			const int stickX = exists ? static_cast< int >( GetPrivateProfileIntA( "gamepad1", "STICK2_INPUT_X", 4, path ) ) : 4;
			const int stickY = exists ? static_cast< int >( GetPrivateProfileIntA( "gamepad1", "STICK2_INPUT_Y", 5, path ) ) : 5;
			// Axes numbered from 1: lX, lY, lZ, lRx, lRy, lRz, slider 0, slider 1.
			map.rightX = stickX >= 1 && stickX <= 8 ? stickX - 1 : 3;
			map.rightY = stickY >= 1 && stickY <= 8 ? stickY - 1 : 4;
			PSU_LOG( "%s STICK2=%d,%d\n", line, stickX, stickY );

			std::lock_guard lock( s_mutex );
			s_map = map;
		}

		float Deadzone( float value, float deadzone )
		{
			const float magnitude = std::fabs( value );
			if( magnitude <= deadzone )
			{
				return 0.0f;
			}
			const float scaled = ( magnitude - deadzone ) / ( 1.0f - deadzone );
			return ( value < 0.0f ? -1.0f : 1.0f ) * ( scaled > 1.0f ? 1.0f : scaled );
		}

		float FrameSeconds()
		{
			LARGE_INTEGER now{}, frequency{};
			QueryPerformanceCounter( &now );
			QueryPerformanceFrequency( &frequency );
			float dt = 0.0f;
			if( s_lastTime.QuadPart && frequency.QuadPart )
			{
				dt = static_cast< float >( now.QuadPart - s_lastTime.QuadPart ) / static_cast< float >( frequency.QuadPart );
			}
			s_lastTime = now;
			return dt < 0.0f ? 0.0f : ( dt > 0.1f ? 0.1f : dt );
		}

		void Buzz( int hand )
		{
			VRControllers::Haptic( hand, 0.5f, 0.04f );
		}

		// Just Attack window from the weapon type's motion table: a tick in the hand that attacked
		// when the window of the current motion opens, once per motion (stronger before the last
		// hit). Falls back to the +0x710 == 0x0E edge when the motion table can't be read.
		void ComboFeedback()
		{
			PSUCombat::State combat;
			const bool valid = PSUCombat::Read( combat );
			const bool fromTable = valid && PSUCombat::HasJustAttackWindow( combat );
			const bool open = valid && ( fromTable ? PSUCombat::JustAttackWindowOpen( combat ) : PSUCombat::ComboWindowOpen( combat ) );
			const int motion = valid ? combat.motionId : -1;
			if( motion != s_comboMotion )
			{
				s_comboBuzzed = false;
			}
			const int buzzHand = VRControllers::Active( s_lastAttackHand ) ? s_lastAttackHand : 1;
			if( open && !s_comboWindow && !s_comboBuzzed && Config::Get().vr.m_comboHaptic && VRControllers::Active( buzzHand ) )
			{
				VRControllers::Haptic( buzzHand, combat.comboStep >= 2 ? 0.8f : 0.5f, 0.03f );
				s_comboBuzzed = true;
			}
#ifdef _DEBUG
			if( open != s_comboWindow || ( valid ? combat.comboStep : 0 ) != s_comboStep || motion != s_comboMotion )
			{
				PSU_LOG( "VRCombat: class 0x%02X motion 0x%X step %d frame %.1f JA [%.1f..%.1f] %s%s (state 0x%08X%s)\n",
					valid ? combat.motionClass & 0xFF : 0, motion, valid ? combat.comboStep : 0, valid ? combat.motionFrame : 0.0f,
					valid ? combat.jaStart : -1.0f, valid ? combat.jaEnd : -1.0f, open ? "OPEN" : "closed",
					fromTable ? "" : " (no table, 0x0E fallback)", valid ? combat.attackState : 0u,
					valid && combat.earlyPress ? ", early press" : "" );
			}
			// First time a motion is seen: its whole timing entry (16 floats), to check the other fields.
			if( valid && combat.motionEntry && motion != s_comboMotion && combat.comboStep > 0 )
			{
				const float *f = reinterpret_cast< const float * >( combat.motionEntry );
				PSU_LOG( "VRCombat: entry 0x%08X class 0x%02X motion 0x%X: %.1f %.1f | %.1f %.1f | %.1f %.1f %.1f | %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n",
					static_cast< unsigned >( combat.motionEntry ), combat.motionClass & 0xFF, motion, f[ 0 ], f[ 1 ], f[ 2 ], f[ 3 ],
					f[ 4 ], f[ 5 ], f[ 6 ], f[ 7 ], f[ 8 ], f[ 9 ], f[ 10 ], f[ 11 ], f[ 12 ], f[ 13 ], f[ 14 ] );
			}
#endif
			s_comboMotion = motion;
			s_comboWindow = open;
			s_comboStep = valid ? combat.comboStep : 0;
			s_combatValid = valid;
			s_combat = combat;
		}

		// Motion combat (vr.swingAttack): gestures press the game's attack buttons, chosen by
		// what each hand holds (PSUCombat weapon classes). X = attack and Y = Photon Art with
		// the active weapon; R1 switches the active weapon between the right and left hand
		// (+0x1DC). Swing a melee hand to attack, swing with that hand's trigger held for a
		// Photon Art; pull the trigger of the hand holding a gun to shoot. A request first
		// switches to that hand's weapon if needed, waits for the combo window when made
		// during the swing animation (vr.comboBuffer), and on the first hit of a chain turns
		// the character toward where you look (vr.attackFaceLook).
		bool s_swingArmed[ 2 ] = { true, true };
		int s_swingFrames[ 2 ]{};
		int s_pendingButton = -1;     // requested attack button (X / Y), not sent yet
		float s_pendingTime = 0.0f;   // seconds the request may still wait
		int s_pendingHand = 1;
		uint32_t s_pendingWeapon = 0; // weapon class that must be active first (0 = any)
		bool s_switchSent = false;    // R1 sent for this request
		int s_pulseButton = -1;       // pad button being pressed
		float s_pulseTime = 0.0f;
		float s_faceHold = 0.0f;      // left stick still held toward the look direction after the turn
		bool s_turning = false;       // turning toward the look direction; the press waits
		float s_turnTime = 0.0f;
		constexpr float kSwingRearmSpeed = 0.8f;
		constexpr float kSwingBufferSeconds = 0.25f;
		// Held for the Just Attack window: windows open up to frame ~33 of the motion (~1.1 s at 30 fps).
		constexpr float kJustAttackWaitSeconds = 1.5f;
		constexpr float kSwitchWaitSeconds = 0.6f;
		constexpr float kAttackPulseSeconds = 0.08f;
		constexpr float kMinTurnSeconds = 0.05f;
		constexpr float kMaxTurnSeconds = 0.5f;
		constexpr float kFaceHoldAfterSeconds = 0.05f;
		// The game keeps turning during the hold after the stop (about 75 degrees per 0.1 s), so the
		// turn ends this much before the tolerance is reached.
		constexpr float kFaceLeadRadians = 10.0f * ( 3.14159265f / 180.0f );
		float FaceTolerance()
		{
			return std::clamp( Config::Get().vr.m_attackFaceTolerance, 5.0f, 90.0f ) * ( 3.14159265f / 180.0f );
		}

		bool MeleeKind( PSUCombat::WeaponKind kind )
		{
			return kind == PSUCombat::WeaponKind::Melee1H || kind == PSUCombat::WeaponKind::Melee2H ||
				kind == PSUCombat::WeaponKind::Twin;
		}

		// What a hand's gestures do, from both hands' weapons (by category, +0x2D4 / +0x2D8).
		//   swing: a swing attacks (melee, techniques).
		//   trigger: a trigger click attacks (guns always; melee / techniques with grip = PA).
		//   shoot: presses go through Gun() (R1 held for the left hand) instead of RequestAttack.
		struct HandRoles
		{
			bool swing[ 2 ]{};
			bool trigger[ 2 ]{};
			bool shoot[ 2 ]{};
			bool Any( int hand ) const { return swing[ hand ] || trigger[ hand ] || shoot[ hand ]; }
		};

		HandRoles Roles()
		{
			HandRoles roles;
			const auto &vr = Config::Get().vr;
			// No combat gestures while a menu or the palette has the input.
			if( !s_combatValid || !vr.m_swingAttack || s_ui || s_combat.paletteOpen )
			{
				return roles;
			}
			const bool gripPa = vr.m_gripPhotonArt;
			const uint32_t rightCat = s_combat.rightCategory;
			const uint32_t leftCat = s_combat.leftCategory;
			const PSUCombat::WeaponKind right = PSUCombat::ClassifyWeapon( s_combat.weaponClass );
			const PSUCombat::WeaponKind left = PSUCombat::ClassifyWeapon( s_combat.leftClass );
			// Melee or technique on the active weapon: swing (and the trigger with grip = PA).
			const auto direct = [ & ]( int hand )
			{
				roles.swing[ hand ] = true;
				roles.trigger[ hand ] = gripPa;
			};
			// Through Gun(): trigger (and swing for techniques).
			const auto shoot = [ & ]( int hand, bool swing )
			{
				roles.shoot[ hand ] = true;
				roles.trigger[ hand ] = true;
				roles.swing[ hand ] = swing;
			};
			// Right hand.
			if( rightCat == 1 || rightCat == 7 || rightCat == 3 || rightCat == 8 )
			{
				direct( 1 );  // melee, rod, staff
			}
			else if( rightCat == 2 )
			{
				shoot( PSUCombat::GunHand( s_combat.weaponClass ), false );  // two-handed gun, the active weapon: no R1
			}
			else if( rightCat == 0 && MeleeKind( right ) )
			{
				direct( 1 );  // category not read yet / unmapped: by class
			}
			else if( rightCat == 0 && right == PSUCombat::WeaponKind::Gun2H )
			{
				shoot( PSUCombat::GunHand( s_combat.weaponClass ), false );
			}
			// Left hand.
			if( rightCat == 1 || ( rightCat == 0 && ( right == PSUCombat::WeaponKind::Melee2H || right == PSUCombat::WeaponKind::Twin ) ) )
			{
				direct( 0 );  // two-handed and twin melee: either hand swings
			}
			else if( roles.shoot[ 0 ] )
			{
				// The bow: the left hand already fires it.
			}
			else if( leftCat == 9 || leftCat == 0xA )
			{
				shoot( 0, false );  // handgun, machinegun
			}
			else if( leftCat == 0xB )
			{
				shoot( 0, true );  // TCSM: R1 + X / Y
			}
			else if( leftCat == 0 && rightCat == 3 )
			{
				shoot( 0, true );  // rod: slots 3 / 4 with R1
			}
			else if( leftCat == 0 && left == PSUCombat::WeaponKind::Gun1H )
			{
				shoot( 0, false );
			}
			// 0xC RCSM fires by itself: nothing.
			return roles;
		}

		float WrapRadians( float angle )
		{
			while( angle > 3.14159265f ) angle -= 6.2831853f;
			while( angle < -3.14159265f ) angle += 6.2831853f;
			return angle;
		}

		// Angle from the character's facing to where the player looks; false when unknown.
		bool FacingOff( float &off )
		{
			float look = 0.0f;
			if( !s_combatValid || !VRCamera::LookYaw( look ) )
			{
				return false;
			}
			const float facing = static_cast< float >( s_combat.facing ) * ( 6.2831853f / 65536.0f );
			off = WrapRadians( look - facing );
			return true;
		}

		// The character faces away from where the player looks (first hit of a chain only).
		bool NeedsFacing()
		{
			float off = 0.0f;
			// First person only: in third person the camera isn't where the character should face.
			if( !Config::Get().vr.m_attackFaceLook || !VRCamera::FirstPersonActive() || s_combat.comboStep > 0 ||
				!FacingOff( off ) )
			{
				return false;
			}
#ifdef _DEBUG
			PSU_LOG( "VRFace: off %.0f deg (facing raw %04X)\n", off * 57.29578f, s_combat.facing );
#endif
			return std::fabs( off ) > FaceTolerance();
		}

		void StartAttack( int button, int hand )
		{
			s_lastAttackHand = hand;
			s_pulseButton = button;
			s_pulseTime = kAttackPulseSeconds;
			// PSU aims an attack along the stick held when it starts, so the stick stays toward
			// the look direction through the press, not just before it.
			// The game turns at its own speed (about 75 degrees per 0.1 s), so the press waits
			// until the facing is within tolerance, at most kMaxTurnSeconds.
			s_turning = NeedsFacing();
			s_turnTime = 0.0f;
			s_faceHold = 0.0f;
		}

		// The weapon X / Y use now; 0 (weapon not drawn yet) counts as the right-hand one.
		uint32_t ActiveWeapon()
		{
			return s_combat.activeClass ? s_combat.activeClass : s_combat.weaponClass;
		}

		// vr.comboBuffer: a press made mid-combo waits. With vr.comboBufferJustAttack on and the
		// motion's Just Attack window known it waits until the window opens (the press then lands as
		// a Just Attack); otherwise until the swing animation ends (+0x710 == 0x02, the old check).
		// The first hit of a chain never waits.
		bool HoldForCombo()
		{
			if( !Config::Get().vr.m_comboBuffer || !s_combatValid || s_combat.comboStep <= 0 )
			{
				return false;
			}
			if( Config::Get().vr.m_comboBufferJustAttack && PSUCombat::HasJustAttackWindow( s_combat ) )
			{
				return s_combat.motionFrame < s_combat.jaStart;
			}
			return PSUCombat::Swinging( s_combat );
		}

		// A gesture asked for `button` with `weapon` (0 = whatever is active). Sent by
		// PumpRequest once that weapon is active and the combo allows it.
		void RequestAttack( int button, int hand, uint32_t weapon, const char *what )
		{
			VRControllers::Haptic( hand, 0.25f, 0.02f );
			s_pendingButton = button;
			s_pendingHand = hand;
			s_pendingWeapon = weapon;
			s_switchSent = false;
			const bool needSwitch = weapon && ActiveWeapon() != weapon;
			s_pendingTime = needSwitch ? kSwitchWaitSeconds :
				HoldForCombo() && Config::Get().vr.m_comboBufferJustAttack && PSUCombat::HasJustAttackWindow( s_combat ) ? kJustAttackWaitSeconds : kSwingBufferSeconds;
#ifdef _DEBUG
			PSU_LOG( "VRSwing: %s hand %s -> %s, step %d, active %X%s\n", hand == 0 ? "left" : "right", what,
				button == kPadY ? "Y (PA)" : "X", s_combatValid ? s_combat.comboStep : 0, ActiveWeapon(),
				needSwitch ? ", switching weapon (R1)" : "" );
#endif
		}

		void PumpRequest( float dt )
		{
			if( s_pendingButton < 0 || s_pulseButton >= 0 )
			{
				return;
			}
			if( s_pendingWeapon && ActiveWeapon() != s_pendingWeapon )
			{
				// Switch hands first: R1, then wait for +0x1DC to follow.
				if( !s_switchSent )
				{
					s_switchSent = true;
					s_pulseButton = kPadR1;
					s_pulseTime = kAttackPulseSeconds;
				}
			}
			else if( s_switchSent && s_combatValid && ( s_combat.attackState & 0xFF ) == 0x06 )
			{
				// Still in the weapon switch animation (+0x710 0x..06): presses would be lost.
			}
			else if( !HoldForCombo() )
			{
#ifdef _DEBUG
				if( s_combatValid && s_combat.comboStep > 0 )
				{
					PSU_LOG( "VRSwing: press sent, motion 0x%X step %d frame %.1f JA [%.1f..%.1f], %.2f s left\n",
						s_combat.motionId, s_combat.comboStep, s_combat.motionFrame, s_combat.jaStart, s_combat.jaEnd,
						s_pendingTime );
				}
#endif
				StartAttack( s_pendingButton, s_pendingHand );
				s_pendingButton = -1;
				return;
			}
			if( ( s_pendingTime -= dt ) <= 0.0f )
			{
#ifdef _DEBUG
				PSU_LOG( "VRSwing: request dropped (%s)\n", s_switchSent && ActiveWeapon() != s_pendingWeapon ?
					"weapon didn't switch" : "window didn't open in time" );
#endif
				s_pendingButton = -1;
			}
		}

		// Swing detection for one hand: fires once per swing, re-arms when the hand slows.
		bool Swung( int hand )
		{
			const ControllerState &c = VRControllers::State( hand );
			if( !c.active || !c.velocityValid )
			{
				s_swingArmed[ hand ] = true;
				s_swingFrames[ hand ] = 0;
				return false;
			}
			const float *v = c.linearVelocity;
			const float speed = std::sqrt( v[ 0 ] * v[ 0 ] + v[ 1 ] * v[ 1 ] + v[ 2 ] * v[ 2 ] );
			if( speed < kSwingRearmSpeed )
			{
				s_swingArmed[ hand ] = true;
				s_swingFrames[ hand ] = 0;
				return false;
			}
			if( s_swingArmed[ hand ] && speed > Config::Get().vr.m_swingSpeed && ++s_swingFrames[ hand ] >= 2 )
			{
				s_swingArmed[ hand ] = false;
				return true;
			}
			return false;
		}

		// Left-hand gun. The left weapon is the active one only while R1 is held (+0x1DC 0x22xx),
		// so a shot holds R1, waits for the gun to come up, then presses X. A click fires once;
		// holding the trigger fires again each time the game takes the next shot. R1 stays held
		// until the shot is over, so releasing doesn't cancel it.
		bool s_gunShot = false;          // a click still owed a shot
		bool s_gunPhotonArt = false;     // left grip held with the trigger: Y (Photon Art) instead of X
		float s_gunShotTime = 0.0f;
		float s_gunTail = 0.0f;          // keep R1 held a little after the last shot
		float s_gunGap = 0.0f;           // after an X press, wait before judging the state
		float s_gunUpTime = 0.0f;        // how long the left gun has been the active weapon
		constexpr float kGunSettleSeconds = 0.1f;
		// Left techniques (TCSM, rod slots 3 / 4) don't change +0x1DC (TCSM and staff are both 0x28,
		// the rod has no left class): wait this long with R1 held instead.
		constexpr float kCastSettleSeconds = 0.25f;
		constexpr float kGunShotWaitSeconds = 0.8f;
		constexpr float kGunTailSeconds = 0.2f;
		constexpr float kGunGapSeconds = 0.15f;

		void Gun( float dt, const HandRoles &roles, PadState &pad )
		{
			// The left hand's gun (with R1) or a two-handed gun (the right trigger).
			const int hand = roles.shoot[ 0 ] ? 0 : ( roles.shoot[ 1 ] ? 1 : -1 );
			if( hand < 0 )
			{
				s_gunShot = false;
				s_gunTail = 0.0f;
				s_gunUpTime = 0.0f;
				return;
			}
			// R1 only for a left-hand weapon; a two-handed gun (also the bow, held left) is active already.
			const bool twoHanded = s_combat.rightCategory == 2 ||
				( s_combat.rightCategory == 0 && PSUCombat::ClassifyWeapon( s_combat.weaponClass ) == PSUCombat::WeaponKind::Gun2H );
			const bool leftGun = hand == 0 && !twoHanded;
			const bool held = s_triggerDown[ hand ];
			if( s_gunShot && ( s_gunShotTime -= dt ) <= 0.0f )
			{
				s_gunShot = false;
#ifdef _DEBUG
				PSU_LOG( "VRGun: shot dropped (gun didn't come up)\n" );
#endif
			}
			const bool shooting = held || s_gunShot || ( s_lastAttackHand == hand && s_combat.comboStep > 0 );
			if( shooting )
			{
				s_gunTail = kGunTailSeconds;
			}
			else if( s_gunTail > 0.0f )
			{
				s_gunTail -= dt;
			}
			if( s_gunTail <= 0.0f )
			{
				s_gunUpTime = 0.0f;
				return;
			}
			// Free aim: the gun is already up and R1 would start scan mode; only press X. A
			// two-handed gun is the active weapon already: no R1 either.
			const bool freeAim = VRCamera::CameraState() == 4;
			if( !freeAim && leftGun )
			{
				pad.buttons |= Bit( kPadR1 );
			}
			if( s_gunGap > 0.0f )
			{
				s_gunGap -= dt;
				return;
			}
			// +0x710 stays 0x..06 (gun stance) the whole time R1 is held: only the active weapon
			// tells that the gun is up; give it a moment to settle.
			const bool byClass = !leftGun || ( s_combat.leftClass != 0xFFFFFFFFu && s_combat.leftClass != s_combat.weaponClass );
			if( !byClass || s_combat.activeClass == ( leftGun ? s_combat.leftClass : s_combat.weaponClass ) )
			{
				s_gunUpTime += dt;
			}
			else
			{
				s_gunUpTime = 0.0f;
			}
			const bool gunUp = freeAim || s_gunUpTime >= ( byClass ? kGunSettleSeconds : kCastSettleSeconds );
			const bool ready = !PSUCombat::Swinging( s_combat );
			if( ( held || s_gunShot ) && gunUp && ready && s_pulseButton < 0 )
			{
				// Same as a swing: the first shot of a chain turns toward where you look. A held
				// trigger keeps the choice made by its click (grip held = Photon Art).
				StartAttack( s_gunPhotonArt ? kPadY : kPadX, hand );
				s_gunGap = kAttackPulseSeconds + kGunGapSeconds;
#ifdef _DEBUG
				PSU_LOG( "VRGun: %s (%s), step %d\n", s_gunPhotonArt ? "Photon Art" : "shot", s_gunShot ? "click" : "held",
					s_combat.comboStep );
#endif
				s_gunShot = false;
			}
		}

		// Free aim (camera state 4, R3 with a gun): the game aims with the right stick, so the
		// controller steers that stick until the aim matches where the gun hand points. On entry
		// the hand's direction and the aim are paired; turning the hand by some angle turns the
		// aim by the same angle (at the game's aim speed). Input only: nothing is written.
		bool s_freeAim = false;
		int s_aimHand = -1;            // the hand whose gun free aim follows (-1: none; set in AimModeFollow)
		float s_lastWantYaw = 0.0f, s_lastWantPitch = 0.0f;
		bool s_directAim = false;
		DWORD s_directAimTick = 0;
		float s_aimHandYaw0 = 0.0f, s_aimHandPitch0 = 0.0f;
		float s_aimCamYaw0 = 0.0f, s_aimCamPitch0 = 0.0f;
		DWORD s_aimLogTick = 0;
		constexpr float kAimGain = 3.0f;              // stick per radian of error (full at ~19 degrees)
		constexpr float kAimDeadband = 0.6f * ( 3.14159265f / 180.0f );

		// Yaw (OpenXR, +left) and pitch (+up) of a controller's aim ray.
		bool AimAngles( const ControllerState &c, float &yaw, float &pitch )
		{
			if( !c.active || !c.aimValid )
			{
				return false;
			}
			const float x = c.aimOrientation[ 0 ], y = c.aimOrientation[ 1 ], z = c.aimOrientation[ 2 ], w = c.aimOrientation[ 3 ];
			// q * (0, 0, -1)
			const float fx = -( 2.0f * ( x * z + w * y ) );
			const float fy = -( 2.0f * ( y * z - w * x ) );
			const float fz = -( 1.0f - 2.0f * ( x * x + y * y ) );
			yaw = std::atan2( -fx, -fz );
			pitch = std::asin( fy < -1.0f ? -1.0f : ( fy > 1.0f ? 1.0f : fy ) );
			return true;
		}

		void FreeAim( PadState &pad )
		{
			const bool active = Config::Get().vr.m_controllerAim && VRCamera::CameraState() == 4;
			// The gun hand, as AimModeFollow picked it (the bow is held, and aimed, left).
			const int hand = s_aimHand >= 0 ? s_aimHand
				: ( s_combatValid && PSUCombat::ClassifyWeapon( s_combat.leftClass ) == PSUCombat::WeaponKind::Gun1H ? 0 : 1 );
			float handYaw = 0.0f, handPitch = 0.0f, camYaw = 0.0f, camPitch = 0.0f;
			s_directAim = false;
			if( !active || !AimAngles( VRControllers::State( hand ), handYaw, handPitch ) ||
				!VRCamera::RenderCameraYawPitch( camYaw, camPitch ) )
			{
				s_freeAim = false;
				return;
			}
			if( !s_freeAim )
			{
				s_freeAim = true;
				s_aimHandYaw0 = handYaw;
				s_aimHandPitch0 = handPitch;
				s_aimCamYaw0 = camYaw;
				s_aimCamPitch0 = camPitch;
#ifdef _DEBUG
				PSU_LOG( "VRAim: free aim with the %s hand (camera yaw %.0f pitch %.0f)\n", hand == 0 ? "left" : "right",
					camYaw * 57.29578f, camPitch * 57.29578f );
#endif
			}
			// First person: aim where the controller points in the world (the view doesn't move).
			// Otherwise: turning the hand turns the aim by the same angle from where it started.
			float wantYaw = 0.0f, wantPitch = 0.0f;
			if( !VRCamera::WorldAimFromTracking( handYaw, handPitch, wantYaw, wantPitch ) )
			{
				// Game yaw turns the same way as OpenXR yaw (both grow turning left).
				wantYaw = WrapRadians( s_aimCamYaw0 + WrapRadians( handYaw - s_aimHandYaw0 ) );
				wantPitch = s_aimCamPitch0 + ( handPitch - s_aimHandPitch0 );
			}
			// Leash: aim from where the game fires (above the player) to the point on the hand's line
			// where the reticle is, so the shot lands on the reticle.
			if( Config::Get().vr.m_aimLeash )
			{
				const ControllerState &c = VRControllers::State( hand );
				const float x = c.aimOrientation[ 0 ], y = c.aimOrientation[ 1 ], z = c.aimOrientation[ 2 ], w = c.aimOrientation[ 3 ];
				const float dirTracking[ 3 ] = { -( 2.0f * ( x * z + w * y ) ), -( 2.0f * ( y * z - w * x ) ),
					-( 1.0f - 2.0f * ( x * x + y * y ) ) };
				const float meters = Config::Get().vr.m_aimLeashMeters > 1.0f ? Config::Get().vr.m_aimLeashMeters : 1.0f;
				const float target[ 3 ] = { c.aimPosition[ 0 ] + dirTracking[ 0 ] * meters, c.aimPosition[ 1 ] + dirTracking[ 1 ] * meters,
					c.aimPosition[ 2 ] + dirTracking[ 2 ] * meters };
				float targetWorld[ 3 ], start[ 3 ];
				if( VRCamera::TrackingToWorld( target, targetWorld ) && VRControllers::ShotStartWorld( start ) )
				{
					const float vx = targetWorld[ 0 ] - start[ 0 ];
					const float vy = targetWorld[ 1 ] - start[ 1 ];
					const float vz = targetWorld[ 2 ] - start[ 2 ];
					const float flat = std::sqrt( vx * vx + vz * vz );
					if( flat > 1.0e-3f )
					{
						// Game forward is ( sin, 0, cos ) of the yaw.
						wantYaw = std::atan2( vx, vz );
						wantPitch = std::atan2( vy, flat );
					}
				}
			}
			s_lastWantYaw = wantYaw;
			s_lastWantPitch = wantPitch;
			// First person: the camera hook writes these into the game's aim; no stick needed.
			float worldYaw = 0.0f, worldPitch = 0.0f;
			if( Config::Get().vr.m_aimDirect && VRCamera::WorldAimFromTracking( handYaw, handPitch, worldYaw, worldPitch ) )
			{
				s_directAim = true;
				s_directAimTick = GetTickCount();
				pad.rx = 0.0f;
				pad.ry = 0.0f;
				return;
			}
			const float yawError = WrapRadians( wantYaw - camYaw );
			const float pitchError = wantPitch - camPitch;
			const auto drive = []( float error )
			{
				if( std::fabs( error ) < kAimDeadband )
				{
					return 0.0f;
				}
				const float v = error * kAimGain;
				return v > 1.0f ? 1.0f : ( v < -1.0f ? -1.0f : v );
			};
			// Stick right lowers the game yaw (turns right); stick up raises the aim.
			pad.rx = -drive( yawError );
			pad.ry = drive( pitchError ) * ( Config::Get().vr.m_aimInvertPitch ? -1.0f : 1.0f );
#ifdef _DEBUG
			if( GetTickCount() - s_aimLogTick > 500 )
			{
				s_aimLogTick = GetTickCount();
				PSU_LOG( "VRAim: yaw err %.1f pitch err %.1f -> stick (%.2f, %.2f)\n", yawError * 57.29578f,
					pitchError * 57.29578f, pad.rx, pad.ry );
			}
#endif
		}

		// Free aim mode (left stick click): while on, PSU's free aim (R3, camera state 4) follows
		// the gun hand: raised into view = free aim with the reticle, lowered out of view =
		// back to normal targeting. The mode only does anything with a left-hand gun.
		float s_r3Pulse = 0.0f;        // R3 held for the current toggle
		float s_r3Cooldown = 0.0f;     // wait for the camera to follow before toggling again
		// Moving (left stick) or turning (right stick) leaves free aim until the sticks rest.
		float s_stickRest = 1.0f;
		constexpr float kStickAimThreshold = 0.35f;
		constexpr float kStickResumeSeconds = 0.4f;
		constexpr float kR3PulseSeconds = 0.08f;
		constexpr float kR3CooldownSeconds = 0.5f;
		constexpr float kInViewEnterRadians = 30.0f * ( 3.14159265f / 180.0f );
		constexpr float kInViewLeaveRadians = 45.0f * ( 3.14159265f / 180.0f );

		// Angle between where the head looks and the direction to the controller.
		bool AngleFromView( const ControllerState &c, float &angle )
		{
			EyePose views[ 2 ];
			EyePose viewSpace[ 2 ];
			if( !c.active || !c.gripValid || !System::Get().NextViews( views, viewSpace ) )
			{
				return false;
			}
			const float hx = ( views[ 0 ].position[ 0 ] + views[ 1 ].position[ 0 ] ) * 0.5f;
			const float hy = ( views[ 0 ].position[ 1 ] + views[ 1 ].position[ 1 ] ) * 0.5f;
			const float hz = ( views[ 0 ].position[ 2 ] + views[ 1 ].position[ 2 ] ) * 0.5f;
			float dx = c.gripPosition[ 0 ] - hx, dy = c.gripPosition[ 1 ] - hy, dz = c.gripPosition[ 2 ] - hz;
			const float length = std::sqrt( dx * dx + dy * dy + dz * dz );
			if( length < 1.0e-3f )
			{
				return false;
			}
			dx /= length; dy /= length; dz /= length;
			// Head forward: q * (0, 0, -1).
			const float *q = views[ 0 ].orientation;
			const float x = q[ 0 ], y = q[ 1 ], z = q[ 2 ], w = q[ 3 ];
			const float fx = -( 2.0f * ( x * z + w * y ) );
			const float fy = -( 2.0f * ( y * z - w * x ) );
			const float fz = -( 1.0f - 2.0f * ( x * x + y * y ) );
			float dot = dx * fx + dy * fy + dz * fz;
			dot = dot > 1.0f ? 1.0f : ( dot < -1.0f ? -1.0f : dot );
			angle = std::acos( dot );
			return true;
		}

		void ToggleAimMode()
		{
			s_aimModeOn = !s_aimModeOn;
			VRBindings::ShowToast( s_aimModeOn ? "Free aim mode on" : "Free aim mode off" );
			Buzz( 0 );
		}

		// Keeps the game's free aim matching the gun hand while the mode is on (R3 toggles it).
		void AimModeFollow( float dt, PadState &pad )
		{
			if( s_r3Cooldown > 0.0f )
			{
				s_r3Cooldown -= dt;
			}
			if( s_r3Pulse > 0.0f )
			{
				s_r3Pulse -= dt;
				pad.buttons |= Bit( kPadR3 );
				return;
			}
			// The gun that aims: the left hand's, or a two-handed one (the hand it is held in).
			s_aimHand = !s_combatValid ? -1 : PSUCombat::FreeAimCapable( s_combat.leftClass ) ? 0
				: ( PSUCombat::FreeAimCapable( s_combat.weaponClass ) &&
					PSUCombat::ClassifyWeapon( s_combat.weaponClass ) == PSUCombat::WeaponKind::Gun2H
						? PSUCombat::GunHand( s_combat.weaponClass ) : -1 );
			const bool gun = s_aimHand >= 0;
			const bool inGame = VRCamera::CameraState() == 4;
			const auto pushed = []( const ControllerState &c ) {
				return c.stick[ 0 ] * c.stick[ 0 ] + c.stick[ 1 ] * c.stick[ 1 ] > kStickAimThreshold * kStickAimThreshold;
			};
			// Turning the gun model (both grips + right stick) is not moving: free aim stays on.
			s_stickRest = !s_gunAdjusting && ( pushed( VRControllers::State( 0 ) ) || pushed( VRControllers::State( 1 ) ) ) ? 0.0f
				: s_stickRest + dt;
			bool want = false;
			if( s_aimModeOn && gun && !s_ui )
			{
				float angle = 0.0f;
				if( AngleFromView( VRControllers::State( s_aimHand ), angle ) )
				{
					s_gunInView = s_gunInView ? angle < kInViewLeaveRadians : angle < kInViewEnterRadians;
				}
				want = s_gunInView && s_stickRest >= kStickResumeSeconds;
			}
			else
			{
				s_gunInView = false;
			}
			// Mode off (or no gun): leave free aim if we are in it.
			if( want != inGame && s_r3Cooldown <= 0.0f && ( s_aimModeOn || inGame ) && ( gun || inGame ) )
			{
				s_r3Pulse = kR3PulseSeconds;
				s_r3Cooldown = kR3CooldownSeconds;
				pad.buttons |= Bit( kPadR3 );
#ifdef _DEBUG
				PSU_LOG( "VRAim: %s free aim (gun %s view%s)\n", want ? "entering" : "leaving", s_gunInView ? "in" : "out of",
					s_stickRest < kStickResumeSeconds ? ", sticks moving" : "" );
#endif
			}
		}

		// Adds the combat presses to the pad. Returns the roles so the caller can keep the
		// gesture controls (triggers, right grip) from also sending their plain buttons.
		HandRoles Combat( float dt, bool modifier, PadState &pad )
		{
			const HandRoles roles = Roles();
			const auto &vr = Config::Get().vr;
			if( !modifier )
			{
				for( int hand = 0; hand < 2; ++hand )
				{
					const bool gripDown = ( VRControllers::State( hand ).buttons & kControllerGrip ) != 0;
					const bool swung = Swung( hand );  // always run: keeps the armed state current
					const bool clicked = roles.trigger[ hand ] && ( VRControllers::Pressed( hand ) & kControllerTrigger );
					if( !( roles.swing[ hand ] && swung ) && !clicked )
					{
						continue;
					}
					// Photon Art / technique slot 2: that hand's grip held (old scheme: a melee
					// swing with the trigger held).
					const bool byGrip = vr.m_gripPhotonArt || roles.shoot[ hand ];
					const bool pa = vr.m_paGesture && ( byGrip ? gripDown : s_triggerDown[ hand ] );
					if( pa && byGrip && s_grip[ hand ] != Grip::Modifier )
					{
						// The grip was the modifier: it sends no L1 / R1.
						s_grip[ hand ] = Grip::Consumed;
						s_pulse[ hand ] = 0.0f;
					}
					if( roles.shoot[ hand ] )
					{
						// One press = one shot / cast (even if released before the weapon is up).
						s_gunShot = true;
						s_gunShotTime = kGunShotWaitSeconds;
						s_gunPhotonArt = pa;
						VRControllers::Haptic( hand, 0.25f, 0.02f );
					}
					else
					{
						RequestAttack( pa ? kPadY : kPadX, hand, 0,
							clicked ? ( pa ? "grip+trigger" : "trigger" ) : ( pa ? ( byGrip ? "grip+swing" : "swing+trigger" ) : "swing" ) );
					}
				}
			}

			PumpRequest( dt );
			Gun( dt, roles, pad );

			if( s_turning )
			{
				// Stick toward the look direction (forward in the camera's frame) until the
				// character faces it; PSU aims the attack along the stick held as it starts,
				// so the stick stays there through the press.
				s_turnTime += dt;
				float off = 0.0f;
				const bool facing = FacingOff( off ) && std::fabs( off ) <= FaceTolerance() + kFaceLeadRadians;
				if( ( facing && s_turnTime >= kMinTurnSeconds ) || s_turnTime >= kMaxTurnSeconds )
				{
					s_turning = false;
					s_faceHold = kAttackPulseSeconds + kFaceHoldAfterSeconds;
#ifdef _DEBUG
					PSU_LOG( "VRFace: turned in %.2f s (off %.0f deg)\n", s_turnTime, off * 57.29578f );
#endif
				}
				pad.lx = 0.0f;
				pad.ly = 1.0f;
			}
			else if( s_faceHold > 0.0f )
			{
				s_faceHold -= dt;
				pad.lx = 0.0f;
				pad.ly = 1.0f;
			}
			if( s_turning )
			{
				// The press waits for the turn.
			}
			else if( s_pulseTime > 0.0f && s_pulseButton >= 0 )
			{
				s_pulseTime -= dt;
				pad.buttons |= Bit( s_pulseButton );
				if( s_pulseTime <= 0.0f )
				{
					s_pulseButton = -1;
				}
			}
			return roles;
		}

		using VRMotionLayout::Control;
		using VRMotionLayout::Output;

		// Digital state of one layout control.
		bool ControlDown( const ControllerState &c, int hand, Control control )
		{
			switch( control )
			{
				case Control::Trigger: return s_triggerDown[ hand ];
				case Control::StickClick: return ( c.buttons & kControllerStick ) != 0;
				case Control::Primary: return ( c.buttons & kControllerPrimary ) != 0;
				case Control::Secondary: return ( c.buttons & kControllerSecondary ) != 0;
				default: return false;
			}
		}

		uint32_t ControlBit( Control control )
		{
			switch( control )
			{
				case Control::StickClick: return kControllerStick;
				case Control::Primary: return kControllerPrimary;
				case Control::Secondary: return kControllerSecondary;
				default: return 0;
			}
		}

		int PadButtonFor( Output output )
		{
			switch( output )
			{
				case Output::A: return kPadA;
				case Output::B: return kPadB;
				case Output::X: return kPadX;
				case Output::Y: return kPadY;
				case Output::L1: return kPadL1;
				case Output::R1: return kPadR1;
				case Output::L2: return kPadL2;
				case Output::R2: return kPadR2;
				case Output::L3: return kPadL3;
				case Output::R3: return kPadR3;
				case Output::Select: return kPadSelect;
				case Output::Start: return kPadStart;
				default: return -1;
			}
		}

		int ActionFor( Output output )
		{
			switch( output )
			{
				case Output::Recenter: return static_cast< int >( VRAction::Recenter );
				case Output::ToggleCamera: return static_cast< int >( VRAction::ToggleCameraMode );
				case Output::ToggleSettings: return static_cast< int >( VRAction::ToggleSettings );
				case Output::TogglePause: return static_cast< int >( VRAction::TogglePause );
				default: return -1;
			}
		}

		// One grip's state machine. Returns whether L1 / R1 is pressed this frame.
		bool UpdateGrip( int hand, float dt, bool otherGripDown )
		{
			const bool down = ( VRControllers::State( hand ).buttons & kControllerGrip ) != 0;
			Grip &grip = s_grip[ hand ];
			Grip &other = s_grip[ hand ^ 1 ];
			if( s_pulse[ hand ] > 0.0f )
			{
				s_pulse[ hand ] -= dt;
			}
			switch( grip )
			{
				case Grip::Idle:
					if( down )
					{
						grip = Grip::Pending;
						s_gripTime[ hand ] = 0.0f;
						// The other grip went down just before: a VR chord.
						if( other == Grip::Pending )
						{
							grip = Grip::Modifier;
							other = Grip::Modifier;
							Buzz( 0 );
							Buzz( 1 );
						}
					}
					break;
				case Grip::Pending:
					s_gripTime[ hand ] += dt;
					if( !down )
					{
						grip = Grip::Idle;
						s_pulse[ hand ] = kTapPulse;
					}
					else if( s_gripTime[ hand ] >= kGripDelay )
					{
						grip = Grip::Held;
					}
					break;
				case Grip::Held:
				case Grip::Consumed:
					if( !down )
					{
						grip = Grip::Idle;
					}
					break;
				case Grip::Modifier:
					// The chord lasts until both grips are released.
					if( !down && !otherGripDown )
					{
						grip = Grip::Idle;
						other = Grip::Idle;
					}
					break;
			}
			return grip == Grip::Held || s_pulse[ hand ] > 0.0f;
		}

		uint32_t DpadButtons( float x, float y )
		{
			uint32_t buttons = 0;
			if( x * x + y * y < kDpadThreshold * kDpadThreshold )
			{
				return 0;
			}
			// 8-way: a direction counts within 67.5 degrees of its axis.
			const float angle = std::atan2( y, x );  // 0 = right, +pi/2 = up
			constexpr float kHalfSector = 67.5f * ( 3.14159265f / 180.0f );
			const auto within = [ & ]( float target )
			{
				float d = angle - target;
				while( d > 3.14159265f ) d -= 6.2831853f;
				while( d < -3.14159265f ) d += 6.2831853f;
				return std::fabs( d ) < kHalfSector;
			};
			if( within( 0.0f ) ) buttons |= Bit( kPadRight );
			if( within( 1.5707963f ) ) buttons |= Bit( kPadUp );
			if( within( 3.14159265f ) ) buttons |= Bit( kPadLeft );
			if( within( -1.5707963f ) ) buttons |= Bit( kPadDown );
			return buttons;
		}

		LONG AxisValue( float value, LONG minimum, LONG maximum )
		{
			const float center = ( static_cast< float >( minimum ) + static_cast< float >( maximum ) ) * 0.5f;
			const float half = ( static_cast< float >( maximum ) - static_cast< float >( minimum ) ) * 0.5f;
			return static_cast< LONG >( std::lround( center + value * half ) );
		}

		// Larger deflection from the centre wins (so a physical pad still works alongside).
		void MergeAxis( LONG &axis, float value, LONG minimum, LONG maximum )
		{
			if( value == 0.0f || maximum <= minimum )
			{
				return;
			}
			const LONG ours = AxisValue( value, minimum, maximum );
			const double center = ( static_cast< double >( minimum ) + static_cast< double >( maximum ) ) * 0.5;
			if( std::fabs( ours - center ) > std::fabs( axis - center ) )
			{
				axis = ours;
			}
		}

		LONG &AxisRef( DIJOYSTATE2 &state, int index )
		{
			switch( index )
			{
				case 0: return state.lX;
				case 1: return state.lY;
				case 2: return state.lZ;
				case 3: return state.lRx;
				case 4: return state.lRy;
				case 5: return state.lRz;
				case 6: return state.rglSlider[ 0 ];
				default: return state.rglSlider[ 1 ];
			}
		}
	}

	namespace VRSyntheticPad
	{
		// ---- Voice button script (item use through the palette) ------------------------------
		// Steps: hold:<btn> (held to the end), wait:open (palette shown), tap:<btn> / tap:slot,
		// wait:<seconds>. Runs on the virtual pad, so the game sees plain presses.
		struct ScriptStep
		{
			enum Kind { Hold, WaitOpen, Tap, Wait } kind = Wait;
			int button = -1;
			float time = 0.0f;
		};
		std::vector< ScriptStep > s_script;
		size_t s_scriptIndex = 0;
		float s_scriptTimer = 0.0f;
		bool s_scriptStepStarted = false;
		uint32_t s_scriptHeld = 0;
		float s_scriptTotal = 0.0f;

		int ScriptButtonId( std::string name )
		{
			for( char &c : name )
			{
				c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
			}
			static const char *const names[ kPadButtonCount ] = {
				"a", "b", "x", "y", "l1", "r1", "l2", "r2", "l3", "r3", "select", "start", "up", "right", "down", "left" };
			for( int i = 0; i < kPadButtonCount; ++i )
			{
				if( name == names[ i ] )
				{
					return i;
				}
			}
			return -1;
		}

		void ScriptFinish( const char *why )
		{
			PSU_LOG( "VRVoice: pad script %s\n", why );
			s_script.clear();
			s_scriptIndex = 0;
			s_scriptStepStarted = false;
			s_scriptHeld = 0;
		}

		// Adds the script's pad buttons for this frame.
		void RunScript( float dt, PadState &pad )
		{
			if( s_script.empty() )
			{
				return;
			}
			s_scriptTotal += dt;
			if( s_scriptTotal > 6.0f )
			{
				ScriptFinish( "timed out" );
				return;
			}
			// Holds complete at once; the first timed step ends the frame's work.
			for( int guard = 0; guard < 8 && s_scriptIndex < s_script.size(); ++guard )
			{
				const ScriptStep step = s_script[ s_scriptIndex ];
				if( !s_scriptStepStarted )
				{
					s_scriptStepStarted = true;
					s_scriptTimer = step.kind == ScriptStep::Tap ? 0.24f : step.time;
					if( step.kind == ScriptStep::Hold )
					{
						s_scriptHeld |= Bit( step.button );
					}
				}
				bool done = false;
				switch( step.kind )
				{
					case ScriptStep::Hold:
						done = true;
						break;
					case ScriptStep::WaitOpen:
						s_scriptTimer -= dt;
						if( s_combatValid && s_combat.paletteOpen )
						{
							done = true;
						}
						else if( s_scriptTimer <= 0.0f )
						{
							ScriptFinish( "aborted: the palette never opened" );
							return;
						}
						break;
					case ScriptStep::Tap:
						// 0.12 s down, 0.12 s up.
						s_scriptTimer -= dt;
						if( s_scriptTimer > 0.12f )
						{
							pad.buttons |= Bit( step.button );
						}
						done = s_scriptTimer <= 0.0f;
						break;
					case ScriptStep::Wait:
						s_scriptTimer -= dt;
						done = s_scriptTimer <= 0.0f;
						break;
				}
				if( !done )
				{
					break;
				}
				s_scriptStepStarted = false;
				++s_scriptIndex;
				if( step.kind != ScriptStep::Hold )
				{
					break;
				}
			}
			if( s_scriptIndex >= s_script.size() )
			{
				ScriptFinish( "done" );
				return;
			}
			pad.buttons |= s_scriptHeld;
		}

		// Parses one script. slotButton: what "tap:slot" presses. False on a bad token.
		bool ScriptParse( const std::string &text, int slotButton, std::vector< ScriptStep > &out )
		{
			out.clear();
			size_t pos = 0;
			while( pos < text.size() )
			{
				const size_t end = text.find( ' ', pos );
				const std::string token = text.substr( pos, end == std::string::npos ? std::string::npos : end - pos );
				pos = end == std::string::npos ? text.size() : end + 1;
				if( token.empty() )
				{
					continue;
				}
				const size_t colon = token.find( ':' );
				const std::string verb = token.substr( 0, colon );
				const std::string arg = colon == std::string::npos ? std::string() : token.substr( colon + 1 );
				ScriptStep step;
				if( verb == "hold" || verb == "tap" )
				{
					step.kind = verb == "hold" ? ScriptStep::Hold : ScriptStep::Tap;
					step.button = arg == "slot" ? slotButton : ScriptButtonId( arg );
					if( step.button < 0 )
					{
						return false;
					}
				}
				else if( verb == "wait" && arg == "open" )
				{
					step.kind = ScriptStep::WaitOpen;
					step.time = 1.5f;
				}
				else if( verb == "wait" )
				{
					step.kind = ScriptStep::Wait;
					step.time = static_cast< float >( atof( arg.c_str() ) );
				}
				else
				{
					return false;
				}
				out.push_back( step );
			}
			return !out.empty();
		}

		void Update()
		{
			const float dt = FrameSeconds();
			// Also with the gamepad: the buzz needs only an active right controller.
			{
				VRPerf::Scope perf( VRPerf::Stage::PadCombo );
				ComboFeedback();
			}
			const bool motion = Config::Get().vr.m_inputMode == "motion";
			if( !motion || !VRControllers::AnyActive() )
			{
				s_grip[ 0 ] = s_grip[ 1 ] = Grip::Idle;
				s_pulse[ 0 ] = s_pulse[ 1 ] = 0.0f;
				s_dpadMode = false;
				s_leftClickConsumed = false;
				s_leftClickL3 = false;
				s_pendingButton = -1;
				s_pulseButton = -1;
				s_pulseTime = 0.0f;
				s_turning = false;
				s_faceHold = 0.0f;
				s_gunShot = false;
				s_gunTail = 0.0f;
				s_r3Pulse = 0.0f;
				s_script.clear();
				s_scriptStepStarted = false;
				s_scriptHeld = 0;
				VRSpeech::EndTalk();
				std::lock_guard lock( s_mutex );
				s_active = false;
				s_out = {};
				return;
			}
			if( !s_mapLoaded || GetTickCount() - s_iniCheckTick > 3000 )
			{
				s_iniCheckTick = GetTickCount();
				LoadMap();
			}

			const ControllerState &left = VRControllers::State( 0 );
			const ControllerState &right = VRControllers::State( 1 );
			const bool leftGripDown = ( left.buttons & kControllerGrip ) != 0;
			const bool rightGripDown = ( right.buttons & kControllerGrip ) != 0;
			const ControllerState *hands[ 2 ] = { &left, &right };
			// Triggers with a little hysteresis.
			for( int hand = 0; hand < 2; ++hand )
			{
				const float value = hands[ hand ]->trigger;
				s_triggerDown[ hand ] = s_triggerDown[ hand ] ? value > 0.4f : value > 0.6f;
			}
			// Speech to text is hold-to-talk: releasing left Square (anywhere) ends the utterance.
			if( VRSpeech::Talking() && !( left.buttons & ControlBit( Control::Primary ) ) )
			{
				VRSpeech::EndTalk();
			}

			// Menus switch the left stick to the D-pad; a context change drops the manual flip.
			const bool ui = UiContext();
			if( ui != s_ui )
			{
				s_ui = ui;
				s_dpadMode = false;
#ifdef _DEBUG
				PSU_LOG( "VRSyntheticPad: %s (left stick = %s)\n", ui ? "menu input" : "field input",
					DpadActive() ? "D-pad" : "move" );
#endif
			}

			// Both grips + left stick click toggles the D-pad. Left grip alone + left stick click is
			// L3 (that grip sends no L1).
			if( !s_leftClickConsumed && ( VRControllers::Pressed( 0 ) & kControllerStick ) && leftGripDown )
			{
				s_leftClickConsumed = true;
				if( s_grip[ 0 ] == Grip::Modifier && s_grip[ 1 ] == Grip::Modifier )
				{
					s_dpadMode = !s_dpadMode;
					VRBindings::ShowToast( DpadActive() ? "D-pad on" : "D-pad off" );
				}
				else if( s_grip[ 1 ] != Grip::Modifier )
				{
					s_leftClickL3 = true;
					s_grip[ 0 ] = Grip::Consumed;
					s_pulse[ 0 ] = 0.0f;
				}
				Buzz( 0 );
			}
			if( s_leftClickConsumed && !( left.buttons & kControllerStick ) )
			{
				s_leftClickConsumed = false;
				s_leftClickL3 = false;
			}

			const bool l1 = UpdateGrip( 0, dt, rightGripDown );
			const bool r1 = UpdateGrip( 1, dt, leftGripDown );
			const bool modifier = s_grip[ 0 ] == Grip::Modifier;
			// The right controller points at a PSUEx window (VR settings): the game gets nothing but
			// the both-grip chords (to close it, recenter, ...).
			const bool uiOwns = VRUiPointer::Wanted();

			PadState pad;
			const HandRoles roles = Roles();
			// In free aim (manual aim on) L1 / R1 are the scan: the grips send nothing there, so
			// left grip + trigger is only the gun's Photon Art (Y). Scan stays on R3.
			const bool freeAim = s_aimModeOn && VRCamera::CameraState() == 4;
			if( l1 && !modifier && !freeAim ) pad.buttons |= Bit( kPadL1 );
			// Grip = Photon Art: the right grip is the right weapon's modifier, not R1 (the left
			// weapon is used from the left hand).
			const bool rightGripPa = Config::Get().vr.m_gripPhotonArt && roles.Any( 1 );
			if( r1 && !modifier && !freeAim && !rightGripPa ) pad.buttons |= Bit( kPadR1 );
			if( s_leftClickL3 ) pad.buttons |= Bit( kPadL3 );
			for( const VRMotionLayout::Entry &entry : VRMotionLayout::kLayout )
			{
				if( entry.control == Control::Grip || entry.control == Control::Stick )
				{
					continue;
				}
				const ControllerState &c = *hands[ entry.hand ];
				if( uiOwns && !modifier )
				{
					continue;
				}
				if( entry.hand == 0 && entry.control == Control::StickClick && s_leftClickConsumed )
				{
					continue;
				}
				// Both grips + both triggers: hand angle calibration (once, when the second
				// trigger goes down). Triggers send nothing else in the chord.
				if( modifier && entry.control == Control::Trigger )
				{
					if( entry.chord == Output::CalibrateHands && entry.hand == 1 &&
						( ( VRControllers::Pressed( 0 ) & kControllerTrigger ) || ( VRControllers::Pressed( 1 ) & kControllerTrigger ) ) &&
						( hands[ 0 ]->buttons & kControllerTrigger ) && ( hands[ 1 ]->buttons & kControllerTrigger ) )
					{
						++Config::Get().vr.m_handCalibRequest;
						VRBindings::ShowToast( "Hand calibration: arms forward, fists, palms facing each other. Keep still 5 s" );
						PSU_LOG( "VRSyntheticPad: hand calibration requested\n" );
					}
					continue;
				}
				// A hand's trigger is its attack (or, old scheme, its melee Photon Art modifier).
				if( entry.control == Control::Trigger && roles.Any( entry.hand ) )
				{
					continue;
				}
				Output output = modifier ? entry.chord : entry.plain;
				// Free aim mode with the gun up (in view, game in free aim): the right stick
				// click is the scan (R1) instead of START.
				if( output == Output::Start && s_aimModeOn && s_gunInView && VRCamera::CameraState() == 4 )
				{
					output = Output::R1;
				}
				// Square / Triangle: X / Y while a menu or the palette has the input; in the field the
				// chat log (the game's R2 with vr.triangleChatL2, else the PSUEx window) and speech to text.
				if( output == Output::ChatLog || output == Output::SpeechToText )
				{
					const bool menu = s_ui || ( s_combatValid && s_combat.paletteOpen );
					if( menu )
					{
						output = output == Output::ChatLog ? Output::Y : Output::X;
					}
					else if( output == Output::ChatLog && Config::Get().vr.m_triangleChatL2 )
					{
						output = Output::R2;
					}
					else
					{
						if( VRControllers::Pressed( entry.hand ) & ControlBit( entry.control ) )
						{
							if( output == Output::ChatLog )
							{
								VRBindings::RequestChatLog();
							}
							else
							{
								VRSpeech::BeginTalk();
							}
							Buzz( entry.hand );
						}
						continue;
					}
				}
				const int button = PadButtonFor( output );
				if( button >= 0 )
				{
					if( ControlDown( c, entry.hand, entry.control ) )
					{
						pad.buttons |= Bit( button );
					}
					continue;
				}
				if( output == Output::ToggleControllerLabels )
				{
					if( VRControllers::Pressed( entry.hand ) & ControlBit( entry.control ) )
					{
						auto &vr = Config::Get().vr;
						vr.m_controllerLabels = !vr.m_controllerLabels;
						Config::Get().SaveToFile();
						VRBindings::ShowToast( vr.m_controllerLabels ? "Controller labels on (look down)" : "Controller labels off" );
						Buzz( 0 );
						Buzz( 1 );
					}
					continue;
				}
				if( output == Output::FreeAimMode )
				{
					if( VRControllers::Pressed( entry.hand ) & ControlBit( entry.control ) )
					{
						ToggleAimMode();
					}
					continue;
				}
				// VR actions fire once, on the press.
				const int action = ActionFor( output );
				if( action >= 0 && ( VRControllers::Pressed( entry.hand ) & ControlBit( entry.control ) ) )
				{
					s_actions |= 1u << action;
					Buzz( 0 );
					Buzz( 1 );
				}
			}
			if( DpadActive() )
			{
				pad.buttons |= DpadButtons( left.stick[ 0 ], left.stick[ 1 ] );
			}
			else
			{
				pad.lx = Deadzone( left.stick[ 0 ], 0.15f );
				pad.ly = Deadzone( left.stick[ 1 ], 0.15f );
			}
			// Walking in the room walks the character (the player's own stick takes over).
			{
				float roomX = 0.0f, roomY = 0.0f;
				const bool userIdle = pad.lx == 0.0f && pad.ly == 0.0f;
				VRPerf::Scope perf( VRPerf::Stage::PadRoomWalk );
				if( VRCamera::RoomWalkStick( userIdle && !s_ui && !DpadActive(), roomX, roomY ) )
				{
					pad.lx = roomX;
					pad.ly = roomY;
				}
			}
			{
				VRPerf::Scope perf( VRPerf::Stage::PadCombat );
				Combat( dt, modifier, pad );
			}
			pad.rx = Deadzone( right.stick[ 0 ], 0.15f );
			pad.ry = Deadzone( right.stick[ 1 ], 0.15f );
			if( uiOwns )
			{
				pad = PadState{};
			}
			// Free aim, both grips held: the right stick turns the gun model onto the reticle
			// (saved when the grips are let go). The stick sends nothing to the game meanwhile.
			{
				int aimHand = 0;
				float *adjustPitch = nullptr, *adjustYaw = nullptr;
				const bool haveFields = GunAdjustFields( adjustPitch, adjustYaw );
				const bool aiming = Config::Get().vr.m_aimLeash && GunAiming( aimHand ) && haveFields;
				// Both grips down (held in any order; in free aim they send nothing else).
				const bool bothGrips = leftGripDown && rightGripDown;
				s_gunAdjusting = aiming && bothGrips;
				if( s_gunAdjusting )
				{
					constexpr float kDegreesPerSecond = 25.0f;
					const auto clamp = []( float v ) { return v < -60.0f ? -60.0f : ( v > 60.0f ? 60.0f : v ); };
					if( pad.rx != 0.0f || pad.ry != 0.0f )
					{
						*adjustYaw = clamp( *adjustYaw - pad.rx * kDegreesPerSecond * dt );
						*adjustPitch = clamp( *adjustPitch + pad.ry * kDegreesPerSecond * dt );
						s_gunAdjustDirty = true;
					}
					pad.rx = 0.0f;
					pad.ry = 0.0f;
				}
				else if( s_gunAdjustDirty && !bothGrips )
				{
					s_gunAdjustDirty = false;
					Config::Get().SaveToFile();
					static char s_text[ 96 ];
					sprintf_s( s_text, "Gun angle saved (up %.0f, left %.0f)", adjustPitch ? *adjustPitch : 0.0f,
						adjustYaw ? *adjustYaw : 0.0f );
					VRBindings::ShowToast( s_text );
				}
			}
			{
				VRPerf::Scope perf( VRPerf::Stage::PadAim );
				FreeAim( pad );
				AimModeFollow( dt, pad );
			}
			RunScript( dt, pad );

			std::lock_guard lock( s_mutex );
			s_out = pad;
			s_active = true;
			s_updateTick = GetTickCount();
		}

		bool Active()
		{
			std::lock_guard lock( s_mutex );
			// Stale when VR frames stop (runtime paused or lost).
			return s_active && GetTickCount() - s_updateTick < 250;
		}

		bool GunAiming( int &hand )
		{
			hand = s_aimHand < 0 ? 0 : s_aimHand;
			return s_aimModeOn && s_gunInView && VRCamera::CameraState() == 4;
		}

		bool GunAdjust( float &pitchDegrees, float &yawDegrees )
		{
			float *pitch = nullptr, *yaw = nullptr;
			if( !GunAdjustFields( pitch, yaw ) )
			{
				return false;
			}
			pitchDegrees = *pitch;
			yawDegrees = *yaw;
			return true;
		}

		bool DirectAim( float &yaw, float &pitch )
		{
			if( !s_directAim || GetTickCount() - s_directAimTick > 200 || VRCamera::CameraState() != 4 )
			{
				return false;
			}
			yaw = s_lastWantYaw;
			pitch = Config::Get().vr.m_aimInvertPitch ? -s_lastWantPitch : s_lastWantPitch;
			return true;
		}

		bool LastFreeAim( float &yaw, float &pitch )
		{
			yaw = s_lastWantYaw;
			pitch = s_lastWantPitch;
			return s_freeAim;
		}

		void DescribeControls( int gripMask, HandLabels out[ 2 ] )
		{
			out[ 0 ] = {};
			out[ 1 ] = {};
			const auto &vr = Config::Get().vr;
			const auto set = [ & ]( int hand, Control control, const char *text )
			{
				out[ hand ].control[ static_cast< int >( control ) ] = text;
			};

			// Both grips: the VR chords (VRMotionLayout::kLayout plus the rules in Update).
			if( gripMask == 3 )
			{
				for( const VRMotionLayout::Entry &entry : VRMotionLayout::kLayout )
				{
					if( entry.chord != Output::None && entry.control != Control::Trigger )
					{
						set( entry.hand, entry.control, VRMotionLayout::OutputName( entry.chord ) );
					}
				}
				set( 0, Control::Trigger, "Both triggers: calibrate hands" );
				set( 1, Control::Trigger, "Both triggers: calibrate hands" );
				int aimHand = 0;
				if( vr.m_aimLeash && GunAiming( aimHand ) )
				{
					set( 1, Control::Stick, "Gun model angle" );
				}
				return;
			}

			const bool menu = s_ui || ( s_combatValid && s_combat.paletteOpen );
			const HandRoles roles = Roles();
			const bool freeAim = s_aimModeOn && VRCamera::CameraState() == 4;
			const uint32_t rightCat = s_combatValid ? s_combat.rightCategory : 0;
			const uint32_t leftCat = s_combatValid ? s_combat.leftCategory : 0;
			const bool rightTech = rightCat == 3 || rightCat == 8;
			const bool leftTech = leftCat == 0xB || ( leftCat == 0 && rightCat == 3 );

			for( int hand = 0; hand < 2; ++hand )
			{
				const bool gripHeld = ( gripMask & ( 1 << hand ) ) != 0;
				// Grip + attack is the Photon Art (technique slot 2 / 4) when the grip is the modifier.
				const bool byGrip = vr.m_gripPhotonArt || roles.shoot[ hand ];
				const bool pa = gripHeld && byGrip && vr.m_paGesture;
				const bool tech = hand == 1 ? rightTech : leftTech;
				const char *attack = nullptr;
				if( roles.Any( hand ) )
				{
					if( tech )
					{
						attack = hand == 1 ? ( pa ? "Technique slot 2" : "Technique slot 1" ) : ( pa ? "Technique slot 4" : "Technique slot 3" );
					}
					else if( roles.shoot[ hand ] )
					{
						attack = pa ? "Photon Art (shot)" : "Fire";
					}
					else
					{
						attack = pa ? "Photon Art" : "Attack";
					}
				}
				if( roles.swing[ hand ] )
				{
					out[ hand ].swing = attack;
				}
				if( roles.trigger[ hand ] )
				{
					set( hand, Control::Trigger, attack );
				}
				else
				{
					set( hand, Control::Trigger, hand == 0 ? "L2" : "R2" );
				}
			}

			// Grips.
			if( freeAim )
			{
				set( 0, Control::Grip, "Hold + trigger: gun Photon Art" );
				set( 1, Control::Grip, nullptr );
			}
			else
			{
				set( 0, Control::Grip, roles.Any( 0 ) && ( vr.m_gripPhotonArt || roles.shoot[ 0 ] )
					? "L1 (hold + attack: Photon Art)" : "L1 (lock on)" );
				set( 1, Control::Grip, vr.m_gripPhotonArt && roles.Any( 1 )
					? "Hold + attack: Photon Art" : "R1" );
			}

			// Sticks.
			set( 0, Control::Stick, DpadActive() ? "D-pad" : "Move" );
			set( 1, Control::Stick, "Turn / camera" );
			if( gripMask == 1 )
			{
				set( 0, Control::StickClick, "L3" );
			}
			else
			{
				set( 0, Control::StickClick, s_aimModeOn ? "Free aim mode off" : "Free aim mode on" );
			}
			set( 1, Control::StickClick, ( s_aimModeOn && s_gunInView && VRCamera::CameraState() == 4 ) ? "Scan (R1)" : "Main menu (START)" );

			// Face buttons.
			set( 1, Control::Primary, menu ? "Confirm" : "Action / talk / confirm" );
			set( 1, Control::Secondary, menu ? "Cancel" : "Cancel (hold: palette)" );
			if( menu )
			{
				set( 0, Control::Primary, "X" );
				set( 0, Control::Secondary, "Y" );
			}
			else
			{
				set( 0, Control::Primary, vr.m_speechToText ? "Hold: talk / tap: voice mode" : "Speech to text (off)" );
				set( 0, Control::Secondary, vr.m_triangleChatL2 ? "R2 (chat log)" : "Chat log" );
			}
		}

		unsigned TakeActions()
		{
			const unsigned actions = s_actions;
			s_actions = 0;
			return actions;
		}

		bool UseItemSlot( int slot )
		{
			if( slot < 0 || slot > 3 || !s_active || !s_script.empty() || !s_combatValid || s_combat.paletteOpen || s_ui )
			{
				return false;
			}
			const auto &vr = Config::Get().vr;
			// voiceItemButtons: "Up,Right,Down,Left" = the pad button of items one..four.
			const std::string &list = vr.m_voiceItemButtons;
			std::string button;
			size_t pos = 0;
			for( int i = 0; i <= slot; ++i )
			{
				const size_t end = list.find( ',', pos );
				button = pos <= list.size() ? list.substr( pos, end == std::string::npos ? std::string::npos : end - pos ) : std::string();
				pos = end == std::string::npos ? list.size() + 1 : end + 1;
			}
			std::vector< ScriptStep > steps;
			if( !ScriptParse( vr.m_voiceItemScript, ScriptButtonId( button ), steps ) )
			{
				PSU_LOG( "VRVoice: bad voiceItemScript \"%s\" / button \"%s\"\n", vr.m_voiceItemScript.c_str(), button.c_str() );
				return false;
			}
			s_script = std::move( steps );
			s_scriptIndex = 0;
			s_scriptStepStarted = false;
			s_scriptHeld = 0;
			s_scriptTotal = 0.0f;
			PSU_LOG( "VRVoice: pad script \"%s\" (item %d = %s)\n", vr.m_voiceItemScript.c_str(), slot + 1, button.c_str() );
			return true;
		}

		bool CastTechSlot( int slot )
		{
			if( slot < 0 || slot > 3 || !s_active )
			{
				return false;
			}
			const HandRoles roles = Roles();
			if( slot < 2 )
			{
				// Right staff / rod: X = slot 1, Y = slot 2, like a swing / grip + swing.
				if( !roles.swing[ 1 ] || ( s_combat.rightCategory != 3 && s_combat.rightCategory != 8 ) )
				{
					return false;
				}
				s_lastAttackHand = 1;
				RequestAttack( slot == 0 ? kPadX : kPadY, 1, 0, "voice" );
				return true;
			}
			// Rod slots 3 / 4 and the TCSM: R1 held, then X / Y, through the left-hand cast.
			if( !roles.shoot[ 0 ] )
			{
				return false;
			}
			s_gunShot = true;
			s_gunShotTime = kGunShotWaitSeconds;
			s_gunPhotonArt = slot == 3;
			VRControllers::Haptic( 0, 0.25f, 0.02f );
			return true;
		}

		bool MergeJoystickState( void *statePtr, unsigned size, const long axisMin[ 8 ], const long axisMax[ 8 ], bool replace )
		{
			if( !statePtr || size != sizeof( DIJOYSTATE2 ) || !Active() )
			{
				return false;
			}
			PadState pad;
			PadMap map;
			{
				std::lock_guard lock( s_mutex );
				pad = s_out;
				map = s_map;
			}
			DIJOYSTATE2 &state = *static_cast< DIJOYSTATE2 * >( statePtr );
			if( replace )
			{
				// The device is a VR controller seen through DirectInput: its own buttons
				// and axes would be a second, differently mapped copy of the same input.
				std::memset( &state, 0, sizeof( state ) );
				for( int i = 0; i < 8; ++i )
				{
					AxisRef( state, i ) = AxisValue( 0.0f, axisMin[ i ], axisMax[ i ] );
				}
				for( DWORD &pov : state.rgdwPOV )
				{
					pov = 0xFFFFFFFFu;
				}
			}

			// DirectInput Y grows downward.
			MergeAxis( state.lX, pad.lx, axisMin[ 0 ], axisMax[ 0 ] );
			MergeAxis( state.lY, -pad.ly, axisMin[ 1 ], axisMax[ 1 ] );
			MergeAxis( AxisRef( state, map.rightX ), pad.rx, axisMin[ map.rightX ], axisMax[ map.rightX ] );
			MergeAxis( AxisRef( state, map.rightY ), -pad.ry, axisMin[ map.rightY ], axisMax[ map.rightY ] );

			for( int i = 0; i < kPadButtonCount; ++i )
			{
				if( ( pad.buttons & Bit( i ) ) && map.button[ i ] >= 0 && map.button[ i ] < 128 )
				{
					state.rgbButtons[ map.button[ i ] ] = 0x80;
				}
			}
			// D-pad on the POV hat unless [gamepad1] gives it buttons.
			const bool up = ( pad.buttons & Bit( kPadUp ) ) && map.button[ kPadUp ] < 0;
			const bool down = ( pad.buttons & Bit( kPadDown ) ) && map.button[ kPadDown ] < 0;
			const bool leftDir = ( pad.buttons & Bit( kPadLeft ) ) && map.button[ kPadLeft ] < 0;
			const bool rightDir = ( pad.buttons & Bit( kPadRight ) ) && map.button[ kPadRight ] < 0;
			if( ( up || down || leftDir || rightDir ) && LOWORD( state.rgdwPOV[ 0 ] ) == 0xFFFF )
			{
				const int dx = ( rightDir ? 1 : 0 ) - ( leftDir ? 1 : 0 );
				const int dy = ( up ? 1 : 0 ) - ( down ? 1 : 0 );
				// Hundredths of a degree clockwise from north.
				static const DWORD kPov[ 3 ][ 3 ] = {
					{ 22500, 18000, 13500 },   // dy = -1: down-left, down, down-right
					{ 27000, 0xFFFFFFFFu, 9000 },
					{ 31500, 0, 4500 } };      // dy = +1: up-left, up, up-right
				state.rgdwPOV[ 0 ] = kPov[ dy + 1 ][ dx + 1 ];
			}

			if( !s_loggedMerge )
			{
				s_loggedMerge = true;
				PSU_LOG( "VRSyntheticPad: merging motion controllers into the game's pad (replace=%d, range X %ld..%ld)\n",
					replace ? 1 : 0, axisMin[ 0 ], axisMax[ 0 ] );
			}
			return true;
		}
	}
}
