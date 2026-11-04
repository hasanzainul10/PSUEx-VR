#include "PSUEx.h"
#include "ExceptionHandler.h"
#include "Misc/Log.h"
#include "PSUVR/VRSystem.h"
#include "PSUVR/VRSyntheticPad.h"

// online.exe injects its own PSUEx.dll into PSUC (and its updater rewrites that file on every
// launch); two copies would hook everything twice. This build ships as PSUExVR.dll (release)
// or PSUExD.dll (debug) so the updater leaves it alone.
static HMODULE s_selfModule = nullptr;

// Read by the dinput8 loader: everything PSUEx logged recently, so it reaches
// dinput8_proxy.log even when psuex.log can't be written.
extern "C" __declspec( dllexport ) const char *PSUEx_LogTail()
{
	return PSULog::g_tail;
}

// Read by the dinput8 loader: whether psuex.log is being written (if so, no copy needed).
extern "C" __declspec( dllexport ) int PSUEx_LogFileOpen()
{
	return PSULog::g_fileOpened ? 1 : 0;
}

// Called by the dinput8 loader after the game reads its gamepad (DIJOYSTATE2): merges the
// motion controllers' virtual pad (vr.inputMode "motion"). 1 = merged.
extern "C" __declspec( dllexport ) int PSUEx_MergeJoystickState( void *state, unsigned size,
	const long *axisMin, const long *axisMax, int replace )
{
	return PSUVR::VRSyntheticPad::MergeJoystickState( state, size, axisMin, axisMax, replace != 0 ) ? 1 : 0;
}
static bool s_hooked = false;
static decltype( &LoadLibraryA ) Original_LoadLibraryA = LoadLibraryA;
static decltype( &LoadLibraryW ) Original_LoadLibraryW = LoadLibraryW;
static decltype( &LoadLibraryExA ) Original_LoadLibraryExA = LoadLibraryExA;
static decltype( &LoadLibraryExW ) Original_LoadLibraryExW = LoadLibraryExW;

static bool
IsPSUExName( const char *base )
{
	return _stricmp( base, "PSUEx.dll" ) == 0 || _stricmp( base, "PSUExD.dll" ) == 0 ||
		_stricmp( base, "PSUExVR.dll" ) == 0;
}

static bool
IsOtherPSUExA( LPCSTR path )
{
	if( !path )
	{
		return false;
	}
	const char *slash = strrchr( path, '\\' );
	const char *fwd = strrchr( path, '/' );
	const char *base = ( fwd > slash ) ? fwd + 1 : ( slash ? slash + 1 : path );
	return IsPSUExName( base );
}

static bool
IsOtherPSUExW( LPCWSTR path )
{
	if( !path )
	{
		return false;
	}
	char narrow[ MAX_PATH ]{};
	WideCharToMultiByte( CP_ACP, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr );
	return IsOtherPSUExA( narrow );
}

static HMODULE WINAPI
Hook_LoadLibraryA( LPCSTR path )
{
	if( IsOtherPSUExA( path ) )
	{
		PSU_LOG( "Blocked second PSUEx load: %s\n", path );
		return s_selfModule;
	}
	return Original_LoadLibraryA( path );
}

static HMODULE WINAPI
Hook_LoadLibraryW( LPCWSTR path )
{
	if( IsOtherPSUExW( path ) )
	{
		PSU_LOG( "Blocked second PSUEx load: %ls\n", path );
		return s_selfModule;
	}
	return Original_LoadLibraryW( path );
}

static HMODULE WINAPI
Hook_LoadLibraryExA( LPCSTR path, HANDLE file, DWORD flags )
{
	if( IsOtherPSUExA( path ) )
	{
		PSU_LOG( "Blocked second PSUEx load: %s\n", path );
		return s_selfModule;
	}
	return Original_LoadLibraryExA( path, file, flags );
}

static HMODULE WINAPI
Hook_LoadLibraryExW( LPCWSTR path, HANDLE file, DWORD flags )
{
	if( IsOtherPSUExW( path ) )
	{
		PSU_LOG( "Blocked second PSUEx load: %ls\n", path );
		return s_selfModule;
	}
	return Original_LoadLibraryExW( path, file, flags );
}

static HMODULE
FindOtherPSUEx()
{
	for( const char *name : { "PSUEx.dll", "PSUExD.dll", "PSUExVR.dll" } )
	{
		HMODULE module = GetModuleHandleA( name );
		if( module && module != s_selfModule )
		{
			return module;
		}
	}
	return nullptr;
}

static void
DetoursHookAll()
{
	DetourTransactionBegin();
	{
		DetourUpdateThread( GetCurrentThread() );
		DetourAttach( &( PVOID & )Original_WinMain, Hook_WinMain );
		DetourAttach( &( PVOID & )Original_CreateWindowExA, Hook_CreateWindowExA );
		DetourAttach( &( PVOID & )Original_LoadLibraryA, Hook_LoadLibraryA );
		DetourAttach( &( PVOID & )Original_LoadLibraryW, Hook_LoadLibraryW );
		DetourAttach( &( PVOID & )Original_LoadLibraryExA, Hook_LoadLibraryExA );
		DetourAttach( &( PVOID & )Original_LoadLibraryExW, Hook_LoadLibraryExW );

		sub_774F90::Hook();			// Initialize DirectInput	
		sub_776590::Hook();			// Initialize Direct3D

		sub_50B790::Hook();			// Chat Logger
		sub_74B490::Hook();			// memset Crash
		sub_74B760::Hook();			// Bone pose build (VR hands)
		sub_5089C0::Hook();			// Near-camera hide (VR hands in free aim)
		sub_562070::Hook();			// Model part cull (VR hands in free aim)
		sub_74DA40::Hook();			// Frustum sphere test (VR: eye frustum in the eye passes)
		sub_74D5B0::Hook();			// Frustum box test (VR: eye frustum in the eye passes)
#ifdef _DEBUG
		sub_4F9F50::Hook();			// Aim shot angles (free aim reticle calibration log)
		sub_4D50B0::Hook();			// Technique aim (log each technique once)
		sub_4FFFA0::Hook();			// Bone query (who reads the player's bones; FBT plan phase 0)
#endif
		sub_7738E0::Hook();			// Keyboard held keys (VR speech to text: chat key / Enter)
		sub_4CB3A0::Hook();			// Bullet direction (free aim shot start for the VR reticle; Debug log)
		sub_814040::Hook();			// File Processing

		HardwareInfo::Hook();		// CPU Affinity Fix
		TItemBulletSlicer::Hook();	// Slicer Crash
		TFlagManager::Hook();		// Flag Manager
		TCameraTaskPlay::Hook();	// Camera Position
		FrameRender::Hook();		// Per-frame scene↔UI orchestrator
		UIScaling::Hook();			// UI Scaling
		TextDraw::Hook();			// Translation

		//sub_76E330::Hook();	// ADX Audio Player
		//sub_777160::Hook();	// Unknown Function
		//sub_770AF0::Hook();	// Something with File Processing
		//sub_783120::Hook();
	}
	DetourTransactionCommit();
	PSU_LOG( "Detours attached\n" );
}

static void
DetoursUnhookAll()
{
	DetourTransactionBegin();
	{
		DetourUpdateThread( GetCurrentThread() );
		DetourDetach( &( PVOID & )Original_CreateWindowExA, Hook_CreateWindowExA );
		DetourDetach( &( PVOID & )Original_WinMain, Hook_WinMain );
	}
	DetourTransactionCommit();
}

BOOL
APIENTRY DllMain( HMODULE hModule,
					   DWORD  ul_reason_for_call,
					   LPVOID lpReserved
)
{
	switch( ul_reason_for_call )
	{
		case DLL_PROCESS_ATTACH:
		{
			DisableThreadLibraryCalls( hModule );
			SetupCrashHandler();
			PSULog::Init();

		#ifdef _DEBUG
			AllocConsole();
			freopen_s( ( FILE ** )stdout, "CONOUT$", "w", stdout );
		#endif

			PSU_LOG( "PSUEx DllMain PROCESS_ATTACH\n" );

			char hostExe[ MAX_PATH ]{};
			GetModuleFileNameA( nullptr, hostExe, MAX_PATH );
			const char *hostBase = strrchr( hostExe, '\\' );
			hostBase = hostBase ? hostBase + 1 : hostExe;
			const bool isPSUC = ( _stricmp( hostBase, "PSUC.exe" ) == 0 )
				|| ( _stricmp( hostBase, "PSUC_Uncompressed.exe" ) == 0 );
			if( !isPSUC )
			{
				PSU_LOG( "Skip Detours/VR — host is not PSUC (%s)\n", hostExe );
				break;
			}

			s_selfModule = hModule;
			if( HMODULE other = FindOtherPSUEx() )
			{
				char otherPath[ MAX_PATH ]{};
				GetModuleFileNameA( other, otherPath, MAX_PATH );
				PSU_LOG( "Skip Detours/VR — another PSUEx is already loaded (%s)\n", otherPath );
				break;
			}

			DetoursHookAll();
			s_hooked = true;
			PSUVR::System::Get().OnProcessAttach();
		}
		break;

		case DLL_PROCESS_DETACH:
		{
			if( s_hooked )
			{
				PSUVR::System::Get().OnProcessDetach();
				Config().Get().SaveToFile();
				DetoursUnhookAll();
			}

		#ifdef _DEBUG
			fclose( stdout );
			FreeConsole();
		#endif

			PSULog::Shutdown();
		}
		break;
	}
	return TRUE;
}
