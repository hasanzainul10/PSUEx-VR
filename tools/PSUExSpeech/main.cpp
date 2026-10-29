// PSUExSpeech: the speech-to-text helper for PSUEx VR. A 64-bit process, so the recognizer's
// memory stays out of the 32-bit game. The game DLL starts it with redirected stdin / stdout and
// talks in UTF-8 lines:
//
//   game -> helper   BEGIN | END | CANCEL | MODE push|auto | KEYTERMS <a,b,c> | QUIT
//   helper -> game   STATUS <text> | READY <engine> <size> | ERROR <text> | PARTIAL <text> | FINAL <text>
//                    | AUTO <text>
//
// The microphone stays open while the helper runs; audio only reaches the recognizer between
// BEGIN and END (plus a short pre-roll so the first word isn't cut). With MODE auto the helper
// also cuts the microphone into utterances by itself (energy gate) and sends each as AUTO.
// stdin closing (the game exited or crashed) ends the helper.
//
//   PSUExSpeech.exe --model <models dir> --size tiny|small|medium
//   PSUExSpeech.exe --download tiny|small|medium --models <models dir>   (console, progress)

#include "SpeechEngine.h"

#include <Windows.h>
#include <mmsystem.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	constexpr int kSampleRate = 16000;
	constexpr int kBufferMs = 50;
	constexpr int kBufferCount = 16;
	constexpr int kPreRollMs = 300;
	constexpr int kPartialMs = 400;

	std::mutex s_outMutex;

	void Send( const char *kind, const std::string &text = {} )
	{
		std::lock_guard< std::mutex > lock( s_outMutex );
		std::string line = kind;
		if( !text.empty() )
		{
			line += ' ';
			for( const char c : text )
			{
				line += ( c == '\n' || c == '\r' ) ? ' ' : c;
			}
		}
		line += '\n';
		fwrite( line.data(), 1, line.size(), stdout );
		fflush( stdout );
	}

	// ---- Model files -------------------------------------------------------------------

	const char *const kModelFiles[] = { "adapter.ort", "cross_kv.ort", "decoder_kv.ort", "encoder.ort", "frontend.model.ort",
		"frontend.weights.ort", "streaming_config.json", "tokenizer.bin" };
	// Re-quantized releases go into a new dated directory on the CDN (see Moonshine's model catalog).
	const wchar_t *const kModelHost = L"download.moonshine.ai";
	const char *const kModelPath = "/model/%s-streaming-en/quantized_26_08_21/%s";

	std::filesystem::path ModelDir( const std::filesystem::path &models, const std::string &size )
	{
		return models / ( size + "-streaming-en" );
	}

	bool ModelInstalled( const std::filesystem::path &dir )
	{
		for( const char *file : kModelFiles )
		{
			std::error_code ec;
			if( !std::filesystem::exists( dir / file, ec ) )
			{
				return false;
			}
		}
		return true;
	}

	bool DownloadFile( HINTERNET session, const std::string &urlPath, const std::filesystem::path &target, const char *label )
	{
		bool ok = false;
		HINTERNET connect = WinHttpConnect( session, kModelHost, INTERNET_DEFAULT_HTTPS_PORT, 0 );
		const std::wstring wpath( urlPath.begin(), urlPath.end() );
		HINTERNET request = connect ? WinHttpOpenRequest( connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE ) : nullptr;
		if( request && WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) &&
			WinHttpReceiveResponse( request, nullptr ) )
		{
			DWORD status = 0, statusSize = sizeof( status );
			WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
				&status, &statusSize, WINHTTP_NO_HEADER_INDEX );
			wchar_t lengthText[ 32 ]{};
			DWORD lengthSize = sizeof( lengthText );
			WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize,
				WINHTTP_NO_HEADER_INDEX );
			const unsigned long long total = _wcstoui64( lengthText, nullptr, 10 );
			std::filesystem::path part = target;
			part += ".part";
			FILE *file = status == 200 ? _wfopen( part.c_str(), L"wb" ) : nullptr;
			if( file )
			{
				std::vector< char > buffer( 1 << 16 );
				unsigned long long done = 0;
				DWORD read = 0;
				ok = true;
				int lastPercent = -1;
				while( WinHttpReadData( request, buffer.data(), static_cast< DWORD >( buffer.size() ), &read ) && read > 0 )
				{
					if( fwrite( buffer.data(), 1, read, file ) != read )
					{
						ok = false;
						break;
					}
					done += read;
					const int percent = total ? static_cast< int >( done * 100 / total ) : 0;
					if( percent != lastPercent )
					{
						lastPercent = percent;
						printf( "\r  %-24s %3d%%  (%llu MB)", label, percent, done >> 20 );
						fflush( stdout );
					}
				}
				fclose( file );
				printf( "\n" );
				std::error_code ec;
				if( ok && ( !total || done == total ) )
				{
					std::filesystem::rename( part, target, ec );
					ok = !ec;
				}
				else
				{
					ok = false;
					std::filesystem::remove( part, ec );
				}
			}
			else
			{
				printf( "  %s: HTTP %lu\n", label, status );
			}
		}
		if( request ) WinHttpCloseHandle( request );
		if( connect ) WinHttpCloseHandle( connect );
		return ok;
	}

	int DownloadModel( const std::filesystem::path &models, const std::string &size )
	{
		const std::filesystem::path dir = ModelDir( models, size );
		printf( "PSUEx speech: downloading the Moonshine %s English model to\n  %ls\n\n", size.c_str(), dir.c_str() );
		std::error_code ec;
		std::filesystem::create_directories( dir, ec );
		HINTERNET session = WinHttpOpen( L"PSUExSpeech/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0 );
		bool ok = session != nullptr;
		for( const char *file : kModelFiles )
		{
			if( !ok )
			{
				break;
			}
			if( std::filesystem::exists( dir / file, ec ) )
			{
				printf( "  %-24s already there\n", file );
				continue;
			}
			char path[ 256 ];
			sprintf_s( path, kModelPath, size.c_str(), file );
			ok = DownloadFile( session, path, dir / file, file );
		}
		if( session ) WinHttpCloseHandle( session );
		printf( ok ? "\nDone. Pick \"%s\" in the VR settings (Voice) and restart speech to text.\n"
			: "\nThe download failed (%s). Check the internet connection and try again.\n", size.c_str() );
		printf( "Press Enter to close.\n" );
		getchar();
		return ok ? 0 : 1;
	}

	// ---- Microphone ----------------------------------------------------------------------

	std::atomic< bool > s_listening{ false };
	std::atomic< bool > s_quit{ false };
	std::mutex s_preRollMutex;
	std::deque< float > s_preRoll;

	// Always-on mode (MODE auto): an energy gate cuts the microphone into utterances by itself.
	// An utterance starts after kAutoStartBuffers loud buffers in a row (the pre-roll keeps its
	// first syllable) and ends after kAutoEndSilenceMs of quiet; the loudness gate follows the
	// room's noise floor. Held-button talking (s_listening) always wins.
	constexpr float kAutoMinLevel = 0.015f;       // RMS floor of the gate
	constexpr float kAutoNoiseFactor = 3.0f;      // gate = noise floor x this
	constexpr int kAutoStartBuffers = 2;          // 2 x 50 ms of speech to start
	constexpr int kAutoEndSilenceMs = 600;
	constexpr int kAutoMaxMs = 10000;
	constexpr int kAutoMinSpeechMs = 200;
	std::atomic< bool > s_autoMode{ false };
	std::atomic< bool > s_autoActive{ false };    // an auto utterance is open on the engine
	std::mutex s_sessionMutex;                    // engine session ownership: capture thread vs BEGIN / END

	void CaptureThread( ISpeechEngine *engine )
	{
		WAVEFORMATEX format{};
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = 1;
		format.nSamplesPerSec = kSampleRate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = 2;
		format.nAvgBytesPerSec = kSampleRate * 2;
		HANDLE ready = CreateEventW( nullptr, FALSE, FALSE, nullptr );
		HWAVEIN device = nullptr;
		const MMRESULT opened = waveInOpen( &device, WAVE_MAPPER, &format, reinterpret_cast< DWORD_PTR >( ready ), 0,
			CALLBACK_EVENT );
		if( opened != MMSYSERR_NOERROR )
		{
			Send( "ERROR", "no microphone (waveInOpen " + std::to_string( opened ) + ")" );
			return;
		}
		constexpr int samplesPerBuffer = kSampleRate * kBufferMs / 1000;
		std::vector< std::vector< short > > data( kBufferCount, std::vector< short >( samplesPerBuffer ) );
		std::vector< WAVEHDR > headers( kBufferCount );
		for( int i = 0; i < kBufferCount; ++i )
		{
			headers[ i ].lpData = reinterpret_cast< LPSTR >( data[ i ].data() );
			headers[ i ].dwBufferLength = samplesPerBuffer * 2;
			waveInPrepareHeader( device, &headers[ i ], sizeof( WAVEHDR ) );
			waveInAddBuffer( device, &headers[ i ], sizeof( WAVEHDR ) );
		}
		waveInStart( device );
		std::vector< float > samples;
		bool wasListening = false;
		constexpr size_t preRollSamples = kSampleRate * kPreRollMs / 1000;
		float noise = 0.004f;
		int loudRun = 0, silentMs = 0, speechMs = 0, totalMs = 0;
		const auto pushPreRoll = [ & ]()
		{
			std::lock_guard< std::mutex > lock( s_preRollMutex );
			s_preRoll.insert( s_preRoll.end(), samples.begin(), samples.end() );
			while( s_preRoll.size() > preRollSamples )
			{
				s_preRoll.pop_front();
			}
		};
		const auto takePreRoll = [ & ]()
		{
			std::vector< float > pre;
			std::lock_guard< std::mutex > lock( s_preRollMutex );
			pre.assign( s_preRoll.begin(), s_preRoll.end() );
			s_preRoll.clear();
			return pre;
		};
		while( !s_quit )
		{
			WaitForSingleObject( ready, 200 );
			for( WAVEHDR &header : headers )
			{
				if( !( header.dwFlags & WHDR_DONE ) )
				{
					continue;
				}
				const size_t count = header.dwBytesRecorded / 2;
				samples.resize( count );
				const short *pcm = reinterpret_cast< const short * >( header.lpData );
				float energy = 0.0f;
				for( size_t i = 0; i < count; ++i )
				{
					samples[ i ] = pcm[ i ] / 32768.0f;
					energy += samples[ i ] * samples[ i ];
				}
				{
				std::lock_guard< std::mutex > session( s_sessionMutex );
				const bool listening = s_listening;
				if( listening && !wasListening )
				{
					// Audio from just before the press.
					const std::vector< float > pre = takePreRoll();
					if( !pre.empty() )
					{
						engine->AddAudio( pre.data(), pre.size(), kSampleRate );
					}
				}
				wasListening = listening;
				if( listening )
				{
					// BEGIN took the engine over: an open auto utterance is gone with its stream.
					s_autoActive = false;
					loudRun = 0;
					engine->AddAudio( samples.data(), count, kSampleRate );
				}
				else if( s_autoMode && count > 0 )
				{
					const float rms = std::sqrt( energy / static_cast< float >( count ) );
					const bool loud = rms > ( std::max )( kAutoMinLevel, noise * kAutoNoiseFactor );
					const int ms = static_cast< int >( count * 1000 / kSampleRate );
					if( !s_autoActive )
					{
						if( loud )
						{
							++loudRun;
						}
						else
						{
							loudRun = 0;
							noise += ( rms - noise ) * 0.03f;
						}
						pushPreRoll();
						if( loudRun >= kAutoStartBuffers )
						{
							const std::vector< float > pre = takePreRoll();
							engine->Begin();
							engine->AddAudio( pre.data(), pre.size(), kSampleRate );
							s_autoActive = true;
							silentMs = 0;
							speechMs = loudRun * ms;
							totalMs = static_cast< int >( pre.size() * 1000 / kSampleRate );
						}
					}
					else
					{
						engine->AddAudio( samples.data(), count, kSampleRate );
						totalMs += ms;
						if( loud )
						{
							silentMs = 0;
							speechMs += ms;
						}
						else
						{
							silentMs += ms;
						}
						if( silentMs >= kAutoEndSilenceMs || totalMs >= kAutoMaxMs )
						{
							if( totalMs >= kAutoMaxMs )
							{
								noise = ( std::max )( noise, rms );  // steady noise, not speech: raise the gate
							}
							s_autoActive = false;
							loudRun = 0;
							const std::string text = engine->End();
							if( speechMs >= kAutoMinSpeechMs && !text.empty() )
							{
								Send( "AUTO", text );
							}
						}
					}
				}
				else
				{
					if( s_autoActive )
					{
						// Always-on was switched off mid-utterance.
						s_autoActive = false;
						engine->Cancel();
					}
					loudRun = 0;
					pushPreRoll();
				}
				}
				header.dwFlags &= ~WHDR_DONE;
				waveInAddBuffer( device, &header, sizeof( WAVEHDR ) );
			}
		}
		waveInStop( device );
		waveInReset( device );
		for( WAVEHDR &header : headers )
		{
			waveInUnprepareHeader( device, &header, sizeof( WAVEHDR ) );
		}
		waveInClose( device );
		CloseHandle( ready );
	}

	// Partial transcripts while the button is held.
	void PartialThread( ISpeechEngine *engine )
	{
		while( !s_quit )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( kPartialMs ) );
			std::string text;
			if( s_listening && engine->Partial( text ) && !text.empty() )
			{
				Send( "PARTIAL", text );
			}
		}
	}

	std::string Arg( int argc, wchar_t **argv, const wchar_t *name )
	{
		for( int i = 1; i + 1 < argc; ++i )
		{
			if( wcscmp( argv[ i ], name ) == 0 )
			{
				char out[ MAX_PATH * 3 ]{};
				WideCharToMultiByte( CP_UTF8, 0, argv[ i + 1 ], -1, out, sizeof( out ), nullptr, nullptr );
				return out;
			}
		}
		return {};
	}

	std::filesystem::path ArgPath( int argc, wchar_t **argv, const wchar_t *name )
	{
		for( int i = 1; i + 1 < argc; ++i )
		{
			if( wcscmp( argv[ i ], name ) == 0 )
			{
				return argv[ i + 1 ];
			}
		}
		return {};
	}

	int TranscribeWav( ISpeechEngine &engine, const std::filesystem::path &path )
	{
		FILE *file = _wfopen( path.c_str(), L"rb" );
		if( !file )
		{
			Send( "ERROR", "can't open the wav" );
			return 1;
		}
		std::vector< char > bytes;
		char chunk[ 4096 ];
		size_t read = 0;
		while( ( read = fread( chunk, 1, sizeof( chunk ), file ) ) > 0 )
		{
			bytes.insert( bytes.end(), chunk, chunk + read );
		}
		fclose( file );
		// Find the "data" chunk (format assumed 16-bit mono).
		size_t pos = 12;
		uint32_t rate = kSampleRate;
		while( pos + 8 <= bytes.size() )
		{
			const uint32_t length = *reinterpret_cast< const uint32_t * >( &bytes[ pos + 4 ] );
			if( memcmp( &bytes[ pos ], "fmt ", 4 ) == 0 && pos + 16 <= bytes.size() )
			{
				rate = *reinterpret_cast< const uint32_t * >( &bytes[ pos + 12 ] );
			}
			if( memcmp( &bytes[ pos ], "data", 4 ) == 0 )
			{
				const size_t count = std::min< size_t >( length, bytes.size() - pos - 8 ) / 2;
				const short *pcm = reinterpret_cast< const short * >( &bytes[ pos + 8 ] );
				std::vector< float > samples( count );
				for( size_t i = 0; i < count; ++i )
				{
					samples[ i ] = pcm[ i ] / 32768.0f;
				}
				const auto start = std::chrono::steady_clock::now();
				engine.Begin();
				engine.AddAudio( samples.data(), samples.size(), static_cast< int >( rate ) );
				const std::string text = engine.End();
				const auto ms = std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now() - start ).count();
				Send( "FINAL", text );
				Send( "STATUS", std::to_string( count * 1000 / rate ) + " ms of audio in " + std::to_string( ms ) + " ms" );
				return 0;
			}
			pos += 8 + length + ( length & 1 );
		}
		Send( "ERROR", "no data chunk" );
		return 1;
	}

	bool ValidSize( const std::string &size )
	{
		return size == "tiny" || size == "small" || size == "medium";
	}
}

int wmain( int argc, wchar_t **argv )
{
	std::filesystem::path models = ArgPath( argc, argv, L"--models" );
	if( models.empty() )
	{
		wchar_t self[ MAX_PATH ]{};
		GetModuleFileNameW( nullptr, self, MAX_PATH );
		models = std::filesystem::path( self ).parent_path() / "models";
	}
	const std::string download = Arg( argc, argv, L"--download" );
	if( !download.empty() )
	{
		if( !ValidSize( download ) )
		{
			printf( "Unknown model size \"%s\" (tiny, small or medium).\n", download.c_str() );
			return 2;
		}
		return DownloadModel( models, download );
	}

	std::string size = Arg( argc, argv, L"--size" );
	if( !ValidSize( size ) )
	{
		size = "small";
	}
	const std::filesystem::path dir = ModelDir( models, size );
	if( !ModelInstalled( dir ) )
	{
		Send( "ERROR", "the " + size + " model isn't installed (Voice settings: Download)" );
		return 1;
	}

	std::unique_ptr< ISpeechEngine > engine = CreateMoonshineEngine();
	Send( "STATUS", "loading the " + size + " model" );
	std::string error;
	if( !engine->Load( dir.string(), size, error ) )
	{
		Send( "ERROR", "model load failed: " + error );
		return 1;
	}
	const std::string keyterms = Arg( argc, argv, L"--keyterms" );
	if( !keyterms.empty() )
	{
		engine->SetKeyterms( keyterms );
	}
	// Test mode: one 16-bit mono 16 kHz WAV through the same path as a held button.
	const std::filesystem::path wav = ArgPath( argc, argv, L"--wav" );
	if( !wav.empty() )
	{
		return TranscribeWav( *engine, wav );
	}
	std::thread capture( CaptureThread, engine.get() );
	std::thread partial( PartialThread, engine.get() );
	Send( "READY", std::string( engine->Name() ) + " " + size );

	char lineBuffer[ 8192 ];
	while( fgets( lineBuffer, sizeof( lineBuffer ), stdin ) )
	{
		std::string line = lineBuffer;
		while( !line.empty() && ( line.back() == '\n' || line.back() == '\r' ) )
		{
			line.pop_back();
		}
		if( line == "BEGIN" )
		{
			std::lock_guard< std::mutex > session( s_sessionMutex );
			engine->Begin();
			s_listening = true;
		}
		else if( line == "END" )
		{
			s_listening = false;
			// Let the capture thread hand over the last buffer.
			std::this_thread::sleep_for( std::chrono::milliseconds( kBufferMs * 2 ) );
			std::lock_guard< std::mutex > session( s_sessionMutex );
			Send( "FINAL", engine->End() );
		}
		else if( line == "CANCEL" )
		{
			std::lock_guard< std::mutex > session( s_sessionMutex );
			s_listening = false;
			engine->Cancel();
		}
		else if( line.rfind( "MODE", 0 ) == 0 )
		{
			s_autoMode = line.size() > 5 && line.substr( 5 ) == "auto";
		}
		else if( line.rfind( "KEYTERMS", 0 ) == 0 )
		{
			engine->SetKeyterms( line.size() > 9 ? line.substr( 9 ) : std::string() );
		}
		else if( line == "QUIT" )
		{
			break;
		}
	}
	s_quit = true;
	s_listening = false;
	capture.join();
	partial.join();
	return 0;
}
