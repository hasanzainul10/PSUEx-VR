#include "VRSystem.h"

#include "D3D9ExUpgrade.h"
#include "D3D9ExCompat.h"
#include "D3D11Bridge.h"
#include "OpenXRRuntime.h"
#include "StereoRenderer.h"
#include "VRBindings.h"
#include "VRCamera.h"
#include "VRControllers.h"
#include "VRProbe.h"
#include "VRSyntheticPad.h"
#include "VRUiPointer.h"
#include "VRControllerLabels.h"
#include "VRSpeech.h"
#include "VRHud.h"
#include "VRPerf.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"

#include <d3d9.h>
#include <d3d11.h>

#pragma comment( lib, "d3d11.lib" )
#pragma comment( lib, "dxgi.lib" )

namespace PSUVR
{
	System &System::Get()
	{
		static System instance;
		return instance;
	}

	void System::LoadConfig()
	{
		m_enabled = Config::Get().vr.m_enabled;
		m_forceD3D9Ex = Config::Get().vr.m_forceD3D9Ex;
		m_d3d9exOnly = Config::Get().vr.m_d3d9exOnly;
		m_solidColorPrototype = Config::Get().vr.m_solidColorPrototype;
		PSU_LOG(
			"VR config: enabled=%d forceD3D9Ex=%d d3d9exOnly=%d solidColor=%d runtime=%s camera=%s input=%s\n",
			( int )m_enabled, ( int )m_forceD3D9Ex, ( int )m_d3d9exOnly, ( int )m_solidColorPrototype,
			Config::Get().vr.m_runtime.c_str(),
			Config::Get().vr.m_cameraMode.c_str(),
			Config::Get().vr.m_inputMode.c_str() );
	}

	void System::OnProcessAttach()
	{
		Config::Get().LoadFromFile();
		LoadConfig();
		if( m_enabled || m_forceD3D9Ex || m_d3d9exOnly )
		{
			m_d3d9ExInstalled = InstallD3D9ExUpgrade();
		}
	}

	void System::OnProcessDetach()
	{
		LogD3D9ExCompatStats();
		if( m_bridge )
		{
			m_bridge->Shutdown();
			delete m_bridge;
			m_bridge = nullptr;
		}
		m_bridgeReady = false;
		if( m_xr )
		{
			m_xr->Shutdown();
			delete m_xr;
			m_xr = nullptr;
		}
		m_runtimeReady = false;
		m_runtimeFailed = false;
		m_device = nullptr;
		if( m_d3d11Context )
		{
			m_d3d11Context->Release();
			m_d3d11Context = nullptr;
		}
		if( m_d3d11 )
		{
			m_d3d11->Release();
			m_d3d11 = nullptr;
		}
		if( m_d3d9ExInstalled )
		{
			RemoveD3D9ExUpgrade();
			m_d3d9ExInstalled = false;
		}
	}

	void System::OnDirect3DCreated( IDirect3D9 *d3d9 )
	{
		LoadConfig();
		PSU_LOG( "VR: OnDirect3DCreated %p d3d9exActive=%d\n", d3d9, ( int )IsD3D9ExActive() );
	}

	bool System::EnsureD3D11()
	{
		if( m_d3d11 )
		{
			return true;
		}

		D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
			&level, 1, D3D11_SDK_VERSION, &m_d3d11, nullptr, &m_d3d11Context );
		if( FAILED( hr ) )
		{
			PSU_LOG( "VR: D3D11CreateDevice failed 0x%08X\n", hr );
			return false;
		}
		PSU_LOG( "VR: D3D11 device created %p\n", m_d3d11 );
		return true;
	}

	bool System::EnsureRuntime()
	{
		if( m_runtimeReady )
		{
			return true;
		}
		if( m_runtimeFailed || !m_enabled || m_d3d9exOnly )
		{
			return false;
		}
		if( !EnsureD3D11() )
		{
			m_runtimeFailed = true;
			return false;
		}
		if( !m_xr )
		{
			m_xr = new OpenXRRuntime();
		}
		m_runtimeReady = m_xr->Init( m_d3d11 );
		if( !m_runtimeReady )
		{
			m_runtimeFailed = true;
			PSU_LOG( "VR: OpenXR init failed (start SteamVR / OpenXR runtime); continuing flat\n" );
		}
		return m_runtimeReady;
	}

	static bool GetBackBufferSize( IDirect3DDevice9 *device, UINT &w, UINT &h )
	{
		w = 0;
		h = 0;
		if( !device )
		{
			return false;
		}
		IDirect3DSurface9 *bb = nullptr;
		if( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb ) ) || !bb )
		{
			return false;
		}
		D3DSURFACE_DESC desc{};
		bb->GetDesc( &desc );
		bb->Release();
		w = desc.Width;
		h = desc.Height;
		return w > 0 && h > 0;
	}

	bool System::EnsureBridge()
	{
		if( !m_device || !m_d3d11 || !IsD3D9ExActive() || m_d3d9exOnly )
		{
			return false;
		}

		UINT bbW = 0, bbH = 0;
		if( !GetBackBufferSize( m_device, bbW, bbH ) )
		{
			return false;
		}

		if( !m_bridge )
		{
			m_bridge = new D3D11Bridge();
		}

		if( !m_bridge->Ready() )
		{
			m_bridgeReady = m_bridge->Init( m_device, m_d3d11, bbW, bbH );
		}
		else
		{
			m_bridgeReady = m_bridge->EnsureSize( m_device, bbW, bbH );
		}

		if( m_bridgeReady && m_runtimeReady && m_xr )
		{
			m_xr->ResizeSwapchains( static_cast< int32_t >( bbW ), static_cast< int32_t >( bbH ) );
		}

		return m_bridgeReady;
	}

	D3D11Bridge *System::StereoBridge() const
	{
		return ( m_enabled && m_runtimeReady && m_bridgeReady && !m_solidColorPrototype ) ? m_bridge : nullptr;
	}

	bool System::NextViews( EyePose localOut[ 2 ], EyePose viewSpaceOut[ 2 ] ) const
	{
		if( !m_haveNextViews )
		{
			return false;
		}
		localOut[ 0 ] = m_nextViews[ 0 ];
		localOut[ 1 ] = m_nextViews[ 1 ];
		viewSpaceOut[ 0 ] = m_nextViewSpacePoses[ 0 ];
		viewSpaceOut[ 1 ] = m_nextViewSpacePoses[ 1 ];
		return true;
	}

	bool System::EyeSwapchainSize( uint32_t &width, uint32_t &height ) const
	{
		if( !m_xr || m_xr->EyeSwapchainWidth() < 1 || m_xr->EyeSwapchainHeight() < 1 )
		{
			return false;
		}
		width = static_cast< uint32_t >( m_xr->EyeSwapchainWidth() );
		height = static_cast< uint32_t >( m_xr->EyeSwapchainHeight() );
		return true;
	}

	void System::OnDeviceCreated( IDirect3DDevice9 *device )
	{
		LoadConfig();
		m_device = device;
		m_bridgeReady = false;
		m_runtimeFailed = false;
		m_loggedVirtualScreen = false;
		PSU_LOG( "VR: OnDeviceCreated %p d3d9exActive=%d\n", device, ( int )IsD3D9ExActive() );

		if( IsD3D9ExActive() )
		{
			InstallD3D9ExCompatHooks( device );
		}

		// Flat D3D9Ex soak / managed-pool validation — no OpenXR.
		if( m_d3d9exOnly )
		{
			PSU_LOG( "VR: d3d9exOnly — Ex device active, OpenXR/bridge skipped\n" );
			return;
		}

		if( !m_enabled )
		{
			return;
		}

		if( !m_solidColorPrototype && IsD3D9ExActive() && EnsureD3D11() )
		{
			EnsureBridge();
			if( m_bridgeReady && m_bridge->BlitBackBuffer( m_device ) )
			{
				PSU_LOG( "VR: D3D11Bridge probe blit ok\n" );
			}
		}

		EnsureRuntime();
		if( !m_solidColorPrototype && m_runtimeReady )
		{
			EnsureBridge();
		}
	}

	void System::OnPresentCapture()
	{
		m_capturedThisFrame = false;
		if( m_d3d9exOnly )
		{
			static unsigned s_presents = 0;
			++s_presents;
			if( s_presents == 1 )
			{
				LogD3D9ExCompatStats();
				PSU_LOG( "VR: d3d9exOnly Present path alive (no OpenXR)\n" );
			}
			else if( s_presents == 60 || s_presents == 300 )
			{
				PSU_LOG( "VR: d3d9exOnly still presenting (frame %u)\n", s_presents );
				LogD3D9ExCompatStats();
			}
			return;
		}
		if( !m_enabled )
		{
			return;
		}
		if( !EnsureRuntime() || !m_xr )
		{
			return;
		}
		// Always poll OpenXR events (solid-color path used to skip this).
		m_xr->PumpEvents();
		if( m_solidColorPrototype )
		{
			return;
		}
		if( !EnsureBridge() || !m_bridge || !m_device )
		{
			return;
		}
		const bool stereoFrame = StereoRenderer::Get().HasFrame();
		if( stereoFrame && !Config::Get().vr.m_stereoPanel && Config::Get().vr.m_perfSkipFlatCopy )
		{
			// Only the stereo panel shows the flat frame; the fence after BeginFrame covers the eye RTs.
			return;
		}
		m_capturedThisFrame = m_bridge->BlitBackBuffer( m_device );
		// Stereo eye RTs are drawn earlier in the tick; fence if the BB path did not.
		if( !m_capturedThisFrame && stereoFrame )
		{
			m_bridge->Fence( m_device );
		}
	}

	void System::OnPresentSubmit()
	{
		VRProbe::Update();
		if( !m_enabled || m_d3d9exOnly )
		{
			return;
		}
		if( !EnsureRuntime() || !m_xr )
		{
			return;
		}

		__try
		{
			if( !m_xr->BeginFrame() )
			{
				StereoRenderer::Get().ConsumeFrame();
				return;
			}

			// The next game frame renders with poses predicted one display period ahead.
			m_haveNextViews = m_xr->LocateNextViews( m_nextViews, m_nextViewSpacePoses );
			m_haveHeadHeight = m_xr->GetHeadHeightAboveFloor( m_headHeight );
			// Controllers for the same display time as those views.
			PSUMemory::fastPointerChecks = Config::Get().vr.m_fastPointerChecks;
			PSULog::g_batchFlush = Config::Get().vr.m_perfLogBatching;
			const int64_t updateStart = VRPerf::Begin();
			int64_t partStart = updateStart;
			m_xr->UpdateInput();
			VRPerf::End( VRPerf::Stage::Input, partStart );
			partStart = VRPerf::Begin();
			VRControllers::Update( m_xr );
			VRPerf::End( VRPerf::Stage::Controllers, partStart );
			partStart = VRPerf::Begin();
			VRUiPointer::Update( m_xr );
			VRPerf::End( VRPerf::Stage::UiPointer, partStart );
			partStart = VRPerf::Begin();
			VRSyntheticPad::Update();
			VRPerf::End( VRPerf::Stage::Pad, partStart );
			partStart = VRPerf::Begin();
			VRControllerLabels::Update( m_haveNextViews ? m_nextViews : nullptr );
			VRPerf::End( VRPerf::Stage::LabelsUpdate, partStart );
			partStart = VRPerf::Begin();
			VRSpeech::Update();
			VRPerf::End( VRPerf::Stage::Speech, partStart );
			VRPerf::End( VRPerf::Stage::Updates, updateStart );

			// VR shortcuts (rebindable keys and Back+button pad chords, see VRBindings).
			DWORD foregroundPid = 0;
			GetWindowThreadProcessId( GetForegroundWindow(), &foregroundPid );
			const unsigned actions = VRBindings::Poll( foregroundPid == GetCurrentProcessId() ) |
				VRSyntheticPad::TakeActions();
			if( ( ( actions & VRBindings::Bit( VRAction::Recenter ) ) || m_recenterRequest ) && m_haveNextViews )
			{
				m_recenterRequest = false;
				VRCamera::Recenter( m_nextViews );
				VRBindings::ShowToast( "Recentered" );
			}
			VRBindings::ApplyToggles( actions );

			bool stereoSubmitted = false;
			StereoRenderer &stereo = StereoRenderer::Get();
			VRHud &hud = VRHud::Get();
			const bool hadStereoFrame = stereo.HasFrame();
			if( !m_solidColorPrototype && hadStereoFrame && m_bridge )
			{
				// Eye RTs are filled earlier in the tick; do not rely only on the BB blit fence.
				m_bridge->Fence( m_device );
				ID3D11Texture2D *eyes[ 2 ] = { m_bridge->EyeTexture( 0 ), m_bridge->EyeTexture( 1 ) };
				const int64_t stereoStart = VRPerf::Begin();
				stereoSubmitted = m_xr->SubmitStereo( eyes, stereo.RenderedPoses(), stereo.RenderedViewSpacePoses() );
				VRPerf::End( VRPerf::Stage::StereoSubmit, stereoStart );
				if( stereoSubmitted )
				{
					VRPerf::NoteStereoFrame();
				}
				if( stereoSubmitted && !m_loggedStereo )
				{
					m_loggedStereo = true;
					PSU_LOG( "VR: stereo projection layer active (head-locked submit; recenter: Ctrl+F9 / Back+X)\n" );
				}
				else if( !stereoSubmitted )
				{
					static unsigned s_stereoMisses = 0;
					++s_stereoMisses;
					if( s_stereoMisses <= 8 || ( s_stereoMisses % 120 ) == 0 )
					{
						PSU_LOG( "VR: stereo submit miss #%u (sky may flicker)\n", s_stereoMisses );
					}
				}

				// Town UI as floating screens (not painted into the eyes).
				if( stereoSubmitted && ( hud.HasUserContent() || hud.HasWorldContent() ) )
				{
					const float dist = Config::Get().vr.m_hudDistance;
					const float hudSize = Config::Get().vr.m_hudSize > 0.1f ? Config::Get().vr.m_hudSize : 1.0f;
					const float userH = 1.4f * hudSize;
					const float worldH = 1.35f * hudSize;
					float worldPos[ 3 ]{};
					float worldOri[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };
					VRCamera::PanelPose( dist, 0.2f, worldPos, worldOri );
					// Body-locked HUD/menu panel with lazy follow (or head-locked if off).
					float userPos[ 3 ]{};
					float userOri[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };
					const bool follow = Config::Get().vr.m_hudFollow && m_haveNextViews;
					if( follow )
					{
						VRCamera::HudPanelPose( m_nextViews, dist, userPos, userOri );
					}
					const int64_t hudStart = VRPerf::Begin();
					m_xr->SubmitHudQuads(
						hud.HasUserContent() ? hud.UserTexture() : nullptr, dist, userH,
						hud.HasWorldContent() ? hud.WorldTexture() : nullptr, dist, worldH,
						worldPos, worldOri,
						follow ? userPos : nullptr, follow ? userOri : nullptr );
					VRPerf::End( VRPerf::Stage::HudQuads, hudStart );
					static bool s_loggedHudQuads = false;
					if( !s_loggedHudQuads )
					{
						s_loggedHudQuads = true;
						PSU_LOG( "VR: HUD quads active (user=%d world=%d, dist=%.2f)\n",
							hud.HasUserContent() ? 1 : 0, hud.HasWorldContent() ? 1 : 0, dist );
					}
				}
			}
			// Transitions only: scene mode, stereo on/off, first-person camera active.
			{
				static unsigned s_lastMode = 0xFFFFFFFF;
				static int s_lastStereo = -1;
				static int s_lastFirstPerson = -1;
				static unsigned s_transitionLogs = 0;
				const unsigned mode = *reinterpret_cast< unsigned * >( 0x00B90EA8 );
				const int firstPerson = VRCamera::FirstPersonActive() ? 1 : 0;
				if( ( mode != s_lastMode || ( stereoSubmitted ? 1 : 0 ) != s_lastStereo || firstPerson != s_lastFirstPerson ) &&
					s_transitionLogs < 200 )
				{
					++s_transitionLogs;
					PSU_LOG( "VR: scene mode=%u stereo=%d firstPerson=%d (hadStereoFrame=%d)\n",
						mode, stereoSubmitted ? 1 : 0, firstPerson, hadStereoFrame ? 1 : 0 );
				}
				s_lastMode = mode;
				s_lastStereo = stereoSubmitted ? 1 : 0;
				s_lastFirstPerson = firstPerson;
			}
			stereo.ConsumeFrame();
			hud.EndStereoFrame();

			if( m_solidColorPrototype )
			{
				if( m_xr->ClearEyes( 0.0f, 0.45f, 0.55f ) )
				{
					static bool s_loggedTeal = false;
					if( !s_loggedTeal )
					{
						s_loggedTeal = true;
						PSU_LOG( "VR: solid-color (teal) submit active\n" );
					}
				}
			}
			else if( m_capturedThisFrame && m_bridge &&
				( !stereoSubmitted || Config::Get().vr.m_stereoPanel ) )
			{
				const float dist = Config::Get().vr.m_hudDistance;
				// Menus / 2D: large head-locked screen. Stereo+panel: small world-locked stand-in.
				const float height = ( stereoSubmitted ? 0.6f : 1.4f ) * Config::Get().vr.m_hudSize;
				m_xr->SetVirtualScreenPlacement( dist, height );
				float panelPosition[ 3 ]{};
				float panelOrientation[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };
				bool worldLocked = stereoSubmitted;
				if( stereoSubmitted )
				{
					VRCamera::PanelPose( dist, 0.35f, panelPosition, panelOrientation );
				}
				else if( Config::Get().vr.m_hudFollow && m_haveNextViews )
				{
					// 2D scenes (login, planet select, clothing shop...): the same body-locked
					// lazy-follow panel as the in-game menus, instead of a head-locked screen.
					if( !VRCamera::HasReference() )
					{
						VRCamera::Recenter( m_nextViews );
					}
					VRCamera::HudPanelPose( m_nextViews, dist, panelPosition, panelOrientation );
					worldLocked = true;
				}
				m_xr->SetPanelWorldPose( worldLocked, panelPosition, panelOrientation );
				if( !stereoSubmitted && Config::Get().vr.m_menuBackdrop )
				{
					// Before the panel: clearing resets the panel flag.
					m_xr->ClearEyes( 0.05f, 0.05f, 0.08f );
				}
				m_xr->SubmitSharedTexture( m_bridge->GetSharedTexture() );
				if( !m_loggedVirtualScreen )
				{
					m_loggedVirtualScreen = true;
					UINT w = 0, h = 0;
					GetBackBufferSize( m_device, w, h );
					const unsigned mode = *reinterpret_cast< unsigned * >( 0x00B90EA8 );
					PSU_LOG( "VR: 2D virtual screen mode=%u (bb %ux%u, xr %dx%d, dist=%.2f h=%.2f)\n",
						mode, w, h, m_xr->SwapchainWidth(), m_xr->SwapchainHeight(), dist, height );
					LogD3D9ExCompatStats();
				}
			}
			else if( !stereoSubmitted )
			{
				m_xr->ClearEyes( 0.05f, 0.05f, 0.08f );
			}

			const int64_t labelStart = VRPerf::Begin();
			VRControllerLabels::Submit( m_xr, m_bridge );
			VRPerf::End( VRPerf::Stage::Labels, labelStart );
			m_xr->EndFrame();
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			PSU_LOG( "VR: exception in OpenXR submit (0x%08X) — disabling VR for this session\n",
				GetExceptionCode() );
			m_enabled = false;
			m_runtimeFailed = true;
		}
	}
}
