#pragma once

#include <string>

// Motion-controller button layout: the one table both the virtual pad (VRSyntheticPad)
// and the settings diagram (VRControllerDiagram) read, so behaviour and picture match.
// SteamVR maps PS VR2 Sense, Index, Vive... onto the Touch layout, so one mapping fits
// all; the profile only changes button names.
namespace PSUVR
{
	namespace VRMotionLayout
	{
		enum class Control : int { Trigger, Grip, Stick, StickClick, Primary, Secondary, Count };

		enum class Output : int
		{
			None,
			// Game pad buttons (PsuIlluminus.ini [gamepad1] names).
			A, B, X, Y, L1, R1, L2, R2, L3, R3, Select, Start,
			LeftStick, RightStick,
			// VR actions.
			Recenter, ToggleCamera, ToggleSettings, TogglePause,
			// Motion combat: free aim follows the gun hand while this mode is on.
			FreeAimMode,
			// Both triggers (in the chord): start the hand angle calibration.
			CalibrateHands,
			// Field only (X / Y while a menu or the palette has the input): the PSUEx chat log
			// window, and speech to text (reserved, not made yet).
			ChatLog, SpeechToText,
			// The left stick sends the D-pad until toggled off (VRSyntheticPad handles the press).
			DpadToggle,
			// Both grips + right stick click: the look-down controller labels on / off.
			ToggleControllerLabels,
			// Both grips + left stick pushed up: open / close the Rogue's Toolkit menu.
			ToggleToolkitMenu,
		};

		struct Entry
		{
			int hand;         // 0 = left, 1 = right
			Control control;
			Output plain;     // normal press
			Output chord;     // while both grips are held
		};

		// The game's own A cancels and B confirms, so Cross (bottom) sends B.
		inline constexpr Entry kLayout[] = {
			{ 0, Control::Trigger, Output::L2, Output::CalibrateHands },
			{ 0, Control::Grip, Output::L1, Output::None },
			{ 0, Control::Stick, Output::LeftStick, Output::ToggleToolkitMenu },
			{ 0, Control::StickClick, Output::FreeAimMode, Output::DpadToggle },
			{ 0, Control::Primary, Output::SpeechToText, Output::ToggleSettings },
			{ 0, Control::Secondary, Output::ChatLog, Output::TogglePause },
			{ 1, Control::Trigger, Output::R2, Output::CalibrateHands },
			{ 1, Control::Grip, Output::R1, Output::None },
			{ 1, Control::Stick, Output::RightStick, Output::None },
			{ 1, Control::StickClick, Output::Start, Output::ToggleControllerLabels },
			{ 1, Control::Primary, Output::B, Output::Recenter },
			{ 1, Control::Secondary, Output::A, Output::ToggleCamera },
		};

		// Left grip + left stick click: L3 (both grips + left stick click is the D-pad toggle).
		inline constexpr const char *kDpadToggleText = "Left grip + left stick click: L3";

		inline bool IsPsvr2( const std::string &profile )
		{
			return profile != "generic";
		}

		inline const char *ControlName( const std::string &profile, int hand, Control control )
		{
			const bool ps = IsPsvr2( profile );
			switch( control )
			{
				case Control::Trigger: return ps ? ( hand == 0 ? "L2 trigger" : "R2 trigger" ) : "Trigger";
				case Control::Grip: return ps ? ( hand == 0 ? "L1 grip" : "R1 grip" ) : "Grip";
				case Control::Stick: return "Stick";
				case Control::StickClick: return ps ? ( hand == 0 ? "L3 (stick click)" : "R3 (stick click)" ) : "Stick click";
				case Control::Primary: return ps ? ( hand == 0 ? "Square" : "Cross" ) : ( hand == 0 ? "X" : "A" );
				case Control::Secondary: return ps ? ( hand == 0 ? "Triangle" : "Circle" ) : ( hand == 0 ? "Y" : "B" );
				default: return "";
			}
		}

		inline const char *OutputName( Output output )
		{
			switch( output )
			{
				case Output::A: return "A (cancel)";
				case Output::B: return "B (confirm)";
				case Output::X: return "X";
				case Output::Y: return "Y";
				case Output::L1: return "L1";
				case Output::R1: return "R1";
				case Output::L2: return "L2";
				case Output::R2: return "R2";
				case Output::L3: return "L3";
				case Output::R3: return "R3";
				case Output::Select: return "SELECT";
				case Output::Start: return "START (menu)";
				case Output::LeftStick: return "Move";
				case Output::RightStick: return "Turn / camera";
				case Output::Recenter: return "Recenter";
				case Output::ToggleCamera: return "First / third person";
				case Output::ToggleSettings: return "VR settings";
				case Output::TogglePause: return "Pause VR";
				case Output::FreeAimMode: return "Free aim mode on / off";
				case Output::CalibrateHands: return "Calibrate hands (both triggers)";
				case Output::ChatLog: return "Chat log, R2 (Y in menus)";
				case Output::SpeechToText: return "Speech to text, soon (X in menus)";
				case Output::DpadToggle: return "D-pad on / off (the left stick moves the D-pad)";
				case Output::ToggleControllerLabels: return "Controller labels on / off (look down)";
				case Output::ToggleToolkitMenu: return "Rogue's Toolkit menu (push the stick up)";
				default: return "";
			}
		}

		// The system / menu buttons stay with SteamVR and the headset.
		inline const char *ReservedText( const std::string &profile )
		{
			return IsPsvr2( profile ) ? "Options / Create / PS: reserved for SteamVR" : "Menu / system: reserved for SteamVR";
		}
	}
}
