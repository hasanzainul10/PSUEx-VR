#include "../Misc/Globals.h"

#include "../PSUExConfig.h"
#include "../PSUVR/VRSystem.h"
#include "../PSUVR/StereoRenderer.h"
#include "../PSUVR/VRHud.h"
#include "../PSUVR/VRPlayerDraw.h"
#include "../PSUVR/VRUiPointer.h"
#include "../PSUVR/VRControllerLabels.h"
#include "../PSUVR/VRPerf.h"
#include "../PSUVR/D3D9ExUpgrade.h"
#include "../PSUVR/D3D9ExCompat.h"
#include "../Misc/Log.h"
#include "PSUOverlay.h"
#include "FrameTrace.h"
#include "Interface/ConfigDisplay.h"
#include "Interface/LootDisplay.h"
#include "Interface/LogDisplay.h"
#include "Interface/CameraDisplay.h"
#include "Interface/VRDisplay.h"
#include "Interface/VRSetupDisplay.h"
#include "../PSUVR/D3D11Bridge.h"
#include <intrin.h>
#ifdef _DEBUG
#include <d3dx9tex.h>
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

std::map<IDirect3DSwapChain9 *, int> swapChainMap;
int swapChainCounter = 0;

PSUOverlay::PSUOverlay()
{
	m_bInitialized = false;
	m_bDisplay = false;
	m_bStyleEditor = false;

	m_iWidth = 0;
	m_iHeight = 0;

	m_hWnd = nullptr;
	m_pOriginalEndScene = nullptr;
	m_pOriginalDrawIndexedPrimitive = nullptr;
	m_pOriginalReset = nullptr;
	m_pOriginalPresent = nullptr;
	m_pOriginalCreateAdditionalSwapChain = nullptr;

	m_style = ImGuiStyle();

	m_pOriginalSwapChainPresentsMap.clear();
	m_swapChainCounter = 0;
	m_swapChainMap.clear();
}

PSUOverlay::~PSUOverlay()
{
}

void PSUOverlay::RenderMenuBar()
{
	if( ImGui::BeginMainMenuBar() )
	{
		if( ImGui::BeginMenu( "Configuration" ) )
		{
			if( ImGui::MenuItem( "Style" ) )
			{
				m_bStyleEditor = !m_bStyleEditor;
			}

			if( ImGui::MenuItem( "Windows" ) )
			{
				m_Controls[ WINDOW_CONTROL_ID::WINDOW_CONFIG_DISPLAY ]->Toggle();
			}
			ImGui::EndMenu();
		}

		if( ImGui::BeginMenu( "Window" ) )
		{
			if (ImGui::MenuItem("Loot Reader" ))
			{
				m_Controls[ WINDOW_CONTROL_ID::WINDOW_LOOT_DISPLAY ]->Toggle();
			}

			if( ImGui::MenuItem( "Chat Log" ) )
			{
				m_Controls[ WINDOW_CONTROL_ID::WINDOW_LOG_DISPLAY ]->Toggle();
			}

			if( ImGui::MenuItem( "Camera Options" ) )
			{
				m_Controls[ WINDOW_CONTROL_ID::WINDOW_CAMERA_DISPLAY ]->Toggle();
			}

			if( ImGui::MenuItem( "VR Settings" ) )
			{
				m_Controls[ WINDOW_CONTROL_ID::WINDOW_VR_DISPLAY ]->Toggle();
			}
			ImGui::EndMenu();
		}

		ImGui::EndMainMenuBar();
	}
}

void PSUOverlay::Initialize( IDirect3D9 *pD3D )
{
	if( nullptr == pD3D )
	{
		printf( "Initialize: pD3D is null!\n" );
		return;
	}

	void **vTable = *( void *** )pD3D;

	if( nullptr == vTable )
	{
		printf( "Initialize: vTable is null!\n" );
		return;
	}

	m_pOriginalCreateDevice = ( FunctionD3DCreateDevice )vTable[ 16 ];

	DetourTransactionBegin();
	{
		DetourUpdateThread( GetCurrentThread() );
		DetourAttach( &( PVOID & )m_pOriginalCreateDevice, Hook_CreateDevice );
	}
	DetourTransactionCommit();
}

void PSUOverlay::AddControl( WINDOW_CONTROL_ID ID, WinCtrlPtr pCtrl )
{
	//printf( "Storing control in m_Controls at ID %d...\n", ( int )ID );
	m_Controls[ ID ] = pCtrl;
}

void PSUOverlay::ReleaseDirectX()
{
	if( m_bInitialized )
	{
		ReleaseWindow();
		ImGui_ImplDX9_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}

	m_bInitialized = false;

	DetourTransactionBegin();
	{
		DetourUpdateThread( GetCurrentThread() );
		DetourDetach( &( PVOID & )m_pOriginalEndScene, Hook_EndScene );
		//DetourDetach( &( PVOID & )m_pOriginalReset, Hook_Reset );
		//DetourDetach( &( PVOID & )m_pOriginalDrawIndexedPrimitive, Hook_DrawIndexedPrimitive );
		//DetourDetach( &( PVOID & )m_pOriginalPresent, Hook_Present );
		//DetourDetach( &( PVOID & )m_pOriginalPresentEx, Hook_PresentEx );
		//DetourDetach( &( PVOID & )m_pOriginalCreateAdditionalSwapChain, Hook_CreateAdditionalSwapChain );
		//DetourDetach( &( PVOID & )m_pOriginalSwapchainPresent, Hook_Swapchain_Present );
	}
	DetourTransactionCommit();
}

void PSUOverlay::InitializeControls()
{
	ImageManager::Get().Initialize( m_pDevice );

	AddControl( WINDOW_CONTROL_ID::WINDOW_CONFIG_DISPLAY, ConfigDisplay::Create() );
	AddControl( WINDOW_CONTROL_ID::WINDOW_LOOT_DISPLAY, LootDisplay::Create() );
	AddControl( WINDOW_CONTROL_ID::WINDOW_LOG_DISPLAY, LogDisplay::Create() );
	AddControl( WINDOW_CONTROL_ID::WINDOW_CAMERA_DISPLAY, CameraDisplay::Create() );
	AddControl( WINDOW_CONTROL_ID::WINDOW_VR_DISPLAY, VRDisplay::Create() );
	AddControl( WINDOW_CONTROL_ID::WINDOW_VR_SETUP_DISPLAY, VRSetupDisplay::Create() );
}

void PSUOverlay::HookWindow()
{
	m_pWndProc = ( WNDPROC )SetWindowLongPtr( Global::hWnd, GWLP_WNDPROC, ( LONG_PTR )WndProc );
}

void PSUOverlay::ReleaseWindow()
{
	SetWindowLongPtr( m_hWnd, GWLP_WNDPROC, ( LONG_PTR )m_pWndProc );
}

LRESULT WINAPI PSUOverlay::WndProc( const HWND hWnd, const UINT msg, const WPARAM wParam, const LPARAM lParam )
{
	if( ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) )
	{
		ImGui::GetIO().MouseDrawCursor = ImGui::IsWindowHovered( ImGuiHoveredFlags_AnyWindow );
		return true;
	}
	
	if( m_bInitialized )
	{
		ImGui::GetIO().MouseDrawCursor = false;
	}
	
	if( msg == WM_CLOSE )
	{
		ReleaseDirectX();
		ReleaseWindow();
		TerminateProcess( GetCurrentProcess(), 0 );
	}
	
	if( ImGui::GetIO().WantCaptureMouse )
	{
		if( ImGui::IsWindowHovered( ImGuiHoveredFlags_AnyWindow ) )
			return true;
		return false;
	}

	return CallWindowProc( m_pWndProc, hWnd, msg, wParam, lParam );
}

HRESULT APIENTRY PSUOverlay::Hook_CreateDevice( IDirect3D9 *pD3D, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface )
{
	static int deviceCreationCount = 0;

	deviceCreationCount++;

	HRESULT hr = E_FAIL;
	if( PSUVR::IsD3D9ExActive() )
	{
		hr = PSUVR::TryCreateDeviceEx(
			pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
			pPresentationParameters, ppReturnedDeviceInterface );
	}
	if( FAILED( hr ) )
	{
		hr = m_pOriginalCreateDevice( pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface );
	}

	if( SUCCEEDED( hr ) && ppReturnedDeviceInterface )
	{
		IDirect3DDevice9 *pDevice = *ppReturnedDeviceInterface;

		// Managed-pool remap must cover every Ex device (temp + real share a vtable).
		if( PSUVR::IsD3D9ExActive() )
		{
			PSUVR::InstallD3D9ExCompatHooks( pDevice );
		}

		// Log the vTable pointer
		void **vTable = *( void *** )pDevice;

		// Hook only the actual rendering device (the second one)
		if( deviceCreationCount == 2 )
		{
			Global::pD3DDevice = pDevice;

			// IDirect3DDevice9 vtable indices (d3d9.h)
			m_pOriginalReset = ( FunctionD3DReset )vTable[ 16 ];
			m_pOriginalPresent = ( FunctionD3DPresent )vTable[ 17 ];
			m_pOriginalDrawPrimitive = ( FunctionD3DDrawPrimitive )vTable[ 81 ];
			m_pOriginalDrawIndexedPrimitive = ( FunctionD3DDrawIndexedPrimitive )vTable[ 82 ];
			m_pOriginalDrawPrimitiveUP = ( FunctionD3DDrawPrimitiveUP )vTable[ 83 ];
			m_pOriginalDrawIndexedPrimitiveUP = ( FunctionD3DDrawIndexedPrimitiveUP )vTable[ 84 ];
			m_pOriginalCreateAdditionalSwapChain = ( FunctionCreateAdditionalSwapChain )vTable[ 13 ];

			m_pOriginalSetRenderTarget = ( FunctionSetRenderTarget )vTable[ 37 ];
			m_pOriginalStretchRect = ( FunctionStretchRect )vTable[ 34 ];
			m_pOriginalBeginScene = ( FunctionD3DBeginScene )vTable[ 41 ];
			m_pOriginalEndScene = ( FunctionD3DEndScene )vTable[ 42 ];
			m_pOriginalSetTransform = ( FunctionSetTransform )vTable[ 44 ];
			m_pOriginalSetScissorRect = ( FunctionSetScissorRect )vTable[ 75 ];
			m_pOriginalSetViewport = ( FunctionSetViewport )vTable[ 47 ];
			m_pOriginalSetVertexShaderConstantF = ( FunctionSetVertexShaderConstantF )vTable[ 94 ];

			DetourTransactionBegin();
			DetourUpdateThread( GetCurrentThread() );

			if( DetourAttach( &( PVOID & )m_pOriginalPresent, Hook_Present ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook Present!\n" );
			}

			if( DetourAttach( &( PVOID & )m_pOriginalBeginScene, Hook_BeginScene ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook BeginScene!\n" );
			}

			if( DetourAttach( &( PVOID & )m_pOriginalEndScene, Hook_EndScene ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook EndScene!\n" );
			}

			if( DetourAttach( &( PVOID & )m_pOriginalSetTransform, Hook_SetTransform ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook SetTransform!\n" );
			}

			if( DetourAttach( &( PVOID & )m_pOriginalSetRenderTarget, Hook_SetRenderTarget ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook SetRenderTarget!\n" );
			}

			if( DetourAttach( &( PVOID & )m_pOriginalSetViewport, Hook_SetViewport ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook SetViewport!\n" );
			}

			if( DetourAttach( &( PVOID & )m_pOriginalSetScissorRect, Hook_SetScissorRect ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook SetScissorRect!\n" );
			}

			if( DetourAttach( &( PVOID & )m_pOriginalStretchRect, Hook_StretchRect ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook StretchRect!\n" );
			}

			// Gated DIP sampling while inside PSU_SceneMode1_Callback (pre-UI only).
			if( DetourAttach( &( PVOID & )m_pOriginalDrawIndexedPrimitive, Hook_DrawIndexedPrimitive ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook DrawIndexedPrimitive!\n" );
			}

			// Render-pass tracing: non-indexed draws and shader-constant uploads.
			if( DetourAttach( &( PVOID & )m_pOriginalDrawPrimitive, Hook_DrawPrimitive ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook DrawPrimitive!\n" );
			}
			if( DetourAttach( &( PVOID & )m_pOriginalDrawPrimitiveUP, Hook_DrawPrimitiveUP ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook DrawPrimitiveUP!\n" );
			}
			// The glare / glow layers draw with DrawIndexedPrimitiveUP: skipped in the eyes when off.
			if( DetourAttach( &( PVOID & )m_pOriginalDrawIndexedPrimitiveUP, Hook_DrawIndexedPrimitiveUP ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook DrawIndexedPrimitiveUP!\n" );
			}
			if( DetourAttach( &( PVOID & )m_pOriginalSetVertexShaderConstantF, Hook_SetVertexShaderConstantF ) != NO_ERROR )
			{
				PSU_LOG( "Failed to hook SetVertexShaderConstantF!\n" );
			}

			PSUVR::System::Get().OnDeviceCreated( pDevice );

			HRESULT result = DetourTransactionCommit();
			if( result != NO_ERROR )
			{
				printf( "2 DetourTransactionCommit failed with error code: %d\n", result );
			}

			printf( "Hooks successfully installed on the real device!\n" );
		}
		else
		{
			printf( "Skipping hook for temporary device.\n" );
		}
	}
	else
	{
		printf( "Failed to create device! HRESULT: 0x%08X\n", hr );
	}

	return hr;
}

HRESULT APIENTRY PSUOverlay::Hook_Reset( LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS *pPresentationParameters )
{
	ImGui_ImplDX9_InvalidateDeviceObjects();
	HRESULT tmpReset = m_pOriginalReset( pDevice, pPresentationParameters );
	ImGui_ImplDX9_CreateDeviceObjects();
	return tmpReset;
}


// The game's own return address for a draw call. With another D3D9 hook layered over ours (Rogue's
// Toolkit) the immediate caller is that mod's code, which hides the game's draw sites that the eye
// passes recognize (e.g. the sprite drawer 0x00747248): walk up the stack to the first call site in the exe.
static __forceinline bool InGameExe( uintptr_t a )
{
	return a >= 0x00401000 && a < 0x00B00000;
}

// Our own DLL's image: its draws (controller reticle, beams, overlay) must keep their own return address,
// not be mistaken for a game draw site found further up the stack.
static bool InOwnModule( uintptr_t a )
{
	static uintptr_t s_base = 0, s_end = 0;
	if( !s_base )
	{
		HMODULE self = nullptr;
		if( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast< LPCSTR >( &InOwnModule ), &self ) )
		{
			const auto *dos = reinterpret_cast< const IMAGE_DOS_HEADER * >( self );
			const auto *nt = reinterpret_cast< const IMAGE_NT_HEADERS * >( reinterpret_cast< const char * >( self ) + dos->e_lfanew );
			s_base = reinterpret_cast< uintptr_t >( self );
			s_end = s_base + nt->OptionalHeader.SizeOfImage;
		}
	}
	return a >= s_base && a < s_end;
}

static __forceinline void *GameRet( void *returnAddressSlot, void *ret )
{
	if( InGameExe( reinterpret_cast< uintptr_t >( ret ) ) || InOwnModule( reinterpret_cast< uintptr_t >( ret ) ) )
	{
		return ret;
	}
	const uintptr_t *slot = static_cast< const uintptr_t * >( returnAddressSlot );
	for( int i = 1; i < 96; ++i )
	{
		const uintptr_t a = slot[ i ];
		if( !InGameExe( a ) )
		{
			continue;
		}
		const unsigned char *p = reinterpret_cast< const unsigned char * >( a );
		const auto indirectCall = []( unsigned char op, unsigned char modrm ) { return op == 0xFF && ( ( modrm >> 3 ) & 7 ) == 2; };
		// call rel32, call [reg+disp32] / [abs], call [reg+disp8], call reg / [reg]
		if( p[ -5 ] == 0xE8 || indirectCall( p[ -6 ], p[ -5 ] ) || indirectCall( p[ -3 ], p[ -2 ] ) || indirectCall( p[ -2 ], p[ -1 ] ) )
		{
			return reinterpret_cast< void * >( a );
		}
	}
	return ret;
}
#define GAME_RET() GameRet( _AddressOfReturnAddress(), _ReturnAddress() )

// Draws issued while the real Present runs: that is Rogue's Toolkit drawing its panels and menu from its
// Present hook (it sits below ours). Counted so the HUD panel is only marked when it actually drew.
static bool s_inToolkitPresent = false;
static unsigned s_toolkitPresentDraws = 0;
typedef HRESULT( APIENTRY *RtFn )( LPDIRECT3DDEVICE9, DWORD, IDirect3DSurface9 * );
typedef HRESULT( APIENTRY *VpFn )( LPDIRECT3DDEVICE9, CONST D3DVIEWPORT9 * );
static bool s_toolkitScene = false;
static IDirect3DSurface9 *s_toolkitSavedRt = nullptr;
static IDirect3DSurface9 *s_toolkitSavedDs = nullptr;
static D3DVIEWPORT9 s_toolkitSavedVp{};

// A caller that is neither the game nor this DLL: Rogue's Toolkit (or D3DX working for it).
static bool CallerIsOther( void *ret )
{
	if( InGameExe( reinterpret_cast< uintptr_t >( ret ) ) )
	{
		return false;
	}
	HMODULE self = nullptr;
	GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast< LPCSTR >( &CallerIsOther ), &self );
	HMODULE caller = nullptr;
	GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast< LPCSTR >( ret ), &caller );
	return caller != self;
}

// Log where the toolkit's calls come from (first few), to find its drawing path.
static void LogToolkitCall( const char *what, void *ret )
{
	static unsigned s_logged = 0;
	if( s_logged >= 24 )
	{
		return;
	}
	HMODULE caller = nullptr;
	char name[ MAX_PATH ] = "?";
	if( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast< LPCSTR >( ret ), &caller ) )
	{
		GetModuleFileNameA( caller, name, MAX_PATH );
	}
	++s_logged;
	PSU_LOG( "VRToolkit: %s from %s (ret %p) inPresent=%d inScene=%d\n", what, name, ret, s_inToolkitPresent ? 1 : 0,
		s_toolkitScene ? 1 : 0 );
}

// Point drawing at the HUD panel (and back).
static bool BeginToolkitRedirect( IDirect3DDevice9 *pDevice, RtFn setRt, VpFn setVp )
{
	auto &hud = PSUVR::VRHud::Get();
	PSUVR::D3D11Bridge *bridge = PSUVR::System::Get().StereoBridge();
	IDirect3DSurface9 *hudSurface = bridge ? bridge->HudUserSurface() : nullptr;
	if( !Config::Get().vr.m_enabled || !Config::Get().vr.m_toolkitOnHud || !hud.Active() || !hudSurface )
	{
		return false;
	}
	D3DSURFACE_DESC desc{};
	hudSurface->GetDesc( &desc );
	pDevice->GetRenderTarget( 0, &s_toolkitSavedRt );
	pDevice->GetDepthStencilSurface( &s_toolkitSavedDs );
	pDevice->GetViewport( &s_toolkitSavedVp );
	setRt( pDevice, 0, hudSurface );
	pDevice->SetDepthStencilSurface( nullptr );
	const D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
	setVp( pDevice, &vp );
	return true;
}

static void EndToolkitRedirect( IDirect3DDevice9 *pDevice, RtFn setRt, VpFn setVp )
{
	setRt( pDevice, 0, s_toolkitSavedRt );
	pDevice->SetDepthStencilSurface( s_toolkitSavedDs );
	setVp( pDevice, &s_toolkitSavedVp );
	if( s_toolkitSavedRt )
	{
		s_toolkitSavedRt->Release();
		s_toolkitSavedRt = nullptr;
	}
	if( s_toolkitSavedDs )
	{
		s_toolkitSavedDs->Release();
		s_toolkitSavedDs = nullptr;
	}
}

#define NOTE_TOOLKIT_DRAW( name ) do { \
	if( s_inToolkitPresent ) { ++s_toolkitPresentDraws; } \
	if( ( s_inToolkitPresent || s_toolkitScene ) && CallerIsOther( _ReturnAddress() ) ) { LogToolkitCall( name, _ReturnAddress() ); } \
} while( 0 )

#ifdef _DEBUG
// Where a screen-space draw happens, for the VREffect log: 0 eye pass, 1 pass B / UI, 2 other.
static int EffectDrawWhere()
{
	return PSUVR::StereoRenderer::InEyePass() ? 0
		: ( FrameTrace::CurrentPassAddress() == 0x004FDBC0 || PSUVR::VRHud::Get().InUiDraw() ) ? 1 : 2;
}
#endif

HRESULT APIENTRY PSUOverlay::Hook_DrawPrimitive( LPDIRECT3DDEVICE9 pDevice, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount )
{
	NOTE_TOOLKIT_DRAW( "DrawPrimitive" );
#ifdef _DEBUG
	PSUVR::StereoRenderer::CountLayerDraw( GAME_RET() );
#endif
	if( PSUVR::StereoRenderer::SkipEyeEffectDraw( GAME_RET() ) )
	{
		return D3D_OK;
	}
	FrameTrace::OnDrawPrimitive( false );
#ifdef _DEBUG
	PSUVR::StereoRenderer::NoteShaderDraw( pDevice, PrimitiveCount, GAME_RET() );
	PSUVR::StereoRenderer::NoteEffectDraw( pDevice, EffectDrawWhere(), static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
		nullptr, 0, GAME_RET() );
#endif
	if( PSUVR::StereoRenderer::ShouldSkipScreenQuad( pDevice, GAME_RET(), PrimitiveCount, false ) )
	{
		return D3D_OK;
	}
	const PSUVR::LocalPlayerDrawScope localPlayer( pDevice );
	if( localPlayer.Skip() )
	{
		return D3D_OK;
	}
	const HRESULT hr = m_pOriginalDrawPrimitive( pDevice, PrimitiveType, StartVertex, PrimitiveCount );
	// Mission map (pass-B layers 0x31 / 0x32) mesh draws onto the HUD panel.
	auto &hud = PSUVR::VRHud::Get();
	if( SUCCEEDED( hr ) && hud.Active() && FrameTrace::CurrentPassAddress() == 0x004FDBC0 )
	{
		hud.CaptureMapMeshDraw( pDevice, 0, static_cast< unsigned >( PrimitiveType ), 0, 0, 0, StartVertex,
			PrimitiveCount, nullptr, 0, nullptr, 0, GAME_RET() );
	}
	return hr;
}

HRESULT APIENTRY PSUOverlay::Hook_SetVertexShaderConstantF( LPDIRECT3DDEVICE9 pDevice, UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount )
{
	FrameTrace::OnVertexShaderConstantF( StartRegister, Vector4fCount, GAME_RET() );
#ifdef _DEBUG
	PSUVR::StereoRenderer::NoteVertexConstants( StartRegister, pConstantData, Vector4fCount, GAME_RET() );
#endif
	return m_pOriginalSetVertexShaderConstantF( pDevice, StartRegister, pConstantData, Vector4fCount );
}

HRESULT APIENTRY PSUOverlay::Hook_DrawIndexedPrimitive( LPDIRECT3DDEVICE9 pDevice, D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount )
{
	NOTE_TOOLKIT_DRAW( "DrawIndexedPrimitive" );
#ifdef _DEBUG
	PSUVR::StereoRenderer::CountLayerDraw( GAME_RET() );
#endif
	if( PSUVR::StereoRenderer::SkipEyeEffectDraw( GAME_RET() ) )
	{
		return D3D_OK;
	}
	FrameTrace::OnDrawIndexed( GAME_RET() );
#ifdef _DEBUG
	PSUVR::StereoRenderer::NoteShaderDraw( pDevice, PrimitiveCount, GAME_RET() );
	PSUVR::StereoRenderer::NoteEffectDraw( pDevice, EffectDrawWhere(), static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
		nullptr, 0, GAME_RET() );
#endif
	if( PSUVR::StereoRenderer::ShouldSkipScreenQuad( pDevice, GAME_RET(), PrimitiveCount, false ) )
	{
		return D3D_OK;
	}
	const PSUVR::LocalPlayerDrawScope localPlayer( pDevice );
	if( localPlayer.Skip() )
	{
		return D3D_OK;
	}
	const HRESULT hr = m_pOriginalDrawIndexedPrimitive(
		pDevice, PrimitiveType, BaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount
	);
	// Font DIP only during Pass B or nested UI — avoid flat BB composites over stereo.
	auto &hudDip = PSUVR::VRHud::Get();
	if( SUCCEEDED( hr ) && hudDip.Active() &&
		( FrameTrace::CurrentPassAddress() == 0x004FDBC0 || hudDip.InUiDraw() ) )
	{
		hudDip.CaptureXyzrhwIndexed( pDevice,
			static_cast< unsigned >( PrimitiveType ), BaseVertexIndex, MinIndex, NumVertices,
			StartIndex, PrimitiveCount, GAME_RET() );
	}
	if( SUCCEEDED( hr ) && hudDip.Active() && FrameTrace::CurrentPassAddress() == 0x004FDBC0 )
	{
		hudDip.CaptureMapMeshDraw( pDevice, 1, static_cast< unsigned >( PrimitiveType ), BaseVertexIndex, MinIndex,
			NumVertices, StartIndex, PrimitiveCount, nullptr, 0, nullptr, 0, GAME_RET() );
	}
	return hr;
}

HRESULT APIENTRY PSUOverlay::Hook_DrawPrimitiveUP( LPDIRECT3DDEVICE9 pDevice, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride )
{
	NOTE_TOOLKIT_DRAW( "DrawPrimitiveUP" );
#ifdef _DEBUG
	PSUVR::StereoRenderer::CountLayerDraw( GAME_RET() );
#endif
	if( PSUVR::StereoRenderer::SkipEyeEffectDraw( GAME_RET() ) )
	{
		return D3D_OK;
	}
	FrameTrace::OnDrawPrimitive( true );
#ifdef _DEBUG
	PSUVR::StereoRenderer::NoteShaderDraw( pDevice, PrimitiveCount, GAME_RET() );
	PSUVR::StereoRenderer::NoteEffectDraw( pDevice, EffectDrawWhere(), static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
		pVertexStreamZeroData, VertexStreamZeroStride, GAME_RET() );
	PSUVR::StereoRenderer::NoteNearCameraDraw( pDevice, static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
		pVertexStreamZeroData, VertexStreamZeroStride, GAME_RET() );
#endif
	if( PSUVR::StereoRenderer::ShouldSkipScreenQuad( pDevice, GAME_RET(), PrimitiveCount, true ) )
	{
		return D3D_OK;
	}
	// Free aim: the game's reticle isn't drawn at all (not even in the game's own frame).
	if( ( FrameTrace::CurrentPassAddress() == 0x004FDBC0 || PSUVR::VRHud::Get().InUiDraw() ) &&
		PSUVR::VRHud::Get().BlockReticleDraw( pDevice, pVertexStreamZeroData, static_cast< unsigned >( PrimitiveType ),
			PrimitiveCount, VertexStreamZeroStride ) )
	{
		return D3D_OK;
	}
	const void *verts = pVertexStreamZeroData;
	bool reprojectedSprite = false;
	if( const void *reprojected = PSUVR::StereoRenderer::ReprojectSpriteIfNeeded(
			pDevice, static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
			pVertexStreamZeroData, VertexStreamZeroStride, GAME_RET() ) )
	{
		verts = reprojected;
		reprojectedSprite = true;
	}
	else if( const void *stretched = PSUVR::StereoRenderer::StretchScreenQuadIfNeeded(
			pDevice, static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
			pVertexStreamZeroData, VertexStreamZeroStride, GAME_RET() ) )
	{
		verts = stretched;
	}
	// A world sprite placed for this eye: hidden behind whatever is nearer (the weapon in your
	// hand), whatever depth test the game left set for its flat screen; it never writes depth.
	DWORD savedZEnable = 0, savedZFunc = 0, savedZWrite = 0;
	if( reprojectedSprite )
	{
		pDevice->GetRenderState( D3DRS_ZENABLE, &savedZEnable );
		pDevice->GetRenderState( D3DRS_ZFUNC, &savedZFunc );
		pDevice->GetRenderState( D3DRS_ZWRITEENABLE, &savedZWrite );
		pDevice->SetRenderState( D3DRS_ZENABLE, D3DZB_TRUE );
		pDevice->SetRenderState( D3DRS_ZFUNC, D3DCMP_LESSEQUAL );
		pDevice->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	}
	// Pass B chrome, or DP-UP nested inside 0x793C40 — not every post-frame fullscreen blit.
	auto &hud = PSUVR::VRHud::Get();
	const bool capture = hud.Active() && ( FrameTrace::CurrentPassAddress() == 0x004FDBC0 || hud.InUiDraw() );
	// flatMirrorEye: the monitor shows the left eye, so a world nameplate is placed there by the capture
	// (before the game's draw, which would put it where the flat camera sees it).
	const bool captureFirst = capture && PSUVR::StereoRenderer::Get().MirrorActive();
	if( captureFirst )
	{
		hud.CapturePassBDraw( pDevice,
			static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
			pVertexStreamZeroData, VertexStreamZeroStride, GAME_RET() );
	}
	const HRESULT hr = captureFirst && hud.NameplateOnMonitor() ? D3D_OK
		: m_pOriginalDrawPrimitiveUP( pDevice, PrimitiveType, PrimitiveCount, verts, VertexStreamZeroStride );
	if( reprojectedSprite )
	{
		pDevice->SetRenderState( D3DRS_ZENABLE, savedZEnable );
		pDevice->SetRenderState( D3DRS_ZFUNC, savedZFunc );
		pDevice->SetRenderState( D3DRS_ZWRITEENABLE, savedZWrite );
	}
	if( SUCCEEDED( hr ) && capture && !captureFirst )
	{
		hud.CapturePassBDraw( pDevice,
			static_cast< unsigned >( PrimitiveType ), PrimitiveCount,
			pVertexStreamZeroData, VertexStreamZeroStride, GAME_RET() );
	}
	return hr;
}

HRESULT APIENTRY PSUOverlay::Hook_DrawIndexedPrimitiveUP( LPDIRECT3DDEVICE9 pDevice, D3DPRIMITIVETYPE Type, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride )
{
	NOTE_TOOLKIT_DRAW( "DrawIndexedPrimitiveUP" );
#ifdef _DEBUG
	PSUVR::StereoRenderer::CountLayerDraw( GAME_RET() );
#endif
	if( PSUVR::StereoRenderer::SkipEyeEffectDraw( GAME_RET() ) )
	{
		return D3D_OK;
	}
	// HUD capture here only for the mission map (pass-B layers 0x31 / 0x32): this hook only became
	// attached for the glare / glow, and the rest of the HUD worked without it.
	const HRESULT hr = m_pOriginalDrawIndexedPrimitiveUP( pDevice, Type, MinVertexIndex, NumVertices, PrimitiveCount,
		pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride );
	auto &hud = PSUVR::VRHud::Get();
	if( SUCCEEDED( hr ) && hud.Active() && FrameTrace::CurrentPassAddress() == 0x004FDBC0 )
	{
		hud.CaptureMapMeshDraw( pDevice, 2, static_cast< unsigned >( Type ), 0, MinVertexIndex, NumVertices, 0,
			PrimitiveCount, pIndexData, static_cast< unsigned >( IndexDataFormat ), pVertexStreamZeroData,
			VertexStreamZeroStride, GAME_RET() );
	}
	return hr;
}

void PrintMatrix( const Matrix4x4 &matrix, const char *name )
{
	printf( "%s Matrix:\n", name );
	for( int row = 0; row < 4; ++row )
	{
		printf( "[ %.2f, %.2f, %.2f, %.2f ]\n",
				matrix.m[ row ][ 0 ], matrix.m[ row ][ 1 ], matrix.m[ row ][ 2 ], matrix.m[ row ][ 3 ] );
	}
	printf( "\n" );
}

HRESULT APIENTRY PSUOverlay::Hook_BeginScene( LPDIRECT3DDEVICE9 pDevice )
{
	// A scene opened by Rogue's Toolkit (not the game, not us): its panels and menu draw into it, so point the
	// scene at the HUD panel until its EndScene (vr.toolkitOnHud).
	if( !s_toolkitScene && !s_inToolkitPresent && CallerIsOther( _ReturnAddress() ) )
	{
		LogToolkitCall( "BeginScene", _ReturnAddress() );
		const HRESULT hr = m_pOriginalBeginScene( pDevice );
		if( SUCCEEDED( hr ) && BeginToolkitRedirect( pDevice, m_pOriginalSetRenderTarget, m_pOriginalSetViewport ) )
		{
			s_toolkitScene = true;
		}
		return hr;
	}
	FrameTrace::OnBeginScene( GAME_RET() );
	return m_pOriginalBeginScene( pDevice );
}

HRESULT APIENTRY PSUOverlay::Hook_EndScene( LPDIRECT3DDEVICE9 pDevice )
{
	static bool s_logged = false;
	if( !s_logged )
	{
		s_logged = true;
		PSU_LOG( "EndScene hooked (first call) device=%p\n", pDevice );
	}

	if( s_toolkitScene && CallerIsOther( _ReturnAddress() ) )
	{
		LogToolkitCall( "EndScene", _ReturnAddress() );
		const HRESULT hr = m_pOriginalEndScene( pDevice );
		EndToolkitRedirect( pDevice, m_pOriginalSetRenderTarget, m_pOriginalSetViewport );
		s_toolkitScene = false;
		PSUVR::VRHud::Get().MarkUserContent();
		return hr;
	}

	FrameTrace::OnEndScene( GAME_RET() );

	if( !m_bInitialized )
	{
		m_pDevice = pDevice;
		InitializeImGui( pDevice );
		InitializeControls();
		HookWindow();
	}

	PSUVR::VRPerf::Scope overlayTimer( PSUVR::VRPerf::Stage::Overlay );
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	// After the Win32 backend, so the controller pointer wins over the OS mouse.
	PSUVR::VRUiPointer::FeedImGui();
	ImGui::NewFrame();
	{
		if( ImGui::IsKeyPressed( ImGuiKey_Insert, false ) )
		{
			m_bDisplay ^= 1;
		}

		if( m_bDisplay )
		{
			RenderMenuBar();
			if( m_bStyleEditor )
				ImGui::ShowStyleEditor( &m_style );
		}

		for( auto &i : m_Controls )
		{
			i.second->Render();
		}
		PSUVR::VRPerf::DrawOverlay();
	}
	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData( ImGui::GetDrawData() );

	// In stereo the headset never shows the back buffer: draw the overlay into the HUD
	// panel too (VR settings, toasts, any open PSUEx window).
	{
		auto &hud = PSUVR::VRHud::Get();
		ImDrawData *drawData = ImGui::GetDrawData();
		PSUVR::D3D11Bridge *bridge = PSUVR::System::Get().StereoBridge();
		IDirect3DSurface9 *hudSurface = bridge ? bridge->HudUserSurface() : nullptr;
		if( hud.Active() && hudSurface && drawData && drawData->CmdListsCount > 0 )
		{
			IDirect3DSurface9 *savedRt = nullptr;
			IDirect3DSurface9 *savedDs = nullptr;
			D3DVIEWPORT9 savedVp{};
			pDevice->GetRenderTarget( 0, &savedRt );
			pDevice->GetDepthStencilSurface( &savedDs );
			pDevice->GetViewport( &savedVp );
			pDevice->SetRenderTarget( 0, hudSurface );
			pDevice->SetDepthStencilSurface( nullptr );
			ImGui_ImplDX9_RenderDrawData( drawData );
			pDevice->SetRenderTarget( 0, savedRt );
			pDevice->SetDepthStencilSurface( savedDs );
			pDevice->SetViewport( &savedVp );
			if( savedRt )
			{
				savedRt->Release();
			}
			if( savedDs )
			{
				savedDs->Release();
			}
			hud.MarkUserContent();
		}
	}
	// Controller label cards into their own target (submitted as quads at the controllers).
	PSUVR::VRControllerLabels::Render( pDevice, PSUVR::System::Get().StereoBridge() );

#ifdef _DEBUG
	// Ctrl+Shift+End: save the flat frame and the HUD panel as PNGs in the game folder.
	{
		static bool s_endDown = false;
		const bool end = ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) && ( GetAsyncKeyState( VK_SHIFT ) & 0x8000 ) &&
			( GetAsyncKeyState( VK_END ) & 0x8000 );
		if( end && !s_endDown )
		{
			IDirect3DSurface9 *backBuffer = nullptr;
			HRESULT bbHr = pDevice->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer );
			if( SUCCEEDED( bbHr ) && backBuffer )
			{
				bbHr = D3DXSaveSurfaceToFileA( "psuex_dump_frame.png", D3DXIFF_PNG, backBuffer, nullptr, nullptr );
				backBuffer->Release();
			}
			PSUVR::D3D11Bridge *dumpBridge = PSUVR::System::Get().StereoBridge();
			IDirect3DSurface9 *hudUser = dumpBridge ? dumpBridge->HudUserSurface() : nullptr;
			HRESULT hudHr = E_FAIL;
			if( hudUser )
			{
				hudHr = D3DXSaveSurfaceToFileA( "psuex_dump_hud.png", D3DXIFF_PNG, hudUser, nullptr, nullptr );
			}
			PSU_LOG( "Dump: frame hr=0x%08lX hud hr=0x%08lX\n", static_cast< unsigned long >( bbHr ),
				static_cast< unsigned long >( hudHr ) );
		}
		s_endDown = end;
	}
#endif

	return m_pOriginalEndScene( pDevice );
}

HRESULT APIENTRY PSUOverlay::Hook_Present( LPDIRECT3DDEVICE9 pDevice, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion )
{
	static bool s_logged = false;
	if( !s_logged )
	{
		s_logged = true;
		PSU_LOG( "Present hooked (first call) device=%p\n", pDevice );
	}

	FrameTrace::OnPresent();

	// Capture BB + GPU fence BEFORE Present; OpenXR WaitFrame AFTER so the
	// monitor is not blocked / blacked out by the VR compositor.
	PSUVR::VRPerf::BeforePresent( pDevice );
	{
		PSUVR::VRPerf::Scope timer( PSUVR::VRPerf::Stage::Capture );
		PSUVR::System::Get().OnPresentCapture();
	}
	HRESULT hr;
	{
		PSUVR::VRPerf::Scope timer( PSUVR::VRPerf::Stage::Present );
		// Rogue's Toolkit draws its panels and menu inside its Present hook, below this one. In stereo the headset
		// never shows the back buffer, so point that drawing at the HUD panel (vr.toolkitOnHud).
		auto &hud = PSUVR::VRHud::Get();
		const bool toHud = BeginToolkitRedirect( pDevice, m_pOriginalSetRenderTarget, m_pOriginalSetViewport );
		if( toHud )
		{
			s_toolkitPresentDraws = 0;
			s_inToolkitPresent = true;
		}
		hr = m_pOriginalPresent( pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion );
		if( toHud )
		{
			s_inToolkitPresent = false;
			EndToolkitRedirect( pDevice, m_pOriginalSetRenderTarget, m_pOriginalSetViewport );
			if( s_toolkitPresentDraws > 0 )
			{
				hud.MarkUserContent();
				static bool s_loggedToolkit = false;
				if( !s_loggedToolkit )
				{
					s_loggedToolkit = true;
					PSU_LOG( "VRToolkit: %u draws inside Present went to the HUD panel\n", s_toolkitPresentDraws );
				}
			}
		}
	}
	{
		PSUVR::VRPerf::Scope timer( PSUVR::VRPerf::Stage::Submit );
		PSUVR::System::Get().OnPresentSubmit();
	}
	PSUVR::VRPerf::EndFrame();
	return hr;
}

HRESULT APIENTRY PSUOverlay::Hook_CreateAdditionalSwapChain( LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **ppSwapChain )
{
	if( !pPresentationParameters || !ppSwapChain )
	{
		return D3DERR_INVALIDCALL;
	}

	HRESULT hr = m_pOriginalCreateAdditionalSwapChain( pDevice, pPresentationParameters, ppSwapChain );

	// The game does some aggressive swap chain creation at times
	// but I'm not sure if we'll need it at all.

	//if( SUCCEEDED( hr ) && ppSwapChain && *ppSwapChain )
	//{
	//	// Assign a unique ID to this swap chain
	//	swapChainCounter = swapChainMap.size();
	//	swapChainMap[ *ppSwapChain ] = swapChainCounter;
	//
	//	// Grab and save the original swap chain functions
	//	void **vTable = *( void *** )*ppSwapChain;
	//	auto originalSwapChainPresent = ( FunctionSwapChainPresent )vTable[ 3 ];
	//	auto originalSwapChainRelease = ( FunctionSwapChainRelease )vTable[ 2 ];
	//	auto originalSwapChainDrawIndexedPrimitiveUP = ( FunctionD3DDrawIndexedPrimitiveUP )vTable[ 53 ];
	//
	//	m_pOriginalSwapChainPresentsMap[ *ppSwapChain ] = originalSwapChainPresent;
	//	m_pOriginalSwapChainReleasesMap[ *ppSwapChain ] = originalSwapChainRelease;
	//	m_pOriginalSwapChainDrawIndexedPrimitiveUPsMap[ *ppSwapChain ] = originalSwapChainDrawIndexedPrimitiveUP;
	//
	//	// Hook the function
	//	DWORD oldProtect;
	//	VirtualProtect( &vTable[ 3 ], sizeof( void * ), PAGE_EXECUTE_READWRITE, &oldProtect );
	//	vTable[ 3 ] = ( void * )Hook_Swapchain_Present;
	//	VirtualProtect( &vTable[ 3 ], sizeof( void * ), oldProtect, &oldProtect );
	//
	//	VirtualProtect( &vTable[ 2 ], sizeof( void * ), PAGE_EXECUTE_READWRITE, &oldProtect );
	//	vTable[ 2 ] = ( void * )Hook_Swapchain_Release;
	//	VirtualProtect( &vTable[ 2 ], sizeof( void * ), oldProtect, &oldProtect );
	//
	//	VirtualProtect( &vTable[ 53 ], sizeof( void * ), PAGE_EXECUTE_READWRITE, &oldProtect );
	//	vTable[ 53 ] = ( void * )Hook_DrawIndexedPrimitiveUP;
	//	VirtualProtect( &vTable[ 53 ], sizeof( void * ), oldProtect, &oldProtect );
	//}

	return hr;
}

HRESULT APIENTRY PSUOverlay::Hook_Swapchain_Present( IDirect3DSwapChain9 *pSwapChain, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags )
{
	int swapChainID = swapChainMap.count( pSwapChain ) ? swapChainMap[ pSwapChain ] : -1;
	
	if( m_pOriginalSwapChainPresentsMap.count( pSwapChain ) )
	{
		auto originalPresent = m_pOriginalSwapChainPresentsMap[ pSwapChain ];
		return originalPresent( pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags );
	}

	return D3DERR_INVALIDCALL;
}

HRESULT APIENTRY PSUOverlay::Hook_Swapchain_Release( IDirect3DSwapChain9 *pSwapChain )
{
	int swapChainID = swapChainMap.count( pSwapChain ) ? swapChainMap[ pSwapChain ] : -1;

	// Retrieve the original Release function before deleting the entry
	auto originalReleaseIterator = m_pOriginalSwapChainReleasesMap.find( pSwapChain );

	// Remove the swap chain from the maps to avoid memory leaks
	m_pOriginalSwapChainPresentsMap.erase( pSwapChain );
	swapChainMap.erase( pSwapChain );

	// Check if the original Release function exists
	if( originalReleaseIterator != m_pOriginalSwapChainReleasesMap.end() )
	{
		// Store the original function pointer and erase it from the map AFTER retrieval
		auto originalRelease = originalReleaseIterator->second;
		m_pOriginalSwapChainReleasesMap.erase( originalReleaseIterator );

		// Call the original Release function
		return originalRelease( pSwapChain );
	}

	return 0;
}

// The game's SetTransform wrapper (0x00749D40) only forwards (device, state, matrix): when the
// call comes from it, its own caller (4 dwords up the stack) says which object this is.
static void *TransformCaller( void *returnAddressSlot )
{
	void **slot = static_cast< void ** >( returnAddressSlot );
	if( reinterpret_cast< uintptr_t >( slot[ 0 ] ) == 0x00749D5C )
	{
		return slot[ 4 ];
	}
	return slot[ 0 ];
}

HRESULT APIENTRY PSUOverlay::Hook_SetTransform( LPDIRECT3DDEVICE9 pDevice, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix )
{
	FrameTrace::OnSetTransform( State, pMatrix, _ReturnAddress() );
	D3DMATRIX recentered;
	if( State == D3DTS_WORLD && pMatrix &&
		PSUVR::StereoRenderer::RecenterCameraAnchored( *pMatrix, recentered, TransformCaller( _AddressOfReturnAddress() ) ) )
	{
		return m_pOriginalSetTransform( pDevice, State, &recentered );
	}
	return m_pOriginalSetTransform( pDevice, State, pMatrix );
}

HRESULT APIENTRY PSUOverlay::Hook_SetViewport( LPDIRECT3DDEVICE9 pDevice, CONST D3DVIEWPORT9 *pViewport )
{
	// Pass A restores the game 16:9 viewport after shadows; on an OpenXR-sized eye RT
	// that leaves the lower band black. Keep the full eye viewport whenever the eye RT is bound.
	if( pViewport && PSUVR::StereoRenderer::EyeColorTargetBound( pDevice ) )
	{
		D3DVIEWPORT9 eyeVp{};
		PSUVR::StereoRenderer::GetEyeViewport( eyeVp );
		// Keep the game's depth range: the sky is drawn pushed to the back (MinZ / MaxZ near 1),
		// and the full 0..1 range would put it at its real distance, in front of far scenery.
		eyeVp.MinZ = pViewport->MinZ;
		eyeVp.MaxZ = pViewport->MaxZ;
#ifdef _DEBUG
		static unsigned s_depthLogs = 0;
		if( s_depthLogs < 12 && ( pViewport->MinZ != 0.0f || pViewport->MaxZ != 1.0f ) )
		{
			++s_depthLogs;
			PSU_LOG( "StereoViewport: game depth range %.3f..%.3f kept (layer 0x%X)\n", pViewport->MinZ, pViewport->MaxZ,
				*reinterpret_cast< const int * >( 0x009946C8 ) );
		}
#endif
		static unsigned s_vpLogs = 0;
		if( s_vpLogs < 8 &&
			( pViewport->Width != eyeVp.Width || pViewport->Height != eyeVp.Height ) )
		{
			++s_vpLogs;
			PSU_LOG( "StereoViewport: override %ux%u -> %ux%u\n",
				pViewport->Width, pViewport->Height, eyeVp.Width, eyeVp.Height );
		}
		return m_pOriginalSetViewport( pDevice, &eyeVp );
	}
	return m_pOriginalSetViewport( pDevice, pViewport );
}

HRESULT APIENTRY PSUOverlay::Hook_SetRenderTarget( LPDIRECT3DDEVICE9 pDevice, DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget )
{
	FrameTrace::OnSetRenderTarget( pDevice, RenderTargetIndex, pRenderTarget, _ReturnAddress() );
	if( RenderTargetIndex == 0 )
	{
		PSUVR::StereoRenderer::OnSetRenderTarget( pRenderTarget );
	}
	const HRESULT hr = m_pOriginalSetRenderTarget( pDevice, RenderTargetIndex, pRenderTarget );
	if( SUCCEEDED( hr ) && RenderTargetIndex == 0 && PSUVR::StereoRenderer::EyeColorTargetIs( pRenderTarget ) )
	{
		D3DVIEWPORT9 eyeVp{};
		RECT eyeScissor{};
		PSUVR::StereoRenderer::GetEyeViewport( eyeVp );
		PSUVR::StereoRenderer::GetEyeScissor( eyeScissor );
		m_pOriginalSetViewport( pDevice, &eyeVp );
		if( m_pOriginalSetScissorRect )
		{
			m_pOriginalSetScissorRect( pDevice, &eyeScissor );
		}
	}
	return hr;
}

HRESULT APIENTRY PSUOverlay::Hook_StretchRect( LPDIRECT3DDEVICE9 pDevice, IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, CONST RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter )
{
	PSUVR::StereoRenderer::OnStretchRect( pSourceSurface, pDestSurface, _ReturnAddress() );
	return m_pOriginalStretchRect( pDevice, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter );
}

HRESULT APIENTRY PSUOverlay::Hook_SetScissorRect( LPDIRECT3DDEVICE9 pDevice, CONST RECT *pRect )
{
	if( PSUVR::StereoRenderer::EyeColorTargetBound( pDevice ) )
	{
		RECT eyeScissor{};
		PSUVR::StereoRenderer::GetEyeScissor( eyeScissor );
		return m_pOriginalSetScissorRect( pDevice, &eyeScissor );
	}
	return m_pOriginalSetScissorRect( pDevice, pRect );
}

void PSUOverlay::InitializeImGui( const LPDIRECT3DDEVICE9 pDevice )
{
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	auto &config = Config::Get();

	io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
	io.IniFilename = nullptr;
	io.Fonts->AddFontDefault();
	// The controller label cards are read at arm's length in the headset: a large TrueType font
	// (the default bitmap font blurs when scaled). Only when VR is on.
	if( config.vr.m_enabled )
	{
		char fontPath[ MAX_PATH ]{};
		const UINT length = GetWindowsDirectoryA( fontPath, MAX_PATH );
		if( length > 0 && length < MAX_PATH - 24 )
		{
			strcat_s( fontPath, "\\Fonts\\segoeui.ttf" );
			if( GetFileAttributesA( fontPath ) != INVALID_FILE_ATTRIBUTES )
			{
				PSUVR::VRControllerLabels::SetFont( io.Fonts->AddFontFromFileTTF( fontPath, 32.0f ) );
			}
		}
	}

	ImGui::StyleColorsDark( &m_style );

	auto &style = ImGui::GetStyle();

	printf( "ImGui Style:\n" );
	printf( "Alpha: %f\n", config.m_style.Alpha );
	printf( "WindowPadding: %f %f\n", config.m_style.WindowPadding.x, config.m_style.WindowPadding.y );
	printf( "FramePadding: %f %f\n", config.m_style.FramePadding.x, config.m_style.FramePadding.y );
	printf( "ItemSpacing: %f %f\n", config.m_style.ItemSpacing.x, config.m_style.ItemSpacing.y );
	printf( "WindowRounding: %f\n", config.m_style.WindowRounding );

	style.Alpha = config.m_style.Alpha;
	style.WindowPadding = config.m_style.WindowPadding;
	style.FramePadding = config.m_style.FramePadding;
	style.ItemSpacing = config.m_style.ItemSpacing;

	style.WindowRounding = config.m_style.WindowRounding;
	style.FrameRounding = config.m_style.FrameRounding;
	style.GrabRounding = config.m_style.GrabRounding;

	for( int i = 0; i < ImGuiCol_COUNT; ++i )
	{
		const ImVec4 &col = config.m_style.Colors[ i ];
		style.Colors[ i ] = col;
	}

	//style.WindowRounding = 0.0f;
	//style.FrameRounding = 0.0f;
	//style.Colors[ ImGuiCol_WindowBg ] = ImVec4( 0.1f, 0.1f, 0.1f, 0.2f );
	//style.Colors[ ImGuiCol_ChildBg ] = ImVec4( 0.1f, 0.1f, 0.1f, 0.2f );
	//style.Colors[ ImGuiCol_PopupBg ] = ImVec4( 0.1f, 0.1f, 0.1f, 0.2f );

	ImGui_ImplWin32_Init( Global::hWnd );
	ImGui_ImplDX9_Init( pDevice );

	m_bInitialized = true;
}
