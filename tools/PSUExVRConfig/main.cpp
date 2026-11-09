// PSUEx VR configurator: edits psuex_vr.json without running the game.
// Uses the same settings model and panel as the in-game "VR Settings" window.

#include "../../PSUVR/VRSettings.h"
#include "../../PSUVR/VRSettingsPanel.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <Windows.h>
#include <d3d11.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <fstream>
#include <sstream>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

namespace
{
	ID3D11Device *g_device = nullptr;
	ID3D11DeviceContext *g_context = nullptr;
	IDXGISwapChain *g_swapChain = nullptr;
	ID3D11RenderTargetView *g_rtv = nullptr;
	UINT g_resizeWidth = 0;
	UINT g_resizeHeight = 0;

	VRSettings g_settings;
	std::wstring g_folder;  // game folder holding psuex_vr.json
	char g_folderUtf8[ MAX_PATH * 3 ]{};
	bool g_fileFound = false;
	PSUVR::VRPanelContext g_panel;

	std::wstring ExeFolder()
	{
		wchar_t path[ MAX_PATH ]{};
		GetModuleFileNameW( nullptr, path, MAX_PATH );
		std::wstring folder( path );
		const size_t slash = folder.find_last_of( L"\\/" );
		return slash == std::wstring::npos ? folder : folder.substr( 0, slash );
	}

	std::wstring ConfigPath()
	{
		return g_folder + L"\\psuex_vr.json";
	}

	std::string Narrow( const std::wstring &text )
	{
		const int size = WideCharToMultiByte( CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr );
		std::string out( size > 0 ? size - 1 : 0, '\0' );
		if( size > 1 )
		{
			WideCharToMultiByte( CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr );
		}
		return out;
	}

	std::wstring Widen( const char *text )
	{
		const int size = MultiByteToWideChar( CP_UTF8, 0, text, -1, nullptr, 0 );
		std::wstring out( size > 0 ? size - 1 : 0, L'\0' );
		if( size > 1 )
		{
			MultiByteToWideChar( CP_UTF8, 0, text, -1, out.data(), size );
		}
		return out;
	}

	void SetFolder( const std::wstring &folder )
	{
		g_folder = folder;
		strncpy_s( g_folderUtf8, Narrow( folder ).c_str(), _TRUNCATE );
	}

	bool LoadSettings()
	{
		g_settings = VRSettings{};
		std::ifstream file( ConfigPath() );
		g_fileFound = file.is_open();
		if( !g_fileFound )
		{
			return false;
		}
		std::stringstream text;
		text << file.rdbuf();
		return LoadVRSettingsJson( g_settings, text.str() );
	}

	bool SaveSettings()
	{
		std::ofstream file( ConfigPath(), std::ios::trunc );
		if( !file.is_open() )
		{
			return false;
		}
		file << SaveVRSettingsJson( g_settings );
		g_fileFound = true;
		return true;
	}

	bool GameRunning()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
		if( snapshot == INVALID_HANDLE_VALUE )
		{
			return false;
		}
		PROCESSENTRY32W entry{ sizeof( entry ) };
		bool running = false;
		for( BOOL ok = Process32FirstW( snapshot, &entry ); ok && !running; ok = Process32NextW( snapshot, &entry ) )
		{
			running = _wcsicmp( entry.szExeFile, L"PSUC.exe" ) == 0;
		}
		CloseHandle( snapshot );
		return running;
	}

	void BrowseFolder( HWND owner )
	{
		BROWSEINFOW info{};
		info.hwndOwner = owner;
		info.lpszTitle = L"Select the PSU game folder (where PSUC.exe and psuex_vr.json are)";
		info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		if( PIDLIST_ABSOLUTE item = SHBrowseForFolderW( &info ) )
		{
			wchar_t path[ MAX_PATH ]{};
			if( SHGetPathFromIDListW( item, path ) )
			{
				SetFolder( path );
				LoadSettings();
			}
			CoTaskMemFree( item );
		}
	}

	void CreateRenderTarget()
	{
		ID3D11Texture2D *backBuffer = nullptr;
		g_swapChain->GetBuffer( 0, IID_PPV_ARGS( &backBuffer ) );
		if( backBuffer )
		{
			g_device->CreateRenderTargetView( backBuffer, nullptr, &g_rtv );
			backBuffer->Release();
		}
	}

	void CleanupRenderTarget()
	{
		if( g_rtv )
		{
			g_rtv->Release();
			g_rtv = nullptr;
		}
	}

	bool CreateDevice( HWND hwnd )
	{
		DXGI_SWAP_CHAIN_DESC desc{};
		desc.BufferCount = 2;
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BufferDesc.RefreshRate.Numerator = 60;
		desc.BufferDesc.RefreshRate.Denominator = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.OutputWindow = hwnd;
		desc.SampleDesc.Count = 1;
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL level{};
		HRESULT hr = D3D11CreateDeviceAndSwapChain( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
			D3D11_SDK_VERSION, &desc, &g_swapChain, &g_device, &level, &g_context );
		if( hr == DXGI_ERROR_UNSUPPORTED )
		{
			hr = D3D11CreateDeviceAndSwapChain( nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
				D3D11_SDK_VERSION, &desc, &g_swapChain, &g_device, &level, &g_context );
		}
		if( FAILED( hr ) )
		{
			return false;
		}
		CreateRenderTarget();
		return true;
	}

	void CleanupDevice()
	{
		CleanupRenderTarget();
		if( g_swapChain ) g_swapChain->Release();
		if( g_context ) g_context->Release();
		if( g_device ) g_device->Release();
	}

	LRESULT WINAPI WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
	{
		if( ImGui_ImplWin32_WndProcHandler( hWnd, msg, wParam, lParam ) )
		{
			return true;
		}
		switch( msg )
		{
			case WM_SIZE:
				if( wParam != SIZE_MINIMIZED )
				{
					g_resizeWidth = LOWORD( lParam );
					g_resizeHeight = HIWORD( lParam );
				}
				return 0;
			case WM_SYSCOMMAND:
				if( ( wParam & 0xfff0 ) == SC_KEYMENU )
				{
					return 0;
				}
				break;
			case WM_DESTROY:
				PostQuitMessage( 0 );
				return 0;
		}
		return DefWindowProcW( hWnd, msg, wParam, lParam );
	}

	void DrawApp( HWND hwnd )
	{
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos( viewport->WorkPos );
		ImGui::SetNextWindowSize( viewport->WorkSize );
		ImGui::Begin( "PSUEx VR configurator", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings );
		// Same text size setting as the in-game window (a desktop window reads fine a little smaller).
		ImGui::SetWindowFontScale( g_settings.m_panelTextScale < 1.0f ? 1.0f : ( g_settings.m_panelTextScale > 2.5f ? 2.5f : g_settings.m_panelTextScale ) );

		ImGui::TextUnformatted( "Game folder" );
		ImGui::SetNextItemWidth( -170.0f );
		if( ImGui::InputText( "##folder", g_folderUtf8, sizeof( g_folderUtf8 ), ImGuiInputTextFlags_EnterReturnsTrue ) )
		{
			g_folder = Widen( g_folderUtf8 );
			LoadSettings();
		}
		ImGui::SameLine();
		if( ImGui::Button( "Browse..." ) )
		{
			BrowseFolder( hwnd );
		}
		ImGui::SameLine();
		if( ImGui::Button( "Reload" ) )
		{
			g_folder = Widen( g_folderUtf8 );
			g_panel.message = LoadSettings() ? "Reloaded." : "psuex_vr.json not found; Save creates it.";
		}
		if( !g_fileFound )
		{
			ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ),
				"No psuex_vr.json in this folder. Showing defaults; Save creates the file." );
		}
		if( GameRunning() )
		{
			ImGui::TextColored( ImVec4( 1.0f, 0.5f, 0.4f, 1.0f ),
				"PSU is running: it rewrites psuex_vr.json when it saves. Use the in-game VR Settings, or close the game first." );
		}
		ImGui::Separator();
		PSUVR::DrawVRSettingsPanel( g_settings, g_panel );
		ImGui::End();
	}
}

// Packaging: PSUExVRConfig.exe --write-defaults <path> writes the release starter
// psuex_vr.json (VR on, first person) and exits without a window.
static bool WriteReleaseDefaults( const wchar_t *path )
{
	VRSettings starter;
	starter.m_enabled = true;
	starter.m_cameraMode = "firstPerson";
	// New users get the in-headset setup wizard.
	starter.m_setupDone = false;
	std::ofstream file( path, std::ios::trunc );
	if( !file.is_open() )
	{
		return false;
	}
	file << SaveVRSettingsJson( starter );
	return true;
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int )
{
	int argc = 0;
	if( LPWSTR *argv = CommandLineToArgvW( GetCommandLineW(), &argc ) )
	{
		const bool writeDefaults = argc >= 3 && _wcsicmp( argv[ 1 ], L"--write-defaults" ) == 0;
		const int result = writeDefaults ? ( WriteReleaseDefaults( argv[ 2 ] ) ? 0 : 1 ) : -1;
		LocalFree( argv );
		if( result >= 0 )
		{
			return result;
		}
	}

	CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
	SetFolder( ExeFolder() );
	LoadSettings();
	g_panel.inGame = false;
	g_panel.save = []()
	{
		g_folder = Widen( g_folderUtf8 );
		return SaveSettings();
	};

	HICON icon = LoadIconW( instance, MAKEINTRESOURCEW( 1 ) );
	WNDCLASSEXW wc{ sizeof( wc ), CS_CLASSDC, WndProc, 0, 0, instance, icon, nullptr, nullptr, nullptr,
		L"PSUExVRConfig", icon };
	RegisterClassExW( &wc );
	HWND hwnd = CreateWindowW( wc.lpszClassName, L"PSUEx VR Settings", WS_OVERLAPPEDWINDOW,
		100, 100, 720, 900, nullptr, nullptr, wc.hInstance, nullptr );
	if( !CreateDevice( hwnd ) )
	{
		CleanupDevice();
		UnregisterClassW( wc.lpszClassName, wc.hInstance );
		MessageBoxW( nullptr, L"Direct3D 11 is not available.", L"PSUEx VR Settings", MB_ICONERROR );
		return 1;
	}
	ShowWindow( hwnd, SW_SHOWDEFAULT );
	UpdateWindow( hwnd );

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init( hwnd );
	ImGui_ImplDX11_Init( g_device, g_context );

	bool done = false;
	while( !done )
	{
		MSG msg;
		while( PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) )
		{
			TranslateMessage( &msg );
			DispatchMessageW( &msg );
			done = done || msg.message == WM_QUIT;
		}
		if( done )
		{
			break;
		}
		if( g_resizeWidth != 0 && g_resizeHeight != 0 )
		{
			CleanupRenderTarget();
			g_swapChain->ResizeBuffers( 0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0 );
			g_resizeWidth = g_resizeHeight = 0;
			CreateRenderTarget();
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		DrawApp( hwnd );
		ImGui::Render();
		const float clear[ 4 ] = { 0.08f, 0.08f, 0.1f, 1.0f };
		g_context->OMSetRenderTargets( 1, &g_rtv, nullptr );
		g_context->ClearRenderTargetView( g_rtv, clear );
		ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );
		g_swapChain->Present( 1, 0 );
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDevice();
	DestroyWindow( hwnd );
	UnregisterClassW( wc.lpszClassName, wc.hInstance );
	CoUninitialize();
	return 0;
}
