#include "VRPlayerDraw.h"

#include "StereoRenderer.h"
#include "VRCamera.h"
#include "VRHands.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUMemory/Memory.hpp"
#include "../PSUStructs/PSUPlayerData.h"

#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace PSUVR
{
	namespace
	{
		// PSU_DrawLayer 0x00560830: *(0x00B9DB38) -> 64 entries x 8 bytes (+0 first node).
		// Node: +0x08 next, +0x0C object, +0x10 cdecl void callback( object ).
		constexpr uintptr_t kLayerTablePtr = 0x00B9DB38;
		constexpr unsigned kLayerCount = 0x40;
		// Pass A (3D world) draws layers 0x02..0x29.
		constexpr unsigned kFirstWorldLayer = 0x02;
		constexpr unsigned kLastWorldLayer = 0x29;
		constexpr unsigned kLayerStride = 8;
		constexpr uintptr_t kNodeNext = 0x08;
		constexpr uintptr_t kNodeObject = 0x0C;
		constexpr uintptr_t kNodeCallback = 0x10;
		constexpr unsigned kMaxNodesPerLayer = 4096;

		typedef void( __cdecl *DrawCallback )( void *object );

		struct Patch
		{
			uint8_t *node;
			DrawCallback original;
		};

		constexpr unsigned kMaxPatches = 8;
		Patch s_patches[ kMaxPatches ]{};
		unsigned s_patchCount = 0;
		bool s_drawingLocalPlayer = false;
		bool s_forcingVisible = false;
		void *s_playerObject = nullptr;
		bool s_loggedPatch = false;
		bool s_loggedDiscovery = false;
		unsigned s_frame = 0;
		unsigned s_nextScanFrame = 0;

		// Full layer walks are the expensive part (hundreds of nodes). Once patched, only
		// the patched nodes are re-checked; a full walk runs on a miss or as a safety net.
		constexpr unsigned kRescanFrames = 300;      // ~3 s at 90 Hz while patched
		constexpr unsigned kRetryScanFrames = 45;    // while the player has no node yet

		// Small structs never straddle a page boundary in practice; check the end only
		// for larger spans (VirtualQuery per node adds up at 90 Hz).
		bool Readable( const void *p, size_t bytes )
		{
			if( !p || !PSUMemory::IsValidPointer( p ) )
			{
				return false;
			}
			return bytes <= 0x40 || PSUMemory::IsValidPointer( static_cast< const uint8_t * >( p ) + bytes - 1 );
		}

		// The loaded player character: TDivModel at render object +0x04 (render object at draw
		// object +0x160). VRHands edits its bone pose when the game builds it (0x0074B760).
		const uint8_t *s_divModel = nullptr;

		void NotePlayerModel( void *drawObject )
		{
			__try
			{
				const uint8_t *render = *reinterpret_cast< const uint8_t * const * >( static_cast< const uint8_t * >( drawObject ) + 0x160 );
				const uint8_t *div = Readable( render, 0x30 ) ? *reinterpret_cast< const uint8_t * const * >( render + 0x04 ) : nullptr;
				s_divModel = Readable( div, 0x70 ) ? div : nullptr;
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				s_divModel = nullptr;
			}
		}

		void __cdecl LocalPlayerDrawThunk( void *object )
		{
			DrawCallback original = nullptr;
			for( unsigned i = 0; i < s_patchCount; ++i )
			{
				if( *reinterpret_cast< void ** >( s_patches[ i ].node + kNodeObject ) == object )
				{
					original = s_patches[ i ].original;
					break;
				}
			}
			if( !original && s_patchCount > 0 )
			{
				original = s_patches[ 0 ].original;
			}
			if( !original )
			{
				return;
			}
#ifdef _DEBUG
			// How often the player is drawn: per patched node, in / outside the eye passes.
			{
				static unsigned s_calls[ kMaxPatches ][ 2 ]{};
				static unsigned s_firstFrame = 0;
				static DWORD s_lastCallLog = 0;
				for( unsigned i = 0; i < s_patchCount; ++i )
				{
					if( *reinterpret_cast< void ** >( s_patches[ i ].node + kNodeObject ) == object &&
						s_patches[ i ].original == original )
					{
						++s_calls[ i ][ StereoRenderer::InEyePass() ? 1 : 0 ];
						break;
					}
				}
				if( GetTickCount() - s_lastCallLog >= 3000 )
				{
					s_lastCallLog = GetTickCount();
					char line[ 512 ];
					int len = 0;
					for( unsigned i = 0; i < s_patchCount && len < 440; ++i )
					{
						len += sprintf_s( line + len, sizeof( line ) - len, " node%u=%p flat %u eye %u", i, s_patches[ i ].node,
							s_calls[ i ][ 0 ], s_calls[ i ][ 1 ] );
					}
					PSU_LOG( "VRPlayerDraw: player draws over %u frames:%s\n", s_frame - s_firstFrame, line );
					memset( s_calls, 0, sizeof( s_calls ) );
					s_firstFrame = s_frame;
				}
			}
#endif
			NotePlayerModel( object );
			// Free aim (arms following): the layer draw already reaches this node (BeginEyePasses);
			// the game still culls the player against its aim camera (see IgnoreNearHide /
			// IgnorePartCull).
			const auto &vr = Config::Get().vr;
			const bool forceShow = VRCamera::CameraState() == 4 && vr.m_handsFollow && VRCamera::FirstPersonActive();
			s_drawingLocalPlayer = true;
			s_forcingVisible = forceShow;
			original( object );
			s_forcingVisible = false;
			s_drawingLocalPlayer = false;
			if( s_divModel )
			{
				VRHands::AfterPlayerDraw( const_cast< uint8_t * >( s_divModel ) );
			}
		}

		DrawCallback NodeCallback( uint8_t *node )
		{
			return *reinterpret_cast< DrawCallback * >( node + kNodeCallback );
		}

		void RestoreAll()
		{
			for( unsigned i = 0; i < s_patchCount; ++i )
			{
				uint8_t *node = s_patches[ i ].node;
				if( Readable( node, kNodeCallback + 4 ) && NodeCallback( node ) == &LocalPlayerDrawThunk )
				{
					*reinterpret_cast< DrawCallback * >( node + kNodeCallback ) = s_patches[ i ].original;
				}
			}
			s_patchCount = 0;
			s_divModel = nullptr;
		}

		template< typename Fn >
		void ForEachNode( Fn &&fn )
		{
			uint8_t *const *tablePtr = reinterpret_cast< uint8_t *const * >( kLayerTablePtr );
			if( !Readable( tablePtr, 4 ) || !Readable( *tablePtr, kLayerCount * kLayerStride ) )
			{
				return;
			}
			uint8_t *table = *tablePtr;
			for( unsigned layer = kFirstWorldLayer; layer <= kLastWorldLayer; ++layer )
			{
				uint8_t *node = *reinterpret_cast< uint8_t ** >( table + layer * kLayerStride );
				for( unsigned n = 0; node && n < kMaxNodesPerLayer; ++n )
				{
					if( !Readable( node, kNodeCallback + 4 ) )
					{
						break;
					}
					fn( layer, node );
					node = *reinterpret_cast< uint8_t ** >( node + kNodeNext );
				}
			}
		}

		struct FoundNode
		{
			uint8_t *node;
			unsigned layer;
		};

		// Walks the pass-A layers and collects nodes drawing `player`. Reads are guarded
		// by SEH instead of VirtualQuery per node; returns false if a read faulted.
		bool FindPlayerNodes( void *player, FoundNode *out, unsigned maxOut, unsigned &count )
		{
			count = 0;
			__try
			{
				uint8_t *table = *reinterpret_cast< uint8_t *const * >( kLayerTablePtr );
				if( !table )
				{
					return true;
				}
				for( unsigned layer = kFirstWorldLayer; layer <= kLastWorldLayer; ++layer )
				{
					uint8_t *node = *reinterpret_cast< uint8_t ** >( table + layer * kLayerStride );
					for( unsigned n = 0; node && n < kMaxNodesPerLayer; ++n )
					{
						if( *reinterpret_cast< void ** >( node + kNodeObject ) == player && count < maxOut )
						{
							out[ count++ ] = { node, layer };
						}
						node = *reinterpret_cast< uint8_t ** >( node + kNodeNext );
					}
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				count = 0;
				return false;
			}
			return true;
		}

		// Patched nodes still draw `player` (and our thunk is still in place).
		bool PatchesIntact( void *player )
		{
			__try
			{
				for( unsigned i = 0; i < s_patchCount; ++i )
				{
					uint8_t *node = s_patches[ i ].node;
					if( *reinterpret_cast< void ** >( node + kNodeObject ) != player ||
						*reinterpret_cast< DrawCallback * >( node + kNodeCallback ) != &LocalPlayerDrawThunk )
					{
						return false;
					}
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				return false;
			}
			return s_patchCount > 0;
		}

		// One-time dump when no node holds the player object directly.
		void LogDiscovery( void *player )
		{
			if( s_loggedDiscovery )
			{
				return;
			}
			s_loggedDiscovery = true;
			PSU_LOG( "VRPlayerDraw: no draw node holds the player object %p; dumping pass-A layers\n", player );
			unsigned lines = 0;
			ForEachNode( [ & ]( unsigned layer, uint8_t *node )
			{
				if( lines >= 200 )
				{
					return;
				}
				void *object = *reinterpret_cast< void ** >( node + kNodeObject );
				int ref = -1;
				if( Readable( object, 0x400 ) )
				{
					for( unsigned off = 0; off < 0x400; off += 4 )
					{
						if( *reinterpret_cast< void ** >( static_cast< uint8_t * >( object ) + off ) == player )
						{
							ref = static_cast< int >( off );
							break;
						}
					}
				}
				++lines;
				PSU_LOG( "VRPlayerDraw:   layer=0x%02X node=%p object=%p callback=%p%s\n",
					layer, node, object, reinterpret_cast< void * >( NodeCallback( node ) ),
					ref >= 0 ? " <- holds player" : "" );
				if( ref >= 0 )
				{
					PSU_LOG( "VRPlayerDraw:     player pointer at object+0x%X\n", ref );
				}
			} );
		}
	}

	namespace VRPlayerDraw
	{
		void Update( bool firstPerson, bool thirdPersonBody )
		{
			++s_frame;
			// First person "full" needs no head cut; third-person body IK needs the model whatever fpBody says.
			if( !thirdPersonBody && ( !firstPerson || Config::Get().vr.m_fpBody == "full" ) )
			{
				RestoreAll();
				return;
			}
			const PSUPlayer &player = PSUPlayer::Get();
			if( !PSUPlayer::IsReady() )
			{
				RestoreAll();
				return;
			}
			void *playerObject = const_cast< PSUPlayer * >( &player );
			if( playerObject != s_playerObject )
			{
				// New area or character: old nodes may be gone, so drop them without writing.
				s_patchCount = 0;
				s_divModel = nullptr;
			s_divModel = nullptr;
				s_playerObject = playerObject;
				s_nextScanFrame = s_frame;
			}

			// Cheap path: the patched nodes still draw the player.
			const bool intact = PatchesIntact( playerObject );
			if( intact && s_frame < s_nextScanFrame )
			{
				return;
			}
			if( !intact && s_patchCount == 0 && s_frame < s_nextScanFrame )
			{
				return;
			}

			FoundNode found[ kMaxPatches ];
			unsigned foundCount = 0;
			FindPlayerNodes( playerObject, found, kMaxPatches, foundCount );
			s_nextScanFrame = s_frame + ( foundCount > 0 ? kRescanFrames : kRetryScanFrames );

			// Keep only patches whose node is still in the list.
			unsigned kept = 0;
			for( unsigned i = 0; i < s_patchCount; ++i )
			{
				for( unsigned f = 0; f < foundCount; ++f )
				{
					if( found[ f ].node == s_patches[ i ].node )
					{
						s_patches[ kept++ ] = s_patches[ i ];
						break;
					}
				}
			}
			s_patchCount = kept;

			for( unsigned f = 0; f < foundCount; ++f )
			{
				uint8_t *node = found[ f ].node;
				const DrawCallback callback = NodeCallback( node );
				if( callback == &LocalPlayerDrawThunk || !callback )
				{
					continue;
				}
				unsigned slot = s_patchCount;
				for( unsigned i = 0; i < s_patchCount; ++i )
				{
					if( s_patches[ i ].node == node )
					{
						slot = i;
						break;
					}
				}
				if( slot >= kMaxPatches )
				{
					continue;
				}
				if( slot == s_patchCount )
				{
					++s_patchCount;
				}
				s_patches[ slot ] = { node, callback };
				*reinterpret_cast< DrawCallback * >( node + kNodeCallback ) = &LocalPlayerDrawThunk;
				if( !s_loggedPatch )
				{
					s_loggedPatch = true;
					PSU_LOG( "VRPlayerDraw: patched local player draw (layer=0x%02X node=%p object=%p callback=%p)\n",
						found[ f ].layer, node, playerObject, reinterpret_cast< void * >( callback ) );
					// The callback calls the node object's vtable +0x0C (update) and +0x10 (draw):
					// log them and the MSVC RTTI class name (vtable[-1] = complete object locator).
					void *drawObject = *reinterpret_cast< void ** >( node + kNodeObject );
					if( Readable( drawObject, 4 ) )
					{
						void **vtable = *static_cast< void *** >( drawObject );
						if( Readable( vtable - 1, 0x20 ) )
						{
							const char *className = "?";
							const uint8_t *locator = static_cast< const uint8_t * >( vtable[ -1 ] );
							if( Readable( locator, 0x14 ) )
							{
								const uint8_t *typeDescriptor = *reinterpret_cast< const uint8_t * const * >( locator + 0x0C );
								if( Readable( typeDescriptor, 0x20 ) )
								{
									className = reinterpret_cast< const char * >( typeDescriptor + 8 );
								}
							}
							PSU_LOG( "VRPlayerDraw: draw object %p vtable=%p [0]=%p [1]=%p [2]=%p [3]=%p [4]=%p [5]=%p class=%.60s\n",
								drawObject, vtable, vtable[ 0 ], vtable[ 1 ], vtable[ 2 ], vtable[ 3 ], vtable[ 4 ], vtable[ 5 ], className );
							// Hands RE: the render object at +0x160 holds the models at +0x04 / +0x20 / +0x24
							// (bone matrix by index at vtable +0x18, skeleton at vtable +0x28).
							const auto logObject = [ & ]( const char *label, const void *object )
							{
								if( !Readable( object, 4 ) )
								{
									return;
								}
								void *const *vt = *static_cast< void *const *const * >( object );
								if( !Readable( vt - 1, 0x40 ) )
								{
									return;
								}
								const char *name = "?";
								const uint8_t *col = static_cast< const uint8_t * >( vt[ -1 ] );
								if( Readable( col, 0x14 ) )
								{
									const uint8_t *td = *reinterpret_cast< const uint8_t * const * >( col + 0x0C );
									if( Readable( td, 0x20 ) )
									{
										name = reinterpret_cast< const char * >( td + 8 );
									}
								}
								PSU_LOG( "VRPlayerDraw:   %s %p vtable=%p [4]=%p [6]=%p [10]=%p class=%.60s\n",
									label, object, vt, vt[ 4 ], vt[ 6 ], vt[ 10 ], name );
							};
							const uint8_t *renderObject = *reinterpret_cast< const uint8_t * const * >(
								static_cast< const uint8_t * >( drawObject ) + 0x160 );
							if( Readable( renderObject, 0x30 ) )
							{
								logObject( "render+0x04", *reinterpret_cast< void * const * >( renderObject + 0x04 ) );
								logObject( "render+0x20", *reinterpret_cast< void * const * >( renderObject + 0x20 ) );
								logObject( "render+0x24", *reinterpret_cast< void * const * >( renderObject + 0x24 ) );
							}
						}
					}
				}
			}
			if( foundCount == 0 )
			{
				LogDiscovery( playerObject );
			}
		}

		bool DrawingLocalPlayer()
		{
			return s_drawingLocalPlayer;
		}

		const void *PlayerModel()
		{
			return s_divModel;
		}

		unsigned Frame()
		{
			return s_frame;
		}

		namespace
		{
			constexpr uint16_t kNodeSkipBits = 0x223;
			uint16_t *s_forcedFlags = nullptr;
			uint16_t s_savedFlags = 0;
		}

		void BeginEyePasses()
		{
			s_forcedFlags = nullptr;
			if( VRCamera::CameraState() != 4 || !Config::Get().vr.m_handsFollow || s_patchCount == 0 || !VRCamera::FirstPersonActive() )
			{
				return;
			}
			uint8_t *object = *reinterpret_cast< uint8_t ** >( s_patches[ 0 ].node + kNodeObject );
			if( !Readable( object, 0x10 ) )
			{
				return;
			}
			uint16_t *flags = reinterpret_cast< uint16_t * >( object + 8 );
#ifdef _DEBUG
			static DWORD s_lastLog = 0;
			if( GetTickCount() - s_lastLog >= 2000 )
			{
				s_lastLog = GetTickCount();
				PSU_LOG( "VRPlayerDraw: free aim node flags +8=%04X (skip bits %04X)\n", *flags, *flags & kNodeSkipBits );
			}
#endif
			if( ( *flags & kNodeSkipBits ) == 0 )
			{
				return;
			}
			s_savedFlags = *flags;
			*flags = static_cast< uint16_t >( *flags & ~kNodeSkipBits );
			s_forcedFlags = flags;
		}

		void EndEyePasses()
		{
			if( !s_forcedFlags )
			{
				return;
			}
			__try
			{
				*s_forcedFlags = static_cast< uint16_t >( ( *s_forcedFlags & ~kNodeSkipBits ) | ( s_savedFlags & kNodeSkipBits ) );
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
			}
			s_forcedFlags = nullptr;
		}

		bool IgnoreNearHide()
		{
			return s_forcingVisible && Config::Get().vr.m_freeAimNoNearHide;
		}

		bool IgnorePartCull()
		{
			return s_forcingVisible && Config::Get().vr.m_freeAimNoPartCull;
		}

		void OnBonePose( float *out, const uint8_t *skeleton )
		{
			static unsigned s_poses = 0, s_noModel = 0;
			static DWORD s_lastLog = 0;
			++s_poses;
			if( GetTickCount() - s_lastLog >= 3000 )
			{
				s_lastLog = GetTickCount();
#ifdef _DEBUG
				PSU_LOG( "VRPlayerDraw: %u bone poses, %u without a player model (model %p)\n", s_poses, s_noModel, s_divModel );
#endif
			}
			const uint8_t *div = s_divModel;
			if( !out || !skeleton || !div )
			{
				++s_noModel;
				return;
			}
			bool player = false;
			__try
			{
				player = *reinterpret_cast< float * const * >( div + 0x64 ) == out;
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				s_divModel = nullptr;
			}
			if( player )
			{
				VRHands::Apply( const_cast< uint8_t * >( div ), out, skeleton );
			}
		}
	}

	LocalPlayerDrawScope::LocalPlayerDrawScope( IDirect3DDevice9 *device )
	{
		if( !device || !s_drawingLocalPlayer || !StereoRenderer::InEyePass() || !VRCamera::FirstPersonActive() )
		{
			return;
		}
		const auto &vr = Config::Get().vr;
		if( vr.m_fpBody == "hidden" )
		{
			m_skip = true;
			return;
		}
		if( ( vr.m_fpBody != "headless" && vr.m_fpBody != "hands" ) || VRHands::HeadHidden() )
		{
			return;
		}
		// Fixed-function clip planes live in the space after WORLD. WORLD here already
		// holds the game camera, which first person keeps level at eye height, so
		// "y below the neck" is a plane in that space: -y + neckY >= 0.
		DWORD enable = 0;
		device->GetRenderState( D3DRS_CLIPPLANEENABLE, &enable );
		device->GetClipPlane( 0, m_savedPlane );
		m_savedClipEnable = enable;
		const float neckY = -vr.m_fpNeckDrop * VRCamera::kGameUnitsPerMeter;
		const float plane[ 4 ] = { 0.0f, -1.0f, 0.0f, neckY };
		device->SetClipPlane( 0, plane );
		device->SetRenderState( D3DRS_CLIPPLANEENABLE, enable | D3DCLIPPLANE0 );
		m_device = device;
	}

	LocalPlayerDrawScope::~LocalPlayerDrawScope()
	{
		if( !m_device )
		{
			return;
		}
		m_device->SetClipPlane( 0, m_savedPlane );
		m_device->SetRenderState( D3DRS_CLIPPLANEENABLE, m_savedClipEnable );
	}
}
