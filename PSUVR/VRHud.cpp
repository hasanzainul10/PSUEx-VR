#include "VRHud.h"

#include "D3D11Bridge.h"
#include "StereoRenderer.h"
#include "VRCamera.h"
#include "VRSyntheticPad.h"
#include "VRSystem.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PSUVR
{
	namespace
	{
		unsigned FvfStride( DWORD fvf )
		{
			unsigned size = 0;
			switch( fvf & D3DFVF_POSITION_MASK )
			{
				case D3DFVF_XYZ: size += 12; break;
				case D3DFVF_XYZRHW: size += 16; break;
				case D3DFVF_XYZB1: size += 16; break;
				case D3DFVF_XYZB2: size += 20; break;
				case D3DFVF_XYZB3: size += 24; break;
				case D3DFVF_XYZB4: size += 28; break;
				case D3DFVF_XYZB5: size += 32; break;
				default: size += 16; break;
			}
			if( fvf & D3DFVF_NORMAL ) size += 12;
			if( fvf & D3DFVF_PSIZE ) size += 4;
			if( fvf & D3DFVF_DIFFUSE ) size += 4;
			if( fvf & D3DFVF_SPECULAR ) size += 4;
			const unsigned texCount = ( fvf & D3DFVF_TEXCOUNT_MASK ) >> D3DFVF_TEXCOUNT_SHIFT;
			size += texCount * 8;
			return size ? size : 32;
		}

		constexpr unsigned kMaxUiXformVerts = 12288;
		constexpr unsigned kMaxUiXformStride = 64;
		alignas( 16 ) unsigned char s_uiXformBuf[ kMaxUiXformVerts * kMaxUiXformStride ]{};

		bool ComputeAabb( const void *verts, unsigned vertCount, unsigned stride,
			float &minX, float &minY, float &maxX, float &maxY )
		{
			if( !verts || vertCount < 1 || stride < 8 )
			{
				return false;
			}
			minX = 1.0e30f;
			minY = 1.0e30f;
			maxX = -1.0e30f;
			maxY = -1.0e30f;
			const auto *bytes = static_cast< const unsigned char * >( verts );
			for( unsigned i = 0; i < vertCount; ++i )
			{
				const float *xy = reinterpret_cast< const float * >( bytes + i * stride );
				minX = ( std::min )( minX, xy[ 0 ] );
				maxX = ( std::max )( maxX, xy[ 0 ] );
				minY = ( std::min )( minY, xy[ 1 ] );
				maxY = ( std::max )( maxY, xy[ 1 ] );
			}
			return true;
		}

		unsigned VertCountForPrimitive( unsigned primitiveType, unsigned primitiveCount )
		{
			switch( static_cast< D3DPRIMITIVETYPE >( primitiveType ) )
			{
				case D3DPT_TRIANGLELIST: return primitiveCount * 3;
				case D3DPT_TRIANGLESTRIP:
				case D3DPT_TRIANGLEFAN: return primitiveCount + 2;
				case D3DPT_LINELIST: return primitiveCount * 2;
				case D3DPT_LINESTRIP: return primitiveCount + 1;
				case D3DPT_POINTLIST: return primitiveCount;
				default: return 0;
			}
		}

		unsigned IndexCountForPrimitive( unsigned primitiveType, unsigned primitiveCount )
		{
			return VertCountForPrimitive( primitiveType, primitiveCount );
		}

		// Near-fullscreen XYZRHW (flat BB / RT blit) must never land on HUD quads over stereo.
		bool IsNearFullscreenDraw( const void *verts, unsigned vertCount, unsigned stride,
			unsigned gameW, unsigned gameH )
		{
			if( !verts || vertCount < 3 || stride < 8 || gameW < 1 || gameH < 1 )
			{
				return false;
			}
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			if( !ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY ) )
			{
				return false;
			}
			const float gw = static_cast< float >( gameW );
			const float gh = static_cast< float >( gameH );
			const float boxW = maxX - minX;
			const float boxH = maxY - minY;
			if( boxW <= 0.0f || boxH <= 0.0f )
			{
				return false;
			}
			const float areaFrac = ( boxW * boxH ) / ( gw * gh );
			if( areaFrac >= 0.45f )
			{
				return true;
			}
			// Span almost the full width and height even if area is a bit lower.
			return boxW >= gw * 0.90f && boxH >= gh * 0.85f;
		}

		// A menu overlay wider than the screen (mission menu scanline band, 2026-10-19 screenshot): the
		// 2D sprite draw (0x00747248), alpha blended, a plain texture, overhanging both side edges.
		// Drawn on the panel in order, not dropped as a background or placed in the world.
		bool IsMenuOverlay( IDirect3DDevice9 *device, const void *verts, unsigned vertCount, unsigned stride,
			unsigned gameW, unsigned gameH, void *ret )
		{
			if( !Config::Get().vr.m_hudOverlays || reinterpret_cast< uintptr_t >( ret ) != 0x00747248 || !verts ||
				vertCount < 3 || stride < 16 )
			{
				return false;
			}
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			if( !ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY ) )
			{
				return false;
			}
			const float rhw = static_cast< const float * >( verts )[ 3 ];
			( void )gameH;
			if( std::fabs( rhw - 1.0f ) > 0.001f || minX >= 0.0f || maxX <= static_cast< float >( gameW ) )
			{
				return false;
			}
			DWORD blend = 0;
			device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
			if( !blend )
			{
				return false;
			}
			bool plain = false;
			IDirect3DBaseTexture9 *texture = nullptr;
			if( SUCCEEDED( device->GetTexture( 0, &texture ) ) && texture )
			{
				IDirect3DTexture9 *tex2d = nullptr;
				texture->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex2d ) );
				texture->Release();
				if( tex2d )
				{
					D3DSURFACE_DESC desc{};
					tex2d->GetLevelDesc( 0, &desc );
					plain = ( desc.Usage & D3DUSAGE_RENDERTARGET ) == 0;
					tex2d->Release();
				}
			}
			return plain;
		}

		// Screen-space 2D UI: every vertex at rhw 1 and one shared z (nothing came through a 3D camera).
		bool IsFlatScreenSpace( const void *verts, unsigned vertCount, unsigned stride )
		{
			if( !verts || vertCount < 1 || stride < 16 )
			{
				return false;
			}
			const auto *bytes = static_cast< const unsigned char * >( verts );
			const float z0 = reinterpret_cast< const float * >( bytes )[ 2 ];
			for( unsigned i = 0; i < vertCount; ++i )
			{
				const float *v = reinterpret_cast< const float * >( bytes + i * stride );
				if( std::fabs( v[ 3 ] - 1.0f ) > 1.0e-3f || std::fabs( v[ 2 ] - z0 ) > 1.0e-4f )
				{
					return false;
				}
			}
			return true;
		}

		// The HUD RT is game-sized, so the game's viewport and scissor map 1:1. Fall back
		// to the full RT if they don't fit (e.g. an eye-sized viewport left bound).
		void ApplyGameClip( IDirect3DDevice9 *device, const D3DVIEWPORT9 &gameVp, const RECT &gameScissor,
			unsigned w, unsigned h )
		{
			D3DVIEWPORT9 vp = gameVp;
			if( vp.Width < 1 || vp.Height < 1 || vp.X + vp.Width > w || vp.Y + vp.Height > h )
			{
				vp = { 0, 0, w, h, 0.0f, 1.0f };
			}
			device->SetViewport( &vp );
			RECT scissor = gameScissor;
			scissor.left = ( std::max )( scissor.left, 0L );
			scissor.top = ( std::max )( scissor.top, 0L );
			scissor.right = ( std::min )( scissor.right, static_cast< LONG >( w ) );
			scissor.bottom = ( std::min )( scissor.bottom, static_cast< LONG >( h ) );
			if( scissor.right <= scissor.left || scissor.bottom <= scissor.top )
			{
				scissor = { 0, 0, static_cast< LONG >( w ), static_cast< LONG >( h ) };
			}
			device->SetScissorRect( &scissor );
		}

		// Transformed (non-RHW) 2D draws: project through WORLD*VIEW*PROJ and the viewport.
		// The mission map's background is a huge plane that only the 3D world's depth hides
		// in the flat frame; in VR that depth lives in the eye buffers, so it would cover
		// the visor. True when the projection covers most of the screen.
		bool IsProjectedNearFullscreen( IDirect3DDevice9 *device, const void *verts, unsigned vertCount,
			unsigned stride, unsigned gameW, unsigned gameH )
		{
			if( !device || !verts || vertCount < 3 || stride < 12 )
			{
				return false;
			}
			D3DMATRIX world{}, view{}, proj{};
			device->GetTransform( D3DTS_WORLD, &world );
			device->GetTransform( D3DTS_VIEW, &view );
			device->GetTransform( D3DTS_PROJECTION, &proj );
			D3DVIEWPORT9 vp{};
			device->GetViewport( &vp );
			auto mul = []( const float in[ 4 ], const D3DMATRIX &m, float out[ 4 ] )
			{
				for( int c = 0; c < 4; ++c )
				{
					out[ c ] = in[ 0 ] * m.m[ 0 ][ c ] + in[ 1 ] * m.m[ 1 ][ c ] + in[ 2 ] * m.m[ 2 ][ c ] + in[ 3 ] * m.m[ 3 ][ c ];
				}
			};
			float minX = 1.0e30f, minY = 1.0e30f, maxX = -1.0e30f, maxY = -1.0e30f;
			const auto *bytes = static_cast< const unsigned char * >( verts );
			for( unsigned i = 0; i < vertCount; ++i )
			{
				const float *p = reinterpret_cast< const float * >( bytes + i * stride );
				const float v[ 4 ] = { p[ 0 ], p[ 1 ], p[ 2 ], 1.0f };
				float a[ 4 ], b[ 4 ], c[ 4 ];
				mul( v, world, a );
				mul( a, view, b );
				mul( b, proj, c );
				if( c[ 3 ] <= 1.0e-6f )
				{
					// Behind the projection plane: a plane this large spans the screen.
					return true;
				}
				const float sx = vp.X + ( c[ 0 ] / c[ 3 ] * 0.5f + 0.5f ) * vp.Width;
				const float sy = vp.Y + ( 0.5f - c[ 1 ] / c[ 3 ] * 0.5f ) * vp.Height;
				minX = ( std::min )( minX, sx );
				maxX = ( std::max )( maxX, sx );
				minY = ( std::min )( minY, sy );
				maxY = ( std::max )( maxY, sy );
			}
			const float gw = static_cast< float >( gameW );
			const float gh = static_cast< float >( gameH );
			minX = ( std::max )( minX, 0.0f );
			minY = ( std::max )( minY, 0.0f );
			maxX = ( std::min )( maxX, gw );
			maxY = ( std::min )( maxY, gh );
			if( maxX <= minX || maxY <= minY )
			{
				return false;
			}
			return ( ( maxX - minX ) * ( maxY - minY ) ) / ( gw * gh ) >= 0.45f;
		}

		// Map backdrop: near-black vertex colours of transformed 2D draws get their alpha
		// scaled so the backdrop doesn't block the world behind the visor. Returns the
		// copy to draw, or nullptr when nothing changed.
		const void *FadeBlackVertices( const void *verts, unsigned vertCount, unsigned stride,
			DWORD fvf, float alphaScale )
		{
			static alignas( 16 ) unsigned char s_fadeBuf[ kMaxUiXformVerts * kMaxUiXformStride ]{};
			if( !( fvf & D3DFVF_DIFFUSE ) || alphaScale >= 1.0f || !verts ||
				vertCount > kMaxUiXformVerts || stride > kMaxUiXformStride )
			{
				return nullptr;
			}
			unsigned colorOffset = 12;  // XYZ
			if( fvf & D3DFVF_NORMAL ) colorOffset += 12;
			if( fvf & D3DFVF_PSIZE ) colorOffset += 4;
			if( colorOffset + 4 > stride )
			{
				return nullptr;
			}
			constexpr unsigned kBlackLevel = 0x40;
			const float scale = alphaScale < 0.0f ? 0.0f : alphaScale;
			std::memcpy( s_fadeBuf, verts, vertCount * stride );
			bool changed = false;
			for( unsigned i = 0; i < vertCount; ++i )
			{
				auto *color = reinterpret_cast< DWORD * >( s_fadeBuf + i * stride + colorOffset );
				const DWORD c = *color;
				const unsigned r = ( c >> 16 ) & 0xFF, g = ( c >> 8 ) & 0xFF, b = c & 0xFF;
				if( r <= kBlackLevel && g <= kBlackLevel && b <= kBlackLevel )
				{
					const DWORD a = static_cast< DWORD >( static_cast< float >( c >> 24 ) * scale );
					*color = ( c & 0x00FFFFFF ) | ( a << 24 );
					changed = true;
				}
			}
			return changed ? s_fadeBuf : nullptr;
		}

		// Diagnostics for dark panels: each distinct large visor draw (caller + rounded box)
		// once, with FVF, first vertex colour and bound texture.
		void LogLargeDraw( IDirect3DDevice9 *device, void *ret, const char *route, const void *verts,
			unsigned vertCount, unsigned stride, DWORD fvf, unsigned gameW, unsigned gameH )
		{
			constexpr unsigned kMaxLargeLogs = 48;
			struct Seen { void *ret; int x0, y0, x1, y1; };
			static Seen s_seen[ kMaxLargeLogs ]{};
			static unsigned s_count = 0;
			if( s_count >= kMaxLargeLogs || !verts || ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
			{
				return;
			}
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			if( !ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY ) )
			{
				return;
			}
			const float area = ( maxX - minX ) * ( maxY - minY ) /
				( static_cast< float >( gameW ) * static_cast< float >( gameH ) );
			if( area < 0.03f )
			{
				return;
			}
			const Seen key{ ret, static_cast< int >( minX ) / 8, static_cast< int >( minY ) / 8,
				static_cast< int >( maxX ) / 8, static_cast< int >( maxY ) / 8 };
			for( unsigned i = 0; i < s_count; ++i )
			{
				const Seen &k = s_seen[ i ];
				if( k.ret == key.ret && k.x0 == key.x0 && k.y0 == key.y0 && k.x1 == key.x1 && k.y1 == key.y1 )
				{
					return;
				}
			}
			s_seen[ s_count++ ] = key;
			DWORD color = 0;
			unsigned colorOffset = 16;
			if( fvf & D3DFVF_NORMAL ) colorOffset += 12;
			if( fvf & D3DFVF_PSIZE ) colorOffset += 4;
			const bool haveColor = ( fvf & D3DFVF_DIFFUSE ) && colorOffset + 4 <= stride;
			if( haveColor )
			{
				color = *reinterpret_cast< const DWORD * >( static_cast< const unsigned char * >( verts ) + colorOffset );
			}
			IDirect3DBaseTexture9 *texture = nullptr;
			device->GetTexture( 0, &texture );
			DWORD texWidth = 0, texHeight = 0;
			if( texture )
			{
				if( texture->GetType() == D3DRTYPE_TEXTURE )
				{
					D3DSURFACE_DESC desc{};
					static_cast< IDirect3DTexture9 * >( texture )->GetLevelDesc( 0, &desc );
					texWidth = desc.Width;
					texHeight = desc.Height;
				}
				texture->Release();
			}
			DWORD blend = 0, srcBlend = 0, dstBlend = 0;
			device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
			device->GetRenderState( D3DRS_SRCBLEND, &srcBlend );
			device->GetRenderState( D3DRS_DESTBLEND, &dstBlend );
			PSU_LOG( "VRHud large: ret=%p %s box=(%.0f,%.0f)-(%.0f,%.0f) area=%.2f fvf=0x%lX color=%s0x%08lX tex=%lux%lu blend=%lu %lu/%lu\n",
				ret, route, minX, minY, maxX, maxY, area, fvf, haveColor ? "" : "n/a ", color,
				texWidth, texHeight, blend, srcBlend, dstBlend );
		}

		// Additive draws (DESTBLEND = ONE: glows, flares) add light in the flat frame. On the
		// transparent HUD RT they would also add alpha and turn into dark opaque panels.
		// Keep their colour but leave the RT alpha alone; the visor layer is premultiplied,
		// so colour with zero alpha still adds over the world like in the flat game.
		struct AdditiveAlphaGuard
		{
			IDirect3DDevice9 *device = nullptr;
			DWORD separate = 0, srcAlpha = 0, dstAlpha = 0;

			explicit AdditiveAlphaGuard( IDirect3DDevice9 *dev )
			{
				DWORD blend = 0, dstBlend = 0;
				dev->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
				dev->GetRenderState( D3DRS_DESTBLEND, &dstBlend );
				if( !blend || dstBlend != D3DBLEND_ONE )
				{
					return;
				}
				device = dev;
				dev->GetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, &separate );
				dev->GetRenderState( D3DRS_SRCBLENDALPHA, &srcAlpha );
				dev->GetRenderState( D3DRS_DESTBLENDALPHA, &dstAlpha );
				dev->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, TRUE );
				dev->SetRenderState( D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO );
				dev->SetRenderState( D3DRS_DESTBLENDALPHA, D3DBLEND_ONE );
			}

			~AdditiveAlphaGuard()
			{
				if( !device )
				{
					return;
				}
				device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, separate );
				device->SetRenderState( D3DRS_SRCBLENDALPHA, srcAlpha );
				device->SetRenderState( D3DRS_DESTBLENDALPHA, dstAlpha );
			}
		};

		// View depth (game units) of a pre-transformed vertex from its z, through the
		// game PROJ (row vectors, w = z_view * _34): z = ( zv*_33 + _43 ) / ( zv*_34 ).
		float DepthFromZ( float z, const D3DMATRIX &proj )
		{
			const float denom = z * proj._34 - proj._33;
			if( std::fabs( denom ) < 1.0e-6f || std::fabs( proj._34 ) < 1.0e-6f )
			{
				return 1.0f / ( std::max )( 1.0e-6f, 1.0f - z );  // near = 1, infinite far
			}
			const float zView = proj._43 / denom;
			return std::fabs( zView * proj._34 );
		}

		// One line per (layer, route) so new world-anchored layers can be spotted.
		void LogLayerRoute( int layer, const char *route, const void *verts, unsigned vertCount, unsigned stride )
		{
			constexpr unsigned kMaxLayerLogs = 64;
			struct Seen { int layer; const char *route; };
			static Seen s_seen[ kMaxLayerLogs ]{};
			static unsigned s_count = 0;
			if( s_count >= kMaxLayerLogs )
			{
				return;
			}
			for( unsigned i = 0; i < s_count; ++i )
			{
				if( s_seen[ i ].layer == layer && s_seen[ i ].route == route )
				{
					return;
				}
			}
			s_seen[ s_count++ ] = { layer, route };
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY );
			const float z = stride >= 16 ? static_cast< const float * >( verts )[ 2 ] : 0.0f;
			const float depth = z > 0.0f && z < 1.0f ? 1.0f / ( 1.0f - z ) : 0.0f;
			PSU_LOG( "VRHud layer=0x%02X %s box=(%.0f,%.0f)-(%.0f,%.0f) z=%.5f depth~%.1f n=%u\n",
				layer, route, minX, minY, maxX, maxY, z, depth, vertCount );
		}

		// Once per caller: render target size and depth/stencil state of a transformed 2D draw.
		void LogDrawState( IDirect3DDevice9 *device, void *ret )
		{
			constexpr unsigned kMaxStateLogs = 8;
			static void *s_seen[ kMaxStateLogs ]{};
			static unsigned s_count = 0;
			for( unsigned i = 0; i < s_count; ++i )
			{
				if( s_seen[ i ] == ret )
				{
					return;
				}
			}
			if( s_count >= kMaxStateLogs )
			{
				return;
			}
			s_seen[ s_count++ ] = ret;
			D3DSURFACE_DESC rtDesc{};
			D3DSURFACE_DESC dsDesc{};
			IDirect3DSurface9 *rt = nullptr;
			IDirect3DSurface9 *ds = nullptr;
			if( SUCCEEDED( device->GetRenderTarget( 0, &rt ) ) && rt )
			{
				rt->GetDesc( &rtDesc );
				rt->Release();
			}
			if( SUCCEEDED( device->GetDepthStencilSurface( &ds ) ) && ds )
			{
				ds->GetDesc( &dsDesc );
				ds->Release();
			}
			DWORD zEnable = 0, zFunc = 0, stencil = 0, stencilFunc = 0, stencilRef = 0, colorWrite = 0, blend = 0;
			device->GetRenderState( D3DRS_ZENABLE, &zEnable );
			device->GetRenderState( D3DRS_ZFUNC, &zFunc );
			device->GetRenderState( D3DRS_STENCILENABLE, &stencil );
			device->GetRenderState( D3DRS_STENCILFUNC, &stencilFunc );
			device->GetRenderState( D3DRS_STENCILREF, &stencilRef );
			device->GetRenderState( D3DRS_COLORWRITEENABLE, &colorWrite );
			device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
			PSU_LOG( "VRHud route:   rt=%ux%u ds=%ux%u z=%lu zfunc=%lu stencil=%lu sfunc=%lu sref=%lu colorwrite=0x%lX blend=%lu\n",
				rtDesc.Width, rtDesc.Height, dsDesc.Width, dsDesc.Height,
				zEnable, zFunc, stencil, stencilFunc, stencilRef, colorWrite, blend );
		}

		bool IsMapLayer( int layer )
		{
			return layer == 0x31 || layer == 0x32;
		}

#ifdef _DEBUG
		// Mission map diagnostics: every pass-B draw in layers 0x31 / 0x32, by (layer, kind, route,
		// caller, FVF). The first time a key is seen its draw state is logged; counts every 5 s.
		void NoteMapDraw( IDirect3DDevice9 *device, int layer, const char *kind, const char *route,
			unsigned primitiveCount, const void *verts, unsigned stride, void *ret )
		{
			if( !IsMapLayer( layer ) || !device )
			{
				return;
			}
			struct Entry { int layer; const char *kind; const char *route; void *ret; DWORD fvf; unsigned draws; unsigned prims; };
			constexpr unsigned kMaxEntries = 48;
			static Entry s_entries[ kMaxEntries ]{};
			static unsigned s_count = 0;
			static DWORD s_lastFlush = 0;
			static unsigned s_flushes = 0;
			DWORD fvf = 0;
			device->GetFVF( &fvf );
			Entry *entry = nullptr;
			for( unsigned i = 0; i < s_count; ++i )
			{
				Entry &e = s_entries[ i ];
				if( e.layer == layer && e.kind == kind && e.route == route && e.ret == ret && e.fvf == fvf )
				{
					entry = &e;
					break;
				}
			}
			if( !entry && s_count < kMaxEntries )
			{
				entry = &s_entries[ s_count++ ];
				*entry = { layer, kind, route, ret, fvf, 0, 0 };
				IDirect3DSurface9 *rt = nullptr;
				D3DSURFACE_DESC rtDesc{};
				if( SUCCEEDED( device->GetRenderTarget( 0, &rt ) ) && rt )
				{
					rt->GetDesc( &rtDesc );
					rt->Release();
				}
				IDirect3DVertexShader9 *vs = nullptr;
				device->GetVertexShader( &vs );
				if( vs )
				{
					vs->Release();
				}
				IDirect3DVertexDeclaration9 *decl = nullptr;
				device->GetVertexDeclaration( &decl );
				if( decl )
				{
					decl->Release();
				}
				unsigned texW = 0, texH = 0, texUsage = 0;
				IDirect3DBaseTexture9 *texture = nullptr;
				if( SUCCEEDED( device->GetTexture( 0, &texture ) ) && texture )
				{
					IDirect3DTexture9 *tex2d = nullptr;
					texture->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex2d ) );
					texture->Release();
					if( tex2d )
					{
						D3DSURFACE_DESC desc{};
						tex2d->GetLevelDesc( 0, &desc );
						texW = desc.Width;
						texH = desc.Height;
						texUsage = desc.Usage;
						tex2d->Release();
					}
				}
				DWORD blend = 0, src = 0, dst = 0, colorWrite = 0, zEnable = 0, zFunc = 0, zWrite = 0, stencil = 0, cull = 0;
				device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
				device->GetRenderState( D3DRS_SRCBLEND, &src );
				device->GetRenderState( D3DRS_DESTBLEND, &dst );
				device->GetRenderState( D3DRS_COLORWRITEENABLE, &colorWrite );
				device->GetRenderState( D3DRS_ZENABLE, &zEnable );
				device->GetRenderState( D3DRS_ZFUNC, &zFunc );
				device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
				device->GetRenderState( D3DRS_STENCILENABLE, &stencil );
				device->GetRenderState( D3DRS_CULLMODE, &cull );
				float x = 0, y = 0, z = 0;
				if( verts && stride >= 12 )
				{
					const float *v = static_cast< const float * >( verts );
					x = v[ 0 ];
					y = v[ 1 ];
					z = v[ 2 ];
				}
				PSU_LOG( "VRMap new: layer=0x%02X %s %s ret=%p fvf=0x%lX vs=%d decl=%d rt=%ux%u tex=%ux%u%s prims=%u "
					"blend=%lu src=%lu dst=%lu cw=0x%lX z=%lu zf=%lu zw=%lu stencil=%lu cull=%lu v0=(%.2f,%.2f,%.2f)\n",
					layer, kind, route, ret, fvf, vs ? 1 : 0, decl ? 1 : 0, rtDesc.Width, rtDesc.Height, texW, texH,
					( texUsage & D3DUSAGE_RENDERTARGET ) ? "(rt)" : "", primitiveCount, blend, src, dst, colorWrite,
					zEnable, zFunc, zWrite, stencil, cull, x, y, z );
			}
			if( entry )
			{
				++entry->draws;
				entry->prims += primitiveCount;
			}
			const DWORD now = GetTickCount();
			if( now - s_lastFlush >= 5000 && s_flushes < 40 )
			{
				s_lastFlush = now;
				bool any = false;
				for( unsigned i = 0; i < s_count; ++i )
				{
					Entry &e = s_entries[ i ];
					if( e.draws )
					{
						if( !any )
						{
							++s_flushes;
							any = true;
						}
						PSU_LOG( "VRMap 5 s: layer=0x%02X %s %s ret=%p fvf=0x%lX draws=%u prims=%u\n",
							e.layer, e.kind, e.route, e.ret, e.fvf, e.draws, e.prims );
						e.draws = 0;
						e.prims = 0;
					}
				}
			}
		}
		// Mission map geometry, one sample frame every 5 s: the screen box of every map-layer draw
		// (2D as drawn, 3D projected through WORLD * VIEW * PROJ and the viewport), to fit the 3D map
		// into its 2D frame.
		struct MapGeoEntry { int layer; const char *kind; void *ret; void *tex; bool projected; float minX, minY, maxX, maxY; unsigned draws; };
		constexpr unsigned kMaxMapGeo = 32;
		MapGeoEntry s_mapGeo[ kMaxMapGeo ]{};
		unsigned s_mapGeoCount = 0;
		bool s_mapGeoSampling = false;
		DWORD s_mapGeoLast = 0;
		unsigned s_mapGeoSamples = 0;
		bool s_mapGeoMatricesLogged = false;

		bool MapGeoSampling()
		{
			return s_mapGeoSampling;
		}

		// Called once per stereo frame: print the last sample, maybe start a new one.
		void MapGeoFrame()
		{
			if( s_mapGeoSampling )
			{
				s_mapGeoSampling = false;
				for( unsigned i = 0; i < s_mapGeoCount; ++i )
				{
					const MapGeoEntry &e = s_mapGeo[ i ];
					PSU_LOG( "VRMap geo: layer=0x%02X %s ret=%p tex=%p %s box=(%.0f,%.0f)-(%.0f,%.0f) draws=%u\n", e.layer, e.kind,
						e.ret, e.tex, e.projected ? "projected" : "screen", e.minX, e.minY, e.maxX, e.maxY, e.draws );
				}
				s_mapGeoCount = 0;
			}
			const DWORD now = GetTickCount();
			if( s_mapGeoSamples < 12 && now - s_mapGeoLast >= 5000 )
			{
				s_mapGeoLast = now;
				s_mapGeoSampling = true;
			}
		}

		void NoteMapGeometry( IDirect3DDevice9 *device, int layer, const char *kind, void *ret, const unsigned char *verts,
			unsigned vertCount, unsigned stride, bool transformed )
		{
			// Screen-space draws from any pass-B layer (the map frame may sit outside 0x31 / 0x32).
			if( !s_mapGeoSampling || layer < 0 || ( transformed && !IsMapLayer( layer ) ) || !device || !verts ||
				vertCount < 1 || stride < 12 )
			{
				return;
			}
			float minX = 1.0e30f, minY = 1.0e30f, maxX = -1.0e30f, maxY = -1.0e30f;
			if( transformed )
			{
				D3DMATRIX world{}, view{}, proj{};
				device->GetTransform( D3DTS_WORLD, &world );
				device->GetTransform( D3DTS_VIEW, &view );
				device->GetTransform( D3DTS_PROJECTION, &proj );
				D3DVIEWPORT9 vp{};
				device->GetViewport( &vp );
				if( !s_mapGeoMatricesLogged && std::strcmp( kind, "DIP" ) == 0 )
				{
					s_mapGeoMatricesLogged = true;
					const D3DMATRIX *ms[] = { &world, &view, &proj };
					const char *names[] = { "world", "view", "proj" };
					for( int m = 0; m < 3; ++m )
					{
						const D3DMATRIX &x = *ms[ m ];
						PSU_LOG( "VRMap %s: [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]\n",
							names[ m ], x._11, x._12, x._13, x._14, x._21, x._22, x._23, x._24, x._31, x._32, x._33, x._34,
							x._41, x._42, x._43, x._44 );
					}
					PSU_LOG( "VRMap viewport: (%lu,%lu %lux%lu) z=%.2f-%.2f\n", vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ );
				}
				auto mul = []( const float in[ 4 ], const D3DMATRIX &m, float out[ 4 ] )
				{
					for( int c = 0; c < 4; ++c )
					{
						out[ c ] = in[ 0 ] * m.m[ 0 ][ c ] + in[ 1 ] * m.m[ 1 ][ c ] + in[ 2 ] * m.m[ 2 ][ c ] + in[ 3 ] * m.m[ 3 ][ c ];
					}
				};
				for( unsigned i = 0; i < vertCount; ++i )
				{
					const float *p = reinterpret_cast< const float * >( verts + i * stride );
					const float v[ 4 ] = { p[ 0 ], p[ 1 ], p[ 2 ], 1.0f };
					float a[ 4 ], b[ 4 ], c[ 4 ];
					mul( v, world, a );
					mul( a, view, b );
					mul( b, proj, c );
					if( c[ 3 ] <= 1.0e-6f )
					{
						continue;
					}
					const float sx = vp.X + ( c[ 0 ] / c[ 3 ] * 0.5f + 0.5f ) * vp.Width;
					const float sy = vp.Y + ( 0.5f - c[ 1 ] / c[ 3 ] * 0.5f ) * vp.Height;
					minX = ( std::min )( minX, sx );
					maxX = ( std::max )( maxX, sx );
					minY = ( std::min )( minY, sy );
					maxY = ( std::max )( maxY, sy );
				}
			}
			else
			{
				ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY );
			}
			if( minX > maxX )
			{
				return;
			}
			D3DVIEWPORT9 gameVp{};
			device->GetViewport( &gameVp );
			const float gw = static_cast< float >( gameVp.Width ), gh = static_cast< float >( gameVp.Height );
			// Skip the huge backdrop plane, and 2D outside the top-right quarter (the minimap).
			if( transformed ? ( maxX - minX > gw * 2.0f ) :
				( ( minX + maxX ) * 0.5f < gw * 0.5f || ( minY + maxY ) * 0.5f > gh * 0.6f ||
					( maxX - minX ) * ( maxY - minY ) > gw * gh * 0.3f ) )
			{
				return;
			}
			IDirect3DBaseTexture9 *tex = nullptr;
			device->GetTexture( 0, &tex );
			if( tex )
			{
				tex->Release();
			}
			for( unsigned i = 0; i < s_mapGeoCount; ++i )
			{
				MapGeoEntry &e = s_mapGeo[ i ];
				if( e.layer == layer && e.kind == kind && e.ret == ret && e.tex == tex && e.projected == transformed )
				{
					e.minX = ( std::min )( e.minX, minX );
					e.minY = ( std::min )( e.minY, minY );
					e.maxX = ( std::max )( e.maxX, maxX );
					e.maxY = ( std::max )( e.maxY, maxY );
					++e.draws;
					return;
				}
			}
			if( s_mapGeoCount < kMaxMapGeo )
			{
				s_mapGeo[ s_mapGeoCount++ ] = { layer, kind, ret, tex, transformed, minX, minY, maxX, maxY, 1 };
				if( s_mapGeoCount == 1 )
				{
					++s_mapGeoSamples;
				}
			}
		}

		// The map DIP's vertices, read back from its vertex buffer for the geometry sample.
		void NoteMapGeometryDip( IDirect3DDevice9 *device, int layer, void *ret, int baseVertexIndex, unsigned minIndex,
			unsigned numVertices )
		{
			if( !s_mapGeoSampling || !IsMapLayer( layer ) || !device || numVertices < 1 )
			{
				return;
			}
			IDirect3DVertexBuffer9 *vb = nullptr;
			UINT offset = 0, vbStride = 0;
			if( FAILED( device->GetStreamSource( 0, &vb, &offset, &vbStride ) ) || !vb )
			{
				return;
			}
			void *data = nullptr;
			if( vbStride >= 12 && SUCCEEDED( vb->Lock( 0, 0, &data, D3DLOCK_READONLY ) ) && data )
			{
				D3DVERTEXBUFFER_DESC desc{};
				vb->GetDesc( &desc );
				const unsigned first = static_cast< unsigned >( baseVertexIndex + static_cast< int >( minIndex ) );
				const unsigned long end = offset + static_cast< unsigned long >( first + numVertices ) * vbStride;
				if( end <= desc.Size )
				{
					NoteMapGeometry( device, layer, "DIP", ret,
						static_cast< const unsigned char * >( data ) + offset + first * vbStride, numVertices, vbStride, true );
				}
				vb->Unlock();
			}
			else
			{
				static bool s_loggedFail = false;
				if( !s_loggedFail )
				{
					s_loggedFail = true;
					PSU_LOG( "VRMap geo: map VB lock failed (stride=%u)\n", vbStride );
				}
			}
			vb->Release();
		}
#else
		inline void NoteMapDraw( IDirect3DDevice9 *, int, const char *, const char *, unsigned, const void *, unsigned, void * ) {}
#endif

		// One line per unique (caller, route), so psuex.log shows where each UI piece went.
		// Free aim: the game's reticle (replaced by ours at the controller ray). Pieces on draw
		// layer 48 with a 256 x 256 texture around the screen centre: four 192 x 192 corners
		// (at 2560 x 1440), a ring ~45% of the width and four tick marks.
		bool IsGameReticle( IDirect3DDevice9 *device, const void *verts, unsigned vertCount, unsigned stride, unsigned width,
			unsigned height )
		{
			if( !device || !verts || vertCount == 0 || stride < 16 )
			{
				return false;
			}
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY );
			const float gw = static_cast< float >( width );
			const float gh = static_cast< float >( height );
			const float cx = ( minX + maxX ) * 0.5f;
			const float cy = ( minY + maxY ) * 0.5f;
			const float w = maxX - minX;
			const float h = maxY - minY;
			const bool squareSprite = w > gw * 0.02f && w < gw * 0.15f && h > w * 0.7f && h < w * 1.4f;
			if( squareSprite && std::fabs( cx - gw * 0.5f ) < gw * 0.2f && std::fabs( cy - gh * 0.5f ) < gh * 0.25f )
			{
				return true;
			}
			constexpr int kReticleLayer = 48;
			if( VRHud::PassBLayer() != kReticleLayer )
			{
				return false;
			}
			// Around the centre (ring, corners, ticks), or a thin bar on one of the screen's
			// centre lines (the four lines reach out to the edges).
			const bool central = w < gw * 0.6f && std::fabs( cx - gw * 0.5f ) < gw * 0.3f && std::fabs( cy - gh * 0.5f ) < gh * 0.35f;
			const bool crossLine = ( std::fabs( cx - gw * 0.5f ) < gw * 0.02f && w < gw * 0.03f ) ||
				( std::fabs( cy - gh * 0.5f ) < gh * 0.03f && h < gh * 0.04f );
			if( !central && !crossLine )
			{
				return false;
			}
			bool reticle = false;
			IDirect3DBaseTexture9 *texture = nullptr;
			if( SUCCEEDED( device->GetTexture( 0, &texture ) ) && texture )
			{
				IDirect3DTexture9 *tex2d = nullptr;
				texture->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex2d ) );
				texture->Release();
				if( tex2d )
				{
					D3DSURFACE_DESC desc{};
					tex2d->GetLevelDesc( 0, &desc );
					reticle = desc.Width == 256 && desc.Height == 256;
					tex2d->Release();
				}
			}
			return reticle;
		}

		void LogRoute( void *ret, const char *route, const void *verts, unsigned vertCount, unsigned stride,
			const D3DVIEWPORT9 *viewport = nullptr )
		{
			constexpr unsigned kMaxRouteLogs = 64;
			struct Seen { void *ret; const char *route; };
			static Seen s_seen[ kMaxRouteLogs ]{};
			static unsigned s_seenCount = 0;
			if( s_seenCount >= kMaxRouteLogs )
			{
				return;
			}
			for( unsigned i = 0; i < s_seenCount; ++i )
			{
				if( s_seen[ i ].ret == ret && s_seen[ i ].route == route )
				{
					return;
				}
			}
			s_seen[ s_seenCount++ ] = { ret, route };
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY );
			float z = 0.0f, rhw = 0.0f;
			if( verts && stride >= 16 )
			{
				const float *v = static_cast< const float * >( verts );
				z = v[ 2 ];
				rhw = v[ 3 ];
			}
			PSU_LOG( "VRHud route: ret=%p %s box=(%.0f,%.0f)-(%.0f,%.0f) z=%.4f rhw=%.4f n=%u stride=%u\n",
				ret, route, minX, minY, maxX, maxY, z, rhw, vertCount, stride );
			if( viewport )
			{
				PSU_LOG( "VRHud route:   viewport=(%lu,%lu %lux%lu)\n",
					viewport->X, viewport->Y, viewport->Width, viewport->Height );
			}
		}

		// Expand indexed XYZRHW into a triangle-list UP buffer in s_uiXformBuf.
		// Returns expanded vert count, or 0 on failure. Sets outPrimCount for D3DPT_TRIANGLELIST.
		unsigned ExpandIndexedToUp( const unsigned char *vb, unsigned vbStride, int baseVertexIndex,
			const void *ib, bool ib32, unsigned startIndex, unsigned primitiveType,
			unsigned primitiveCount, unsigned &outPrimCount )
		{
			outPrimCount = 0;
			const unsigned indexCount = IndexCountForPrimitive( primitiveType, primitiveCount );
			if( indexCount < 1 || vbStride < 8 || vbStride > kMaxUiXformStride )
			{
				return 0;
			}

			auto readIndex = [ & ]( unsigned i ) -> unsigned
			{
				if( ib32 )
				{
					return static_cast< const unsigned * >( ib )[ startIndex + i ];
				}
				return static_cast< const unsigned short * >( ib )[ startIndex + i ];
			};

			auto copyVert = [ & ]( unsigned outI, unsigned srcIndex ) -> bool
			{
				if( outI >= kMaxUiXformVerts )
				{
					static bool s_loggedCap = false;
					if( !s_loggedCap )
					{
						s_loggedCap = true;
						PSU_LOG( "VR: HUD expand cap hit (%u verts), font batch dropped\n", kMaxUiXformVerts );
					}
					return false;
				}
				const int src = static_cast< int >( srcIndex ) + baseVertexIndex;
				if( src < 0 )
				{
					return false;
				}
				std::memcpy( s_uiXformBuf + outI * vbStride,
					vb + static_cast< unsigned >( src ) * vbStride, vbStride );
				return true;
			};

			unsigned outVerts = 0;
			const auto type = static_cast< D3DPRIMITIVETYPE >( primitiveType );
			if( type == D3DPT_TRIANGLELIST )
			{
				if( ( indexCount % 3 ) != 0 )
				{
					return 0;
				}
				for( unsigned i = 0; i < indexCount; ++i )
				{
					if( !copyVert( outVerts++, readIndex( i ) ) )
					{
						return 0;
					}
				}
				outPrimCount = primitiveCount;
				return outVerts;
			}
			if( type == D3DPT_TRIANGLESTRIP )
			{
				if( indexCount < 3 )
				{
					return 0;
				}
				for( unsigned i = 0; i + 2 < indexCount; ++i )
				{
					const unsigned i0 = readIndex( i );
					const unsigned i1 = readIndex( i + 1 );
					const unsigned i2 = readIndex( i + 2 );
					if( ( i & 1u ) == 0 )
					{
						if( !copyVert( outVerts++, i0 ) || !copyVert( outVerts++, i1 ) ||
							!copyVert( outVerts++, i2 ) )
						{
							return 0;
						}
					}
					else
					{
						if( !copyVert( outVerts++, i1 ) || !copyVert( outVerts++, i0 ) ||
							!copyVert( outVerts++, i2 ) )
						{
							return 0;
						}
					}
					++outPrimCount;
				}
				return outVerts;
			}
			return 0;
		}
	}

	int VRHud::s_passBLayer = -1;

	void VRHud::SetPassBLayer( int layer )
	{
		s_passBLayer = layer;
	}

	VRHud &VRHud::Get()
	{
		static VRHud instance;
		return instance;
	}

	bool VRHud::EnsureTargets( IDirect3DDevice9 *device, unsigned w, unsigned h )
	{
		D3D11Bridge *bridge = System::Get().StereoBridge();
		if( !bridge || !device )
		{
			return false;
		}
		if( !bridge->EnsureHudTargets( device, w, h ) )
		{
			return false;
		}
		m_w = w;
		m_h = h;
		return true;
	}

	void VRHud::BeginStereoFrame( IDirect3DDevice9 *device, unsigned gameW, unsigned gameH )
	{
#ifdef _DEBUG
		MapGeoFrame();
#endif
		m_active = false;
		m_hasUser = false;
		m_hasWorld = false;
		m_haveMapFrame = false;
		m_userAabbCount = 0;
		m_worldAabbCount = 0;
		if( !device || gameW < 1 || gameH < 1 || !Config::Get().vr.m_enabled || !Config::Get().vr.m_stereo )
		{
			return;
		}
		if( !EnsureTargets( device, gameW, gameH ) )
		{
			return;
		}
		D3D11Bridge *bridge = System::Get().StereoBridge();
		IDirect3DSurface9 *user = bridge->HudUserSurface();
		IDirect3DSurface9 *world = bridge->HudWorldSurface();
		if( !user || !world )
		{
			return;
		}

		IDirect3DSurface9 *savedRt = nullptr;
		IDirect3DSurface9 *savedDs = nullptr;
		device->GetRenderTarget( 0, &savedRt );
		device->GetDepthStencilSurface( &savedDs );

		device->SetDepthStencilSurface( nullptr );
		device->SetRenderTarget( 0, user );
		device->Clear( 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB( 0, 0, 0, 0 ), 1.0f, 0 );
		device->SetRenderTarget( 0, world );
		device->Clear( 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB( 0, 0, 0, 0 ), 1.0f, 0 );

		device->SetRenderTarget( 0, savedRt );
		device->SetDepthStencilSurface( savedDs );
		if( savedRt )
		{
			savedRt->Release();
		}
		if( savedDs )
		{
			savedDs->Release();
		}

		m_active = true;
		if( !m_logged )
		{
			m_logged = true;
			PSU_LOG( "VR: HUD quad capture active (%ux%u user+world)\n", gameW, gameH );
		}
	}

	ID3D11Texture2D *VRHud::UserTexture() const
	{
		D3D11Bridge *bridge = System::Get().StereoBridge();
		return bridge ? bridge->HudUserTexture() : nullptr;
	}

	ID3D11Texture2D *VRHud::WorldTexture() const
	{
		D3D11Bridge *bridge = System::Get().StereoBridge();
		return bridge ? bridge->HudWorldTexture() : nullptr;
	}

	bool VRHud::AlreadyCapturedUserAabb( float minX, float minY, float maxX, float maxY ) const
	{
		constexpr float kTol = 3.0f;
		for( unsigned i = 0; i < m_userAabbCount; ++i )
		{
			const Aabb &a = m_userAabbs[ i ];
			if( std::fabs( a.minX - minX ) <= kTol && std::fabs( a.minY - minY ) <= kTol &&
				std::fabs( a.maxX - maxX ) <= kTol && std::fabs( a.maxY - maxY ) <= kTol )
			{
				return true;
			}
		}
		return false;
	}

	void VRHud::RememberUserAabb( float minX, float minY, float maxX, float maxY )
	{
		if( m_userAabbCount >= kMaxLayerAabbs )
		{
			return;
		}
		m_userAabbs[ m_userAabbCount++ ] = { minX, minY, maxX, maxY };
	}

	void VRHud::RememberWorldAabb( float minX, float minY, float maxX, float maxY )
	{
		if( m_worldAabbCount >= kMaxLayerAabbs )
		{
			return;
		}
		m_worldAabbs[ m_worldAabbCount++ ] = { minX, minY, maxX, maxY };
	}

	bool VRHud::TargetsGameFrame( IDirect3DDevice9 *device ) const
	{
		// Render-to-texture (a different-sized target) is not part of the visible frame.
		IDirect3DSurface9 *rt = nullptr;
		if( FAILED( device->GetRenderTarget( 0, &rt ) ) || !rt )
		{
			return false;
		}
		D3DSURFACE_DESC desc{};
		rt->GetDesc( &desc );
		rt->Release();
		return desc.Width == m_w && desc.Height == m_h;
	}

	bool VRHud::NearExistingUserAabb( float cx, float cy, float pad ) const
	{
		for( unsigned i = 0; i < m_userAabbCount; ++i )
		{
			const Aabb &a = m_userAabbs[ i ];
			if( cx >= a.minX - pad && cx <= a.maxX + pad &&
				cy >= a.minY - pad && cy <= a.maxY + pad )
			{
				return true;
			}
		}
		return false;
	}

	bool VRHud::NearExistingWorldAabb( float cx, float cy, float pad ) const
	{
		for( unsigned i = 0; i < m_worldAabbCount; ++i )
		{
			const Aabb &a = m_worldAabbs[ i ];
			if( cx >= a.minX - pad && cx <= a.maxX + pad &&
				cy >= a.minY - pad && cy <= a.maxY + pad )
			{
				return true;
			}
		}
		return false;
	}

	bool VRHud::IsWorldUiWidget( const void *verts, int vertexCount, unsigned stride,
		unsigned gameW, unsigned gameH )
	{
		if( !verts || vertexCount < 3 || stride < 8 || gameW < 1 || gameH < 1 )
		{
			return false;
		}
		float minX = 0, minY = 0, maxX = 0, maxY = 0;
		if( !ComputeAabb( verts, static_cast< unsigned >( vertexCount ), stride, minX, minY, maxX, maxY ) )
		{
			return false;
		}
		const float gw = static_cast< float >( gameW );
		const float gh = static_cast< float >( gameH );
		const float boxW = maxX - minX;
		const float boxH = maxY - minY;
		const float areaFrac = ( boxW * boxH ) / ( gw * gh );
		if( areaFrac >= 0.35f )
		{
			return true;
		}
		const float cx = ( minX + maxX ) * 0.5f;
		const float cy = ( minY + maxY ) * 0.5f;
		const bool centered = ( cx > gw * 0.25f && cx < gw * 0.75f && cy > gh * 0.25f && cy < gh * 0.75f );
		return centered && areaFrac >= 0.15f;
	}

	bool VRHud::IsAvatarAnchoredPassB( const void *verts, unsigned primitiveType,
		unsigned primitiveCount, unsigned stride, unsigned gameW, unsigned gameH )
	{
		if( !verts || stride < 8 || gameW < 1 || gameH < 1 )
		{
			return false;
		}
		const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
		// Menu windows are flat 2D; re-projecting them per eye skews them.
		if( IsFlatScreenSpace( verts, vertCount, stride ) )
		{
			return false;
		}
		return IsAvatarRegion( verts, vertCount, stride, gameW, gameH );
	}

	bool VRHud::IsAvatarRegion( const void *verts, unsigned vertCount, unsigned stride,
		unsigned gameW, unsigned gameH )
	{
		float minX = 0, minY = 0, maxX = 0, maxY = 0;
		if( !ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY ) )
		{
			return false;
		}
		const float gw = static_cast< float >( gameW );
		const float gh = static_cast< float >( gameH );
		const float boxW = maxX - minX;
		const float boxH = maxY - minY;
		if( boxW <= 0.0f || boxH <= 0.0f )
		{
			return false;
		}
		const float areaFrac = ( boxW * boxH ) / ( gw * gh );
		if( areaFrac >= 0.28f )
		{
			return false;
		}
		const float cx = ( minX + maxX ) * 0.5f;
		const float cy = ( minY + maxY ) * 0.5f;
		if( cx < gw * 0.2f || cx > gw * 0.8f )
		{
			return false;
		}
		return cy > gh * 0.12f && cy < gh * 0.65f;
	}

	bool VRHud::IsVisorChrome( float minX, float minY, float maxX, float maxY,
		unsigned gameW, unsigned gameH ) const
	{
		const float gw = static_cast< float >( gameW );
		const float gh = static_cast< float >( gameH );
		const float boxW = maxX - minX;
		const float boxH = maxY - minY;
		if( boxW <= 0.0f || boxH <= 0.0f )
		{
			return false;
		}
		const float areaFrac = ( boxW * boxH ) / ( gw * gh );
		if( areaFrac >= 0.22f )
		{
			return false;
		}
		const float cx = ( minX + maxX ) * 0.5f;
		const float cy = ( minY + maxY ) * 0.5f;

		const float marginX = gw * 0.12f;
		const float marginY = gh * 0.12f;
		if( minX < marginX || maxX > gw - marginX || minY < marginY || maxY > gh - marginY )
		{
			return true;
		}

		// Wide enough for name above HP / lobby title above buttons (not only the very bottom strip).
		const bool bottomLeft = ( cx < gw * 0.42f && cy > gh * 0.55f );
		const bool bottomRight = ( cx > gw * 0.58f && cy > gh * 0.55f );
		const bool topLeft = ( cx < gw * 0.48f && cy < gh * 0.28f );
		const bool topRight = ( cx > gw * 0.52f && cy < gh * 0.38f );
		if( bottomLeft || bottomRight || topLeft || topRight )
		{
			return true;
		}

		const float pad = ( std::min )( gw, gh ) * 0.12f;
		return NearExistingUserAabb( cx, cy, pad );
	}

	bool VRHud::ClassifyWorldLayer( const void *verts, unsigned vertCount, unsigned stride ) const
	{
		// Default: one visor layer keeps the game's exact 2D layout (no split windows).
		if( !Config::Get().vr.m_hudWorldPanel )
		{
			return false;
		}
		if( !verts || vertCount < 1 || stride < 8 || m_w < 1 || m_h < 1 )
		{
			return true;
		}
		if( IsWorldUiWidget( verts, static_cast< int >( vertCount ), stride, m_w, m_h ) )
		{
			return true;
		}
		float minX = 0, minY = 0, maxX = 0, maxY = 0;
		if( !ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY ) )
		{
			return true;
		}
		const float gw = static_cast< float >( m_w );
		const float gh = static_cast< float >( m_h );
		const float cx = ( minX + maxX ) * 0.5f;
		const float cy = ( minY + maxY ) * 0.5f;
		const float pad = ( std::min )( gw, gh ) * 0.12f;
		// Sticky world first so menu labels / titles follow the kiosk, not the visor.
		if( NearExistingWorldAabb( cx, cy, pad ) )
		{
			return true;
		}
		if( IsVisorChrome( minX, minY, maxX, maxY, m_w, m_h ) )
		{
			return false;
		}
		return true;
	}

	void VRHud::ReplayOnto( IDirect3DDevice9 *device, bool worldLayer,
		void *thisPtr, int a4, int vertexCount, void *vertexPtr, int drawType,
		int( __thiscall *drawFn )( void *, int, int, void *, int ) )
	{
		D3D11Bridge *bridge = System::Get().StereoBridge();
		IDirect3DSurface9 *target = worldLayer ? bridge->HudWorldSurface() : bridge->HudUserSurface();
		if( !target || !drawFn )
		{
			return;
		}

		DWORD fvf = 0;
		device->GetFVF( &fvf );
		const unsigned stride = FvfStride( fvf );
		float minX = 0, minY = 0, maxX = 0, maxY = 0;
		const bool haveAabb = vertexPtr && vertexCount > 0 &&
			ComputeAabb( vertexPtr, static_cast< unsigned >( vertexCount ), stride, minX, minY, maxX, maxY );
		if( !worldLayer && haveAabb && AlreadyCapturedUserAabb( minX, minY, maxX, maxY ) )
		{
			return;
		}

		IDirect3DSurface9 *savedRt = nullptr;
		IDirect3DSurface9 *savedDs = nullptr;
		D3DVIEWPORT9 savedVp{};
		RECT savedScissor{};
		DWORD zEnable = 0, zWrite = 0, scissorEnable = 0;
		device->GetRenderTarget( 0, &savedRt );
		device->GetDepthStencilSurface( &savedDs );
		device->GetViewport( &savedVp );
		device->GetScissorRect( &savedScissor );
		device->GetRenderState( D3DRS_ZENABLE, &zEnable );
		device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
		device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissorEnable );

		m_capturing = true;
		device->SetRenderState( D3DRS_ZENABLE, FALSE );
		device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
		const AdditiveAlphaGuard additiveGuard( device );
		device->SetRenderTarget( 0, target );
		device->SetDepthStencilSurface( nullptr );
		// SetRenderTarget reset the viewport; put the game's back so clipped 2D
		// (the mission map's background plane) stays inside its rectangle.
		ApplyGameClip( device, savedVp, savedScissor, m_w, m_h );
		drawFn( thisPtr, a4, vertexCount, vertexPtr, drawType );
		m_capturing = false;

		device->SetRenderTarget( 0, savedRt );
		device->SetDepthStencilSurface( savedDs );
		device->SetViewport( &savedVp );
		device->SetScissorRect( &savedScissor );
		device->SetRenderState( D3DRS_SCISSORTESTENABLE, scissorEnable );
		device->SetRenderState( D3DRS_ZENABLE, zEnable );
		device->SetRenderState( D3DRS_ZWRITEENABLE, zWrite );
		device->SetFVF( fvf );
		if( savedRt )
		{
			savedRt->Release();
		}
		if( savedDs )
		{
			savedDs->Release();
		}

		if( worldLayer )
		{
			m_hasWorld = true;
			if( haveAabb )
			{
				RememberWorldAabb( minX, minY, maxX, maxY );
			}
		}
		else
		{
			m_hasUser = true;
			if( haveAabb )
			{
				RememberUserAabb( minX, minY, maxX, maxY );
			}
		}
	}

	void VRHud::ReplayPassBOnto( IDirect3DDevice9 *device, bool worldLayer, unsigned primitiveType,
		unsigned primitiveCount, const void *verts, unsigned stride, bool rememberAabb )
	{
		D3D11Bridge *bridge = System::Get().StereoBridge();
		IDirect3DSurface9 *target = bridge
			? ( worldLayer ? bridge->HudWorldSurface() : bridge->HudUserSurface() )
			: nullptr;
		if( !target || !verts )
		{
			return;
		}

		const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
		float minX = 0, minY = 0, maxX = 0, maxY = 0;
		const bool haveAabb = ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY );

		IDirect3DSurface9 *savedRt = nullptr;
		IDirect3DSurface9 *savedDs = nullptr;
		D3DVIEWPORT9 savedVp{};
		RECT savedScissor{};
		DWORD zEnable = 0, zWrite = 0, scissorEnable = 0;
		device->GetRenderTarget( 0, &savedRt );
		device->GetDepthStencilSurface( &savedDs );
		device->GetViewport( &savedVp );
		device->GetScissorRect( &savedScissor );
		device->GetRenderState( D3DRS_ZENABLE, &zEnable );
		device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
		device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissorEnable );
		// DP-UP resets stream 0 (and the font DIP path keeps its IB bound), so the
		// game's next DIP on the same VB would draw nothing unless we put them back.
		DWORD savedFvf = 0;
		IDirect3DVertexBuffer9 *savedVb = nullptr;
		UINT savedVbOffset = 0;
		UINT savedVbStride = 0;
		IDirect3DIndexBuffer9 *savedIb = nullptr;
		device->GetFVF( &savedFvf );
		device->GetStreamSource( 0, &savedVb, &savedVbOffset, &savedVbStride );
		device->GetIndices( &savedIb );
		DWORD stencilWriteMask = 0;
		device->GetRenderState( D3DRS_STENCILWRITEMASK, &stencilWriteMask );

		// Transformed 2D (the mission map) is masked by the game's depth/stencil:
		// test against the game's buffer, but never write it.
		const bool transformed = ( savedFvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW;
		IDirect3DSurface9 *replayDs = nullptr;
		if( savedDs )
		{
			D3DSURFACE_DESC dsDesc{};
			if( SUCCEEDED( savedDs->GetDesc( &dsDesc ) ) && dsDesc.Width == m_w && dsDesc.Height == m_h )
			{
				replayDs = savedDs;
			}
		}

		D3DMATRIX savedProj{}, fitProj{};
		const bool fit = transformed && MapFitProjection( device, s_passBLayer, fitProj );
		if( fit )
		{
			device->GetTransform( D3DTS_PROJECTION, &savedProj );
			device->SetTransform( D3DTS_PROJECTION, &fitProj );
		}

		m_capturing = true;
		device->SetRenderState( D3DRS_ZENABLE, ( transformed && replayDs ) ? zEnable : FALSE );
		device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
		const AdditiveAlphaGuard additiveGuard( device );
		device->SetRenderState( D3DRS_STENCILWRITEMASK, 0 );
		device->SetRenderTarget( 0, target );
		device->SetDepthStencilSurface( replayDs );
		// SetRenderTarget reset the viewport; put the game's back.
		ApplyGameClip( device, savedVp, savedScissor, m_w, m_h );
		device->DrawPrimitiveUP( static_cast< D3DPRIMITIVETYPE >( primitiveType ),
			primitiveCount, verts, stride );
		m_capturing = false;
		device->SetRenderState( D3DRS_STENCILWRITEMASK, stencilWriteMask );
		if( fit )
		{
			device->SetTransform( D3DTS_PROJECTION, &savedProj );
		}

		device->SetFVF( savedFvf );
		device->SetStreamSource( 0, savedVb, savedVbOffset, savedVbStride );
		device->SetIndices( savedIb );
		if( savedVb )
		{
			savedVb->Release();
		}
		if( savedIb )
		{
			savedIb->Release();
		}

		device->SetRenderTarget( 0, savedRt );
		device->SetDepthStencilSurface( savedDs );
		device->SetViewport( &savedVp );
		device->SetScissorRect( &savedScissor );
		device->SetRenderState( D3DRS_SCISSORTESTENABLE, scissorEnable );
		device->SetRenderState( D3DRS_ZENABLE, zEnable );
		device->SetRenderState( D3DRS_ZWRITEENABLE, zWrite );
		if( savedRt )
		{
			savedRt->Release();
		}
		if( savedDs )
		{
			savedDs->Release();
		}
		if( worldLayer )
		{
			m_hasWorld = true;
			if( haveAabb && rememberAabb )
			{
				RememberWorldAabb( minX, minY, maxX, maxY );
			}
		}
		else
		{
			m_hasUser = true;
			if( haveAabb && rememberAabb )
			{
				RememberUserAabb( minX, minY, maxX, maxY );
			}
		}
	}

	void VRHud::ReplayFontBatch( IDirect3DDevice9 *device, const unsigned char *verts,
		unsigned vertCount, unsigned stride, void *ret )
	{
		// One font DIP can hold every string on screen, so its batch AABB says nothing.
		// Route each glyph quad on its own and replay one draw per layer.
		static alignas( 16 ) unsigned char s_userBucket[ kMaxUiXformVerts * kMaxUiXformStride ]{};
		static alignas( 16 ) unsigned char s_worldBucket[ kMaxUiXformVerts * kMaxUiXformStride ]{};
		if( !verts || vertCount < 3 || stride > kMaxUiXformStride )
		{
			return;
		}

		// Glyph clusters (strings) for the sticky lists, so single glyphs don't flood them.
		constexpr unsigned kMaxClusters = 32;
		struct Cluster { Aabb box; bool world; };
		Cluster clusters[ kMaxClusters ];
		unsigned clusterCount = 0;
		auto addToCluster = [ & ]( bool world, float x0, float y0, float x1, float y1 )
		{
			const float gap = ( y1 - y0 ) * 1.5f;
			for( unsigned c = 0; c < clusterCount; ++c )
			{
				Aabb &b = clusters[ c ].box;
				if( clusters[ c ].world == world &&
					x0 <= b.maxX + gap && x1 >= b.minX - gap &&
					y0 <= b.maxY + gap && y1 >= b.minY - gap )
				{
					b.minX = ( std::min )( b.minX, x0 );
					b.minY = ( std::min )( b.minY, y0 );
					b.maxX = ( std::max )( b.maxX, x1 );
					b.maxY = ( std::max )( b.maxY, y1 );
					return;
				}
			}
			if( clusterCount < kMaxClusters )
			{
				clusters[ clusterCount++ ] = { { x0, y0, x1, y1 }, world };
			}
		};

		const unsigned group = ( vertCount % 6 ) == 0 ? 6u : 3u;
		unsigned userVerts = 0;
		unsigned worldVerts = 0;
		int aimHand = 0;
		const bool aiming = VRSyntheticPad::GunAiming( aimHand );
		unsigned reticleQuads = 0;
		for( unsigned i = 0; i + group <= vertCount; i += group )
		{
			const unsigned char *quad = verts + i * stride;
			if( IsNearFullscreenDraw( quad, group, stride, m_w, m_h ) )
			{
				continue;
			}
			// Free aim: the game's reticle can come batched with other quads.
			if( aiming && IsGameReticle( device, quad, group, stride, m_w, m_h ) )
			{
				++reticleQuads;
				continue;
			}
			const bool world = ClassifyWorldLayer( quad, group, stride );
			unsigned &count = world ? worldVerts : userVerts;
			unsigned char *dst = ( world ? s_worldBucket : s_userBucket ) + count * stride;
			std::memcpy( dst, quad, group * stride );
			count += group;

			float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			if( ComputeAabb( quad, group, stride, x0, y0, x1, y1 ) )
			{
				addToCluster( world, x0, y0, x1, y1 );
			}
		}

#ifdef _DEBUG
		if( reticleQuads > 0 )
		{
			static unsigned s_logs = 0;
			if( s_logs < 20 )
			{
				++s_logs;
				PSU_LOG( "VRHud aim: SKIP %u batched quads layer=%d ret=%p\n", reticleQuads, s_passBLayer, ret );
			}
		}
#endif
		if( userVerts >= 3 )
		{
			LogRoute( ret, "font-user", s_userBucket, userVerts, stride );
			DWORD fontFvf = 0;
			device->GetFVF( &fontFvf );
			LogLargeDraw( device, ret, "font-user", s_userBucket, userVerts, stride, fontFvf, m_w, m_h );
			ReplayPassBOnto( device, false, D3DPT_TRIANGLELIST, userVerts / 3, s_userBucket, stride, false );
		}
		if( worldVerts >= 3 )
		{
			LogRoute( ret, "font-world", s_worldBucket, worldVerts, stride );
			ReplayPassBOnto( device, true, D3DPT_TRIANGLELIST, worldVerts / 3, s_worldBucket, stride, false );
		}
		for( unsigned c = 0; c < clusterCount; ++c )
		{
			const Aabb &b = clusters[ c ].box;
			if( clusters[ c ].world )
			{
				RememberWorldAabb( b.minX, b.minY, b.maxX, b.maxY );
			}
			else
			{
				RememberUserAabb( b.minX, b.minY, b.maxX, b.maxY );
			}
		}
	}

	bool VRHud::BlockReticleDraw( IDirect3DDevice9 *device, const void *verts, unsigned primitiveType,
		unsigned primitiveCount, unsigned stride ) const
	{
		int aimHand = 0;
		if( !m_active || !device || !verts || !VRSyntheticPad::GunAiming( aimHand ) || m_w == 0 || m_h == 0 )
		{
			return false;
		}
		DWORD fvf = 0;
		device->GetFVF( &fvf );
		if( ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return false;
		}
		const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
		return IsGameReticle( device, verts, vertCount, stride, m_w, m_h );
	}

	void VRHud::CapturePassBDraw( IDirect3DDevice9 *device, unsigned primitiveType,
		unsigned primitiveCount, const void *verts, unsigned stride, void *ret )
	{
		if( m_capturing || !m_active || !device || !verts ||
			StereoRenderer::InEyePass() || StereoRenderer::InNameplateDraw() )
		{
			return;
		}
		auto &stereo = StereoRenderer::Get();
		if( !stereo.HasFrame() )
		{
			return;
		}
		const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
#ifdef _DEBUG
		if( MapGeoSampling() )
		{
			DWORD geoFvf = 0;
			device->GetFVF( &geoFvf );
			NoteMapGeometry( device, s_passBLayer, "DP-UP", ret, static_cast< const unsigned char * >( verts ), vertCount,
				stride, ( geoFvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW );
		}
#endif
		if( !TargetsGameFrame( device ) )
		{
			LogRoute( ret, "skip-offscreen", verts, vertCount, stride );
			NoteMapDraw( device, s_passBLayer, "DP-UP", "skip-offscreen", primitiveCount, verts, stride, ret );
			return;
		}
		if( IsNearFullscreenDraw( verts, vertCount, stride, m_w, m_h ) &&
			!IsMenuOverlay( device, verts, vertCount, stride, m_w, m_h, ret ) )
		{
			LogRoute( ret, "skip-fullscreen", verts, vertCount, stride );
			NoteMapDraw( device, s_passBLayer, "DP-UP", "skip-fullscreen", primitiveCount, verts, stride, ret );
			return;
		}
		DWORD drawFvf = 0;
		device->GetFVF( &drawFvf );
		if( ( drawFvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW &&
			IsProjectedNearFullscreen( device, verts, vertCount, stride, m_w, m_h ) )
		{
			LogRoute( ret, "skip-fullscreen-3d", verts, vertCount, stride );
			NoteMapDraw( device, s_passBLayer, "DP-UP", "skip-fullscreen-3d", primitiveCount, verts, stride, ret );
			return;
		}
		// Free aim with the gun up: the game's reticle (a square sprite near the screen centre)
		// is replaced by ours at the controller ray, so it isn't put on the panel.
		int aimHand = 0;
		if( VRSyntheticPad::GunAiming( aimHand ) )
		{
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY );
			const bool skip = IsGameReticle( device, verts, vertCount, stride, m_w, m_h );
#ifdef _DEBUG
			// Diagnostics: every HUD draw for a while after free aim starts.
			static DWORD s_lastAimTick = 0;
			static unsigned s_aimLogs = 0;
			if( GetTickCount() - s_lastAimTick > 2000 )
			{
				s_aimLogs = 0;
			}
			s_lastAimTick = GetTickCount();
			if( s_aimLogs < 150 )
			{
				++s_aimLogs;
				unsigned texW = 0, texH = 0;
				IDirect3DBaseTexture9 *texture = nullptr;
				if( SUCCEEDED( device->GetTexture( 0, &texture ) ) && texture )
				{
					IDirect3DTexture9 *tex2d = nullptr;
					texture->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex2d ) );
					texture->Release();
					if( tex2d )
					{
						D3DSURFACE_DESC desc{};
						tex2d->GetLevelDesc( 0, &desc );
						texW = desc.Width;
						texH = desc.Height;
						tex2d->Release();
					}
				}
				const float z = stride >= 16 ? static_cast< const float * >( verts )[ 2 ] : 0.0f;
				PSU_LOG( "VRHud aim: %s box=(%.0f,%.0f)-(%.0f,%.0f) z=%.4f n=%u tex=%ux%u layer=%d ret=%p\n", skip ? "SKIP" : "keep",
					minX, minY, maxX, maxY, z, vertCount, texW, texH, s_passBLayer, ret );
			}
#endif
			if( skip )
			{
				LogRoute( ret, "skip-reticle", verts, vertCount, stride );
				NoteMapDraw( device, s_passBLayer, "DP-UP", "skip-reticle", primitiveCount, verts, stride, ret );
				return;
			}
		}
		// World-anchored 2D from draw-layer objects: z holds the projected depth
		// (0.94-0.98 seen), which DrawNameplateToEyes uses to place it in 3D.
		// Exact layer from Hook_PassB (stack scans hit stale return addresses).
		const int layer = s_passBLayer;
		NoteMapFrame( device, layer, verts, vertCount, stride );
		const bool menuOverlay = IsMenuOverlay( device, verts, vertCount, stride, m_w, m_h, ret );
		// Fitted map: its 3D pieces (markers) belong in the frame, not placed by depth.
		D3DMATRIX fitProj{};
		const bool mapFit = ( drawFvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW && MapFitProjection( device, layer, fitProj );
		if( menuOverlay )
		{
			LogLayerRoute( layer, "visor-overlay", verts, vertCount, stride );
		}
		if( layer >= 0 && !menuOverlay && !mapFit )
		{
			float minX = 0, minY = 0, maxX = 0, maxY = 0;
			ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY );
			const float z = stride >= 16 ? static_cast< const float * >( verts )[ 2 ] : 0.0f;
			const bool hasDepth = z > 0.5f && z < 1.0f;
			if( !hasDepth && VRCamera::FirstPersonActive() )
			{
				// Our own nameplate has no depth and sits just above the head, which the
				// steered first-person camera always puts at screen center.
				const float cx = ( minX + maxX ) * 0.5f;
				const float cy = ( minY + maxY ) * 0.5f;
				const float gw = static_cast< float >( m_w );
				const float gh = static_cast< float >( m_h );
				if( std::fabs( cx - gw * 0.5f ) < gw * 0.15f && cy > gh * 0.1f && cy < gh * 0.55f )
				{
					LogLayerRoute( layer, "skip-own-plate", verts, vertCount, stride );
					NoteMapDraw( device, layer, "DP-UP", "skip-own-plate", primitiveCount, verts, stride, ret );
					if( m_inUiDraw )
					{
						++m_uiPassBCaptures;
					}
					return;
				}
			}
			// HUD widgets live in the same layers with a constant HUD z, which decodes to a
			// point just in front of the game camera. In first person that camera sits
			// behind the head, so such points would land behind the eyes and flip.
			// Classify by decoded depth: nearer than the head = HUD (visor), around the
			// head = ours (hidden in first person), beyond = world (eyes).
			const float depth = hasDepth ? DepthFromZ( z, stereo.GameProj() ) : 0.0f;
			const bool firstPerson = VRCamera::FirstPersonActive();
			constexpr float kOwnBand = 8.0f;        // game units around the head
			constexpr float kHudDepthThird = 12.0f; // third person: nearer = HUD
			const float headDepth = firstPerson ? VRCamera::FirstPersonCameraDistance() : 0.0f;
			if( hasDepth && firstPerson && std::fabs( depth - headDepth ) < kOwnBand )
			{
				LogLayerRoute( layer, "skip-own", verts, vertCount, stride );
				NoteMapDraw( device, layer, "DP-UP", "skip-own", primitiveCount, verts, stride, ret );
				if( m_inUiDraw )
				{
					++m_uiPassBCaptures;
				}
				return;
			}
			const float worldDepth = firstPerson ? headDepth + kOwnBand : kHudDepthThird;
			if( hasDepth && depth > worldDepth )
			{
				LogLayerRoute( layer, "eyes", verts, vertCount, stride );
				NoteMapDraw( device, layer, "DP-UP", "eyes", primitiveCount, verts, stride, ret );
				stereo.DrawNameplateToEyes( device, primitiveType, primitiveCount, verts, stride );
				if( m_inUiDraw )
				{
					++m_uiPassBCaptures;
				}
				return;
			}
			LogLayerRoute( layer, "visor", verts, vertCount, stride );
		}
		if( !mapFit && IsAvatarAnchoredPassB( verts, primitiveType, primitiveCount, stride, m_w, m_h ) )
		{
			LogRoute( ret, "eyes", verts, vertCount, stride );
			NoteMapDraw( device, s_passBLayer, "DP-UP", "eyes-avatar", primitiveCount, verts, stride, ret );
			stereo.DrawNameplateToEyes( device, primitiveType, primitiveCount, verts, stride );
			if( m_inUiDraw )
			{
				++m_uiPassBCaptures;
			}
			return;
		}
		const bool world = ClassifyWorldLayer( verts, vertCount, stride );
		DWORD fvf = 0;
		device->GetFVF( &fvf );
		if( ( fvf & D3DFVF_POSITION_MASK ) == D3DFVF_XYZRHW )
		{
			LogRoute( ret, world ? "world" : "user", verts, vertCount, stride );
			LogLargeDraw( device, ret, world ? "world" : "user", verts, vertCount, stride, fvf, m_w, m_h );
		}
		else
		{
			// Transformed 2D (mission map): log what confines it in the flat frame.
			D3DVIEWPORT9 viewport{};
			device->GetViewport( &viewport );
			LogRoute( ret, world ? "world-3d" : "user-3d", verts, vertCount, stride, &viewport );
			LogDrawState( device, ret );
			if( const void *faded = FadeBlackVertices( verts, vertCount, stride, fvf,
					Config::Get().vr.m_hudMapBlackAlpha ) )
			{
				static bool s_loggedFade = false;
				if( !s_loggedFade )
				{
					s_loggedFade = true;
					PSU_LOG( "VRHud: fading black map backdrop to %.2f alpha (ret=%p)\n",
						Config::Get().vr.m_hudMapBlackAlpha, ret );
				}
				verts = faded;
			}
		}
		NoteMapDraw( device, s_passBLayer, "DP-UP", world ? "panel-world" : "panel", primitiveCount, verts, stride, ret );
		ReplayPassBOnto( device, world, primitiveType, primitiveCount, verts, stride );
		if( m_inUiDraw )
		{
			++m_uiPassBCaptures;
		}
	}

	void VRHud::CaptureXyzrhwIndexed( IDirect3DDevice9 *device, unsigned primitiveType,
		int baseVertexIndex, unsigned minIndex, unsigned numVertices,
		unsigned startIndex, unsigned primitiveCount, void *ret )
	{
		( void )minIndex;
		( void )numVertices;
		if( m_capturing || !m_active || !device ||
			StereoRenderer::InEyePass() || StereoRenderer::InNameplateDraw() )
		{
			return;
		}
		if( !StereoRenderer::Get().HasFrame() || !TargetsGameFrame( device ) )
		{
			return;
		}

		DWORD fvf = 0;
		device->GetFVF( &fvf );
		if( ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return;
		}
		const unsigned stride = FvfStride( fvf );
		if( stride < 16 || stride > kMaxUiXformStride )
		{
			return;
		}

		IDirect3DVertexBuffer9 *vb = nullptr;
		UINT offsetBytes = 0;
		UINT vbStride = 0;
		if( FAILED( device->GetStreamSource( 0, &vb, &offsetBytes, &vbStride ) ) || !vb )
		{
			return;
		}
		if( vbStride == 0 )
		{
			vbStride = stride;
		}

		IDirect3DIndexBuffer9 *ib = nullptr;
		if( FAILED( device->GetIndices( &ib ) ) || !ib )
		{
			vb->Release();
			return;
		}

		D3DINDEXBUFFER_DESC ibDesc{};
		ib->GetDesc( &ibDesc );
		const bool ib32 = ( ibDesc.Format == D3DFMT_INDEX32 );

		// perfHudLockRanges: lock only the indices this draw reads, then only the vertices they point at.
		// The locked pointers start at the range, so the expand runs with startIndex 0 and a base
		// vertex shifted by the first locked vertex. Out-of-bounds ranges fall back to whole locks.
		const unsigned indexSize = ib32 ? 4u : 2u;
		const unsigned indexCount = IndexCountForPrimitive( primitiveType, primitiveCount );
		if( Config::Get().vr.m_perfHudLockRanges && indexCount > 0 &&
			static_cast< unsigned long long >( startIndex + indexCount ) * indexSize <= ibDesc.Size )
		{
			void *ibRange = nullptr;
			if( SUCCEEDED( ib->Lock( startIndex * indexSize, indexCount * indexSize, &ibRange, D3DLOCK_READONLY ) ) && ibRange )
			{
				unsigned lo = 0xFFFFFFFFu;
				unsigned hi = 0;
				for( unsigned i = 0; i < indexCount; ++i )
				{
					const unsigned index = ib32 ? static_cast< const unsigned * >( ibRange )[ i ]
						: static_cast< const unsigned short * >( ibRange )[ i ];
					lo = ( std::min )( lo, index );
					hi = ( std::max )( hi, index );
				}
				D3DVERTEXBUFFER_DESC vbDesc{};
				vb->GetDesc( &vbDesc );
				const long long first = static_cast< long long >( baseVertexIndex ) + lo;
				const long long begin = offsetBytes + first * vbStride;
				const long long size = static_cast< long long >( hi - lo + 1 ) * vbStride;
				void *vbRange = nullptr;
				if( first >= 0 && begin + size <= vbDesc.Size &&
					SUCCEEDED( vb->Lock( static_cast< UINT >( begin ), static_cast< UINT >( size ), &vbRange, D3DLOCK_READONLY ) ) &&
					vbRange )
				{
					unsigned outPrimCount = 0;
					const unsigned outVerts = ExpandIndexedToUp( static_cast< const unsigned char * >( vbRange ), vbStride,
						-static_cast< int >( lo ), ibRange, ib32, 0, primitiveType, primitiveCount, outPrimCount );
					vb->Unlock();
					ib->Unlock();
					ib->Release();
					vb->Release();
					if( outVerts < 3 || outPrimCount < 1 )
					{
						return;
					}
#ifdef _DEBUG
					NoteMapGeometry( device, s_passBLayer, "DIP-2D", ret, s_uiXformBuf, outVerts, vbStride, false );
#endif
					ReplayFontBatch( device, s_uiXformBuf, outVerts, vbStride, ret );
					if( m_inUiDraw )
					{
						++m_uiPassBCaptures;
					}
					return;
				}
				ib->Unlock();
			}
		}

		void *vbData = nullptr;
		void *ibData = nullptr;
		const HRESULT vbHr = vb->Lock( 0, 0, &vbData, D3DLOCK_READONLY );
		const HRESULT ibHr = ib->Lock( 0, 0, &ibData, D3DLOCK_READONLY );
		if( FAILED( vbHr ) || FAILED( ibHr ) || !vbData || !ibData )
		{
			if( SUCCEEDED( vbHr ) )
			{
				vb->Unlock();
			}
			if( SUCCEEDED( ibHr ) )
			{
				ib->Unlock();
			}
			ib->Release();
			vb->Release();
			return;
		}

		const auto *vbBytes = static_cast< const unsigned char * >( vbData ) + offsetBytes;
		unsigned outPrimCount = 0;
		const unsigned outVerts = ExpandIndexedToUp( vbBytes, vbStride, baseVertexIndex,
			ibData, ib32, startIndex, primitiveType, primitiveCount, outPrimCount );

		ib->Unlock();
		vb->Unlock();
		ib->Release();
		vb->Release();

		if( outVerts < 3 || outPrimCount < 1 )
		{
			return;
		}

#ifdef _DEBUG
		NoteMapGeometry( device, s_passBLayer, "DIP-2D", ret, s_uiXformBuf, outVerts, vbStride, false );
#endif
		ReplayFontBatch( device, s_uiXformBuf, outVerts, vbStride, ret );
		if( m_inUiDraw )
		{
			++m_uiPassBCaptures;
		}
	}

	void VRHud::CaptureXyzrhwIndexedUp( IDirect3DDevice9 *device, unsigned primitiveType,
		unsigned minVertexIndex, unsigned numVertices, unsigned primitiveCount,
		const void *indexData, unsigned indexFormat, const void *verts, unsigned stride, void *ret )
	{
		( void )minVertexIndex;
		( void )numVertices;
		if( m_capturing || !m_active || !device || !indexData || !verts ||
			StereoRenderer::InEyePass() || StereoRenderer::InNameplateDraw() )
		{
			return;
		}
		if( !StereoRenderer::Get().HasFrame() || !TargetsGameFrame( device ) )
		{
			return;
		}
		if( stride < 16 || stride > kMaxUiXformStride )
		{
			return;
		}

		DWORD fvf = 0;
		device->GetFVF( &fvf );
		if( ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return;
		}

		const bool ib32 = ( indexFormat == D3DFMT_INDEX32 );
		unsigned outPrimCount = 0;
		const unsigned outVerts = ExpandIndexedToUp(
			static_cast< const unsigned char * >( verts ), stride, 0,
			indexData, ib32, 0, primitiveType, primitiveCount, outPrimCount );
		if( outVerts < 3 || outPrimCount < 1 )
		{
			return;
		}

		ReplayFontBatch( device, s_uiXformBuf, outVerts, stride, ret );
		if( m_inUiDraw )
		{
			++m_uiPassBCaptures;
		}
	}

	void VRHud::NoteMapFrame( IDirect3DDevice9 *device, int layer, const void *verts, unsigned vertCount, unsigned stride )
	{
		// The frame: small screen-space pieces on layer 0x30 (a 128 x 128 box at the top right).
		if( layer != 0x30 || !device || !verts )
		{
			return;
		}
		DWORD fvf = 0;
		device->GetFVF( &fvf );
		float minX = 0, minY = 0, maxX = 0, maxY = 0;
		if( ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW ||
			!ComputeAabb( verts, vertCount, stride, minX, minY, maxX, maxY ) ||
			maxX - minX > 400.0f || maxY - minY > 400.0f || ( minX + maxX ) * 0.5f < m_w * 0.5f )
		{
			return;
		}
		if( !m_haveMapFrame )
		{
			m_mapFrame = { minX, minY, maxX, maxY };
			m_haveMapFrame = true;
			return;
		}
		m_mapFrame.minX = ( std::min )( m_mapFrame.minX, minX );
		m_mapFrame.minY = ( std::min )( m_mapFrame.minY, minY );
		m_mapFrame.maxX = ( std::max )( m_mapFrame.maxX, maxX );
		m_mapFrame.maxY = ( std::max )( m_mapFrame.maxY, maxY );
	}

	bool VRHud::MapFitProjection( IDirect3DDevice9 *device, int layer, D3DMATRIX &fitted ) const
	{
		const auto &vr = Config::Get().vr;
		if( !vr.m_hudMapFit || !m_haveMapFrame || !IsMapLayer( layer ) || !device || m_w < 1 || m_h < 1 )
		{
			return false;
		}
		D3DMATRIX proj{};
		device->GetTransform( D3DTS_PROJECTION, &proj );
		// The map camera is a perspective projection pushed off-centre (_31 / _32) to where the
		// game thinks the frame is; w = -z ( _34 = -1 ).
		if( std::fabs( proj._34 ) < 1.0e-4f || std::fabs( proj._44 ) > 1.0e-4f )
		{
			return false;
		}
		// NDC centre of the map now, and where the frame centre is.
		const float cx = proj._31 / proj._34;
		const float cy = proj._32 / proj._34;
		const float fx = ( m_mapFrame.minX + m_mapFrame.maxX ) / static_cast< float >( m_w ) - 1.0f;
		const float fy = 1.0f - ( m_mapFrame.minY + m_mapFrame.maxY ) / static_cast< float >( m_h );
		// The game's 2D is drawn at 640 x 480 pixel size; the 3D map scales with the screen.
		const float zoom = vr.m_hudMapFitScale > 0.05f ? vr.m_hudMapFitScale : 1.0f;
		const float sx = 640.0f / static_cast< float >( m_w ) * zoom;
		const float sy = 480.0f / static_cast< float >( m_h ) * zoom;
		// clip' = clip * M: x' = sx * x + ( fx - sx * cx ) * w, the same for y.
		D3DMATRIX m{};
		m._11 = sx;
		m._22 = sy;
		m._33 = 1.0f;
		m._44 = 1.0f;
		m._41 = fx - sx * cx;
		m._42 = fy - sy * cy;
		for( int r = 0; r < 4; ++r )
		{
			for( int c = 0; c < 4; ++c )
			{
				fitted.m[ r ][ c ] = proj.m[ r ][ 0 ] * m.m[ 0 ][ c ] + proj.m[ r ][ 1 ] * m.m[ 1 ][ c ] +
					proj.m[ r ][ 2 ] * m.m[ 2 ][ c ] + proj.m[ r ][ 3 ] * m.m[ 3 ][ c ];
			}
		}
#ifdef _DEBUG
		static unsigned s_logs = 0;
		if( s_logs < 4 )
		{
			++s_logs;
			PSU_LOG( "VRMap fit: frame=(%.0f,%.0f)-(%.0f,%.0f) centre ndc (%.3f,%.3f) -> (%.3f,%.3f) scale %.3f x %.3f\n",
				m_mapFrame.minX, m_mapFrame.minY, m_mapFrame.maxX, m_mapFrame.maxY, cx, cy, fx, fy, sx, sy );
		}
#endif
		return true;
	}

	void VRHud::CaptureMapMeshDraw( IDirect3DDevice9 *device, int kind, unsigned primitiveType,
		int baseVertexIndex, unsigned minIndex, unsigned numVertices, unsigned start,
		unsigned primitiveCount, const void *indexData, unsigned indexFormat,
		const void *verts, unsigned stride, void *ret )
	{
		const int layer = s_passBLayer;
		if( m_capturing || !m_active || !device || !IsMapLayer( layer ) ||
			StereoRenderer::InEyePass() || StereoRenderer::InNameplateDraw() || !StereoRenderer::Get().HasFrame() )
		{
			return;
		}
		static const char *const kKinds[] = { "DP", "DIP", "DIP-UP" };
		const char *kindName = kKinds[ kind < 0 || kind > 2 ? 0 : kind ];
		DWORD fvf = 0;
		device->GetFVF( &fvf );
		// Screen-space DIPs are the font path's (CaptureXyzrhwIndexed).
		if( kind == 1 && ( fvf & D3DFVF_POSITION_MASK ) == D3DFVF_XYZRHW )
		{
			NoteMapDraw( device, layer, kindName, "font-path", primitiveCount, verts, stride, ret );
			return;
		}
		if( !TargetsGameFrame( device ) )
		{
			NoteMapDraw( device, layer, kindName, "skip-offscreen", primitiveCount, verts, stride, ret );
			return;
		}
		if( !Config::Get().vr.m_hudMapMeshes )
		{
			NoteMapDraw( device, layer, kindName, "off", primitiveCount, verts, stride, ret );
			return;
		}
		D3D11Bridge *bridge = System::Get().StereoBridge();
		IDirect3DSurface9 *target = bridge ? bridge->HudUserSurface() : nullptr;
		if( !target )
		{
			return;
		}
		NoteMapDraw( device, layer, kindName, "panel", primitiveCount, verts, stride, ret );
#ifdef _DEBUG
		if( kind == 1 )
		{
			NoteMapGeometryDip( device, layer, ret, baseVertexIndex, minIndex, numVertices );
		}
		else if( kind == 2 && verts )
		{
			NoteMapGeometry( device, layer, "DIP-UP", ret, static_cast< const unsigned char * >( verts ) + minIndex * stride,
				numVertices, stride, ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW );
		}
#endif

		// Same as ReplayPassBOnto: the game's depth/stencil masks the map, so test against it
		// (the game's own draw just wrote the same depth) but never write it.
		IDirect3DSurface9 *savedRt = nullptr;
		IDirect3DSurface9 *savedDs = nullptr;
		D3DVIEWPORT9 savedVp{};
		RECT savedScissor{};
		DWORD zWrite = 0, scissorEnable = 0, stencilWriteMask = 0;
		device->GetRenderTarget( 0, &savedRt );
		device->GetDepthStencilSurface( &savedDs );
		device->GetViewport( &savedVp );
		device->GetScissorRect( &savedScissor );
		device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
		device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissorEnable );
		device->GetRenderState( D3DRS_STENCILWRITEMASK, &stencilWriteMask );
		DWORD zEnable = 0;
		device->GetRenderState( D3DRS_ZENABLE, &zEnable );
		IDirect3DSurface9 *replayDs = nullptr;
		if( savedDs )
		{
			D3DSURFACE_DESC dsDesc{};
			if( SUCCEEDED( savedDs->GetDesc( &dsDesc ) ) && dsDesc.Width == m_w && dsDesc.Height == m_h )
			{
				replayDs = savedDs;
			}
		}
		// DIP-UP leaves stream 0 and the indices unset afterwards; put back whatever was bound.
		IDirect3DVertexBuffer9 *savedVb = nullptr;
		UINT savedVbOffset = 0, savedVbStride = 0;
		IDirect3DIndexBuffer9 *savedIb = nullptr;
		device->GetStreamSource( 0, &savedVb, &savedVbOffset, &savedVbStride );
		device->GetIndices( &savedIb );

		D3DMATRIX savedProj{}, fitProj{};
		const bool fit = MapFitProjection( device, layer, fitProj );
		if( fit )
		{
			device->GetTransform( D3DTS_PROJECTION, &savedProj );
			device->SetTransform( D3DTS_PROJECTION, &fitProj );
		}
		m_capturing = true;
		{
			const AdditiveAlphaGuard additiveGuard( device );
			device->SetRenderState( D3DRS_ZENABLE, replayDs ? zEnable : FALSE );
			device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
			device->SetRenderState( D3DRS_STENCILWRITEMASK, 0 );
			device->SetRenderTarget( 0, target );
			device->SetDepthStencilSurface( replayDs );
			ApplyGameClip( device, savedVp, savedScissor, m_w, m_h );
			const auto type = static_cast< D3DPRIMITIVETYPE >( primitiveType );
			if( kind == 0 )
			{
				device->DrawPrimitive( type, start, primitiveCount );
			}
			else if( kind == 1 )
			{
				device->DrawIndexedPrimitive( type, baseVertexIndex, minIndex, numVertices, start, primitiveCount );
			}
			else if( indexData && verts )
			{
				device->DrawIndexedPrimitiveUP( type, minIndex, numVertices, primitiveCount, indexData,
					static_cast< D3DFORMAT >( indexFormat ), verts, stride );
			}
		}
		m_capturing = false;
		if( fit )
		{
			device->SetTransform( D3DTS_PROJECTION, &savedProj );
		}

		device->SetStreamSource( 0, savedVb, savedVbOffset, savedVbStride );
		device->SetIndices( savedIb );
		if( savedVb )
		{
			savedVb->Release();
		}
		if( savedIb )
		{
			savedIb->Release();
		}
		device->SetRenderTarget( 0, savedRt );
		device->SetDepthStencilSurface( savedDs );
		device->SetViewport( &savedVp );
		device->SetScissorRect( &savedScissor );
		device->SetRenderState( D3DRS_SCISSORTESTENABLE, scissorEnable );
		device->SetRenderState( D3DRS_STENCILWRITEMASK, stencilWriteMask );
		device->SetRenderState( D3DRS_ZENABLE, zEnable );
		device->SetRenderState( D3DRS_ZWRITEENABLE, zWrite );
		if( savedRt )
		{
			savedRt->Release();
		}
		if( savedDs )
		{
			savedDs->Release();
		}
		m_hasUser = true;
	}

	void VRHud::CaptureUiDraw( IDirect3DDevice9 *device,
		void *thisPtr, int a4, int vertexCount, void *vertexPtr, int drawType,
		int( __thiscall *drawFn )( void *, int, int, void *, int ) )
	{
		if( m_capturing || !m_active || !device || !drawFn || StereoRenderer::InEyePass() )
		{
			return;
		}
		if( !StereoRenderer::Get().HasFrame() )
		{
			return;
		}

		DWORD fvf = 0;
		device->GetFVF( &fvf );
		unsigned stride = FvfStride( fvf );
		// Free aim: the game's reticle can come through a whole UI draw too.
		int aimHand = 0;
		if( VRSyntheticPad::GunAiming( aimHand ) && vertexCount > 0 &&
			IsGameReticle( device, vertexPtr, static_cast< unsigned >( vertexCount ), stride, m_w, m_h ) )
		{
#ifdef _DEBUG
			static unsigned s_logs = 0;
			if( s_logs < 20 )
			{
				++s_logs;
				PSU_LOG( "VRHud aim: SKIP ui draw n=%d layer=%d\n", vertexCount, s_passBLayer );
			}
#endif
			return;
		}
		const bool world = ClassifyWorldLayer( vertexPtr, static_cast< unsigned >( vertexCount ), stride );
		ReplayOnto( device, world, thisPtr, a4, vertexCount, vertexPtr, drawType, drawFn );
	}
}
