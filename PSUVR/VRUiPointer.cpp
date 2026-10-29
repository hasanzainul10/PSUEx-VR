#include "VRUiPointer.h"

#include "VRBindings.h"
#include "VRControllers.h"

#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <imgui.h>

#include <Windows.h>

#include <cfloat>
#include <cmath>

namespace PSUVR
{
	namespace
	{
		constexpr int kHand = 1;                  // the right controller points
		constexpr float kStubMeters = 2.0f;       // beam length when it misses the panel
		constexpr float kWheelPerSecond = 6.0f;   // ImGui wheel steps per second at full stick
		constexpr float kStickDeadzone = 0.25f;
		constexpr DWORD kStaleMs = 250;

		bool s_windowOpen = false;
		bool s_wizardOpen = false;
		bool s_toolkitOpen = false;
		bool s_osButtonDown = false;    // the system mouse button we pressed for the toolkit menu
		float s_panelPixels[ 2 ]{ 1.0f, 1.0f };
		bool s_wanted = false;          // last Update's answer, before the release wait
		bool s_releasePending = false;  // closed with buttons still held: keep them from the game
		DWORD s_updateTick = 0;
		ULONGLONG s_lastTick = 0;

		bool s_hit = false;
		float s_pixel[ 2 ]{};
		float s_beamFrom[ 3 ]{};
		float s_beamTo[ 3 ]{};
		bool s_beamValid = false;

		bool s_triggerDown = false;
		bool s_click = false;           // the trigger went down on the panel and is still held
		float s_wheel = 0.0f;
		bool s_closeRequest = false;

		// What ImGui was last told.
		bool s_sentPos = false;
		bool s_sentButton = false;
		bool s_loggedHit = false;

		struct V3 { float x, y, z; };

		V3 Rotate( const float q[ 4 ], const V3 &v )
		{
			const V3 u{ q[ 0 ], q[ 1 ], q[ 2 ] };
			const float w = q[ 3 ];
			const V3 t{ 2.0f * ( u.y * v.z - u.z * v.y ), 2.0f * ( u.z * v.x - u.x * v.z ), 2.0f * ( u.x * v.y - u.y * v.x ) };
			return { v.x + w * t.x + ( u.y * t.z - u.z * t.y ),
				v.y + w * t.y + ( u.z * t.x - u.x * t.z ),
				v.z + w * t.z + ( u.x * t.y - u.y * t.x ) };
		}

		V3 RotateInverse( const float q[ 4 ], const V3 &v )
		{
			const float c[ 4 ] = { -q[ 0 ], -q[ 1 ], -q[ 2 ], q[ 3 ] };
			return Rotate( c, v );
		}

		// Right grip held while pointing: pull the panel nearer / push it away (vr.hudDistance).
		// Distance multiplies by e^(kDragGain * hand travel): 20 cm of pull roughly halves it.
		constexpr float kDragGain = 3.5f;
		constexpr float kStickMetersPerSecond = 1.5f;  // right stick while the grip is held
		constexpr float kMinDistance = 0.6f;
		constexpr float kMaxDistance = 4.0f;
		bool s_dragging = false;
		V3 s_dragStart{};
		V3 s_dragDir{};
		float s_dragStartDistance = 0.0f;
		float s_dragTickDistance = 0.0f;  // last distance a haptic tick was given at
		float s_dragStick = 0.0f;         // meters added with the right stick during this drag
		float s_dragAlong = 0.0f;

		bool BaseWanted()
		{
			const auto &vr = Config::Get().vr;
			return ( s_windowOpen || s_wizardOpen || s_toolkitOpen ) && vr.m_uiPointer && vr.m_inputMode == "motion" && VRControllers::Active( kHand ) &&
				GetTickCount() - s_updateTick < kStaleMs;
		}

		bool AnyButtonHeld()
		{
			return VRControllers::State( 0 ).buttons != 0 || VRControllers::State( 1 ).buttons != 0;
		}

		// Ray against the quad (it faces +Z). u, v: 0..1 from the top-left corner.
		bool Intersect( const UiQuad &quad, const V3 &origin, const V3 &dir, float &u, float &v, float &distance )
		{
			const V3 rel{ origin.x - quad.position[ 0 ], origin.y - quad.position[ 1 ], origin.z - quad.position[ 2 ] };
			const V3 o = RotateInverse( quad.orientation, rel );
			const V3 d = RotateInverse( quad.orientation, dir );
			// From the front only, pointing at it.
			if( o.z <= 0.0f || d.z > -1.0e-4f )
			{
				return false;
			}
			distance = -o.z / d.z;
			const float x = o.x + d.x * distance;
			const float y = o.y + d.y * distance;
			u = x / quad.width + 0.5f;
			v = 0.5f - y / quad.height;
			return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
		}
	}

	namespace VRUiPointer
	{
		void SetWindowOpen( bool open )
		{
			s_windowOpen = open;
		}

		void SetWizardOpen( bool open )
		{
			s_wizardOpen = open;
		}

		void SetToolkitMenuOpen( bool open )
		{
			s_toolkitOpen = open;
		}

		namespace
		{
			// The toolkit reads the system mouse over the game window: client pixels = panel pixels scaled.
			void DriveSystemMouse()
			{
				if( !s_toolkitOpen )
				{
					if( s_osButtonDown )
					{
						mouse_event( MOUSEEVENTF_LEFTUP, 0, 0, 0, 0 );
						s_osButtonDown = false;
					}
					return;
				}
				HWND window = VRBindings::GameWindow();
				RECT client{};
				if( !window || !GetClientRect( window, &client ) || client.right < 1 || client.bottom < 1 )
				{
					return;
				}
				// Real mouse input only reaches the game while its window is in front; otherwise post the same
				// messages to it (the toolkit watches the window's input).
				// The toolkit reads the button and cursor from the system, so it needs the window in front.
				static ULONGLONG s_lastFront = 0;
				bool foreground = GetForegroundWindow() == window;
				if( !foreground && GetTickCount64() - s_lastFront > 400 )
				{
					s_lastFront = GetTickCount64();
					foreground = VRBindings::BringGameToFront();
				}
				static int s_logs = 0;
				if( s_logs < 12 && ( s_hit || s_click ) )
				{
					++s_logs;
					PSU_LOG( "VRToolkit: pointer %s hit=%d click=%d client %ldx%ld\n", foreground ? "real input" : "posted messages",
						s_hit ? 1 : 0, s_click ? 1 : 0, client.right, client.bottom );
				}
				POINT p{ static_cast< LONG >( s_pixel[ 0 ] / s_panelPixels[ 0 ] * static_cast< float >( client.right ) ),
					static_cast< LONG >( s_pixel[ 1 ] / s_panelPixels[ 1 ] * static_cast< float >( client.bottom ) ) };
				const LPARAM clientPos = MAKELPARAM( p.x, p.y );
				if( s_hit )
				{
					if( foreground )
					{
						ClientToScreen( window, &p );
						SetCursorPos( p.x, p.y );
					}
					else
					{
						PostMessage( window, WM_MOUSEMOVE, s_osButtonDown ? MK_LBUTTON : 0, clientPos );
					}
				}
				if( s_click != s_osButtonDown )
				{
					if( foreground )
					{
						mouse_event( s_click ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0, 0, 0, 0 );
					}
					else
					{
						PostMessage( window, s_click ? WM_LBUTTONDOWN : WM_LBUTTONUP, s_click ? MK_LBUTTON : 0, clientPos );
					}
					s_osButtonDown = s_click;
				}
				// s_wheel is cleared by FeedImGui right after this.
				if( foreground && s_wheel != 0.0f )
				{
					mouse_event( MOUSEEVENTF_WHEEL, 0, 0, static_cast< DWORD >( static_cast< int >( s_wheel * WHEEL_DELTA ) ), 0 );
				}
			}
		}

		bool Wanted()
		{
			return BaseWanted() || s_releasePending;
		}

		void Update( IVRRuntime *runtime )
		{
			const ULONGLONG now = GetTickCount64();
			float dt = s_lastTick ? static_cast< float >( now - s_lastTick ) * 0.001f : 0.0f;
			dt = dt > 0.1f ? 0.1f : dt;
			s_lastTick = now;
			s_updateTick = GetTickCount();

			const bool wanted = BaseWanted();
			if( wanted != s_wanted )
			{
				s_wanted = wanted;
				// Closed with a button down (Circle, the chord): the game must not get it.
				s_releasePending = !wanted && AnyButtonHeld();
				PSU_LOG( "VRUi: pointer %s\n", wanted ? "on" : "off" );
			}
			if( s_releasePending && !AnyButtonHeld() )
			{
				s_releasePending = false;
			}

			s_hit = false;
			s_beamValid = false;
			if( !wanted )
			{
				if( s_dragging )
				{
					s_dragging = false;
					Config::Get().SaveToFile();
				}
				s_click = false;
				s_triggerDown = false;
				s_wheel = 0.0f;
				return;
			}

			const ControllerState &c = VRControllers::State( kHand );
			if( c.aimValid )
			{
				const V3 origin{ c.aimPosition[ 0 ], c.aimPosition[ 1 ], c.aimPosition[ 2 ] };
				const V3 dir = Rotate( c.aimOrientation, { 0.0f, 0.0f, -1.0f } );
				float distance = kStubMeters;
				UiQuad quad{};
				float u = 0.0f, v = 0.0f, hitDistance = 0.0f;
				if( runtime && runtime->GetUiQuad( quad ) && quad.width > 0.0f && quad.height > 0.0f && quad.pixelWidth > 0 &&
					Intersect( quad, origin, dir, u, v, hitDistance ) )
				{
					s_hit = true;
					distance = hitDistance;
					s_pixel[ 0 ] = u * static_cast< float >( quad.pixelWidth );
					s_pixel[ 1 ] = v * static_cast< float >( quad.pixelHeight );
					s_panelPixels[ 0 ] = static_cast< float >( quad.pixelWidth );
					s_panelPixels[ 1 ] = static_cast< float >( quad.pixelHeight );
					if( !s_loggedHit )
					{
						s_loggedHit = true;
						PSU_LOG( "VRUi: first hit at (%.0f, %.0f) of %dx%d, %.2f m\n", s_pixel[ 0 ], s_pixel[ 1 ],
							quad.pixelWidth, quad.pixelHeight, hitDistance );
					}
				}
				s_beamFrom[ 0 ] = origin.x;
				s_beamFrom[ 1 ] = origin.y;
				s_beamFrom[ 2 ] = origin.z;
				s_beamTo[ 0 ] = origin.x + dir.x * distance;
				s_beamTo[ 1 ] = origin.y + dir.y * distance;
				s_beamTo[ 2 ] = origin.z + dir.z * distance;
				s_beamValid = true;
			}

			// Trigger with hysteresis; a click starts only on the panel and lasts until let go
			// (so dragging a slider off its edge keeps dragging).
			const bool wasDown = s_triggerDown;
			s_triggerDown = s_triggerDown ? c.trigger > 0.4f : c.trigger > 0.6f;
			if( s_triggerDown && !wasDown && s_hit )
			{
				s_click = true;
				VRControllers::Haptic( kHand, 0.3f, 0.02f );
			}
			if( !s_triggerDown )
			{
				s_click = false;
			}

			const float y = c.stick[ 1 ];
			// The right stick scrolls, except while the grip moves the panel.
			if( std::fabs( y ) > kStickDeadzone && !s_dragging )
			{
				const float sign = y > 0.0f ? 1.0f : -1.0f;
				s_wheel += sign * ( std::fabs( y ) - kStickDeadzone ) / ( 1.0f - kStickDeadzone ) * kWheelPerSecond * dt;
			}

			if( VRControllers::Pressed( kHand ) & kControllerSecondary )
			{
				s_closeRequest = true;
			}

			// Right grip alone (both grips are the VR chords): move the panel along the pointing
			// direction with the hand.
			auto &vr = Config::Get().vr;
			const bool rightGrip = ( c.buttons & kControllerGrip ) != 0;
			const bool leftGrip = ( VRControllers::State( 0 ).buttons & kControllerGrip ) != 0;
			if( !s_dragging && rightGrip && !leftGrip && ( VRControllers::Pressed( kHand ) & kControllerGrip ) && c.gripValid && c.aimValid )
			{
				s_dragging = true;
				s_dragStart = { c.gripPosition[ 0 ], c.gripPosition[ 1 ], c.gripPosition[ 2 ] };
				// Toward the panel, level: pulling the hand back toward the body brings it nearer.
				UiQuad quad{};
				V3 dir = Rotate( c.aimOrientation, { 0.0f, 0.0f, -1.0f } );
				if( runtime && runtime->GetUiQuad( quad ) )
				{
					dir = { quad.position[ 0 ] - c.gripPosition[ 0 ], 0.0f, quad.position[ 2 ] - c.gripPosition[ 2 ] };
				}
				dir.y = 0.0f;
				const float length = std::sqrt( dir.x * dir.x + dir.z * dir.z );
				s_dragDir = length > 1.0e-3f ? V3{ dir.x / length, 0.0f, dir.z / length } : V3{ 0.0f, 0.0f, -1.0f };
				s_dragStick = 0.0f;
				s_dragAlong = 0.0f;
				s_dragStartDistance = vr.m_hudDistance;
				s_dragTickDistance = vr.m_hudDistance;
				VRControllers::Haptic( kHand, 0.4f, 0.03f );
			}
			if( s_dragging && ( !rightGrip || leftGrip ) )
			{
				s_dragging = false;
				Config::Get().SaveToFile();
				static char s_text[ 64 ];
				sprintf_s( s_text, "Panel distance %.2f m", vr.m_hudDistance );
				VRBindings::ShowToast( s_text );
				PSU_LOG( "VRUi: panel distance %.2f m (from %.2f, hand %.2f m toward the panel, stick %.2f m)\n",
					vr.m_hudDistance, s_dragStartDistance, s_dragAlong, s_dragStick );
			}
			if( s_dragging && c.gripValid )
			{
				const V3 moved{ c.gripPosition[ 0 ] - s_dragStart.x, c.gripPosition[ 1 ] - s_dragStart.y, c.gripPosition[ 2 ] - s_dragStart.z };
				const float along = moved.x * s_dragDir.x + moved.z * s_dragDir.z;
				s_dragAlong = along;
				if( std::fabs( y ) > kStickDeadzone )
				{
					const float sign = y > 0.0f ? 1.0f : -1.0f;
					s_dragStick += sign * ( std::fabs( y ) - kStickDeadzone ) / ( 1.0f - kStickDeadzone ) * kStickMetersPerSecond * dt;
				}
				float distance = s_dragStartDistance * std::exp( along * kDragGain ) + s_dragStick;
				distance = distance < kMinDistance ? kMinDistance : ( distance > kMaxDistance ? kMaxDistance : distance );
				vr.m_hudDistance = distance;
				// A tick every 25 cm so the movement can be felt.
				if( std::fabs( distance - s_dragTickDistance ) >= 0.25f )
				{
					s_dragTickDistance = distance;
					VRControllers::Haptic( kHand, 0.2f, 0.01f );
				}
			}
		}

		void FeedImGui()
		{
			DriveSystemMouse();
			ImGuiIO &io = ImGui::GetIO();
			if( !s_wanted )
			{
				// Hand the mouse back: let go of a held click, and leave the hover once.
				if( s_sentButton )
				{
					io.AddMouseButtonEvent( 0, false );
					s_sentButton = false;
				}
				if( s_sentPos )
				{
					io.AddMousePosEvent( -FLT_MAX, -FLT_MAX );
					s_sentPos = false;
				}
				return;
			}
			if( s_hit )
			{
				io.AddMousePosEvent( s_pixel[ 0 ], s_pixel[ 1 ] );
				s_sentPos = true;
			}
			else if( s_sentPos && !s_click )
			{
				io.AddMousePosEvent( -FLT_MAX, -FLT_MAX );
				s_sentPos = false;
			}
			if( s_click != s_sentButton )
			{
				io.AddMouseButtonEvent( 0, s_click );
				s_sentButton = s_click;
			}
			// Fractional steps scroll smoothly.
			if( s_wheel != 0.0f )
			{
				io.AddMouseWheelEvent( 0.0f, s_wheel );
				s_wheel = 0.0f;
			}
		}

		bool ConsumeCloseRequest()
		{
			const bool request = s_closeRequest;
			s_closeRequest = false;
			return request;
		}

		bool Beam( float from[ 3 ], float to[ 3 ], bool &hit )
		{
			if( !s_wanted || !s_beamValid )
			{
				return false;
			}
			for( int i = 0; i < 3; ++i )
			{
				from[ i ] = s_beamFrom[ i ];
				to[ i ] = s_beamTo[ i ];
			}
			hit = s_hit;
			return true;
		}
	}
}
