#include "VRSpeech.h"

#include "VRBindings.h"
#include "VRSpeechFiles.h"
#include "VRSyntheticPad.h"
#include "../Misc/Globals.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"
#include "../PSUStructs/PSUCombat.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PSUVR
{
	namespace
	{
		// ---- Game addresses (docs/ADDRESS_MAP.md, "Chat input") -----------------------------
		// Text input module: 0x00B9D4AB = a text box takes typing, 0x00B9D4B0 == 1 its mode.
		constexpr uintptr_t kTextInputActive = 0x00B9D4AB;
		constexpr uintptr_t kTextInputMode = 0x00B9D4B0;
		// Keyboard action table slot 7 (0x00992258 + 7): the key that opens the chat box
		// (default Space = game code 0x45, [keyboard] WND_CONVERT).
		constexpr uintptr_t kChatOpenKeySlot = 0x0099225F;
		constexpr uint8_t kChatOpenKeyDefault = 0x45;
		constexpr uint8_t kEnterKey = 0x38;      // VK_RETURN through the VK table 0x00870280
		constexpr int kKeyHoldPolls = 3;         // keyboard polls a key stays held

		constexpr DWORD kRestartDelayMs = 10000;
		constexpr DWORD kChatStepTimeoutMs = 1500;
		constexpr DWORD kTypeSettleMs = 150;
		constexpr size_t kChatMaxChars = 120;

		// ---- Toasts: VRBindings keeps the pointer, so text lives in rotating buffers. --------
		char s_toastText[ 4 ][ 256 ]{};
		int s_toastSlot = 0;

		void Toast( const char *format, ... )
		{
			char *buffer = s_toastText[ s_toastSlot ];
			s_toastSlot = ( s_toastSlot + 1 ) % 4;
			va_list args;
			va_start( args, format );
			vsnprintf( buffer, sizeof( s_toastText[ 0 ] ), format, args );
			va_end( args );
			VRBindings::ShowToast( buffer );
		}

		// ---- Helper process ------------------------------------------------------------
		HANDLE s_process = nullptr;
		HANDLE s_stdinWrite = nullptr;
		HANDLE s_stdoutRead = nullptr;
		std::thread s_reader;
		std::mutex s_lineMutex;
		std::deque< std::string > s_lines;
		std::atomic< bool > s_readerDone{ false };
		std::string s_runningModel;
		bool s_ready = false;
		bool s_talking = false;
		bool s_sentAuto = false;  // the helper's MODE as last sent
		DWORD s_talkTick = 0;
		constexpr DWORD kTapMs = 250;  // a press shorter than this is a tap, not talking
		DWORD s_nextStartTick = 0;
		bool s_missingToastShown = false;

		void ReaderThread( HANDLE pipe )
		{
			std::string pending;
			char buffer[ 1024 ];
			DWORD read = 0;
			while( ReadFile( pipe, buffer, sizeof( buffer ), &read, nullptr ) && read > 0 )
			{
				pending.append( buffer, read );
				size_t end;
				while( ( end = pending.find( '\n' ) ) != std::string::npos )
				{
					std::string line = pending.substr( 0, end );
					pending.erase( 0, end + 1 );
					if( !line.empty() && line.back() == '\r' )
					{
						line.pop_back();
					}
					std::lock_guard< std::mutex > lock( s_lineMutex );
					s_lines.push_back( std::move( line ) );
				}
			}
			s_readerDone = true;
		}

		void SendLine( const std::string &line )
		{
			if( !s_stdinWrite )
			{
				return;
			}
			const std::string text = line + "\n";
			DWORD written = 0;
			WriteFile( s_stdinWrite, text.data(), static_cast< DWORD >( text.size() ), &written, nullptr );
		}

		void StopHelper()
		{
			if( !s_process )
			{
				return;
			}
			SendLine( "QUIT" );
			CloseHandle( s_stdinWrite );
			s_stdinWrite = nullptr;
			if( WaitForSingleObject( s_process, 1500 ) == WAIT_TIMEOUT )
			{
				TerminateProcess( s_process, 0 );
			}
			// The reader ends when the helper's stdout closes.
			if( s_reader.joinable() )
			{
				s_reader.join();
			}
			CloseHandle( s_stdoutRead );
			CloseHandle( s_process );
			s_stdoutRead = nullptr;
			s_process = nullptr;
			s_ready = false;
			s_talking = false;
			s_sentAuto = false;
			s_runningModel.clear();
			std::lock_guard< std::mutex > lock( s_lineMutex );
			s_lines.clear();
		}

		bool StartHelper( const std::string &size )
		{
			if( !VRSpeechFiles::HelperInstalled() )
			{
				if( !s_missingToastShown )
				{
					Toast( "Speech to text: PSUExSpeech\\PSUExSpeech.exe is missing" );
					s_missingToastShown = true;
				}
				return false;
			}
			SECURITY_ATTRIBUTES inherit{ sizeof( inherit ), nullptr, TRUE };
			HANDLE stdinRead = nullptr, stdoutWrite = nullptr;
			if( !CreatePipe( &stdinRead, &s_stdinWrite, &inherit, 0 ) )
			{
				return false;
			}
			if( !CreatePipe( &s_stdoutRead, &stdoutWrite, &inherit, 0 ) )
			{
				CloseHandle( stdinRead );
				CloseHandle( s_stdinWrite );
				s_stdinWrite = nullptr;
				return false;
			}
			// Our ends stay private.
			SetHandleInformation( s_stdinWrite, HANDLE_FLAG_INHERIT, 0 );
			SetHandleInformation( s_stdoutRead, HANDLE_FLAG_INHERIT, 0 );

			STARTUPINFOW startup{ sizeof( startup ) };
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = stdinRead;
			startup.hStdOutput = stdoutWrite;
			startup.hStdError = stdoutWrite;
			std::wstring command = L"\"" + VRSpeechFiles::HelperExe().wstring() + L"\" --size " +
				std::wstring( size.begin(), size.end() );
			PROCESS_INFORMATION process{};
			const BOOL created = CreateProcessW( nullptr, command.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, VRSpeechFiles::SpeechDir().c_str(), &startup, &process );
			CloseHandle( stdinRead );
			CloseHandle( stdoutWrite );
			if( !created )
			{
				PSU_LOG( "VRSpeech: starting the helper failed (%lu)\n", GetLastError() );
				CloseHandle( s_stdinWrite );
				CloseHandle( s_stdoutRead );
				s_stdinWrite = s_stdoutRead = nullptr;
				return false;
			}
			CloseHandle( process.hThread );
			s_process = process.hProcess;
			s_readerDone = false;
			s_reader = std::thread( ReaderThread, s_stdoutRead );
			s_runningModel = size;
			PSU_LOG( "VRSpeech: helper started (pid %lu, model %s)\n", process.dwProcessId, size.c_str() );
			return true;
		}

		// ---- Key injection (read on the game's keyboard poll) ---------------------------------
		std::mutex s_keyMutex;
		uint8_t s_injectKey = 0;
		int s_injectPolls = 0;

		void PressKey( uint8_t key )
		{
			std::lock_guard< std::mutex > lock( s_keyMutex );
			s_injectKey = key;
			s_injectPolls = kKeyHoldPolls;
		}

		bool KeyPending()
		{
			std::lock_guard< std::mutex > lock( s_keyMutex );
			return s_injectPolls > 0;
		}

		bool TextInputOpen()
		{
			return PSUMemory::ReadAbsoluteMemory< uint8_t >( kTextInputActive ) != 0 &&
				PSUMemory::ReadAbsoluteMemory< uint32_t >( kTextInputMode ) == 1;
		}

		// ---- Chat sender ------------------------------------------------------------------
		enum class ChatStep { Idle, Opening, Typing, Settle, Sending, Closing };
		ChatStep s_chatStep = ChatStep::Idle;
		DWORD s_chatTick = 0;
		std::string s_chatText;

		void StartChat( const std::string &text )
		{
			s_chatText.clear();
			for( const char c : text )
			{
				// The game takes 0x20..0x7E from WM_CHAR (0x004C9370) on non-Japanese layouts.
				const unsigned char u = static_cast< unsigned char >( c );
				if( u >= 0x20 && u < 0x7F )
				{
					s_chatText += c;
				}
				else if( u >= 0x80 && ( s_chatText.empty() || s_chatText.back() != ' ' ) )
				{
					s_chatText += ' ';
				}
			}
			if( s_chatText.size() > kChatMaxChars )
			{
				s_chatText.resize( kChatMaxChars );
			}
			if( s_chatText.empty() )
			{
				return;
			}
			s_chatTick = GetTickCount();
			if( TextInputOpen() )
			{
				s_chatStep = ChatStep::Typing;
			}
			else
			{
				uint8_t key = PSUMemory::ReadAbsoluteMemory< uint8_t >( kChatOpenKeySlot );
				PressKey( key ? key : kChatOpenKeyDefault );
				s_chatStep = ChatStep::Opening;
			}
			PSU_LOG( "VRSpeech: chat \"%s\" (%s)\n", s_chatText.c_str(), s_chatStep == ChatStep::Typing ? "box open" : "opening" );
		}

		void UpdateChat()
		{
			const DWORD age = GetTickCount() - s_chatTick;
			switch( s_chatStep )
			{
				case ChatStep::Idle:
					return;
				case ChatStep::Opening:
					if( TextInputOpen() )
					{
						s_chatStep = ChatStep::Typing;
					}
					else if( age > kChatStepTimeoutMs )
					{
						PSU_LOG( "VRSpeech: chat box didn't open (input %u mode %u)\n",
							PSUMemory::ReadAbsoluteMemory< uint8_t >( kTextInputActive ),
							PSUMemory::ReadAbsoluteMemory< uint32_t >( kTextInputMode ) );
						Toast( "Chat: the chat box didn't open" );
						s_chatStep = ChatStep::Idle;
					}
					return;
				case ChatStep::Typing:
					if( KeyPending() )
					{
						return;  // the open key is still held: its release must not land in the text
					}
					for( const char c : s_chatText )
					{
						PostMessageA( Global::hWnd, WM_CHAR, static_cast< WPARAM >( static_cast< unsigned char >( c ) ), 1 );
					}
					s_chatTick = GetTickCount();
					s_chatStep = ChatStep::Settle;
					return;
				case ChatStep::Settle:
					if( age >= kTypeSettleMs )
					{
						PressKey( kEnterKey );
						s_chatTick = GetTickCount();
						s_chatStep = ChatStep::Sending;
					}
					return;
				case ChatStep::Sending:
					if( !KeyPending() && !TextInputOpen() )
					{
						Toast( "Chat: %s", s_chatText.c_str() );
						PSU_LOG( "VRSpeech: chat sent\n" );
						s_chatStep = ChatStep::Idle;
					}
					else if( age > kChatStepTimeoutMs )
					{
						Toast( "Chat: typed, but Enter didn't send it" );
						PSU_LOG( "VRSpeech: chat box still open after Enter\n" );
						s_chatStep = ChatStep::Idle;
					}
					return;
				default:
					s_chatStep = ChatStep::Idle;
					return;
			}
		}

		// ---- Voice commands -----------------------------------------------------------------
		// Technique names by slot id (PSUCombat::TechSlots). PROVISIONAL: the groups follow the
		// technique disks ((id - 1) / 10 + 1 = element) but the order inside a group is a guess
		// from the effect table 0x009C63C0; the Debug log prints each slot's id to fix this.
		struct TechName
		{
			uint8_t id;
			const char *name;
		};
		const TechName kTechNames[] = {
			{ 1, "Foie" }, { 2, "Rafoie" }, { 3, "Damfoie" }, { 4, "Gifoie" }, { 5, "Shifta" }, { 6, "Jellen" },
			{ 11, "Barta" }, { 12, "Rabarta" }, { 13, "Dambarta" }, { 14, "Gibarta" }, { 15, "Deband" }, { 16, "Zalure" },
			{ 21, "Zonde" }, { 22, "Razonde" }, { 23, "Gizonde" }, { 24, "Noszonde" }, { 25, "Zodial" }, { 26, "Retier" },
			{ 31, "Diga" }, { 32, "Radiga" }, { 33, "Damdiga" }, { 34, "Gidiga" }, { 35, "Nosdiga" }, { 36, "Reverser" },
			{ 41, "Resta" }, { 42, "Giresta" }, { 43, "Anti" }, { 44, "Megistar" }, { 45, "Grants" }, { 46, "Regrants" },
			{ 51, "Megid" }, { 52, "Dimegid" }, { 53, "Ramegid" }, { 54, "Nosmegid" },
		};

		const char *TechNameFor( uint8_t id )
		{
			for( const TechName &tech : kTechNames )
			{
				if( tech.id == id )
				{
					return tech.name;
				}
			}
			return nullptr;
		}

		const char *const kSlotWords[ 4 ][ 3 ] = {
			{ "slotone", "one", "first" }, { "slottwo", "two", "second" },
			{ "slotthree", "three", "third" }, { "slotfour", "four", "fourth" } };

		const char *const kItemWords[ 4 ][ 2 ] = {
			{ "itemone", "item1" }, { "itemtwo", "item2" }, { "itemthree", "item3" }, { "itemfour", "item4" } };
		const char *const kItemPhrases[ 4 ] = { "item one", "item two", "item three", "item four" };

		// ---- Command registry ------------------------------------------------------------------
		// Spoken aliases -> an action. Rebuilt about every 500 ms from the live game state, so it
		// follows the equipped weapon. Categories: Tech (cast slot `arg`), Item (palette item `arg`).
		// Phase 3 adds skills / Photon Blast as further kinds.
		enum class CommandKind { Tech, Item };
		struct Command
		{
			CommandKind kind;
			int arg;
			std::vector< std::string > exact;  // Normalize()d words that must match whole
			std::string fuzzy;                 // lower-case name matched within an edit distance ("" = none)
		};
		std::vector< Command > s_commands;

		PSUCombat::TechSlots s_slots;
		bool s_slotsValid = false;
		bool s_stateValid = false;
		int s_mode = 0;  // registry inputs: 1 items, 2 techniques
		DWORD s_slotsTick = 0;
		std::string s_keyterms;

		// Lower-case letters and digits only, minus filler words in front ("cast barta").
		std::string Normalize( const std::string &text )
		{
			std::string words;
			std::string word;
			std::vector< std::string > list;
			for( size_t i = 0; i <= text.size(); ++i )
			{
				const unsigned char c = i < text.size() ? static_cast< unsigned char >( text[ i ] ) : ' ';
				if( std::isalnum( c ) )
				{
					word += static_cast< char >( std::tolower( c ) );
				}
				else if( !word.empty() )
				{
					list.push_back( word );
					word.clear();
				}
			}
			size_t first = 0;
			while( first < list.size() && ( list[ first ] == "cast" || list[ first ] == "use" || list[ first ] == "technique" ) )
			{
				++first;
			}
			for( size_t i = first; i < list.size(); ++i )
			{
				words += list[ i ];
			}
			return words;
		}

		std::string Lower( const char *text )
		{
			std::string out;
			for( ; *text; ++text )
			{
				out += static_cast< char >( std::tolower( static_cast< unsigned char >( *text ) ) );
			}
			return out;
		}

		int EditDistance( const std::string &a, const std::string &b )
		{
			std::vector< int > row( b.size() + 1 );
			for( size_t j = 0; j <= b.size(); ++j )
			{
				row[ j ] = static_cast< int >( j );
			}
			for( size_t i = 1; i <= a.size(); ++i )
			{
				int diagonal = row[ 0 ];
				row[ 0 ] = static_cast< int >( i );
				for( size_t j = 1; j <= b.size(); ++j )
				{
					const int up = row[ j ];
					row[ j ] = ( std::min )( { row[ j ] + 1, row[ j - 1 ] + 1, diagonal + ( a[ i - 1 ] == b[ j - 1 ] ? 0 : 1 ) } );
					diagonal = up;
				}
			}
			return row[ b.size() ];
		}

		const char *TechLabel( int slot )
		{
			const char *name = TechNameFor( s_slots.id[ slot ] );
			return name ? name : "-";
		}

		// Rebuilds the registry from the game state (throttled to 500 ms) and sends the helper
		// its KEYTERMS bias when the spoken names changed.
		void RefreshCommands()
		{
			const DWORD now = GetTickCount();
			if( now - s_slotsTick < 500 )
			{
				return;
			}
			s_slotsTick = now;
			PSUCombat::State state;
			PSUCombat::TechSlots slots;
			const bool stateValid = PSUCombat::Read( state );
			const bool valid = stateValid && PSUCombat::ReadTechSlots( state, slots );
			const bool itemsOn = stateValid && Config::Get().vr.m_voiceItems;
			const bool techsOn = Config::Get().vr.m_voiceCommands;
			const int mode = ( itemsOn ? 1 : 0 ) | ( techsOn ? 2 : 0 );
			const bool changed = valid != s_slotsValid || memcmp( slots.id, s_slots.id, 4 ) != 0 || mode != s_mode;
			s_slots = slots;
			s_slotsValid = valid;
			s_stateValid = stateValid;
			s_mode = mode;
			if( !changed )
			{
				return;
			}
			s_commands.clear();
			std::string keyterms;
			if( valid && techsOn )
			{
				for( int slot = 0; slot < 4; ++slot )
				{
					if( !s_slots.id[ slot ] )
					{
						continue;  // only slots holding a technique count
					}
					Command command{ CommandKind::Tech, slot, {}, {} };
					for( const char *word : kSlotWords[ slot ] )
					{
						command.exact.push_back( word );
					}
					if( const char *name = TechNameFor( s_slots.id[ slot ] ) )
					{
						command.fuzzy = Lower( name );
						keyterms += keyterms.empty() ? "" : ",";
						keyterms += name;
					}
					s_commands.push_back( std::move( command ) );
				}
			}
			if( itemsOn )
			{
				for( int item = 0; item < 4; ++item )
				{
					Command command{ CommandKind::Item, item, {}, {} };
					for( const char *word : kItemWords[ item ] )
					{
						command.exact.push_back( word );
					}
					s_commands.push_back( std::move( command ) );
					keyterms += keyterms.empty() ? "" : ",";
					keyterms += kItemPhrases[ item ];
				}
			}
			PSU_LOG( "VRVoice: slots %u/%u/%u/%u (lv %u/%u/%u/%u) = %s / %s / %s / %s; right %08X [%02X %02X %02X %02X %02X %02X %02X %02X] "
				"left %08X [%02X %02X %02X %02X %02X %02X %02X %02X], categories %X / %X; %u commands\n",
				s_slots.id[ 0 ], s_slots.id[ 1 ], s_slots.id[ 2 ], s_slots.id[ 3 ],
				s_slots.level[ 0 ], s_slots.level[ 1 ], s_slots.level[ 2 ], s_slots.level[ 3 ],
				TechLabel( 0 ), TechLabel( 1 ), TechLabel( 2 ), TechLabel( 3 ),
				s_slots.vtable[ 0 ], s_slots.raw[ 0 ][ 0 ], s_slots.raw[ 0 ][ 1 ], s_slots.raw[ 0 ][ 2 ], s_slots.raw[ 0 ][ 3 ],
				s_slots.raw[ 0 ][ 4 ], s_slots.raw[ 0 ][ 5 ], s_slots.raw[ 0 ][ 6 ], s_slots.raw[ 0 ][ 7 ],
				s_slots.vtable[ 1 ], s_slots.raw[ 1 ][ 0 ], s_slots.raw[ 1 ][ 1 ], s_slots.raw[ 1 ][ 2 ], s_slots.raw[ 1 ][ 3 ],
				s_slots.raw[ 1 ][ 4 ], s_slots.raw[ 1 ][ 5 ], s_slots.raw[ 1 ][ 6 ], s_slots.raw[ 1 ][ 7 ],
				state.rightCategory, state.leftCategory, static_cast< unsigned >( s_commands.size() ) );
			if( keyterms != s_keyterms )
			{
				s_keyterms = keyterms;
				SendLine( "KEYTERMS " + keyterms );
			}
		}

		// The registry entry the words name, or -1.
		int MatchCommand( const std::string &text )
		{
			const std::string said = Normalize( text );
			if( said.empty() )
			{
				return -1;
			}
			int best = -1;
			int bestDistance = 1000;
			for( size_t index = 0; index < s_commands.size(); ++index )
			{
				const Command &command = s_commands[ index ];
				for( const std::string &word : command.exact )
				{
					if( said == word )
					{
						return static_cast< int >( index );
					}
				}
				if( command.fuzzy.empty() )
				{
					continue;
				}
				const int distance = EditDistance( said, command.fuzzy );
				// Exact, or one letter off for longer names ("gibarda").
				const int allowed = command.fuzzy.size() >= 5 ? 1 : 0;
				if( distance <= allowed && distance < bestDistance )
				{
					best = static_cast< int >( index );
					bestDistance = distance;
				}
			}
			return best;
		}

		// Runs a registry entry; true when it was started.
		bool Execute( const Command &command, const std::string &text )
		{
			if( command.kind == CommandKind::Tech )
			{
				const char *name = TechNameFor( s_slots.id[ command.arg ] );
				if( VRSyntheticPad::CastTechSlot( command.arg ) )
				{
					Toast( "Voice: %s (slot %d)", name ? name : "technique", command.arg + 1 );
				}
				else
				{
					Toast( "Voice: %s can't be cast right now", name ? name : "that" );
				}
				PSU_LOG( "VRVoice: \"%s\" -> slot %d (id %u)\n", text.c_str(), command.arg + 1, s_slots.id[ command.arg ] );
				return true;
			}
			if( VRSyntheticPad::UseItemSlot( command.arg ) )
			{
				Toast( "Voice: item %d", command.arg + 1 );
			}
			else
			{
				Toast( "Voice: item %d can't be used right now", command.arg + 1 );
			}
			PSU_LOG( "VRVoice: \"%s\" -> item %d\n", text.c_str(), command.arg + 1 );
			return true;
		}

		// A menu has the input (0x00B9CC80 0x0405): the chat key would go to it.
		bool MenuHasInput()
		{
			return ( PSUMemory::ReadAbsoluteMemory< uint32_t >( 0x00B9CC80 ) & 0xFFFF ) == 0x0405;
		}

		// The "in battle" guess for the auto route: free in the field (no menu, no text box) with a
		// weapon equipped. There is no enemy test yet, so this also holds in a town.
		bool WeaponOutInField()
		{
			PSUCombat::State state;
			return PSUCombat::Read( state ) && !MenuHasInput() && !TextInputOpen() &&
				( state.rightCategory || state.leftCategory );
		}

		enum class Route { Command, Chat, Ignore };

		// Where an utterance goes. The held button (not automatic) always has the chat as its
		// fallback; an always-on utterance only reaches the chat in the explicit chat route, so
		// background talk is never broadcast by accident.
		Route ResolveRoute( bool automatic )
		{
			const std::string &route = Config::Get().vr.m_voiceRoute;
			if( route == "chat" )
			{
				return Route::Chat;
			}
			if( route == "command" || WeaponOutInField() )
			{
				return Route::Command;
			}
			return automatic ? Route::Ignore : Route::Chat;
		}

		const char *RouteLabel( const std::string &route )
		{
			return route == "command" ? "Command" : route == "chat" ? "Chat" : "Auto (command in battle)";
		}

		void HandleFinal( const std::string &text, bool automatic )
		{
			const auto &vr = Config::Get().vr;
			PSU_LOG( "VRSpeech: heard \"%s\"%s\n", text.c_str(), automatic ? " (always-on)" : "" );
			if( text.empty() )
			{
				Toast( "Speech: didn't catch that" );
				return;
			}
			const Route route = ResolveRoute( automatic );
			if( route == Route::Ignore )
			{
				PSU_LOG( "VRVoice: ignored \"%s\" (no weapon out in the field)\n", text.c_str() );
				return;
			}
			if( route == Route::Command && ( vr.m_voiceCommands || vr.m_voiceItems ) )
			{
				RefreshCommands();
				const int match = MatchCommand( text );
				if( match >= 0 )
				{
					Execute( s_commands[ match ], text );
					return;
				}
				if( automatic )
				{
					// Always-on chatter that names no technique: nothing to do.
					PSU_LOG( "VRVoice: no command in \"%s\"\n", text.c_str() );
#ifdef _DEBUG
					Toast( "Heard: %s (no command)", text.c_str() );
#endif
					return;
				}
			}
			else if( route == Route::Command && automatic )
			{
				PSU_LOG( "VRVoice: \"%s\" dropped (voice commands are off)\n", text.c_str() );
				return;
			}
			if( vr.m_speechChat )
			{
				if( s_chatStep != ChatStep::Idle )
				{
					Toast( "Chat: still sending the last message" );
					return;
				}
				if( MenuHasInput() )
				{
					Toast( "Not sent (a menu is open): %s", text.c_str() );
					return;
				}
				StartChat( text );
				return;
			}
			Toast( "Heard: %s", text.c_str() );
		}

		void HandleLine( const std::string &line )
		{
			const size_t space = line.find( ' ' );
			const std::string kind = line.substr( 0, space );
			const std::string text = space == std::string::npos ? std::string() : line.substr( space + 1 );
			if( kind == "READY" )
			{
				s_ready = true;
				s_keyterms.clear();
				s_slotsTick = 0;
				s_slotsValid = false;
				s_stateValid = false;
				s_mode = 0;
				s_commands.clear();
				s_sentAuto = false;  // a fresh helper starts in push-to-talk
				Toast( Config::Get().vr.m_speechAlwaysOn ? "Speech to text ready: always listening" :
					"Speech to text ready: hold left Square and talk" );
				PSU_LOG( "VRSpeech: ready (%s)\n", text.c_str() );
			}
			else if( kind == "STATUS" )
			{
				Toast( "Speech: %s", text.c_str() );
				PSU_LOG( "VRSpeech: %s\n", text.c_str() );
			}
			else if( kind == "ERROR" )
			{
				Toast( "Speech: %s", text.c_str() );
				PSU_LOG( "VRSpeech: helper error: %s\n", text.c_str() );
			}
			else if( kind == "PARTIAL" )
			{
				if( s_talking )
				{
					Toast( "... %s", text.c_str() );
				}
			}
			else if( kind == "FINAL" )
			{
				HandleFinal( text, false );
			}
			else if( kind == "AUTO" )
			{
				HandleFinal( text, true );
			}
			else
			{
				PSU_LOG( "VRSpeech: helper: %s\n", line.c_str() );
			}
		}
	}

	namespace VRSpeech
	{
		void Update()
		{
			const auto &vr = Config::Get().vr;
			const bool want = vr.m_enabled && vr.m_speechToText;
			if( !want || ( s_process && s_runningModel != vr.m_speechModel ) )
			{
				StopHelper();
			}
			if( want && !s_process && GetTickCount() >= s_nextStartTick )
			{
				if( !VRSpeechFiles::ModelInstalled( vr.m_speechModel ) )
				{
					if( !s_missingToastShown )
					{
						Toast( "Speech to text: the %s model isn't installed (VR settings > Voice > Download)", vr.m_speechModel.c_str() );
						s_missingToastShown = true;
					}
					s_nextStartTick = GetTickCount() + kRestartDelayMs;
				}
				else if( !StartHelper( vr.m_speechModel ) )
				{
					s_nextStartTick = GetTickCount() + kRestartDelayMs;
				}
			}
			if( !want )
			{
				s_missingToastShown = false;
			}

			std::deque< std::string > lines;
			{
				std::lock_guard< std::mutex > lock( s_lineMutex );
				lines.swap( s_lines );
			}
			for( const std::string &line : lines )
			{
				HandleLine( line );
			}
			// The helper exited (crash, missing model, error): try again later.
			if( s_process && s_readerDone && WaitForSingleObject( s_process, 0 ) == WAIT_OBJECT_0 )
			{
				DWORD code = 0;
				GetExitCodeProcess( s_process, &code );
				PSU_LOG( "VRSpeech: helper exited (%lu)\n", code );
				StopHelper();
				s_nextStartTick = GetTickCount() + kRestartDelayMs;
			}

			if( s_ready && vr.m_speechAlwaysOn != s_sentAuto )
			{
				s_sentAuto = vr.m_speechAlwaysOn;
				SendLine( s_sentAuto ? "MODE auto" : "MODE push" );
				PSU_LOG( "VRSpeech: %s\n", s_sentAuto ? "always listening" : "push to talk" );
			}
			if( s_ready && ( vr.m_voiceCommands || vr.m_voiceItems ) )
			{
				RefreshCommands();
			}
			UpdateChat();
		}

		void CycleRoute()
		{
			std::string &route = Config::Get().vr.m_voiceRoute;
			route = route == "command" ? "chat" : route == "chat" ? "auto" : "command";
			Toast( "Voice goes to: %s", RouteLabel( route ) );
			PSU_LOG( "VRSpeech: route %s\n", route.c_str() );
		}

		void BeginTalk()
		{
			const auto &vr = Config::Get().vr;
			if( !vr.m_speechToText )
			{
				VRBindings::ShowToast( "Speech to text is off (VR settings > Voice)" );
				return;
			}
			if( !s_ready )
			{
				VRBindings::ShowToast( s_process ? "Speech to text is still loading" : "Speech to text isn't running" );
				return;
			}
			SendLine( "BEGIN" );
			s_talking = true;
			s_talkTick = GetTickCount();
			VRBindings::ShowToast( "Listening..." );
		}

		void EndTalk()
		{
			if( !s_talking )
			{
				return;
			}
			s_talking = false;
			if( GetTickCount() - s_talkTick < kTapMs )
			{
				// A tap on the talk button switches where speech goes instead.
				SendLine( "CANCEL" );
				CycleRoute();
				return;
			}
			SendLine( "END" );
		}

		bool Talking()
		{
			return s_talking;
		}

		unsigned InjectKeys( uint8_t *keys, unsigned count )
		{
			std::lock_guard< std::mutex > lock( s_keyMutex );
			if( s_injectPolls <= 0 || !s_injectKey )
			{
				return count;
			}
			--s_injectPolls;
			for( unsigned i = 0; i < count; ++i )
			{
				if( keys[ i ] == s_injectKey )
				{
					return count;
				}
			}
			if( count < 14 )
			{
				keys[ count++ ] = s_injectKey;
			}
			return count;
		}
	}
}
