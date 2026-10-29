/*
* Per-frame render orchestrator + mode-1 scene callback (3D+UI while offscreen).
*/

#pragma once

#include "../PSUEx.h"
#include "../Misc/Log.h"
#include "../PSUOverlay/FrameTrace.h"
#include "../PSUVR/StereoRenderer.h"
#include "../PSUVR/VRBindings.h"
#include "../PSUVR/VRCamera.h"
#include "../PSUVR/VRHud.h"
#include "../PSUVR/VRPerf.h"
#include <intrin.h>

namespace FrameRender
{
	// unsigned char PSU_FrameRender(void);
	typedef unsigned char( __cdecl *FrameRender_t )();
	FrameRender_t pOriginalFrameRender = reinterpret_cast< FrameRender_t >( 0x00479F50 );

	// void PSU_SceneMode1_Callback(void); — table slot for mode==1
	typedef void( __cdecl *SceneCallback_t )();
	SceneCallback_t pOriginalSceneCallback = reinterpret_cast< SceneCallback_t >( 0x0046F080 );

	static void LogMode( const char *tag )
	{
		const unsigned mode = PSUMemory::ReadAbsoluteMemory< unsigned >( 0x00B90EA8 );
		const uintptr_t table = 0x0099325C;
		const uintptr_t slot = table + static_cast< uintptr_t >( mode ) * 0x10;
		const uintptr_t callback = PSUMemory::ReadAbsoluteMemory< uintptr_t >( slot );
		const unsigned skipFlag = PSUMemory::ReadAbsoluteMemory< unsigned >( 0x00B90ED0 );
		PSU_LOG( "PSU_FrameRender %s mode=%u skip=%u callback=%p slot=%p\n",
			tag, mode, skipFlag, ( void * )callback, ( void * )slot );
	}

	unsigned char __cdecl Hook_FrameRender()
	{
		static bool s_logged = false;
		static bool s_gameplayModeLogged = false;
		static unsigned s_lastMode = 0xFFFFFFFFu;
		const unsigned mode = PSUMemory::ReadAbsoluteMemory< unsigned >( 0x00B90EA8 );
		if( !s_logged )
		{
			s_logged = true;
			s_lastMode = mode;
			LogMode( "first" );
		}
		else if( mode != s_lastMode )
		{
			s_lastMode = mode;
			LogMode( "mode-change" );
		}
		if( !s_gameplayModeLogged && FrameTrace::GameplayArmed() )
		{
			s_gameplayModeLogged = true;
			LogMode( "gameplay" );
		}
		return pOriginalFrameRender();
	}

	void __cdecl Hook_SceneMode1_Callback()
	{
		static bool s_logged = false;
		if( !s_logged )
		{
			s_logged = true;
			PSU_LOG( "PSU_SceneMode1_Callback @ 0x0046F080 first hit\n" );
		}
		FrameTrace::SceneCallbackEnter( 0x0046F080 );
		pOriginalSceneCallback();
		FrameTrace::SceneCallbackLeave();
	}

	// Other slot-0 render callbacks from table 0x0099325C (called as `call eax`, no args).
	// 0x0046ACB0 = mode 5 (town); 0x00466360 = modes 3/9; 0x00474C00 = mode 11.
	SceneCallback_t pOriginalSceneMode5 = reinterpret_cast< SceneCallback_t >( 0x0046ACB0 );
	SceneCallback_t pOriginalSceneMode3 = reinterpret_cast< SceneCallback_t >( 0x00466360 );
	SceneCallback_t pOriginalSceneMode11 = reinterpret_cast< SceneCallback_t >( 0x00474C00 );

	void __cdecl Hook_SceneMode5_Callback()
	{
		FrameTrace::SceneCallbackEnter( 0x0046ACB0 );
		pOriginalSceneMode5();
		FrameTrace::SceneCallbackLeave();
	}

	void __cdecl Hook_SceneMode3_Callback()
	{
		FrameTrace::SceneCallbackEnter( 0x00466360 );
		pOriginalSceneMode3();
		FrameTrace::SceneCallbackLeave();
	}

	void __cdecl Hook_SceneMode11_Callback()
	{
		FrameTrace::SceneCallbackEnter( 0x00474C00 );
		pOriginalSceneMode11();
		FrameTrace::SceneCallbackLeave();
	}

	// Every scene callback does `if( byte[0x00993328] ) { 0x4FD990(); 0x4FDBC0(); }`.
	// Each pass calls 0x00560830 per render layer with the layer index in EDI.
	typedef void( __cdecl *RenderPass_t )();
	RenderPass_t pOriginalPassA = reinterpret_cast< RenderPass_t >( 0x004FD990 ); // layers 0x02-0x29
	RenderPass_t pOriginalPassB = reinterpret_cast< RenderPass_t >( 0x004FDBC0 ); // layers 0x2B-0x3F

	// The game leaves VIEW at identity and bakes the camera into WORLD, so VIEW is
	// free to carry a per-eye offset for the duration of pass A.
	static IDirect3DDevice9 *BeginEyeShift()
	{
		const auto &vr = Config::Get().vr;
		if( !vr.m_enabled || vr.m_debugEyeShift == 0.0f )
		{
			return nullptr;
		}
		IDirect3DDevice9 *device = PSUOverlay::Device();
		if( !device )
		{
			return nullptr;
		}
		static bool s_logged = false;
		if( !s_logged )
		{
			s_logged = true;
			PSU_LOG( "EyeShift: active on pass 0x004FD990, +/-%.3f view units\n", vr.m_debugEyeShift );
		}
		D3DMATRIX view{};
		view._11 = view._22 = view._33 = view._44 = 1.0f;
		view._41 = ( ( GetTickCount() / 1000 ) & 1 ) ? vr.m_debugEyeShift : -vr.m_debugEyeShift;
		device->SetTransform( D3DTS_VIEW, &view );
		return device;
	}

	static void EndEyeShift( IDirect3DDevice9 *device )
	{
		if( !device )
		{
			return;
		}
		D3DMATRIX identity{};
		identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
		device->SetTransform( D3DTS_VIEW, &identity );
	}

	// Mid-pass flush between layers 0x0C and 0x0D: draws the deferred lists at
	// [0x00B911A4] / [0x00B911A8] through 0x005621F0, which empties them.
	typedef void( __cdecl *DeferredFlush_t )();
	DeferredFlush_t pOriginalDeferredFlush = reinterpret_cast< DeferredFlush_t >( 0x005620A0 );

	static unsigned s_passAInFrame = 0;
	static unsigned s_flushLogs = 0;
	constexpr unsigned kMaxFlushLogs = 24;

	static int DeferredCount( unsigned listGlobal )
	{
		const auto *list = *reinterpret_cast< const int *const * >( listGlobal );
		return list ? list[ 1 ] : -1;
	}

	static int s_enterA4 = 0;
	static int s_enterA8 = 0;

	void __cdecl Hook_DeferredFlush()
	{
		const int a4 = DeferredCount( 0x00B911A4 );
		const int a8 = DeferredCount( 0x00B911A8 );
		if( FrameTrace::GameplayArmed() && s_flushLogs < kMaxFlushLogs && ( a4 > 0 || a8 > 0 || s_enterA4 > 0 || s_enterA8 > 0 ) )
		{
			++s_flushLogs;
			PSU_LOG( "DeferredFlush passA#%u flags=0x%08X listA4 %d->%d listA8 %d->%d (pass start -> flush)\n",
				s_passAInFrame, *reinterpret_cast< unsigned * >( 0x00B9DB88 ), s_enterA4, a4, s_enterA8, a8 );
		}
		pOriginalDeferredFlush();
	}

	static void RunPassA()
	{
		s_enterA4 = DeferredCount( 0x00B911A4 );
		s_enterA8 = DeferredCount( 0x00B911A8 );
		FrameTrace::PassEnter( 0x004FD990 );
		IDirect3DDevice9 *shifted = BeginEyeShift();
		pOriginalPassA();
		EndEyeShift( shifted );
		FrameTrace::PassLeave();
	}

	// Debug layer bisect: Ctrl+Shift+PageUp / PageDown picks one render layer (0x02..0x3F) to
	// hide, to find which layer draws something (a toast shows the number; past either end = none).
	static int s_hiddenLayer = -1;

	static void PollLayerBisect()
	{
#ifdef _DEBUG
		static bool s_upDown = false, s_downDown = false, s_insDown = false, s_delDown = false;
		const bool mods = ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) && ( GetAsyncKeyState( VK_SHIFT ) & 0x8000 );
		const bool up = mods && ( GetAsyncKeyState( VK_PRIOR ) & 0x8000 );
		const bool down = mods && ( GetAsyncKeyState( VK_NEXT ) & 0x8000 );
		// Ctrl+Shift+Insert / Delete: the screen-space draw site bisect (eyes only).
		const bool ins = mods && ( GetAsyncKeyState( VK_INSERT ) & 0x8000 );
		const bool del = mods && ( GetAsyncKeyState( VK_DELETE ) & 0x8000 );
		if( ( ins && !s_insDown ) || ( del && !s_delDown ) )
		{
			PSUVR::VRBindings::ShowToast( PSUVR::StereoRenderer::StepSiteBisect( ins && !s_insDown ? 1 : -1 ) );
		}
		s_insDown = ins;
		s_delDown = del;
		int step = 0;
		if( up && !s_upDown ) step = 1;
		if( down && !s_downDown ) step = -1;
		s_upDown = up;
		s_downDown = down;
		if( !step )
		{
			return;
		}
		if( s_hiddenLayer < 0 )
		{
			s_hiddenLayer = step > 0 ? 0x02 : 0x3F;
		}
		else
		{
			s_hiddenLayer += step;
			if( s_hiddenLayer < 0x02 || s_hiddenLayer > 0x3F )
			{
				s_hiddenLayer = -1;
			}
		}
		static char s_text[ 48 ];
		if( s_hiddenLayer < 0 )
		{
			sprintf_s( s_text, "Layer bisect: nothing hidden" );
		}
		else
		{
			sprintf_s( s_text, "Layer bisect: hiding layer 0x%02X", s_hiddenLayer );
		}
		PSUVR::VRBindings::ShowToast( s_text );
#endif
	}

	// The layer's disabled byte (g_DrawLayers entry +6); null when the table isn't there.
	static uint8_t *LayerDisabledByte( int layer )
	{
		uint8_t *const *table = reinterpret_cast< uint8_t *const * >( 0x00B9DB38 );
		if( layer < 0 || layer >= 64 || !PSUMemory::IsValidPointer( table ) || !*table ||
			!PSUMemory::IsValidPointer( *table + layer * 8 + 6 ) )
		{
			return nullptr;
		}
		return *table + layer * 8 + 6;
	}

	// RTTI class name of an object (vtable[-1] = complete object locator), or "?".
	static const char *ObjectClassName( const void *object )
	{
		__try
		{
			const uint8_t *const *vt = *static_cast< const uint8_t *const *const * >( object );
			const uint8_t *col = reinterpret_cast< const uint8_t *const * >( vt )[ -1 ];
			const uint8_t *typeDescriptor = *reinterpret_cast< const uint8_t *const * >( col + 0x0C );
			return reinterpret_cast< const char * >( typeDescriptor + 8 );
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			return "?";
		}
	}

	// Debug: Ctrl+Shift+Home logs every render layer's nodes (draw callback, object, its class).
	static void PollLayerDump()
	{
#ifdef _DEBUG
		static bool s_homeDown = false;
		const bool mods = ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) && ( GetAsyncKeyState( VK_SHIFT ) & 0x8000 );
		const bool home = mods && ( GetAsyncKeyState( VK_HOME ) & 0x8000 );
		const bool pressed = home && !s_homeDown;
		s_homeDown = home;
		if( !pressed )
		{
			return;
		}
		uint8_t *const *table = reinterpret_cast< uint8_t *const * >( 0x00B9DB38 );
		if( !PSUMemory::IsValidPointer( table ) || !*table )
		{
			return;
		}
		PSU_LOG( "LayerDump: begin (camera state %d)\n", PSUVR::VRCamera::CameraState() );
		for( int layer = 0x02; layer <= 0x3F; ++layer )
		{
			__try
			{
				const uint8_t *entry = *table + layer * 8;
				const uint8_t *node = *reinterpret_cast< const uint8_t *const * >( entry );
				int count = 0;
				while( node && count < 60 && PSUMemory::IsValidPointer( node + 0x14 ) )
				{
					const void *object = *reinterpret_cast< const void *const * >( node + 0x0C );
					const void *callback = *reinterpret_cast< const void *const * >( node + 0x10 );
					const uint16_t flags = object && PSUMemory::IsValidPointer( static_cast< const uint8_t * >( object ) + 8 )
						? *reinterpret_cast< const uint16_t * >( static_cast< const uint8_t * >( object ) + 8 ) : 0;
					PSU_LOG( "LayerDump: layer 0x%02X%s node %p cb=%p obj=%p flags=%04X class=%s\n", layer, entry[ 6 ] ? " (off)" : "",
						node, callback, object, flags, object ? ObjectClassName( object ) : "-" );
					node = *reinterpret_cast< const uint8_t *const * >( node + 0x08 );
					++count;
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				PSU_LOG( "LayerDump: layer 0x%02X unreadable\n", layer );
			}
		}
		PSU_LOG( "LayerDump: end\n" );
		PSUVR::VRBindings::ShowToast( "Layers written to the log" );
#endif
	}

	void __cdecl Hook_PassA()
	{
		PollLayerBisect();
		PollLayerDump();
		// A pass A layer picked by the bisect: off for this frame's flat and eye passes.
		uint8_t *hidden = s_hiddenLayer >= 0x02 && s_hiddenLayer <= 0x29 ? LayerDisabledByte( s_hiddenLayer ) : nullptr;
		const uint8_t hiddenWas = hidden ? *hidden : 0;
		if( hidden )
		{
			*hidden = 1;
		}
		const auto &vr = Config::Get().vr;
		const bool twice = vr.m_enabled && vr.m_debugDoublePass;
		IDirect3DDevice9 *device = PSUOverlay::Device();
		auto &stereo = PSUVR::StereoRenderer::Get();
		// flatMirrorEye: in stereo the monitor gets the left eye instead of its own 3D pass. The stereo panel
		// shows the flat picture in the headset, so it keeps the flat pass.
		const bool mirror = vr.m_enabled && vr.m_flatMirrorEye && !vr.m_stereoPanel && !twice && stereo.CanRenderEyes( device );
		if( !mirror )
		{
			s_passAInFrame = 1;
			{
				PSUVR::VRPerf::Scope timer( PSUVR::VRPerf::Stage::FlatPass );
				PSUVR::VRPerf::GpuBegin( device, PSUVR::VRPerf::Gpu::FlatPass );
				RunPassA();
				PSUVR::VRPerf::GpuEnd( device, PSUVR::VRPerf::Gpu::FlatPass );
			}
			if( twice )
			{
				s_passAInFrame = 2;
				RunPassA();
			}
			s_passAInFrame = 0;
		}
		// Stereo only in the configured scene mode (default 5 = town). Menus stay mono.
		const unsigned mode = PSUMemory::ReadAbsoluteMemory< unsigned >( 0x00B90EA8 );
		bool eyesDrawn = false;
		if( vr.m_enabled && vr.m_stereo && ( vr.m_stereoMode == 0 || mode == vr.m_stereoMode ) )
		{
			eyesDrawn = stereo.RenderEyes( device, pOriginalPassA );
		}
		if( mirror )
		{
			// The flat stage times the mirror copy here, so the overlay shows what replaced the pass.
			PSUVR::VRPerf::Scope timer( PSUVR::VRPerf::Stage::FlatPass );
			PSUVR::VRPerf::GpuBegin( device, PSUVR::VRPerf::Gpu::FlatPass );
			if( !eyesDrawn || !stereo.MirrorLeftEye( device ) )
			{
				// The eyes didn't draw after all (eye targets failed): the monitor still needs a picture.
				s_passAInFrame = 1;
				RunPassA();
				s_passAInFrame = 0;
			}
			PSUVR::VRPerf::GpuEnd( device, PSUVR::VRPerf::Gpu::FlatPass );
		}
		if( hidden )
		{
			*hidden = hiddenWas;
		}
	}

	// PSU_DrawLayer takes the layer index in EDI.
	static void DrawLayer( int layer )
	{
		static const uintptr_t s_drawLayer = 0x00560830;
		__asm
		{
			push edi
			mov edi, layer
			call dword ptr [ s_drawLayer ]
			pop edi
		}
	}

	// Same as 0x004FDBC0 (`mov edi, layer; call 0x00560830` for 0x2B..0x3F), but records
	// the current layer so VRHud knows exactly which layer object a 2D draw belongs to.
	void __cdecl Hook_PassB()
	{
		FrameTrace::PassEnter( 0x004FDBC0 );
		PSUVR::VRPerf::Scope timer( PSUVR::VRPerf::Stage::PassB );
		PSUVR::VRPerf::GpuBegin( PSUOverlay::Device(), PSUVR::VRPerf::Gpu::PassB );
		for( int layer = 0x2B; layer <= 0x3F; ++layer )
		{
			PSUVR::VRHud::SetPassBLayer( layer );
			if( layer != s_hiddenLayer )
			{
				DrawLayer( layer );
			}
		}
		PSUVR::VRHud::SetPassBLayer( -1 );
		PSUVR::VRPerf::GpuEnd( PSUOverlay::Device(), PSUVR::VRPerf::Gpu::PassB );
		FrameTrace::PassLeave();
	}

	// Aggregator candidate — PE has 12 sites; town session: never hit.
	typedef int( __thiscall *DrawAgg_t )( void *self, int flag );
	DrawAgg_t pOriginalDrawAgg = reinterpret_cast< DrawAgg_t >( 0x004FF1B0 );

	int __fastcall Hook_DrawAgg( void *self, void * /*edx*/, int flag )
	{
		static unsigned s_hits = 0;
		if( s_hits < 8 )
		{
			++s_hits;
			PSU_LOG( "DrawAgg 0x4FF1B0 #%u this=%p flag=%d inSceneCB=%d ret=%p\n",
				s_hits, self, flag, FrameTrace::InSceneCallback() ? 1 : 0, _ReturnAddress() );
		}
		return pOriginalDrawAgg( self, flag );
	}

	static uintptr_t s_textLo = 0;
	static uintptr_t s_textHi = 0;

	static void InitTextRange()
	{
		if( s_textHi )
		{
			return;
		}
		// The code section's header lacks IMAGE_SCN_MEM_EXECUTE (only a tiny stub
		// section has it), so take the first section rather than filtering on flags.
		const auto base = reinterpret_cast< uintptr_t >( GetModuleHandleA( nullptr ) );
		const auto dos = reinterpret_cast< const IMAGE_DOS_HEADER * >( base );
		const auto nt = reinterpret_cast< const IMAGE_NT_HEADERS32 * >( base + dos->e_lfanew );
		const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION( nt );
		if( nt->FileHeader.NumberOfSections > 0 )
		{
			s_textLo = base + sec->VirtualAddress;
			s_textHi = s_textLo + sec->Misc.VirtualSize;
		}
		if( !s_textHi )
		{
			s_textLo = 0x00401000;
			s_textHi = 0x0086B000;
		}
	}

	// Game code is built without frame pointers, so walk raw stack slots and keep
	// values that point just past a call instruction in the game's code section.
	static bool LooksLikeReturn( uintptr_t v )
	{
		if( v < s_textLo + 6 || v >= s_textHi )
		{
			return false;
		}
		const auto *p = reinterpret_cast< const uint8_t * >( v );
		if( p[ -5 ] == 0xE8 )
		{
			const uintptr_t target = v + *reinterpret_cast< const int32_t * >( p - 4 );
			return target >= s_textLo && target < s_textHi;
		}
		if( p[ -6 ] == 0xFF && ( p[ -5 ] == 0x15 || ( p[ -5 ] & 0xF8 ) == 0x90 ) )
		{
			return true;
		}
		if( p[ -3 ] == 0xFF && ( p[ -2 ] & 0xF8 ) == 0x50 )
		{
			return true;
		}
		if( p[ -2 ] == 0xFF && ( ( p[ -1 ] & 0xF8 ) == 0xD0 || ( p[ -1 ] & 0xF8 ) == 0x10 ) )
		{
			return true;
		}
		return false;
	}

	static unsigned CaptureChain( uintptr_t *out, unsigned max )
	{
		InitTextRange();
		auto *sp = reinterpret_cast< const uintptr_t * >( _AddressOfReturnAddress() );
		const auto *tib = reinterpret_cast< const NT_TIB * >( NtCurrentTeb() );
		const auto *top = reinterpret_cast< const uintptr_t * >( tib->StackBase );
		const uintptr_t *end = sp + 1024;
		if( end > top )
		{
			end = top;
		}
		unsigned n = 0;
		for( const uintptr_t *p = sp; p < end && n < max; ++p )
		{
			const uintptr_t v = *p;
			if( LooksLikeReturn( v ) && ( n == 0 || out[ n - 1 ] != v ) )
			{
				out[ n++ ] = v;
			}
		}
		return n;
	}

	// DIP leaf — town verified. For each unique caller, log the stack chain.
	typedef void( __cdecl *DipWrap_t )( void *a1, void *a2 );
	DipWrap_t pOriginalDipWrap = reinterpret_cast< DipWrap_t >( 0x0074E490 );

	void __cdecl Hook_DipWrap( void *a1, void *a2 )
	{
		static void *s_rets[ 16 ]{};
		static unsigned s_count = 0;
		void *ret = _ReturnAddress();
		bool seen = false;
		for( unsigned i = 0; i < s_count; ++i )
		{
			if( s_rets[ i ] == ret )
			{
				seen = true;
				break;
			}
		}
		if( !seen && s_count < 16 )
		{
			s_rets[ s_count++ ] = ret;
			PSU_LOG( "DipWrap 0x74E490 callerRet=%p a1=%p a2=%p inSceneCB=%d gameplay=%d (#%u unique)\n",
				ret, a1, a2, FrameTrace::InSceneCallback() ? 1 : 0,
				FrameTrace::GameplayArmed() ? 1 : 0, s_count );

			uintptr_t chain[ 20 ]{};
			const unsigned n = CaptureChain( chain, 20 );
			char line[ 320 ];
			int pos = snprintf( line, sizeof( line ), "DipChain #%u:", s_count );
			for( unsigned i = 0; i < n && pos > 0 && pos < static_cast< int >( sizeof( line ) ) - 12; ++i )
			{
				pos += snprintf( line + pos, sizeof( line ) - pos, " %08X", static_cast< unsigned >( chain[ i ] ) );
			}
			PSU_LOG( "%s\n", line );
		}
		pOriginalDipWrap( a1, a2 );
	}

	void Hook()
	{
		DetourAttach( &( PVOID & )pOriginalFrameRender, reinterpret_cast< PVOID >( Hook_FrameRender ) );
		DetourAttach( &( PVOID & )pOriginalSceneCallback, reinterpret_cast< PVOID >( Hook_SceneMode1_Callback ) );
		DetourAttach( &( PVOID & )pOriginalSceneMode5, reinterpret_cast< PVOID >( Hook_SceneMode5_Callback ) );
		DetourAttach( &( PVOID & )pOriginalSceneMode3, reinterpret_cast< PVOID >( Hook_SceneMode3_Callback ) );
		DetourAttach( &( PVOID & )pOriginalSceneMode11, reinterpret_cast< PVOID >( Hook_SceneMode11_Callback ) );
		DetourAttach( &( PVOID & )pOriginalPassA, reinterpret_cast< PVOID >( Hook_PassA ) );
		DetourAttach( &( PVOID & )pOriginalPassB, reinterpret_cast< PVOID >( Hook_PassB ) );
		DetourAttach( &( PVOID & )pOriginalDeferredFlush, reinterpret_cast< PVOID >( Hook_DeferredFlush ) );
		DetourAttach( &( PVOID & )pOriginalDrawAgg, reinterpret_cast< PVOID >( Hook_DrawAgg ) );
		DetourAttach( &( PVOID & )pOriginalDipWrap, reinterpret_cast< PVOID >( Hook_DipWrap ) );
	}
}
