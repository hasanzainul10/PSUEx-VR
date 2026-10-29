#pragma once

#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>

#include <Windows.h>

namespace PSULog
{
	inline FILE *g_file = nullptr;
	inline std::mutex g_mutex;
	// Last part of everything logged, kept in memory so the dinput8 loader can copy it
	// into its own log (PSUEx_LogTail) when psuex.log can't be written.
	inline char g_tail[ 16 * 1024 ]{};
	inline size_t g_tailLength = 0;
	inline char g_openError[ 300 ]{};
	inline bool g_fileOpened = false;  // psuex.log was opened at attach (stays true after detach)
	// Flush about once a second instead of after every line (VR setting perfLogBatching, synced each frame).
	inline bool g_batchFlush = false;
	inline ULONGLONG g_lastFlush = 0;
	// At attach, a psuex.log larger than this is renamed psuex.log.1 (replacing the old one).
	constexpr long long kRotateBytes = 20ll * 1024 * 1024;

	inline void AppendTail( const char *text )
	{
		const size_t length = strlen( text );
		if( length >= sizeof( g_tail ) )
		{
			return;
		}
		if( g_tailLength + length >= sizeof( g_tail ) )
		{
			// Drop the oldest half.
			const size_t keep = g_tailLength / 2;
			memmove( g_tail, g_tail + g_tailLength - keep, keep );
			g_tailLength = keep;
		}
		memcpy( g_tail + g_tailLength, text, length );
		g_tailLength += length;
		g_tail[ g_tailLength ] = '\0';
	}

	inline void Init()
	{
		std::lock_guard lock( g_mutex );
		if( g_file )
		{
			return;
		}

		char path[ MAX_PATH ]{};
		GetModuleFileNameA( nullptr, path, MAX_PATH );
		if( char *slash = strrchr( path, '\\' ) )
		{
			slash[ 1 ] = '\0';
		}
		strcat_s( path, "psuex.log" );
		WIN32_FILE_ATTRIBUTE_DATA info{};
		if( GetFileAttributesExA( path, GetFileExInfoStandard, &info ) &&
			( ( static_cast< long long >( info.nFileSizeHigh ) << 32 ) | info.nFileSizeLow ) > kRotateBytes )
		{
			char oldPath[ MAX_PATH ]{};
			sprintf_s( oldPath, "%s.1", path );
			MoveFileExA( path, oldPath, MOVEFILE_REPLACE_EXISTING );
		}
		// Share read/write so tools can copy the log while the game runs.
		g_file = _fsopen( path, "a", _SH_DENYNO );
		if( g_file )
		{
			g_fileOpened = true;
			fprintf( g_file, "\n==== PSUEx attach ====\n" );
			fflush( g_file );
		}
		else
		{
			sprintf_s( g_openError, "psuex.log could not be opened (errno %d, Win32 %lu): %s\n",
				errno, GetLastError(), path );
			AppendTail( g_openError );
		}
	}

	inline void Shutdown()
	{
		std::lock_guard lock( g_mutex );
		if( g_file )
		{
			fprintf( g_file, "==== PSUEx detach ====\n" );
			fclose( g_file );
			g_file = nullptr;
		}
	}

	inline void Write( const char *fmt, ... )
	{
		char buffer[ 1024 ];
		va_list args;
		va_start( args, fmt );
		vsnprintf( buffer, sizeof( buffer ), fmt, args );
		va_end( args );

		printf( "%s", buffer );

		std::lock_guard lock( g_mutex );
		AppendTail( buffer );
		if( g_file )
		{
			fputs( buffer, g_file );
			const ULONGLONG now = GetTickCount64();
			if( !g_batchFlush || now - g_lastFlush >= 1000 )
			{
				fflush( g_file );
				g_lastFlush = now;
			}
		}
	}

	// Crash handler: write out what is buffered. Skips if the crash hit while the log was locked.
	inline void Flush()
	{
		if( g_mutex.try_lock() )
		{
			if( g_file )
			{
				fflush( g_file );
			}
			g_mutex.unlock();
		}
	}
}

#define PSU_LOG( ... ) ::PSULog::Write( __VA_ARGS__ )
