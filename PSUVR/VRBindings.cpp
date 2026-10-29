#include "VRBindings.h"

#include "VRCamera.h"
#include "VRGamepad.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <Windows.h>
#include <Xinput.h>

namespace PSUVR
{
	namespace
	{
		bool s_keyWasDown[ kVRActionCount ]{};
		unsigned short s_padPrevious = 0;
		const char *s_toast = nullptr;
		DWORD s_toastTick = 0;
		bool s_settingsRequested = false;
		bool s_chatLogRequested = false;

		// Rogue's Toolkit menu key: it reads real key state, so send a real F6.
		constexpr WORD kToolkitKey = VK_F6;
		constexpr DWORD kToolkitHoldMs = 80;
		bool s_toolkitPending = false;
		bool s_toolkitDown = false;
		DWORD s_toolkitDownTick = 0;

		void SendToolkitKey( bool down )
		{
			INPUT input{};
			input.type = INPUT_KEYBOARD;
			input.ki.wVk = kToolkitKey;
			input.ki.wScan = static_cast< WORD >( MapVirtualKeyA( kToolkitKey, MAPVK_VK_TO_VSC ) );
			input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
			SendInput( 1, &input, sizeof( input ) );
		}

		void TickToolkitKey()
		{
			if( s_toolkitDown )
			{
				if( GetTickCount() - s_toolkitDownTick >= kToolkitHoldMs )
				{
					SendToolkitKey( false );
					s_toolkitDown = false;
				}
				return;
			}
			if( !s_toolkitPending )
			{
				return;
			}
			// The toolkit only takes the key alone (Ctrl+F6 from the shortcut itself is still held).
			if( ( GetAsyncKeyState( VK_CONTROL ) | GetAsyncKeyState( VK_SHIFT ) | GetAsyncKeyState( VK_MENU ) ) & 0x8000 )
			{
				return;
			}
			s_toolkitPending = false;
			SendToolkitKey( true );
			s_toolkitDown = true;
			s_toolkitDownTick = GetTickCount();
			PSU_LOG( "VRToolkit: sent F6\n" );
		}

		constexpr DWORD kToastMs = 1500;
		constexpr DWORD kToastFadeMs = 400;
	}

	namespace VRBindings
	{
		unsigned Poll( bool windowFocused )
		{
			const auto &bindings = Config::Get().vr.m_bindings;
			unsigned short buttons = 0;
			VRGamepad::Buttons( buttons );
			const bool backHeld = ( buttons & XINPUT_GAMEPAD_BACK ) != 0;
			const unsigned short newlyPressed = static_cast< unsigned short >( buttons & ~s_padPrevious );
			s_padPrevious = buttons;

			uint8_t mods = 0;
			if( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) mods |= kVRModCtrl;
			if( GetAsyncKeyState( VK_SHIFT ) & 0x8000 ) mods |= kVRModShift;
			if( GetAsyncKeyState( VK_MENU ) & 0x8000 ) mods |= kVRModAlt;

			unsigned actions = 0;
			for( int i = 0; i < kVRActionCount; ++i )
			{
				const VRBinding &binding = bindings[ i ];
				// Exact modifiers: Ctrl+F7 doesn't fire on plain F7 (a game shortcut) and back.
				const bool keyDown = windowFocused && binding.key > 0 && mods == binding.mods &&
					( GetAsyncKeyState( binding.key ) & 0x8000 ) != 0;
				const bool keyEdge = keyDown && !s_keyWasDown[ i ];
				s_keyWasDown[ i ] = keyDown;
				const bool padEdge = backHeld && binding.pad != 0 && ( newlyPressed & binding.pad ) != 0;
				if( keyEdge || padEdge )
				{
					actions |= 1u << i;
				}
			}
			return actions;
		}

		void ApplyToggles( unsigned actions )
		{
			auto &vr = Config::Get().vr;
			if( actions & Bit( VRAction::ToggleCameraMode ) )
			{
				const bool toFirst = vr.m_cameraMode != "firstPerson";
				vr.m_cameraMode = toFirst ? "firstPerson" : "thirdPerson";
				// Re-prime first person from the camera so the switch doesn't jump.
				VRCamera::ClearFirstPerson();
				ShowToast( toFirst ? "First person" : "Third person" );
			}
			if( actions & Bit( VRAction::TogglePause ) )
			{
				vr.m_paused = !vr.m_paused;
				ShowToast( vr.m_paused ? "VR paused" : "VR resumed" );
			}
			if( actions & Bit( VRAction::ToggleSnapTurn ) )
			{
				// Each camera mode has its own turn setting.
				if( vr.m_cameraMode == "firstPerson" )
				{
					vr.m_snapTurn = !vr.m_snapTurn;
					ShowToast( vr.m_snapTurn ? "First person: snap turn" : "First person: smooth turn" );
				}
				else
				{
					vr.m_tpSnapTurn = !vr.m_tpSnapTurn;
					ShowToast( vr.m_tpSnapTurn ? "Third person: snap turn" : "Third person: smooth turn" );
				}
			}
			if( actions & Bit( VRAction::ToggleHudFollow ) )
			{
				vr.m_hudFollow = !vr.m_hudFollow;
				ShowToast( vr.m_hudFollow ? "HUD: body-locked" : "HUD: head-locked" );
			}
			if( actions & Bit( VRAction::ToggleSettings ) )
			{
				s_settingsRequested = true;
			}
			if( actions & Bit( VRAction::ToggleToolkitMenu ) )
			{
				OpenToolkitMenu();
			}
			TickToolkitKey();
		}

		void OpenToolkitMenu()
		{
			s_toolkitPending = true;
			ShowToast( "Rogue's Toolkit menu" );
		}

		void ShowToast( const char *text )
		{
			s_toast = text;
			s_toastTick = GetTickCount();
			PSU_LOG( "VR: %s\n", text );
		}

		const char *CurrentToast( float &alpha )
		{
			alpha = 0.0f;
			if( !s_toast )
			{
				return nullptr;
			}
			const DWORD age = GetTickCount() - s_toastTick;
			if( age >= kToastMs )
			{
				s_toast = nullptr;
				return nullptr;
			}
			const DWORD fadeStart = kToastMs - kToastFadeMs;
			alpha = age < fadeStart ? 1.0f : 1.0f - static_cast< float >( age - fadeStart ) / kToastFadeMs;
			return s_toast;
		}

		void RequestChatLog()
		{
			s_chatLogRequested = true;
		}

		bool ConsumeChatLogRequest()
		{
			const bool requested = s_chatLogRequested;
			s_chatLogRequested = false;
			return requested;
		}

		bool ConsumeSettingsRequest()
		{
			const bool requested = s_settingsRequested;
			s_settingsRequested = false;
			return requested;
		}
	}
}
