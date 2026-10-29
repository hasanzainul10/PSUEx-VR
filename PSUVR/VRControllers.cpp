#include "VRControllers.h"

#include "VRCamera.h"
#include "../PSUStructs/PSUCombat.h"
#include "VRControllerSim.h"
#include "VRSyntheticPad.h"
#include "VRUiPointer.h"

#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <d3d9.h>

#include <cmath>

namespace PSUVR
{
	namespace
	{
		IVRRuntime *s_runtime = nullptr;
		ControllerState s_state[ 2 ]{};
		uint32_t s_pressed[ 2 ]{};
		uint32_t s_released[ 2 ]{};
		bool s_loggedActive[ 2 ]{};
		unsigned s_buttonLogs = 0;

		const char *const kHandNames[ 2 ] = { "left", "right" };

		struct V3 { float x, y, z; };

		V3 Sub( const V3 &a, const V3 &b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

		// v rotated by q (x,y,z,w).
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

		// LOCAL point -> eye RT pixels. False when behind or too close to the eye.
		struct Projector
		{
			V3 eyePos;
			const float *eyeRot;
			float tanL, tanR, tanU, tanD;
			float width, height;

			bool Project( const V3 &p, float &sx, float &sy, float &depth ) const
			{
				const V3 d = RotateInverse( eyeRot, Sub( p, eyePos ) );
				if( d.z > -0.05f )
				{
					return false;
				}
				depth = -d.z;
				const float tx = d.x / depth;
				const float ty = d.y / depth;
				sx = ( tx - tanL ) / ( tanR - tanL ) * width;
				sy = ( tanU - ty ) / ( tanU - tanD ) * height;
				return true;
			}

			// Pixels per meter at depth.
			float Scale( float depth ) const
			{
				return width / ( ( tanR - tanL ) * depth );
			}
		};

		struct Vertex { float x, y, z, rhw; DWORD color; };

		// Saves the fixed-function state an overlay draw changes and restores it after.
		class OverlayState
		{
		public:
			explicit OverlayState( IDirect3DDevice9 *device ) : m_device( device )
			{
				device->GetFVF( &m_fvf );
				device->GetTexture( 0, &m_texture );
				device->GetVertexShader( &m_vs );
				device->GetPixelShader( &m_ps );
				device->GetStreamSource( 0, &m_vb, &m_vbOffset, &m_vbStride );
				for( int i = 0; i < kStates; ++i )
				{
					device->GetRenderState( kRenderStates[ i ], &m_render[ i ] );
				}
				for( int i = 0; i < kStages; ++i )
				{
					device->GetTextureStageState( 0, kStageStates[ i ], &m_stage[ i ] );
				}

				device->SetVertexShader( nullptr );
				device->SetPixelShader( nullptr );
				device->SetTexture( 0, nullptr );
				device->SetFVF( D3DFVF_XYZRHW | D3DFVF_DIFFUSE );
				device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
				device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
				device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
				device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, FALSE );
				device->SetRenderState( D3DRS_ZENABLE, FALSE );
				device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
				device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
				device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
				device->SetRenderState( D3DRS_FOGENABLE, FALSE );
				device->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
				device->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
				device->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE );
				device->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
				device->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE );
			}

			~OverlayState()
			{
				for( int i = 0; i < kStages; ++i )
				{
					m_device->SetTextureStageState( 0, kStageStates[ i ], m_stage[ i ] );
				}
				for( int i = 0; i < kStates; ++i )
				{
					m_device->SetRenderState( kRenderStates[ i ], m_render[ i ] );
				}
				m_device->SetFVF( m_fvf );
				m_device->SetStreamSource( 0, m_vb, m_vbOffset, m_vbStride );
				m_device->SetTexture( 0, m_texture );
				m_device->SetVertexShader( m_vs );
				m_device->SetPixelShader( m_ps );
				if( m_texture ) m_texture->Release();
				if( m_vs ) m_vs->Release();
				if( m_ps ) m_ps->Release();
				if( m_vb ) m_vb->Release();
			}

		private:
			static constexpr int kStates = 11;
			static constexpr D3DRENDERSTATETYPE kRenderStates[ kStates ] = {
				D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_SEPARATEALPHABLENDENABLE,
				D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE, D3DRS_ALPHATESTENABLE, D3DRS_FOGENABLE,
				D3DRS_CLIPPLANEENABLE, D3DRS_SCISSORTESTENABLE };
			static constexpr int kStages = 4;
			static constexpr D3DTEXTURESTAGESTATETYPE kStageStates[ kStages ] = {
				D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1 };

			IDirect3DDevice9 *m_device;
			DWORD m_fvf = 0;
			IDirect3DBaseTexture9 *m_texture = nullptr;
			IDirect3DVertexShader9 *m_vs = nullptr;
			IDirect3DPixelShader9 *m_ps = nullptr;
			IDirect3DVertexBuffer9 *m_vb = nullptr;
			UINT m_vbOffset = 0, m_vbStride = 0;
			DWORD m_render[ kStates ]{};
			DWORD m_stage[ kStages ]{};
		};

		// Rogue's Toolkit hooks DrawPrimitiveUP (its UI scaling) and moves our screen-space pixels, so the markers go
		// through a vertex buffer and DrawPrimitive instead; the buffer lives for one DrawMarkers call.
		IDirect3DVertexBuffer9 *s_markerVb = nullptr;

		void DrawVerts( IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, UINT primitives, const Vertex *verts, UINT vertexCount )
		{
			const UINT bytes = vertexCount * sizeof( Vertex );
			void *data = nullptr;
			if( s_markerVb && bytes <= 4096 && SUCCEEDED( s_markerVb->Lock( 0, bytes, &data, D3DLOCK_DISCARD ) ) )
			{
				memcpy( data, verts, bytes );
				s_markerVb->Unlock();
				device->SetStreamSource( 0, s_markerVb, 0, sizeof( Vertex ) );
				device->DrawPrimitive( type, 0, primitives );
				return;
			}
			device->DrawPrimitiveUP( type, primitives, verts, sizeof( Vertex ) );
		}

		// Filled circle (triangle fan).
		void DrawDisc( IDirect3DDevice9 *device, float cx, float cy, float radius, DWORD color )
		{
			constexpr int kSegments = 20;
			Vertex fan[ kSegments + 2 ];
			fan[ 0 ] = { cx, cy, 0.0f, 1.0f, color };
			for( int i = 0; i <= kSegments; ++i )
			{
				const float a = 6.2831853f * static_cast< float >( i ) / kSegments;
				fan[ i + 1 ] = { cx + std::cos( a ) * radius, cy + std::sin( a ) * radius, 0.0f, 1.0f, color };
			}
			DrawVerts( device, D3DPT_TRIANGLEFAN, kSegments, fan, kSegments + 2 );
		}

		// Ring + centre dot (with a dark outline so it reads on bright and dark backgrounds).
		// radius: pixels; about 1 degree of view.
		void DrawRing( IDirect3DDevice9 *device, float cx, float cy, float inner, float outer, DWORD color )
		{
			constexpr int kSegments = 32;
			Vertex strip[ ( kSegments + 1 ) * 2 ];
			for( int i = 0; i <= kSegments; ++i )
			{
				const float a = 6.2831853f * static_cast< float >( i ) / kSegments;
				const float c = std::cos( a ), s = std::sin( a );
				strip[ i * 2 + 0 ] = { cx + c * inner, cy + s * inner, 0.0f, 1.0f, color };
				strip[ i * 2 + 1 ] = { cx + c * outer, cy + s * outer, 0.0f, 1.0f, color };
			}
			DrawVerts( device, D3DPT_TRIANGLESTRIP, kSegments * 2, strip, ( kSegments + 1 ) * 2 );
		}

		// Free aim shot start relative to the player (game world units), from the last free aim bullet.
		// Default from the 2026-10-19 log: player + ( -0.02 10.65 -0.03 ).
		float s_shotOffset[ 3 ]{ 0.0f, 10.65f, 0.0f };
		bool s_haveShotOffset = true;
		bool s_learnedShotOffset = false;

		// A point `meters` along the shot line in tracking space: the game's aim (facing +0x124, pitch
		// -+0x220) from the free aim shot start. False before the first free aim shot.
		bool ShotLinePoint( float meters, float tracking[ 3 ] )
		{
			PSUCombat::State aim{};
			if( !s_haveShotOffset || VRCamera::CameraState() != 4 || !PSUCombat::Read( aim ) )
			{
				return false;
			}
			constexpr float kGameAngle = 6.2831853f / 65536.0f;
			const float yaw = static_cast< int16_t >( aim.facing ) * kGameAngle;
			const float pitch = -aim.aimPitch * kGameAngle;
			const auto &vr = Config::Get().vr;
			const float units = meters * VRCamera::kGameUnitsPerMeter * ( vr.m_worldScale > 0.01f ? vr.m_worldScale : 1.0f );
			const float world[ 3 ] = {
				aim.position[ 0 ] + s_shotOffset[ 0 ] + std::sin( yaw ) * std::cos( pitch ) * units,
				aim.position[ 1 ] + s_shotOffset[ 1 ] + std::sin( pitch ) * units,
				aim.position[ 2 ] + s_shotOffset[ 2 ] + std::cos( yaw ) * std::cos( pitch ) * units };
			return VRCamera::WorldToTracking( world, tracking );
		}

		// Thick line (one quad), width in pixels.
		void DrawLine( IDirect3DDevice9 *device, float x0, float y0, float x1, float y1, float width, DWORD color )
		{
			float nx = y0 - y1, ny = x1 - x0;
			const float length = std::sqrt( nx * nx + ny * ny );
			if( length < 0.5f )
			{
				return;
			}
			nx *= width * 0.5f / length;
			ny *= width * 0.5f / length;
			const Vertex strip[ 4 ] = {
				{ x0 + nx, y0 + ny, 0.0f, 1.0f, color }, { x0 - nx, y0 - ny, 0.0f, 1.0f, color },
				{ x1 + nx, y1 + ny, 0.0f, 1.0f, color }, { x1 - nx, y1 - ny, 0.0f, 1.0f, color } };
			DrawVerts( device, D3DPT_TRIANGLESTRIP, 2, strip, 4 );
		}

		void DrawReticle( IDirect3DDevice9 *device, float cx, float cy, float radius, bool firing )
		{
			radius = radius < 6.0f ? 6.0f : radius;
			const float width = radius * 0.18f < 1.5f ? 1.5f : radius * 0.18f;
			const DWORD shadow = D3DCOLOR_ARGB( 140, 0, 0, 0 );
			const DWORD light = firing ? D3DCOLOR_ARGB( 240, 255, 200, 80 ) : D3DCOLOR_ARGB( 230, 255, 255, 255 );
			DrawRing( device, cx, cy, radius - width * 1.5f, radius + width * 1.5f, shadow );
			DrawRing( device, cx, cy, radius - width * 0.5f, radius + width * 0.5f, light );
			DrawDisc( device, cx, cy, width * 1.4f, shadow );
			DrawDisc( device, cx, cy, width * 0.8f, light );
		}
	}

	namespace VRControllers
	{
		bool ShotStartWorld( float world[ 3 ] )
		{
			PSUCombat::State player{};
			if( !s_haveShotOffset || !PSUCombat::Read( player ) )
			{
				return false;
			}
			for( int i = 0; i < 3; ++i )
			{
				world[ i ] = player.position[ i ] + s_shotOffset[ i ];
			}
			return true;
		}

		void NoteShotStart( const float start[ 3 ] )
		{
			PSUCombat::State player{};
			if( VRCamera::CameraState() != 4 || !PSUCombat::Read( player ) || !std::isfinite( start[ 0 ] ) )
			{
				return;
			}
			float offset[ 3 ];
			for( int i = 0; i < 3; ++i )
			{
				offset[ i ] = start[ i ] - player.position[ i ];
			}
			// Sanity: by the character (a few metres at most).
			if( offset[ 0 ] * offset[ 0 ] + offset[ 1 ] * offset[ 1 ] + offset[ 2 ] * offset[ 2 ] > 40.0f * 40.0f )
			{
				return;
			}
			const bool first = !s_learnedShotOffset;
			s_learnedShotOffset = true;
			for( int i = 0; i < 3; ++i )
			{
				s_shotOffset[ i ] = offset[ i ];
			}
			s_haveShotOffset = true;
#ifdef _DEBUG
			if( first )
			{
				PSU_LOG( "VRControllers: free aim shots start at player + (%.2f %.2f %.2f)\n", offset[ 0 ], offset[ 1 ], offset[ 2 ] );
			}
#else
			( void )first;
#endif
		}

		void Update( IVRRuntime *runtime )
		{
			s_runtime = runtime;
			VRControllerSim::Update();
			const bool emulated = VRControllerSim::Active();
			for( int hand = 0; hand < 2; ++hand )
			{
				const uint32_t before = s_state[ hand ].buttons;
				if( emulated )
				{
					VRControllerSim::Fill( hand, s_state[ hand ] );
				}
				else if( !runtime || !runtime->GetControllerState( hand, s_state[ hand ] ) )
				{
					s_state[ hand ] = {};
				}
				const uint32_t now = s_state[ hand ].buttons;
				s_pressed[ hand ] = now & ~before;
				s_released[ hand ] = before & ~now;

				if( s_state[ hand ].active != s_loggedActive[ hand ] )
				{
					s_loggedActive[ hand ] = s_state[ hand ].active;
					PSU_LOG( "VRControllers: %s controller %s\n", kHandNames[ hand ],
						s_state[ hand ].active ? "active" : "inactive" );
				}
				if( ( s_pressed[ hand ] || s_released[ hand ] ) && s_buttonLogs < 40 )
				{
					++s_buttonLogs;
					const ControllerState &c = s_state[ hand ];
					PSU_LOG( "VRControllers: %s buttons 0x%02X trigger %.2f grip %.2f stick (%.2f, %.2f) grip pos (%.2f, %.2f, %.2f)\n",
						kHandNames[ hand ], c.buttons, c.trigger, c.grip, c.stick[ 0 ], c.stick[ 1 ],
						c.gripPosition[ 0 ], c.gripPosition[ 1 ], c.gripPosition[ 2 ] );
				}
			}
		}

		const ControllerState &State( int hand )
		{
			return s_state[ hand & 1 ];
		}

		bool Active( int hand )
		{
			return s_state[ hand & 1 ].active;
		}

		bool AnyActive()
		{
			return s_state[ 0 ].active || s_state[ 1 ].active;
		}

		uint32_t Pressed( int hand )
		{
			return s_pressed[ hand & 1 ];
		}

		uint32_t Released( int hand )
		{
			return s_released[ hand & 1 ];
		}

		void Haptic( int hand, float amplitude, float seconds )
		{
			if( VRControllerSim::Active() )
			{
				VRControllerSim::NoteHaptic( hand, amplitude, seconds );
				return;
			}
			if( s_runtime )
			{
				s_runtime->Haptic( hand, amplitude, seconds );
			}
		}

		void DrawMarkers( IDirect3DDevice9 *device, const EyePose &eye, unsigned width, unsigned height )
		{
			if( !device || !Config::Get().vr.m_showControllers || !AnyActive() || width < 1 || height < 1 )
			{
				return;
			}
			Projector proj{};
			proj.eyePos = { eye.position[ 0 ], eye.position[ 1 ], eye.position[ 2 ] };
			proj.eyeRot = eye.orientation;
			proj.tanL = std::tan( eye.fovAngleLeft );
			proj.tanR = std::tan( eye.fovAngleRight );
			proj.tanU = std::tan( eye.fovAngleUp );
			proj.tanD = std::tan( eye.fovAngleDown );
			proj.width = static_cast< float >( width );
			proj.height = static_cast< float >( height );
			if( proj.tanR - proj.tanL < 1.0e-4f || proj.tanU - proj.tanD < 1.0e-4f )
			{
				return;
			}

			OverlayState state( device );
			struct VbScope
			{
				explicit VbScope( IDirect3DDevice9 *d )
				{
					if( FAILED( d->CreateVertexBuffer( 4096, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
							D3DPOOL_DEFAULT, &s_markerVb, nullptr ) ) )
					{
						s_markerVb = nullptr;
					}
				}
				~VbScope()
				{
					if( s_markerVb )
					{
						s_markerVb->Release();
						s_markerVb = nullptr;
					}
				}
			} markerVb( device );
			// Pointing at the PSUEx overlay: a beam from the right controller to the panel.
			float beamFrom[ 3 ], beamTo[ 3 ];
			bool beamHit = false;
			if( VRUiPointer::Beam( beamFrom, beamTo, beamHit ) )
			{
				const V3 a{ beamFrom[ 0 ], beamFrom[ 1 ], beamFrom[ 2 ] };
				V3 b{ beamTo[ 0 ], beamTo[ 1 ], beamTo[ 2 ] };
				float ax, ay, ad, bx, by, bd;
				if( proj.Project( a, ax, ay, ad ) )
				{
					// The far end may be behind the eye only when pointing back; then skip it.
					if( proj.Project( b, bx, by, bd ) )
					{
						const DWORD beam = beamHit ? D3DCOLOR_ARGB( 200, 120, 220, 255 ) : D3DCOLOR_ARGB( 110, 200, 200, 200 );
						float width = 0.004f * proj.Scale( ad );
						width = width < 1.5f ? 1.5f : ( width > 6.0f ? 6.0f : width );
						DrawLine( device, ax, ay, bx, by, width, beam );
					}
				}
			}
			for( int hand = 0; hand < 2; ++hand )
			{
				const ControllerState &c = s_state[ hand ];
				if( !c.active )
				{
					continue;
				}
				// Left blue, right orange; brighter while the trigger is held.
				const bool trigger = ( c.buttons & kControllerTrigger ) != 0;
				const BYTE alpha = trigger ? 230 : 160;
				const DWORD color = hand == 0 ? D3DCOLOR_ARGB( alpha, 80, 160, 255 ) : D3DCOLOR_ARGB( alpha, 255, 150, 60 );

				int aimHand = -1;
				if( c.aimValid && VRSyntheticPad::GunAiming( aimHand ) && aimHand == hand )
				{
					// Reticle a few metres out (both eyes converge there): along the game's aim, which
					// the shot follows, or along the gun hand.
					const V3 origin{ c.aimPosition[ 0 ], c.aimPosition[ 1 ], c.aimPosition[ 2 ] };
					V3 dir = Rotate( c.aimOrientation, { 0.0f, 0.0f, -1.0f } );
					// The shot flies along the player's facing (+0x124) and aim pitch (+0x220).
					PSUCombat::State aim{};
					float trackYaw = 0.0f, trackPitch = 0.0f;
					constexpr float kGameAngle = 6.2831853f / 65536.0f;
					if( Config::Get().vr.m_reticleOnShot && PSUCombat::Read( aim ) &&
						VRCamera::TrackingAimFromWorld( static_cast< int16_t >( aim.facing ) * kGameAngle, -aim.aimPitch * kGameAngle,
							trackYaw, trackPitch ) )
					{
						const float cp = std::cos( trackPitch );
						dir = { -std::sin( trackYaw ) * cp, std::sin( trackPitch ), -std::cos( trackYaw ) * cp };
					}
					constexpr float kReticleDistance = 6.0f;
					V3 point{ origin.x + dir.x * kReticleDistance, origin.y + dir.y * kReticleDistance,
						origin.z + dir.z * kReticleDistance };
					// On the real shot line: from where free aim shots start (by the character, not
					// the hand), along the aim.
					float shotPoint[ 3 ];
					if( Config::Get().vr.m_aimLeash )
					{
						// Leash: on the hand's line; the shots are aimed at this point.
						const float meters = Config::Get().vr.m_aimLeashMeters > 1.0f ? Config::Get().vr.m_aimLeashMeters : 1.0f;
						const V3 handDir = Rotate( c.aimOrientation, { 0.0f, 0.0f, -1.0f } );
						point = { origin.x + handDir.x * meters, origin.y + handDir.y * meters, origin.z + handDir.z * meters };
					}
					else if( Config::Get().vr.m_reticleOnShot && ShotLinePoint( kReticleDistance, shotPoint ) )
					{
						point = { shotPoint[ 0 ], shotPoint[ 1 ], shotPoint[ 2 ] };
					}
					float sx, sy, depth;
					if( proj.Project( point, sx, sy, depth ) )
					{
						// Radius: vr.reticleSize degrees of view at any distance.
						const float degrees = Config::Get().vr.m_reticleSize > 0.1f ? Config::Get().vr.m_reticleSize : 0.1f;
						DrawReticle( device, sx, sy, proj.Scale( 1.0f ) * std::tan( degrees * ( 3.14159265f / 180.0f ) ), trigger );
					}
				}
				if( c.gripValid )
				{
					float sx, sy, depth;
					const V3 grip{ c.gripPosition[ 0 ], c.gripPosition[ 1 ], c.gripPosition[ 2 ] };
					if( proj.Project( grip, sx, sy, depth ) )
					{
						// About a 3 cm ball, never smaller than a few pixels.
						float radius = 0.03f * proj.Scale( depth );
						radius = radius < 3.0f ? 3.0f : radius;
						DrawDisc( device, sx, sy, radius, color );
					}
				}
			}
		}
	}
}
