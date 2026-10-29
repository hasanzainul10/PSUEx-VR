#include "VRSettingsPanel.h"

#include "VRControllerDiagram.h"

#include "VRGamepad.h"
#include "VRSpeechFiles.h"

#include <imgui.h>

#include <Windows.h>
#include <Xinput.h>

#include <cmath>
#include <cstdio>

namespace PSUVR
{
	namespace
	{
		// Rebind capture: which action and whether a key or a pad button is awaited.
		int s_captureAction = -1;
		bool s_captureKey = true;
		bool s_heldAtStart[ 256 ]{};
		unsigned short s_padAtStart = 0;

		bool KeyDown( int vk )
		{
			return ( GetAsyncKeyState( vk ) & 0x8000 ) != 0;
		}

		void BeginCapture( int action, bool key )
		{
			s_captureAction = action;
			s_captureKey = key;
			for( int vk = 0; vk < 256; ++vk )
			{
				s_heldAtStart[ vk ] = KeyDown( vk );
			}
			VRGamepad::Buttons( s_padAtStart );
		}

		// Polls for the binding being captured. Escape cancels.
		void UpdateCapture( VRSettings &settings )
		{
			if( s_captureAction < 0 )
			{
				return;
			}
			for( int vk = 8; vk < 256; ++vk )
			{
				if( vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 )
				{
					continue;
				}
				const bool down = KeyDown( vk );
				if( !down )
				{
					s_heldAtStart[ vk ] = false;
					continue;
				}
				if( s_heldAtStart[ vk ] )
				{
					continue;
				}
				if( vk == VK_ESCAPE )
				{
					s_captureAction = -1;
					return;
				}
				// Modifiers only qualify the main key.
				if( vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
					( vk >= VK_LSHIFT && vk <= VK_RMENU ) || vk == VK_LWIN || vk == VK_RWIN )
				{
					continue;
				}
				if( s_captureKey )
				{
					uint8_t mods = 0;
					if( KeyDown( VK_CONTROL ) ) mods |= kVRModCtrl;
					if( KeyDown( VK_SHIFT ) ) mods |= kVRModShift;
					if( KeyDown( VK_MENU ) ) mods |= kVRModAlt;
					settings.m_bindings[ s_captureAction ].key = vk;
					settings.m_bindings[ s_captureAction ].mods = mods;
					s_captureAction = -1;
					return;
				}
			}
			if( !s_captureKey )
			{
				unsigned short buttons = 0;
				VRGamepad::Buttons( buttons );
				const unsigned short pressed = static_cast< unsigned short >( buttons & ~s_padAtStart ) &
					static_cast< unsigned short >( ~XINPUT_GAMEPAD_BACK );
				s_padAtStart = buttons;
				if( pressed )
				{
					// Lowest newly pressed button.
					settings.m_bindings[ s_captureAction ].pad = static_cast< unsigned short >( pressed & ( ~pressed + 1 ) );
					s_captureAction = -1;
				}
			}
		}

		bool Combo( const char *label, std::string &value, const char *const *ids, const char *const *names, int count )
		{
			int current = 0;
			for( int i = 0; i < count; ++i )
			{
				if( value == ids[ i ] )
				{
					current = i;
				}
			}
			if( ImGui::Combo( label, &current, names, count ) )
			{
				value = ids[ current ];
				return true;
			}
			return false;
		}

		void Help( const char *text )
		{
			ImGui::SameLine();
			ImGui::TextDisabled( "(?)" );
			if( ImGui::IsItemHovered() )
			{
				ImGui::SetTooltip( "%s", text );
			}
		}
	}

	std::string VRKeyName( int virtualKey )
	{
		if( virtualKey <= 0 )
		{
			return "-";
		}
		UINT scan = MapVirtualKeyA( static_cast< UINT >( virtualKey ), MAPVK_VK_TO_VSC );
		LONG param = static_cast< LONG >( scan << 16 );
		switch( virtualKey )
		{
			// Extended keys need bit 24 for the right name.
			case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
			case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
			case VK_DIVIDE: case VK_NUMLOCK:
				param |= 1 << 24;
				break;
			default:
				break;
		}
		char name[ 64 ]{};
		if( scan && GetKeyNameTextA( param, name, sizeof( name ) ) > 0 )
		{
			return name;
		}
		char fallback[ 16 ];
		std::snprintf( fallback, sizeof( fallback ), "VK 0x%02X", virtualKey );
		return fallback;
	}

	std::string VRBindingKeyName( const VRBinding &binding )
	{
		if( binding.key <= 0 )
		{
			return "-";
		}
		std::string name;
		if( binding.mods & kVRModCtrl ) name += "Ctrl + ";
		if( binding.mods & kVRModShift ) name += "Shift + ";
		if( binding.mods & kVRModAlt ) name += "Alt + ";
		return name + VRKeyName( binding.key );
	}

	std::string VRPadName( unsigned short padButton )
	{
		switch( padButton )
		{
			case 0: return "-";
			case XINPUT_GAMEPAD_A: return "Back + A";
			case XINPUT_GAMEPAD_B: return "Back + B";
			case XINPUT_GAMEPAD_X: return "Back + X";
			case XINPUT_GAMEPAD_Y: return "Back + Y";
			case XINPUT_GAMEPAD_START: return "Back + Start";
			case XINPUT_GAMEPAD_LEFT_SHOULDER: return "Back + LB";
			case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "Back + RB";
			case XINPUT_GAMEPAD_LEFT_THUMB: return "Back + L3";
			case XINPUT_GAMEPAD_RIGHT_THUMB: return "Back + R3";
			case XINPUT_GAMEPAD_DPAD_UP: return "Back + D-pad up";
			case XINPUT_GAMEPAD_DPAD_DOWN: return "Back + D-pad down";
			case XINPUT_GAMEPAD_DPAD_LEFT: return "Back + D-pad left";
			case XINPUT_GAMEPAD_DPAD_RIGHT: return "Back + D-pad right";
			default:
			{
				char text[ 24 ];
				std::snprintf( text, sizeof( text ), "Back + 0x%04X", padButton );
				return text;
			}
		}
	}

	// The panel is split into tabs (one screen each in the headset, no long scroll); sections
	// keep their order in the code and say which tab they belong to.
	enum class Tab : int { General, Camera, Hud, Controllers, Combat, Voice, Performance, Keys, Count };
	const char *const kTabNames[] = { "General", "Camera", "HUD", "Controllers", "Combat", "Voice", "Performance", "Keys" };
	static Tab s_tab = Tab::General;

	// A section of the selected tab: a heading, then its widgets.
	static bool Section( Tab tab, const char *title )
	{
		if( s_tab != tab )
		{
			return false;
		}
		ImGui::SeparatorText( title );
		return true;
	}

	void DrawVRSettingsPanel( VRSettings &s, VRPanelContext &context )
	{
		UpdateCapture( s );

		if( !context.status.empty() )
		{
			for( const std::string &line : context.status )
			{
				ImGui::TextUnformatted( line.c_str() );
			}
			ImGui::Separator();
		}

		if( ImGui::BeginTabBar( "vr_settings_tabs", ImGuiTabBarFlags_FittingPolicyResizeDown ) )
		{
			for( int i = 0; i < static_cast< int >( Tab::Count ); ++i )
			{
				if( ImGui::BeginTabItem( kTabNames[ i ] ) )
				{
					s_tab = static_cast< Tab >( i );
					ImGui::EndTabItem();
				}
			}
			ImGui::EndTabBar();
		}
		// The tab scrolls on its own; Save / Reload stay in view below it.
		const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
		ImGui::BeginChild( "vr_settings_tab", ImVec2( 0.0f, -footer ) );

		if( Section( Tab::General, "General" ) )
		{
			ImGui::Checkbox( "VR enabled", &s.m_enabled );
			Help( "Applies on the next game launch (the game's Direct3D device is set up for VR at startup)." );
			if( context.inGame )
			{
				ImGui::Checkbox( "Pause VR", &s.m_paused );
				Help( "Instantly switch the game to flat play; the headset shows it on a big screen. Not saved." );
			}
			ImGui::Checkbox( "Gamepad works when the game isn't focused", &s.m_backgroundInput );
			Help( "With the headset on, the game window often isn't the focused one. Applies on the next game launch." );
			if( s.m_backgroundInput )
			{
				ImGui::Checkbox( "Keyboard too", &s.m_backgroundKeyboard );
				Help( "Keys typed in other apps then also reach the game. Applies on the next game launch." );
			}
			static const char *const cameraIds[] = { "firstPerson", "thirdPerson" };
			static const char *const cameraNames[] = { "First person", "Third person" };
			Combo( "Camera", s.m_cameraMode, cameraIds, cameraNames, 2 );
			ImGui::SliderFloat( "World scale", &s.m_worldScale, 0.5f, 2.0f, "%.2f" );
			ImGui::Checkbox( "VR setup done", &s.m_setupDone );
			Help( "Off: the setup wizard (play position, eye height, hand calibration, controls) opens in the headset "
				"when VR starts with the motion controllers. Finishing or skipping it turns this on." );
			if( context.inGame && context.runSetup && ImGui::Button( "Run VR setup again" ) )
			{
				context.runSetup();
			}
			if( context.inGame && context.openToolkit && ImGui::Button( "Open Rogue's Toolkit menu" ) )
			{
				context.openToolkit();
			}
			ImGui::Checkbox( "Toolkit panels on the VR HUD", &s.m_toolkitOnHud );
			Help( "Rogue's Toolkit's damage meter, drop panel, timers and menu are drawn onto the HUD panel in the headset. "
				"They then no longer show on the flat monitor. Off: monitor only." );
			ImGui::Checkbox( "Pointer drives the toolkit menu", &s.m_toolkitPointer );
			Help( "While the toolkit menu is open, the right controller's pointer moves and clicks the system mouse over it." );
			Help( "Opens the Rogue's Toolkit settings menu (it sends the toolkit's F6 key; this window closes). "
				"Also on the shortcut under Controls (default Ctrl + F6, or Back + A on the pad). "
				"Needs the toolkit installed; set its settings_pad to \"\" so Back + Start only opens VR settings." );
		}

		if( Section( Tab::Camera, "First person" ) )
		{
			static const char *const moveIds[] = { "head", "body" };
			static const char *const moveNames[] = { "Where you look (head)", "Where the body faces" };
			Combo( "Left stick forward", s.m_fpMoveDirection, moveIds, moveNames, 2 );
			static const char *const bodyIds[] = { "headless", "hands", "hidden", "full" };
			static const char *const bodyNames[] = { "Headless (hands, body, legs)", "Hands only", "Hidden", "Full" };
			Combo( "Own body", s.m_fpBody, bodyIds, bodyNames, 4 );
			Help( "Hands only: just the hands and weapons, right on the controllers (no reach limit), while the arms "
				"follow the controllers (otherwise like Headless)." );
			ImGui::Checkbox( "Body faces my head, legs strafe", &s.m_strafeBody );
			Help( "Needs the arms to follow the controllers and a body that shows legs (not Hands only). The body "
				"trails where you look; moving left, right or back turns the legs against the body." );
			if( s.m_strafeBody )
			{
				ImGui::SliderFloat( "Legs toward the body", &s.m_strafeBodyBlend, 0.0f, 1.0f, "%.2f" );
			}
			ImGui::SliderFloat( "Eye height (m)", &s.m_fpEyeHeight, 1.2f, 2.0f, "%.2f" );
			ImGui::SliderFloat( "Eye forward (m)", &s.m_fpEyeForward, 0.0f, 0.3f, "%.2f" );
			ImGui::Checkbox( "Eye follows the walking lean", &s.m_fpEyeFollowsNeck );
			Help( "The eye moves with the animated neck while you walk, so the camera isn't behind the shoulders. "
				"Needs the arms to follow the controllers (first person)." );
			ImGui::Checkbox( "Body in rest pose (no animations)", &s.m_bodyRestPose );
			Help( "Replaces the game's idle, walk and attack animations with the plain skeleton; the arms, torso and legs "
				"are then posed only by VR." );
			if( s.m_bodyRestPose )
			{
				ImGui::Checkbox( "Scale body to my height", &s.m_bodyFitHeight );
				Help( "Scales the model so its eyes are at the Eye height above. Set Eye height to your real eye height." );
				ImGui::Checkbox( "Keep the game's leg animation", &s.m_bodyKeepLegAnim );
				Help( "The pelvis and legs keep the walk / idle animation (the strafe legs turn them); the rest of the body is the plain skeleton." );
				ImGui::Checkbox( "Body follows my head (hips, spine)", &s.m_bodyIk );
				Help( "First person. The hips drop when you crouch and the spine leans toward your head." );
				ImGui::Checkbox( "Planted feet that step", &s.m_bodyProceduralLegs );
				Help( "The feet stay where they are in the world and step to a new spot when the hips move away. Replaces the strafe legs." );
			}
			ImGui::SliderFloat( "Neck cut below eyes (m)", &s.m_fpNeckDrop, 0.05f, 0.5f, "%.2f" );
			ImGui::SliderFloat( "Lean limit (m)", &s.m_fpLeanLimit, 0.0f, 0.5f, "%.2f" );
			ImGui::SliderFloat( "Crouch limit (m)", &s.m_fpCrouchLimit, 0.0f, 0.5f, "%.2f" );
			ImGui::Checkbox( "Walk in the room moves the character", &s.m_roomWalk );
			Help( "Step away from your character and it walks to where you are (the left stick still works and takes "
				"over). Needs the motion controllers (Play with motion controllers)." );
			if( s.m_roomWalk )
			{
				ImGui::SliderFloat( "Room walk starts at (m)", &s.m_roomWalkStart, 0.1f, 0.6f, "%.2f" );
			}
			static const char *const lbIds[] = { "off", "follow" };
			static const char *const lbFpNames[] = { "Off (lock-on only, view stays yours)", "Turn view toward the target" };
			Combo( "LB camera##fp", s.m_fpLbCamera, lbIds, lbFpNames, 2 );
			Help( "Turning the view toward a target rotates the world under you; off is more comfortable." );
			ImGui::SliderFloat( "LB follow time (s)", &s.m_lbWindowSeconds, 0.5f, 5.0f, "%.1f" );
		}

		if( Section( Tab::Camera, "Third person" ) )
		{
			ImGui::TextDisabled( "Right stick: left/right turns, up/down zooms. Hold Back: left/right tilts, up/down height." );
			ImGui::Checkbox( "Snap turn##tp", &s.m_tpSnapTurn );
			if( s.m_tpSnapTurn )
			{
				ImGui::SliderFloat( "Turn step (deg)##tp", &s.m_tpOrbitDegrees, 15.0f, 90.0f, "%.0f" );
			}
			else
			{
				ImGui::SliderFloat( "Turn speed (deg/s)##tp", &s.m_tpTurnSpeed, 30.0f, 360.0f, "%.0f" );
			}
			ImGui::SliderFloat( "Tilt (deg)", &s.m_tpPitch, -60.0f, 30.0f, "%.0f" );
			Help( "Negative looks down at your character. A tilted camera also tilts the horizon." );
			ImGui::SliderFloat( "Tilt speed (deg/s)", &s.m_tpTiltSpeed, 10.0f, 120.0f, "%.0f" );
			static const char *const lbIds[] = { "follow", "off" };
			static const char *const lbTpNames[] = { "Camera behind you / lock-on (vanilla)", "Off" };
			Combo( "LB camera##tp", s.m_tpLbCamera, lbIds, lbTpNames, 2 );
			ImGui::SliderFloat( "Distance (m)", &s.m_tpDistance, s.m_tpMinDistance, s.m_tpMaxDistance, "%.1f" );
			ImGui::SliderFloat( "Height above head (m)", &s.m_tpHeight, -1.0f, 4.0f, "%.1f" );
			ImGui::SliderFloat( "Zoom speed (m/s)", &s.m_tpZoomSpeed, 0.5f, 10.0f, "%.1f" );
			ImGui::SliderFloat( "Closest (m)", &s.m_tpMinDistance, 0.5f, 5.0f, "%.1f" );
			ImGui::SliderFloat( "Farthest (m)", &s.m_tpMaxDistance, 3.0f, 20.0f, "%.1f" );
		}

		if( Section( Tab::General, "Turning" ) )
		{
			ImGui::Checkbox( "Snap turn", &s.m_snapTurn );
			Help( "Snap turn is the comfort default. Smooth turn can cause motion sickness." );
			if( s.m_snapTurn )
			{
				ImGui::SliderFloat( "Snap step (deg)", &s.m_snapTurnDegrees, 15.0f, 90.0f, "%.0f" );
			}
			else
			{
				ImGui::SliderFloat( "Smooth speed (deg/s)", &s.m_fpTurnSpeed, 30.0f, 360.0f, "%.0f" );
			}
		}

		if( Section( Tab::Hud, "HUD and menus" ) )
		{
			ImGui::Checkbox( "Body-locked panel (lazy follow)", &s.m_hudFollow );
			ImGui::Checkbox( "Dark backdrop behind menus", &s.m_menuBackdrop );
			ImGui::Checkbox( "Menu overlays on the menu (scanlines)", &s.m_hudOverlays );
			Help( "Overlays bigger than the screen, like the mission menu's moving scanlines, are drawn on "
				"the menu panel over the menu, as on a flat screen." );
			Help( "Menus and 2D screens float on a dark background. Off: SteamVR's own space shows "
				"around them, and with it the play-area boundary." );
			ImGui::SliderFloat( "Follow angle (deg)", &s.m_hudFollowAngle, 10.0f, 90.0f, "%.0f" );
			ImGui::SliderFloat( "Distance (m)", &s.m_hudDistance, 0.6f, 4.0f, "%.2f" );
			Help( "In the headset: hold the right grip while pointing at this panel and move the hand forward / back." );
			ImGui::SliderFloat( "Size", &s.m_hudSize, 0.5f, 2.0f, "%.2f" );
			ImGui::SliderFloat( "Below eye level (m)", &s.m_hudDrop, -0.3f, 0.6f, "%.2f" );
			ImGui::SliderFloat( "Map backdrop opacity", &s.m_hudMapBlackAlpha, 0.0f, 1.0f, "%.2f" );
			ImGui::Checkbox( "Mission map terrain on the panel", &s.m_hudMapMeshes );
			Help( "Draws the mission map's terrain (its mesh draws) on the HUD panel too. Off: only the "
				"map frame and markers show in the headset." );
			ImGui::Checkbox( "Fit the mission map into its frame", &s.m_hudMapFit );
			Help( "The game sizes the 3D map for the whole screen but draws its frame small at high "
				"resolutions; this shrinks the map and its markers into the frame on the panel." );
			ImGui::SliderFloat( "Fitted map zoom", &s.m_hudMapFitScale, 0.5f, 3.0f, "%.2f" );
			ImGui::SliderFloat( "Nameplate scale", &s.m_uiScale, 1.0f, 2.5f, "%.2f" );
			ImGui::Checkbox( "World-locked kiosk panel (experimental)", &s.m_hudWorldPanel );
		}

		if( Section( Tab::General, "Comfort" ) )
		{
			ImGui::Checkbox( "Vignette while moving / smooth turning", &s.m_comfortVignette );
			ImGui::Checkbox( "Place glows and sprite effects in 3D", &s.m_reprojectSprites );
			ImGui::Checkbox( "Sky infinitely far", &s.m_skyAtEye );
			Help( "The area's sky (galaxy / clouds) is drawn as if infinitely far away, so its edges never show." );
			ImGui::Checkbox( "Sky in the headset", &s.m_eyeSky );
			Help( "Off hides the area's sky in the headset (the flat window keeps it). Some missions' skies are "
				"partial boxes made for the flat camera, and in VR their unfinished sides show." );
			ImGui::Checkbox( "Screen glow (VR)", &s.m_eyeBlur );
			Help( "The game's full-screen glow. It is laid over the whole picture, so in VR an enemy's glow "
				"shows through your weapon. Off by default (the flat window keeps it)." );
			ImGui::Checkbox( "Screen glare (VR)", &s.m_eyeGlare );
			Help( "The game's full-screen glare. In VR it shows as floating rectangles and a glow over your "
				"weapon. Off by default (the flat window keeps it)." );
			ImGui::Checkbox( "Keep screen tints on my view", &s.m_recenterSky );
			Help( "Some missions tint the screen with a sheet in front of the game's camera. It is kept in front "
				"of your eyes instead of showing up as a see-through plane in the room." );
			Help( "Item / combo glows and similar effects the game draws as flat sprites are placed in "
				"3D for each eye. Off: they keep the flat screen's position (doubled, stuck to the view)." );
			ImGui::SliderFloat( "Vignette strength", &s.m_vignetteStrength, 0.0f, 1.0f, "%.2f" );
		}

		if( Section( Tab::Controllers, "Motion controllers" ) )
		{
			bool motion = s.m_inputMode == "motion";
			if( ImGui::Checkbox( "Play with motion controllers", &motion ) )
			{
				s.m_inputMode = motion ? "motion" : "gamepad";
			}
			Help( "The VR controllers act as a gamepad (a connected gamepad keeps working too). "
				"See Controller layout below for every button." );
			if( !context.inGame )
			{
				ImGui::TextDisabled( "Motion controllers: %s (applies when the game starts)", motion ? "on" : "off" );
			}
			ImGui::Checkbox( "Show controllers", &s.m_showControllers );
			ImGui::Checkbox( "Point at PSUEx windows with the right controller", &s.m_uiPointer );
			ImGui::Checkbox( "Controller labels when you look down", &s.m_controllerLabels );
			Help( "A card above each controller lists what its buttons do now (weapon, menu, aim mode). "
				"Both grips + right stick click turns this on / off." );
			ImGui::Checkbox( "Controller labels while a grip is held still", &s.m_controllerLabelsGrip );
			Help( "Hold a grip (or both) without pressing anything else: the cards show what the buttons do with "
				"that grip held. They hide as soon as you use another button, so grip + attack never shows them." );
			ImGui::SliderFloat( "Grip hold before the labels (s)", &s.m_controllerLabelsHoldSeconds, 0.2f, 1.5f, "%.2f" );
			ImGui::SliderFloat( "Label card size (m)", &s.m_controllerLabelsSize, 0.08f, 0.30f, "%.2f" );
			Help( "While this panel is open in the headset: aim the right controller at it, the trigger clicks "
				"(hold it to drag a slider), the right stick scrolls, the right grip held moves the panel nearer / "
				"farther with the hand, and Circle closes the panel. The game gets "
				"no controller input meanwhile, apart from the both-grip chords." );
			ImGui::Checkbox( "Emulate controllers (testing)", &s.m_emulateControllers );
			Help( "For testing without the headset on: Ctrl+Shift+F9 toggles. Ctrl+numpad: 7/8/9 left/both/right, 1-5 rest/forward/gun/"
				"raised/calib, 0 trigger, . grip, / Cross (A), + Circle (B), - stick click, 6 swing, * both grips. Ctrl+T/G/F/H "
				"stick. A command file (psuex_vr_script.txt) runs whenever it changes; see VR_GUIDE.md." );
			ImGui::Checkbox( "Arms follow the controllers (first person)", &s.m_handsFollow );
			Help( "The character's arms reach for where you hold the controllers." );
			if( s.m_handsFollow )
			{
				ImGui::Checkbox( "Stretch arms to reach my hands", &s.m_armStretch );
				Help( "The character's arms are shorter than most players'. On: they stretch (up to half again) so the "
					"hands sit on the controllers. Off: the arm stops at its own length." );
				bool attackAnimation = s.m_handsAttack == "animation";
				if( ImGui::Checkbox( "Play the game's attack animation", &attackAnimation ) )
				{
					s.m_handsAttack = attackAnimation ? "animation" : "controller";
				}
				Help( "Off: your arms stay on the controllers while attacking (your swing is the swing). "
					"On: the arms hand over to the game's attack animation." );
				ImGui::Checkbox( "Play the game's Photon Art / technique animation", &s.m_handsPhotonArtAnimation );
				Help( "The same for grip + swing / trigger (Photon Arts, technique slot 2). Off: your arms stay on "
					"the controllers." );
				ImGui::Checkbox( "Staff / rod stays in the hand while casting", &s.m_techWeaponStill );
				Help( "The staff or rod keeps the hold it had before the cast instead of moving with the casting "
					"animation. The technique itself is unchanged. The TCSM keeps its animation." );
				if( context.inGame )
				{
					if( ImGui::Button( "Calibrate hand angles" ) )
					{
						++s.m_handCalibRequest;
					}
					ImGui::SameLine();
					ImGui::TextDisabled( s.m_handCalibrated ? "(calibrated)" : "(automatic)" );
					Help( "After the click, hold both arms straight out in front, hands in fists around the controllers, "
						"palms facing each other (knuckles forward), and keep still: the angle is taken after 5 seconds "
						"(a buzz each second, a long buzz when done); it is saved. Also on both grips + both triggers. "
						"Without it the angle is estimated, which suits most controllers." );
					if( s.m_handCalibrated && ImGui::Button( "Back to automatic" ) )
					{
						s.m_handCalibrated = false;
					}
				}
				{
					ImGui::SliderFloat( "Hand pitch (deg)", &s.m_handPitch, -90.0f, 90.0f, "%.0f" );
					ImGui::SliderFloat( "Hand roll (deg)", &s.m_handRoll, -180.0f, 180.0f, "%.0f" );
					Help( "Fine tune the hand angle on top of the calibration (tip up / down, twist)." );
				}
			}
			ImGui::Checkbox( "Left stick works menus (D-pad) automatically", &s.m_autoDpad );
			Help( "In menus, shops, the mission counter, login screens and the weapon / item palette the left stick sends the D-pad. "
				"Both grips + left stick click still flips it by hand." );
			ImGui::Checkbox( "Buzz when the next combo hit can be pressed", &s.m_comboHaptic );
			Help( "A short buzz in the right hand the moment the game accepts the next attack of a melee combo." );
		}

		if( Section( Tab::Combat, "Motion combat" ) )
		{
			ImGui::Checkbox( "Motion combat (swing / shoot)", &s.m_swingAttack );
			Help( "Follows what each hand holds: swing or pull the trigger of a melee / technique hand to "
				"attack or cast, pull the trigger of the hand holding a gun to shoot. Two-handed and twin "
				"weapons swing with either hand; with a rod the left hand casts slots 3 / 4. "
				"Needs 'Play with motion controllers'." );
			if( s.m_swingAttack )
			{
				ImGui::Checkbox( "Photon Art gestures", &s.m_paGesture );
				Help( "Hold that hand's grip while you swing or pull the trigger: Photon Art, or technique slot 2." );
				if( s.m_paGesture )
				{
					ImGui::Checkbox( "Grip = Photon Art (trigger also attacks)", &s.m_gripPhotonArt );
					Help( "On: trigger or swing attacks, grip + trigger / swing is the Photon Art; the right grip "
						"sends no R1 while the right weapon has controls. Off: the old scheme: melee attacks by "
						"swing only, swing with the trigger held is the Photon Art." );
				}
				ImGui::Checkbox( "Turn to where I look before attacking", &s.m_attackFaceLook );
				if( s.m_attackFaceLook )
				{
					ImGui::SliderFloat( "Turn only if off by more than (deg)", &s.m_attackFaceTolerance, 5.0f, 90.0f, "%.0f" );
					Help( "Smaller: the character turns more often before an attack. Larger: less turning, "
						"the attack lands a little off where you look." );
				}
				ImGui::Checkbox( "Aim free aim with the gun hand", &s.m_controllerAim );
				Help( "Left stick click turns free aim mode on / off. While on, raising the gun hand into view "
					"enters the game's free aim (reticle) and lowering it leaves it; turning the gun hand turns "
					"the aim by the same angle." );
				if( s.m_controllerAim )
				{
					ImGui::Checkbox( "Invert aim up / down", &s.m_aimInvertPitch );
					ImGui::Checkbox( "Set the aim directly (first person)", &s.m_aimDirect );
					Help( "The game's aim follows the gun hand exactly, with no lag. Off: it is steered toward the "
						"hand like a stick, which trails behind." );
					ImGui::Checkbox( "Shots land on the reticle (leash)", &s.m_aimLeash );
					Help( "The reticle sits on the line your gun hand points along, and every shot is aimed at it, "
						"so what you point at is what you hit (first person). The distance is where shots and "
						"reticle meet exactly; nearer or farther is a little off." );
					if( s.m_aimLeash )
					{
						ImGui::SliderFloat( "Reticle distance (m)", &s.m_aimLeashMeters, 3.0f, 30.0f, "%.0f" );
						ImGui::TextDisabled( "Gun angle (in the headset: both grips + right stick while aiming):" );
						ImGui::SliderFloat( "Handgun up / down", &s.m_gunAdjustHandgunPitch, -60.0f, 60.0f, "%.0f" );
						ImGui::SliderFloat( "Handgun left / right", &s.m_gunAdjustHandgunYaw, -60.0f, 60.0f, "%.0f" );
						ImGui::SliderFloat( "Bow up / down", &s.m_gunAdjustBowPitch, -60.0f, 60.0f, "%.0f" );
						ImGui::SliderFloat( "Bow left / right", &s.m_gunAdjustBowYaw, -60.0f, 60.0f, "%.0f" );
						ImGui::SliderFloat( "Rifle up / down", &s.m_gunAdjustRiflePitch, -60.0f, 60.0f, "%.0f" );
						ImGui::SliderFloat( "Rifle left / right", &s.m_gunAdjustRifleYaw, -60.0f, 60.0f, "%.0f" );
					}
					ImGui::Checkbox( "Reticle shows where the shot goes", &s.m_reticleOnShot );
					Help( "On: the reticle sits on the game's actual aim. Off: on the line the gun hand points along." );
					ImGui::Checkbox( "Gun points at the reticle", &s.m_gunPointsAtAim );
					Help( "In free aim the gun hand turns at the wrist so the barrel points where the shot goes." );
					ImGui::TextDisabled( "Hands and gun in free aim (turn one off if free aim gets slow):" );
					ImGui::Checkbox( "Ignore near-camera hide", &s.m_freeAimNoNearHide );
					ImGui::Checkbox( "Ignore part culling", &s.m_freeAimNoPartCull );
					ImGui::SliderFloat( "Reticle size (degrees)", &s.m_reticleSize, 0.5f, 8.0f, "%.1f" );
				}
				Help( "First person: if the character faces another way (after strafing or backing up), the "
					"first hit turns it toward your view first." );
				ImGui::SliderFloat( "Swing speed (m/s)", &s.m_swingSpeed, 1.0f, 4.0f, "%.1f" );
				Help( "Lower: gentler swings count. Higher: only big swings count." );
				ImGui::Checkbox( "Hold early swings for the combo window", &s.m_comboBuffer );
				Help( "A swing made while the attack is still playing is held, then sent when the next hit can connect." );
				if( s.m_comboBuffer )
				{
					ImGui::Checkbox( "Hold until the Just Attack timing", &s.m_comboBufferJustAttack );
					Help( "On: the held swing goes out on the Just Attack buzz (up to 1.5 s). Off: as soon as the "
						"swing animation ends (up to 0.25 s), not timed for a Just Attack." );
				}
			}
		}

		if( Section( Tab::Controllers, "Buttons" ) )
		{
			ImGui::Checkbox( "Left Triangle = game chat log (R2)", &s.m_triangleChatL2 );
			Help( "On: Left Triangle in the field presses R2, which opens the game's chat log. "
				"Off: it opens the PSUEx chat log window. Y in menus either way." );
			static const char *const profileIds[] = { "psvr2", "generic" };
			static const char *const profileNames[] = { "PlayStation VR2 Sense", "Other (Touch, Index, ...)" };
			Combo( "Controllers", s.m_motionProfile, profileIds, profileNames, 2 );
			if( ImGui::TreeNodeEx( "Controller layout", ImGuiTreeNodeFlags_DefaultOpen ) )
			{
				DrawControllerDiagram( s.m_motionProfile );
				ImGui::TreePop();
			}
		}

		if( Section( Tab::Voice, "Voice" ) )
		{
			ImGui::Checkbox( "Speech to text (hold left Square)", &s.m_speechToText );
			Help( "Hold left Square in the field, talk, let go. Recognition runs on this PC (Moonshine, English) in "
				"PSUExSpeech.exe, which starts with the game while this is on. The microphone stays open while it runs." );
			static const char *const modelIds[] = { "tiny", "small", "medium" };
			static const char *const modelNames[] = { "Tiny (43 MB, fastest)", "Small (135 MB, recommended)", "Medium (256 MB, best)" };
			Combo( "Speech model", s.m_speechModel, modelIds, modelNames, 3 );
			if( !VRSpeechFiles::HelperInstalled() )
			{
				ImGui::TextDisabled( "PSUExSpeech\\PSUExSpeech.exe is missing from the game folder." );
			}
			else if( VRSpeechFiles::ModelInstalled( s.m_speechModel ) )
			{
				ImGui::TextDisabled( "Model installed." );
			}
			else
			{
				ImGui::TextDisabled( "Model not installed." );
				ImGui::SameLine();
				if( ImGui::Button( "Download" ) )
				{
					context.message = VRSpeechFiles::LaunchDownload( s.m_speechModel ) ?
						"Downloading the speech model (see its window)." : "Couldn't start the download.";
				}
				Help( "Fetches the model from download.moonshine.ai into PSUExSpeech\\models. Needs internet once." );
			}
			ImGui::Checkbox( "Send what I say to the game chat", &s.m_speechChat );
			Help( "Opens the chat box, types the text and presses Enter, like the keyboard would. "
				"Off: the text is only shown as a toast." );
			ImGui::Checkbox( "Voice commands (say a technique to cast it)", &s.m_voiceCommands );
			Help( "With a rod, staff or TCSM: saying the name of a technique set on the weapon (\"Barta\") casts it from "
				"its slot, like a swing (slot 1) or grip + swing (slot 2). \"Slot one\" .. \"slot four\" also work. "
				"Anything else goes to the chat (with the held button). Needs motion combat." );
			ImGui::Checkbox( "Voice items (say \"item two\" to use palette slot 2)", &s.m_voiceItems );
			Help( "Experimental: opens the item palette through the virtual pad, picks the slot and releases. The button sequence is voiceItemScript / voiceItemButtons in config.json (a guess until tested). Item names (\"Monomate\") are not supported yet." );
			ImGui::Checkbox( "Always listening (no button)", &s.m_speechAlwaysOn );
			Help( "The microphone is cut into sentences by itself, so you can just talk. The held button still works. "
				"Needs speech to text. A noisy room or loud game audio can set it off: it then only acts when the words "
				"match a command." );
			static const char *const routeIds[] = { "auto", "command", "chat" };
			static const char *const routeNames[] = { "Auto (command in battle)", "Command", "Chat" };
			Combo( "Always-on speech goes to", s.m_voiceRoute, routeIds, routeNames, 3 );
			Help( "Auto: with a weapon out in the field it is matched to a technique, anything else is ignored. "
				"Command: a technique, nothing else. Chat: everything you say goes to the game chat. "
				"Tap left Square (a short press) to cycle these in the game. The held button always falls back to the chat." );
		}

		if( Section( Tab::Performance, "Performance" ) )
		{
			// Presets only write the settings below; whatever doesn't match one reads as Custom.
			struct Preset { const char *name; float scale; bool blur, glare, sprites; };
			static const Preset presets[] = { { "Low", 0.7f, false, false, false }, { "Medium", 0.85f, false, false, true },
				{ "High", 1.0f, true, false, true } };
			int preset = 3;
			for( int i = 0; i < 3; ++i )
			{
				if( std::fabs( s.m_eyeResolutionScale - presets[ i ].scale ) < 0.005f && s.m_eyeBlur == presets[ i ].blur &&
					s.m_eyeGlare == presets[ i ].glare && s.m_reprojectSprites == presets[ i ].sprites )
				{
					preset = i;
				}
			}
			static const char *const presetNames[] = { "Low", "Medium", "High", "Custom" };
			if( ImGui::Combo( "Quality", &preset, presetNames, 4 ) && preset < 3 )
			{
				s.m_eyeResolutionScale = presets[ preset ].scale;
				s.m_eyeBlur = presets[ preset ].blur;
				s.m_eyeGlare = presets[ preset ].glare;
				s.m_reprojectSprites = presets[ preset ].sprites;
			}
			Help( "Low: eye resolution 0.7, no screen glow / glare, glows and sprites not placed in 3D. Medium: 0.85, no "
				"screen glow / glare. High: the defaults. Changing one of those settings by hand shows Custom." );
			ImGui::SliderFloat( "Eye resolution", &s.m_eyeResolutionScale, 0.5f, 1.5f, "%.2f" );
			Help( "Size of the headset images, times what the VR runtime recommends (SteamVR's own resolution setting "
				"also counts). Lower costs less GPU and looks softer. Applies within a frame." );
			static const char *const layerIds[] = { "view", "local" };
			static const char *const layerNames[] = { "Head-locked (view)", "World (local)" };
			Combo( "Headset image space", s.m_stereoLayerSpace, layerIds, layerNames, 2 );
			Help( "World: the headset gets the head pose each picture was drawn for, so SteamVR can correct head turns "
				"when the game (60 fps) is slower than the headset. Less judder when turning your head; motion smoothing "
				"may smear the sky or water. Head-locked: the old behaviour, more judder." );
			ImGui::Checkbox( "Show frame timing", &s.m_perfOverlay );
			Help( "A small window with the time each part of the frame takes (CPU and GPU, in ms) and how often a "
				"frame misses the headset's refresh. Also written to psuex.log every 5 s (\"Perf 5 s:\")." );
			ImGui::Checkbox( "Fast game memory checks", &s.m_fastPointerChecks );
			Help( "Checks game pointers (player, weapon, motion tables) with a guarded read instead of asking Windows "
				"about the memory each time. Saves about 4 ms per frame with motion controllers. Turn off only to compare." );
			ImGui::Checkbox( "Skip the flat frame copy in stereo", &s.m_perfSkipFlatCopy );
			Help( "In stereo the headset shows the eye images, so the monitor image is not copied to the headset or "
				"waited for. Menus and 2D screens still copy it. Turn off only to compare." );
			ImGui::Checkbox( "Plain GPU copies for the headset images", &s.m_perfCopyBlits );
			Help( "Copies the eye, menu and HUD images to the headset directly when their size and format match, "
				"instead of drawing them with a shader. Turn off only to compare." );
			ImGui::Checkbox( "Lock only the HUD vertices used", &s.m_perfHudLockRanges );
			Help( "When copying the game's HUD for the headset, reads only the part of the vertex and index buffers "
				"each draw uses. Turn off only to compare." );
			ImGui::Checkbox( "Monitor shows the left eye", &s.m_flatMirrorEye );
			Help( "In stereo the monitor shows the left eye's picture (cropped to the window) with the HUD on top, "
				"instead of drawing the whole 3D scene a third time for the monitor. Menus and 2D screens are unchanged. "
				"Turn off to get the normal flat camera on the monitor." );
			ImGui::Checkbox( "Batch log writes", &s.m_perfLogBatching );
			Help( "Writes psuex.log to disk about once a second instead of after every line. A crash still writes "
				"what was logged. Turn off when chasing a crash that kills the game outright." );
		}

		if( Section( Tab::Keys, "Keyboard and gamepad shortcuts" ) )
		{
			ImGui::TextDisabled( "Pad chords: hold Back/Select, then press the button." );
			if( ImGui::BeginTable( "vr_bindings", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg ) )
			{
				ImGui::TableSetupColumn( "Action" );
				ImGui::TableSetupColumn( "Key" );
				ImGui::TableSetupColumn( "Pad" );
				ImGui::TableSetupColumn( "" );
				ImGui::TableHeadersRow();
				for( int i = 0; i < kVRActionCount; ++i )
				{
					VRBinding &binding = s.m_bindings[ i ];
					ImGui::PushID( i );
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted( VRActionLabel( static_cast< VRAction >( i ) ) );

					ImGui::TableNextColumn();
					const bool capturingKey = s_captureAction == i && s_captureKey;
					const std::string keyText = capturingKey ? std::string( "press a key..." ) : VRBindingKeyName( binding );
					if( ImGui::Button( ( keyText + "##key" ).c_str() ) )
					{
						BeginCapture( i, true );
					}

					ImGui::TableNextColumn();
					const bool capturingPad = s_captureAction == i && !s_captureKey;
					const std::string padText = capturingPad ? std::string( "press a button..." ) : VRPadName( binding.pad );
					if( ImGui::Button( ( padText + "##pad" ).c_str() ) )
					{
						BeginCapture( i, false );
					}

					ImGui::TableNextColumn();
					if( ImGui::SmallButton( "Clear" ) )
					{
						binding = VRBinding{};
						s_captureAction = -1;
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if( s_captureAction >= 0 )
			{
				ImGui::TextDisabled( s_captureKey ? "Hold Ctrl / Shift / Alt with the key if you want them. Esc cancels." : "Esc cancels." );
			}
			if( ImGui::Button( "Reset controls" ) )
			{
				s.m_bindings = DefaultVRBindings();
				s_captureAction = -1;
			}
		}
		ImGui::EndChild();

		ImGui::Separator();
		if( context.save && ImGui::Button( "Save" ) )
		{
			context.message = context.save() ? "Saved." : "Save failed.";
		}
		if( context.inGame && context.reload )
		{
			ImGui::SameLine();
			if( ImGui::Button( "Reload from file" ) )
			{
				context.message = context.reload() ? "Reloaded psuex_vr.json." : "psuex_vr.json not found.";
			}
		}
		ImGui::SameLine();
		if( ImGui::Button( "Reset VR defaults" ) )
		{
			// Keep whether VR is on and the session pause; reset everything else.
			const bool enabled = s.m_enabled;
			const bool paused = s.m_paused;
			s = VRSettings{};
			s.m_enabled = enabled;
			s.m_paused = paused;
			context.message = "Defaults restored (not saved yet).";
		}
		if( !context.message.empty() )
		{
			ImGui::TextDisabled( "%s", context.message.c_str() );
		}
	}
}
