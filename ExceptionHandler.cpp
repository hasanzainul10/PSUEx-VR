
#include "ExceptionHandler.h"
#include "Misc/Log.h"

#include <cstring>

#pragma comment(lib, "Dbghelp.lib")

static void LogAccessViolation( EXCEPTION_POINTERS *pExceptionPointers )
{
	EXCEPTION_RECORD *rec = pExceptionPointers->ExceptionRecord;
	const DWORD_PTR faultAddr = reinterpret_cast< DWORD_PTR >( rec->ExceptionAddress );
	const ULONG_PTR accessType = rec->NumberParameters >= 1 ? rec->ExceptionInformation[ 0 ] : 0;
	const ULONG_PTR accessAddr = rec->NumberParameters >= 2 ? rec->ExceptionInformation[ 1 ] : 0;

	HMODULE mod = nullptr;
	char modName[ MAX_PATH ] = "(unknown)";
	DWORD_PTR offset = faultAddr;
	if( GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast< LPCSTR >( faultAddr ),
			&mod ) &&
		mod )
	{
		char path[ MAX_PATH ]{};
		if( GetModuleFileNameA( mod, path, MAX_PATH ) )
		{
			const char *base = strrchr( path, '\\' );
			strncpy_s( modName, base ? base + 1 : path, _TRUNCATE );
		}
		offset = faultAddr - reinterpret_cast< DWORD_PTR >( mod );
	}

	if( rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION )
	{
		PSU_LOG(
			"CRASH: ACCESS_VIOLATION code=0x%08X fault=%s+0x%X (0x%p) access=%s target=0x%p tid=%lu\n",
			rec->ExceptionCode,
			modName,
			static_cast< unsigned >( offset ),
			reinterpret_cast< void * >( faultAddr ),
			accessType == 0 ? "read" : ( accessType == 1 ? "write" : "exec/dep" ),
			reinterpret_cast< void * >( accessAddr ),
			GetCurrentThreadId() );
	}
	else
	{
		PSU_LOG(
			"CRASH: code=0x%08X fault=%s+0x%X (0x%p) tid=%lu\n",
			rec->ExceptionCode,
			modName,
			static_cast< unsigned >( offset ),
			reinterpret_cast< void * >( faultAddr ),
			GetCurrentThreadId() );
	}
}

// Create a timestamped filename
std::string GetCrashDumpFilename()
{
    SYSTEMTIME st;
    GetLocalTime( &st );

    std::ostringstream oss;
    oss << "Crash_"
        << std::setfill( '0' ) << std::setw( 4 ) << st.wYear
        << std::setw( 2 ) << st.wMonth
        << std::setw( 2 ) << st.wDay << "_"
        << std::setw( 2 ) << st.wHour
        << std::setw( 2 ) << st.wMinute
        << std::setw( 2 ) << st.wSecond << ".dmp";

    return oss.str();
}

// Write the crash dump file
bool WriteMinidump( EXCEPTION_POINTERS *pExceptionPointers, const std::string &dumpPath )
{
    HANDLE hFile = CreateFileA( dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
    if( hFile == INVALID_HANDLE_VALUE )
        return false;

    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = GetCurrentThreadId();
    mdei.ExceptionPointers = pExceptionPointers;
    mdei.ClientPointers = FALSE;

    BOOL success = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        MiniDumpWithDataSegs,
        &mdei,
        nullptr,
        nullptr
    );

    CloseHandle( hFile );
    return success;
}

// The crash handler function
LONG WINAPI GlobalCrashHandler( EXCEPTION_POINTERS *pExceptionPointers )
{
	LogAccessViolation( pExceptionPointers );
	PSULog::Flush();

	// Only prompt/dump on access violations; still log every unhandled exception.
    if( pExceptionPointers->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION )
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    int response = MessageBoxA( nullptr,
                                "Oops! PSUEx detected a crash in the game.\n\nWould you like to save a crash report to help us figure out what went wrong?",
                                "Game Crash Detected",
                                MB_YESNO | MB_ICONERROR | MB_SYSTEMMODAL );

    if( response == IDYES )
    {
        char exePath[ MAX_PATH ];
        GetModuleFileNameA( nullptr, exePath, MAX_PATH );
        std::filesystem::path dumpDir = std::filesystem::path( exePath ).parent_path();
        std::string dumpName = GetCrashDumpFilename();
        std::string dumpPath = ( dumpDir / dumpName ).string();

        if( WriteMinidump( pExceptionPointers, dumpPath ) )
        {
            std::string msg = "Crash report saved to:\n" + dumpPath;
            MessageBoxA( nullptr, msg.c_str(), "Crash Report Saved", MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL );

            // Open the folder where the dump was saved
            ShellExecuteA( nullptr, "open", dumpDir.string().c_str(), nullptr, nullptr, SW_SHOWDEFAULT );
        }
        else
        {
            MessageBoxA( nullptr, "Failed to save crash report.", "Error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL );
        }
    }

    //return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}

// Call this during DLL initialization
void SetupCrashHandler()
{
    SetUnhandledExceptionFilter( GlobalCrashHandler );
}