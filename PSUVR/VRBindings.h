#pragma once

#include "VRSettings.h"

namespace PSUVR
{
	// Runtime side of the VR shortcuts in Config::vr.m_bindings: keyboard keys and
	// Back+button pad chords, edge-triggered once per frame.
	namespace VRBindings
	{
		constexpr unsigned Bit( VRAction action ) { return 1u << static_cast< int >( action ); }

		// Actions pressed this frame. Keys only count while the game window has focus.
		unsigned Poll( bool windowFocused );

		// Applies the toggles in `actions` (camera mode, pause, snap turn, HUD follow,
		// settings panel request) and shows their toasts. Recenter is applied by the caller.
		void ApplyToggles( unsigned actions );

		// Opens / closes the Rogue's Toolkit menu by sending its key (F6) to the system once no
		// Ctrl / Shift / Alt is held. Safe to call when the toolkit isn't installed (does nothing).
		void OpenToolkitMenu();

		// Short feedback message for the headset ("First person", "Recentered", ...).
		void ShowToast( const char *text );
		// Current toast and its opacity (0..1), or nullptr when none is showing.
		const char *CurrentToast( float &alpha );

		// "Show / hide VR settings" was pressed; the overlay consumes it.
		bool ConsumeSettingsRequest();
		// Show / hide the chat log window (left Triangle); the overlay consumes it.
		void RequestChatLog();
		bool ConsumeChatLogRequest();
	}
}
