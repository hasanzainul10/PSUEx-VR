#include "StereoRenderer.h"

#include "D3D11Bridge.h"
#include "VRCamera.h"
#include "VRControllers.h"
#include "VRHud.h"
#include "VRPerf.h"
#include "VRPlayerDraw.h"
#include "VRSyntheticPad.h"
#include "VRSystem.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"
#include "../PSUStructs/PSUPlayerData.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace PSUVR
{
	namespace
	{
		// Comfort vignette (vr.comfortVignette): darken the periphery during artificial
		// motion (smooth turning, stick locomotion). Strength eases in fast, out slower.
		float s_vignette = 0.0f;
		bool s_vignetteInit = false;
		LARGE_INTEGER s_vignetteTime{};
		float s_vignetteBodyYaw = 0.0f;
		float s_vignettePlayer[ 3 ]{};

		float UpdateVignetteStrength()
		{
			const auto &vr = Config::Get().vr;
			LARGE_INTEGER now{};
			LARGE_INTEGER frequency{};
			QueryPerformanceCounter( &now );
			QueryPerformanceFrequency( &frequency );
			float dt = 0.0f;
			if( s_vignetteTime.QuadPart != 0 && frequency.QuadPart > 0 )
			{
				dt = static_cast< float >( now.QuadPart - s_vignetteTime.QuadPart ) /
					static_cast< float >( frequency.QuadPart );
			}
			s_vignetteTime = now;
			if( dt <= 0.0f || dt > 0.25f )
			{
				dt = 0.0f;
			}

			const float bodyYaw = VRCamera::BodyYaw();
			const PSUPlayer &player = PSUPlayer::Get();
			const bool playerReady = PSUPlayer::IsReady();
			float target = 0.0f;
			if( s_vignetteInit && dt > 0.0f )
			{
				// Smooth turning only: a snap step (several degrees in one frame) isn't motion.
				float yawStep = bodyYaw - s_vignetteBodyYaw;
				while( yawStep > 3.14159265f ) yawStep -= 6.2831853f;
				while( yawStep < -3.14159265f ) yawStep += 6.2831853f;
				yawStep = yawStep < 0.0f ? -yawStep : yawStep;
				const float turnRate = yawStep < 0.14f ? yawStep / dt : 0.0f;  // rad/s
				float speed = 0.0f;  // m/s along the ground
				if( playerReady )
				{
					const float dx = player.position.x - s_vignettePlayer[ 0 ];
					const float dz = player.position.z - s_vignettePlayer[ 2 ];
					speed = std::sqrt( dx * dx + dz * dz ) / VRCamera::kGameUnitsPerMeter / dt;
					speed = speed > 20.0f ? 0.0f : speed;  // teleport / area change
				}
				const float fromTurn = turnRate / 1.05f;  // full at 60 deg/s
				const float fromMove = speed / 3.0f;      // full at 3 m/s
				target = fromTurn > fromMove ? fromTurn : fromMove;
				target = target > 1.0f ? 1.0f : target;
			}
			s_vignetteInit = true;
			s_vignetteBodyYaw = bodyYaw;
			if( playerReady )
			{
				s_vignettePlayer[ 0 ] = player.position.x;
				s_vignettePlayer[ 1 ] = player.position.y;
				s_vignettePlayer[ 2 ] = player.position.z;
			}
			const float rate = target > s_vignette ? 10.0f : 3.0f;  // attack ~0.1 s, release ~0.3 s
			const float blend = 1.0f - std::exp( -rate * dt );
			s_vignette += ( target - s_vignette ) * blend;
			return s_vignette * ( vr.m_vignetteStrength < 0.0f ? 0.0f : vr.m_vignetteStrength );
		}

		// Black ring over the bound eye RT: transparent inside, opaque at the corners.
		void DrawVignette( IDirect3DDevice9 *device, unsigned width, unsigned height, float strength )
		{
			if( strength < 0.01f || width < 1 || height < 1 )
			{
				return;
			}
			struct Vertex { float x, y, z, rhw; DWORD color; };
			constexpr unsigned kSegments = 48;
			static Vertex s_ring[ ( kSegments + 1 ) * 2 ];
			const float cx = width * 0.5f;
			const float cy = height * 0.5f;
			const float corner = std::sqrt( cx * cx + cy * cy );
			const float k = strength > 1.0f ? 1.0f : strength;
			const float inner = corner * ( 1.0f - 0.5f * k );
			const float outer = corner * 1.02f;
			for( unsigned i = 0; i <= kSegments; ++i )
			{
				const float a = 6.2831853f * static_cast< float >( i ) / kSegments;
				const float c = std::cos( a ), s = std::sin( a );
				s_ring[ i * 2 + 0 ] = { cx + c * inner, cy + s * inner, 0.0f, 1.0f, D3DCOLOR_ARGB( 0, 0, 0, 0 ) };
				s_ring[ i * 2 + 1 ] = { cx + c * outer, cy + s * outer, 0.0f, 1.0f, D3DCOLOR_ARGB( 255, 0, 0, 0 ) };
			}

			DWORD fvf = 0, blend = 0, src = 0, dst = 0, zEnable = 0, zWrite = 0, cull = 0, alphaTest = 0, fog = 0, sepAlpha = 0;
			DWORD colorOp = 0, colorArg = 0, alphaOp = 0, alphaArg = 0;
			IDirect3DBaseTexture9 *texture = nullptr;
			IDirect3DVertexShader9 *vs = nullptr;
			IDirect3DPixelShader9 *ps = nullptr;
			IDirect3DVertexBuffer9 *vb = nullptr;
			UINT vbOffset = 0, vbStride = 0;
			device->GetFVF( &fvf );
			device->GetTexture( 0, &texture );
			device->GetVertexShader( &vs );
			device->GetPixelShader( &ps );
			device->GetStreamSource( 0, &vb, &vbOffset, &vbStride );
			device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
			device->GetRenderState( D3DRS_SRCBLEND, &src );
			device->GetRenderState( D3DRS_DESTBLEND, &dst );
			device->GetRenderState( D3DRS_ZENABLE, &zEnable );
			device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
			device->GetRenderState( D3DRS_CULLMODE, &cull );
			device->GetRenderState( D3DRS_ALPHATESTENABLE, &alphaTest );
			device->GetRenderState( D3DRS_FOGENABLE, &fog );
			device->GetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, &sepAlpha );
			device->GetTextureStageState( 0, D3DTSS_COLOROP, &colorOp );
			device->GetTextureStageState( 0, D3DTSS_COLORARG1, &colorArg );
			device->GetTextureStageState( 0, D3DTSS_ALPHAOP, &alphaOp );
			device->GetTextureStageState( 0, D3DTSS_ALPHAARG1, &alphaArg );

			device->SetVertexShader( nullptr );
			device->SetPixelShader( nullptr );
			device->SetTexture( 0, nullptr );
			device->SetFVF( D3DFVF_XYZRHW | D3DFVF_DIFFUSE );
			device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
			device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
			device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, FALSE );
			device->SetRenderState( D3DRS_ZENABLE, FALSE );
			device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
			device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
			device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
			device->SetRenderState( D3DRS_FOGENABLE, FALSE );
			device->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
			device->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE );
			device->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
			device->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE );
			device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, kSegments * 2, s_ring, sizeof( Vertex ) );

			device->SetTextureStageState( 0, D3DTSS_COLOROP, colorOp );
			device->SetTextureStageState( 0, D3DTSS_COLORARG1, colorArg );
			device->SetTextureStageState( 0, D3DTSS_ALPHAOP, alphaOp );
			device->SetTextureStageState( 0, D3DTSS_ALPHAARG1, alphaArg );
			device->SetRenderState( D3DRS_ALPHABLENDENABLE, blend );
			device->SetRenderState( D3DRS_SRCBLEND, src );
			device->SetRenderState( D3DRS_DESTBLEND, dst );
			device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, sepAlpha );
			device->SetRenderState( D3DRS_ZENABLE, zEnable );
			device->SetRenderState( D3DRS_ZWRITEENABLE, zWrite );
			device->SetRenderState( D3DRS_CULLMODE, cull );
			device->SetRenderState( D3DRS_ALPHATESTENABLE, alphaTest );
			device->SetRenderState( D3DRS_FOGENABLE, fog );
			device->SetFVF( fvf );
			device->SetStreamSource( 0, vb, vbOffset, vbStride );
			device->SetTexture( 0, texture );
			device->SetVertexShader( vs );
			device->SetPixelShader( ps );
			if( texture ) texture->Release();
			if( vs ) vs->Release();
			if( ps ) ps->Release();
			if( vb ) vb->Release();
		}

		bool s_inEyePass = false;
		bool s_inNameplateDraw = false;
		int s_passEyeIndex = 0;  // the eye being drawn (valid while s_inEyePass)
		unsigned char s_spriteBuf[ 64 * 1024 ];
		IDirect3DSurface9 *s_eyeSurface = nullptr;
		unsigned s_eyeW = 0;
		unsigned s_eyeH = 0;
		unsigned s_gameW = 0;
		unsigned s_gameH = 0;
		unsigned s_stretchQuadLogCount = 0;
		alignas( 16 ) unsigned char s_stretchQuadBuf[ 64 * 64 ]{};
		alignas( 16 ) unsigned char s_nameplateVertBuf[ 64 * 64 ]{};
		std::unordered_set< IDirect3DBaseTexture9 * > s_writtenTextures;
		unsigned s_screenQuadLogCount = 0;
		unsigned s_stretchLogCount = 0;
		constexpr unsigned kMaxScreenQuadLogs = 24;
		constexpr unsigned kMaxStretchLogs = 16;
		constexpr unsigned kMaxStretchVerts = 64;
		constexpr unsigned kMaxStretchStride = 64;

		bool HasStencil( D3DFORMAT format )
		{
			return format == D3DFMT_D24S8 || format == D3DFMT_D24X4S4 || format == D3DFMT_D15S1 || format == D3DFMT_D24FS8;
		}

		bool InvertMatrix( const D3DMATRIX &m, D3DMATRIX &out )
		{
			const float *a = &m._11;
			float inv[ 16 ]{};
			inv[ 0 ] = a[ 5 ] * a[ 10 ] * a[ 15 ] - a[ 5 ] * a[ 11 ] * a[ 14 ] - a[ 9 ] * a[ 6 ] * a[ 15 ]
				+ a[ 9 ] * a[ 7 ] * a[ 14 ] + a[ 13 ] * a[ 6 ] * a[ 11 ] - a[ 13 ] * a[ 7 ] * a[ 10 ];
			inv[ 4 ] = -a[ 4 ] * a[ 10 ] * a[ 15 ] + a[ 4 ] * a[ 11 ] * a[ 14 ] + a[ 8 ] * a[ 6 ] * a[ 15 ]
				- a[ 8 ] * a[ 7 ] * a[ 14 ] - a[ 12 ] * a[ 6 ] * a[ 11 ] + a[ 12 ] * a[ 7 ] * a[ 10 ];
			inv[ 8 ] = a[ 4 ] * a[ 9 ] * a[ 15 ] - a[ 4 ] * a[ 11 ] * a[ 13 ] - a[ 8 ] * a[ 5 ] * a[ 15 ]
				+ a[ 8 ] * a[ 7 ] * a[ 13 ] + a[ 12 ] * a[ 5 ] * a[ 11 ] - a[ 12 ] * a[ 7 ] * a[ 9 ];
			inv[ 12 ] = -a[ 4 ] * a[ 9 ] * a[ 14 ] + a[ 4 ] * a[ 10 ] * a[ 13 ] + a[ 8 ] * a[ 5 ] * a[ 14 ]
				- a[ 8 ] * a[ 6 ] * a[ 13 ] - a[ 12 ] * a[ 5 ] * a[ 10 ] + a[ 12 ] * a[ 6 ] * a[ 9 ];
			inv[ 1 ] = -a[ 1 ] * a[ 10 ] * a[ 15 ] + a[ 1 ] * a[ 11 ] * a[ 14 ] + a[ 9 ] * a[ 2 ] * a[ 15 ]
				- a[ 9 ] * a[ 3 ] * a[ 14 ] - a[ 13 ] * a[ 2 ] * a[ 11 ] + a[ 13 ] * a[ 3 ] * a[ 10 ];
			inv[ 5 ] = a[ 0 ] * a[ 10 ] * a[ 15 ] - a[ 0 ] * a[ 11 ] * a[ 14 ] - a[ 8 ] * a[ 2 ] * a[ 15 ]
				+ a[ 8 ] * a[ 3 ] * a[ 14 ] + a[ 12 ] * a[ 2 ] * a[ 11 ] - a[ 12 ] * a[ 3 ] * a[ 10 ];
			inv[ 9 ] = -a[ 0 ] * a[ 9 ] * a[ 15 ] + a[ 0 ] * a[ 11 ] * a[ 13 ] + a[ 8 ] * a[ 1 ] * a[ 15 ]
				- a[ 8 ] * a[ 3 ] * a[ 13 ] - a[ 12 ] * a[ 1 ] * a[ 11 ] + a[ 12 ] * a[ 3 ] * a[ 9 ];
			inv[ 13 ] = a[ 0 ] * a[ 9 ] * a[ 14 ] - a[ 0 ] * a[ 10 ] * a[ 13 ] - a[ 8 ] * a[ 1 ] * a[ 14 ]
				+ a[ 8 ] * a[ 2 ] * a[ 13 ] + a[ 12 ] * a[ 1 ] * a[ 10 ] - a[ 12 ] * a[ 2 ] * a[ 9 ];
			inv[ 2 ] = a[ 1 ] * a[ 6 ] * a[ 15 ] - a[ 1 ] * a[ 7 ] * a[ 14 ] - a[ 5 ] * a[ 2 ] * a[ 15 ]
				+ a[ 5 ] * a[ 3 ] * a[ 14 ] + a[ 13 ] * a[ 2 ] * a[ 7 ] - a[ 13 ] * a[ 3 ] * a[ 6 ];
			inv[ 6 ] = -a[ 0 ] * a[ 6 ] * a[ 15 ] + a[ 0 ] * a[ 7 ] * a[ 14 ] + a[ 4 ] * a[ 2 ] * a[ 15 ]
				- a[ 4 ] * a[ 3 ] * a[ 14 ] - a[ 12 ] * a[ 2 ] * a[ 7 ] + a[ 12 ] * a[ 3 ] * a[ 6 ];
			inv[ 10 ] = a[ 0 ] * a[ 5 ] * a[ 15 ] - a[ 0 ] * a[ 7 ] * a[ 13 ] - a[ 4 ] * a[ 1 ] * a[ 15 ]
				+ a[ 4 ] * a[ 3 ] * a[ 13 ] + a[ 12 ] * a[ 1 ] * a[ 7 ] - a[ 12 ] * a[ 3 ] * a[ 5 ];
			inv[ 14 ] = -a[ 0 ] * a[ 5 ] * a[ 14 ] + a[ 0 ] * a[ 6 ] * a[ 13 ] + a[ 4 ] * a[ 1 ] * a[ 14 ]
				- a[ 4 ] * a[ 2 ] * a[ 13 ] - a[ 12 ] * a[ 1 ] * a[ 6 ] + a[ 12 ] * a[ 2 ] * a[ 5 ];
			inv[ 3 ] = -a[ 1 ] * a[ 6 ] * a[ 11 ] + a[ 1 ] * a[ 7 ] * a[ 10 ] + a[ 5 ] * a[ 2 ] * a[ 11 ]
				- a[ 5 ] * a[ 3 ] * a[ 10 ] - a[ 9 ] * a[ 2 ] * a[ 7 ] + a[ 9 ] * a[ 3 ] * a[ 6 ];
			inv[ 7 ] = a[ 0 ] * a[ 6 ] * a[ 11 ] - a[ 0 ] * a[ 7 ] * a[ 10 ] - a[ 4 ] * a[ 2 ] * a[ 11 ]
				+ a[ 4 ] * a[ 3 ] * a[ 10 ] + a[ 8 ] * a[ 2 ] * a[ 7 ] - a[ 8 ] * a[ 3 ] * a[ 6 ];
			inv[ 11 ] = -a[ 0 ] * a[ 5 ] * a[ 11 ] + a[ 0 ] * a[ 7 ] * a[ 9 ] + a[ 4 ] * a[ 1 ] * a[ 11 ]
				- a[ 4 ] * a[ 3 ] * a[ 9 ] - a[ 8 ] * a[ 1 ] * a[ 7 ] + a[ 8 ] * a[ 3 ] * a[ 5 ];
			inv[ 15 ] = a[ 0 ] * a[ 5 ] * a[ 10 ] - a[ 0 ] * a[ 6 ] * a[ 9 ] - a[ 4 ] * a[ 1 ] * a[ 10 ]
				+ a[ 4 ] * a[ 2 ] * a[ 9 ] + a[ 8 ] * a[ 1 ] * a[ 6 ] - a[ 8 ] * a[ 2 ] * a[ 5 ];

			const float det = a[ 0 ] * inv[ 0 ] + a[ 1 ] * inv[ 4 ] + a[ 2 ] * inv[ 8 ] + a[ 3 ] * inv[ 12 ];
			if( det > -1.0e-12f && det < 1.0e-12f )
			{
				return false;
			}
			const float idet = 1.0f / det;
			float *o = &out._11;
			for( int i = 0; i < 16; ++i )
			{
				o[ i ] = inv[ i ] * idet;
			}
			return true;
		}

		void MulMatrix( const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX &out )
		{
			D3DMATRIX r{};
			for( int i = 0; i < 4; ++i )
			{
				for( int j = 0; j < 4; ++j )
				{
					r.m[ i ][ j ] = a.m[ i ][ 0 ] * b.m[ 0 ][ j ] + a.m[ i ][ 1 ] * b.m[ 1 ][ j ] + a.m[ i ][ 2 ] * b.m[ 2 ][ j ] +
						a.m[ i ][ 3 ] * b.m[ 3 ][ j ];
				}
			}
			out = r;
		}

		void MulVec4Mat( const float v[ 4 ], const D3DMATRIX &m, float out[ 4 ] )
		{
			out[ 0 ] = v[ 0 ] * m._11 + v[ 1 ] * m._21 + v[ 2 ] * m._31 + v[ 3 ] * m._41;
			out[ 1 ] = v[ 0 ] * m._12 + v[ 1 ] * m._22 + v[ 2 ] * m._32 + v[ 3 ] * m._42;
			out[ 2 ] = v[ 0 ] * m._13 + v[ 1 ] * m._23 + v[ 2 ] * m._33 + v[ 3 ] * m._43;
			out[ 3 ] = v[ 0 ] * m._14 + v[ 1 ] * m._24 + v[ 2 ] * m._34 + v[ 3 ] * m._44;
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
	}

	StereoRenderer &StereoRenderer::Get()
	{
		static StereoRenderer instance;
		return instance;
	}

	namespace
	{
		// The eye pose of the pass being drawn (set by RenderEyes before each eye pass).
		EyePose s_passEye{};

		// Camera state 5 (dressing room): the background is a full-screen image. Stretched
		// over the eye it would follow the head, so it is mapped onto a large picture fixed in
		// the play space (kBackdropDistance in front of the recenter point) and drawn per eye
		// with perspective-correct coordinates, behind everything else.
		constexpr float kBackdropDistance = 4.0f;
		constexpr float kBackdropHalfWidth = 4.0f;

		struct BV3 { float x, y, z; };

		BV3 RotateQ( const float q[ 4 ], const BV3 &v )
		{
			const float ux = q[ 0 ], uy = q[ 1 ], uz = q[ 2 ], w = q[ 3 ];
			const BV3 t{ 2.0f * ( uy * v.z - uz * v.y ), 2.0f * ( uz * v.x - ux * v.z ), 2.0f * ( ux * v.y - uy * v.x ) };
			return { v.x + w * t.x + ( uy * t.z - uz * t.y ), v.y + w * t.y + ( uz * t.x - ux * t.z ),
				v.z + w * t.z + ( ux * t.y - uy * t.x ) };
		}

		bool MapBackdropVertex( float gx, float gy, const float center[ 3 ], const float orientation[ 4 ],
			float &sx, float &sy, float &rhw )
		{
			const float u = gx / static_cast< float >( s_gameW ) - 0.5f;
			const float v = 0.5f - gy / static_cast< float >( s_gameH );
			const float halfH = kBackdropHalfWidth * static_cast< float >( s_gameH ) / static_cast< float >( s_gameW );
			const BV3 right = RotateQ( orientation, { 1.0f, 0.0f, 0.0f } );
			const BV3 up = RotateQ( orientation, { 0.0f, 1.0f, 0.0f } );
			const BV3 p{ center[ 0 ] + right.x * u * 2.0f * kBackdropHalfWidth + up.x * v * 2.0f * halfH,
				center[ 1 ] + right.y * u * 2.0f * kBackdropHalfWidth + up.y * v * 2.0f * halfH,
				center[ 2 ] + right.z * u * 2.0f * kBackdropHalfWidth + up.z * v * 2.0f * halfH };
			const float inverse[ 4 ] = { -s_passEye.orientation[ 0 ], -s_passEye.orientation[ 1 ], -s_passEye.orientation[ 2 ],
				s_passEye.orientation[ 3 ] };
			const BV3 d = RotateQ( inverse, { p.x - s_passEye.position[ 0 ], p.y - s_passEye.position[ 1 ], p.z - s_passEye.position[ 2 ] } );
			const float depth = -d.z;
			if( depth < 0.05f )
			{
				return false;
			}
			const float tanL = std::tan( s_passEye.fovAngleLeft ), tanR = std::tan( s_passEye.fovAngleRight );
			const float tanU = std::tan( s_passEye.fovAngleUp ), tanD = std::tan( s_passEye.fovAngleDown );
			sx = ( d.x / depth - tanL ) / ( tanR - tanL ) * static_cast< float >( s_eyeW );
			sy = ( tanU - d.y / depth ) / ( tanU - tanD ) * static_cast< float >( s_eyeH );
			rhw = 1.0f / depth;
			return true;
		}
	}

	void StereoRenderer::SetPassEye( const EyePose &eye )
	{
		s_passEye = eye;
	}

	bool StereoRenderer::InEyePass()
	{
		return s_inEyePass;
	}

	bool StereoRenderer::InNameplateDraw()
	{
		return s_inNameplateDraw;
	}

	void StereoRenderer::MarkSurfaceTexture( IDirect3DSurface9 *surface )
	{
		if( !surface )
		{
			return;
		}
		IDirect3DTexture9 *tex = nullptr;
		if( SUCCEEDED( surface->GetContainer( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex ) ) ) && tex )
		{
			s_writtenTextures.insert( tex );
			tex->Release();
		}
	}

	void StereoRenderer::BeginEyePass( IDirect3DSurface9 *eyeTarget, unsigned eyeW, unsigned eyeH,
		unsigned gameW, unsigned gameH )
	{
		s_writtenTextures.clear();
		s_eyeSurface = eyeTarget;
		s_eyeW = eyeW;
		s_eyeH = eyeH;
		s_gameW = gameW;
		s_gameH = gameH;
		MarkSurfaceTexture( eyeTarget );
		s_inEyePass = true;
	}

	void StereoRenderer::EndEyePass()
	{
		s_inEyePass = false;
		s_eyeSurface = nullptr;
		s_eyeW = 0;
		s_eyeH = 0;
		s_gameW = 0;
		s_gameH = 0;
		s_writtenTextures.clear();
	}

	bool StereoRenderer::EyeColorTargetIs( IDirect3DSurface9 *surface )
	{
		return s_inEyePass && surface && surface == s_eyeSurface && s_eyeW > 0 && s_eyeH > 0;
	}

	bool StereoRenderer::EyeColorTargetBound( IDirect3DDevice9 *device )
	{
		if( !s_inEyePass || !device || !s_eyeSurface || s_eyeW == 0 || s_eyeH == 0 )
		{
			return false;
		}
		IDirect3DSurface9 *rt = nullptr;
		if( FAILED( device->GetRenderTarget( 0, &rt ) ) || !rt )
		{
			return false;
		}
		const bool match = ( rt == s_eyeSurface );
		rt->Release();
		return match;
	}

	void StereoRenderer::GetEyeViewport( D3DVIEWPORT9 &out )
	{
		out = { 0, 0, s_eyeW, s_eyeH, 0.0f, 1.0f };
	}

	void StereoRenderer::GetEyeScissor( RECT &out )
	{
		out.left = 0;
		out.top = 0;
		out.right = static_cast< LONG >( s_eyeW );
		out.bottom = static_cast< LONG >( s_eyeH );
	}

	void StereoRenderer::OnSetRenderTarget( IDirect3DSurface9 *target )
	{
		if( !s_inEyePass )
		{
			return;
		}
		MarkSurfaceTexture( target );
	}

	void StereoRenderer::OnStretchRect( IDirect3DSurface9 *source, IDirect3DSurface9 *dest, void *ret )
	{
		if( !source || !dest )
		{
			return;
		}

		if( s_inEyePass )
		{
			MarkSurfaceTexture( dest );
		}

		if( s_stretchLogCount >= kMaxStretchLogs )
		{
			return;
		}

		D3DSURFACE_DESC srcDesc{};
		D3DSURFACE_DESC dstDesc{};
		source->GetDesc( &srcDesc );
		dest->GetDesc( &dstDesc );
		const bool srcRT = ( srcDesc.Usage & D3DUSAGE_RENDERTARGET ) != 0;
		const bool dstRT = ( dstDesc.Usage & D3DUSAGE_RENDERTARGET ) != 0;
		if( !srcRT && !dstRT )
		{
			return;
		}

		++s_stretchLogCount;
		PSU_LOG( "StereoStretch: eye=%d src=%ux%u usage=0x%X dst=%ux%u usage=0x%X ret=%p\n",
			s_inEyePass ? 1 : 0,
			srcDesc.Width, srcDesc.Height, srcDesc.Usage,
			dstDesc.Width, dstDesc.Height, dstDesc.Usage,
			ret );
	}

	bool StereoRenderer::ShouldSkipScreenQuad( IDirect3DDevice9 *device, void *ret,
		unsigned primitiveCount, bool userPointer )
	{
		// Diagnostics: every draw of the eye pass while the camera is in state 5 (dressing room)
		// or 4 (free aim).
		const int diagState = VRCamera::CameraState();
		if( s_inEyePass && device && ( diagState == 5 || diagState == 4 ) )
		{
			static unsigned s_stateLogs[ 2 ]{};
			unsigned &logs = s_stateLogs[ diagState == 4 ? 1 : 0 ];
			if( logs < 150 )
			{
				++logs;
				DWORD drawFvf = 0;
				device->GetFVF( &drawFvf );
				unsigned texW = 0, texH = 0;
				int texRT = -1;
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
						texRT = ( desc.Usage & D3DUSAGE_RENDERTARGET ) ? 1 : 0;
						tex2d->Release();
					}
				}
				IDirect3DVertexShader9 *vs = nullptr;
				device->GetVertexShader( &vs );
				PSU_LOG( "State%d draw: rhw=%d up=%d prim=%u fvf=0x%X vs=%d tex=%ux%u rt=%d ret=%p\n",
					diagState, ( drawFvf & D3DFVF_POSITION_MASK ) == D3DFVF_XYZRHW ? 1 : 0, userPointer ? 1 : 0, primitiveCount,
					drawFvf, vs ? 1 : 0, texW, texH, texRT, ret );
				if( vs ) vs->Release();
			}
		}
		// Free aim with the gun up: the game's reticle billboard (a 2-triangle 3D quad with a
		// 256 x 256 texture at the aim point) is replaced by our reticle at the controller ray.
		int aimHand = 0;
		if( s_inEyePass && device && primitiveCount == 2 && VRSyntheticPad::GunAiming( aimHand ) )
		{
			DWORD drawFvf = 0;
			device->GetFVF( &drawFvf );
			if( ( drawFvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
			{
				IDirect3DBaseTexture9 *texture = nullptr;
				if( SUCCEEDED( device->GetTexture( 0, &texture ) ) && texture )
				{
					IDirect3DTexture9 *tex2d = nullptr;
					texture->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex2d ) );
					texture->Release();
					bool reticle = false;
					if( tex2d )
					{
						D3DSURFACE_DESC desc{};
						tex2d->GetLevelDesc( 0, &desc );
						reticle = desc.Width == 256 && desc.Height == 256 && ( desc.Usage & D3DUSAGE_RENDERTARGET ) == 0;
						tex2d->Release();
					}
					if( reticle )
					{
						static unsigned s_logs = 0;
						if( s_logs < 10 )
						{
							++s_logs;
							PSU_LOG( "Stereo: free aim, dropped a 256x256 billboard (game reticle?) fvf=0x%X ret=%p\n", drawFvf, ret );
						}
						return true;
					}
				}
			}
		}
		if( !s_inEyePass || !device || !Config::Get().vr.m_stereoSkipScreenQuads )
		{
			return false;
		}

		DWORD fvf = 0;
		if( FAILED( device->GetFVF( &fvf ) ) || ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return false;
		}

		IDirect3DBaseTexture9 *stage0 = nullptr;
		if( FAILED( device->GetTexture( 0, &stage0 ) ) || !stage0 )
		{
			return false;
		}

		IDirect3DTexture9 *tex9 = nullptr;
		stage0->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex9 ) );
		stage0->Release();
		if( !tex9 )
		{
			return false;
		}

		D3DSURFACE_DESC desc{};
		tex9->GetLevelDesc( 0, &desc );
		const bool isRT = ( desc.Usage & D3DUSAGE_RENDERTARGET ) != 0;
		const bool written = s_writtenTextures.find( tex9 ) != s_writtenTextures.end();
		const bool skip = isRT && !written;

		DWORD blendEnable = 0;
		device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blendEnable );

		if( s_screenQuadLogCount < kMaxScreenQuadLogs )
		{
			++s_screenQuadLogCount;
			PSU_LOG( "StereoQuad: skip=%d up=%d prim=%u fvf=0x%X tex=%ux%u rt=%d written=%d blend=%u ret=%p\n",
				skip ? 1 : 0, userPointer ? 1 : 0, primitiveCount, fvf,
				desc.Width, desc.Height, isRT ? 1 : 0, written ? 1 : 0, blendEnable, ret );
		}

		tex9->Release();
		return skip;
	}

#ifdef _DEBUG
	namespace
	{
		// One kind of screen-space draw: where it is drawn from and with what texture.
		struct EffectKind
		{
			void *ret;
			unsigned texW, texH;
			int where;
			unsigned count;  // draws since the last summary
		};
		constexpr unsigned kMaxEffectKinds = 160;
		EffectKind s_effectKinds[ kMaxEffectKinds ];
		unsigned s_effectKindCount = 0;
		unsigned s_effectLogs = 0;
		constexpr unsigned kMaxEffectLogs = 800;
		DWORD s_effectSummaryTick = 0;
		DWORD s_techTick = 0;
	}
#endif

#ifdef _DEBUG
	namespace
	{
		constexpr unsigned kTrackedConstants = 32;
		float s_vsConst[ kTrackedConstants ][ 4 ]{};
		void *s_vsConstRet[ kTrackedConstants ]{};
	}
#endif

#ifdef _DEBUG
	namespace
	{
		// [0] flat, [1] eyes; per layer slot: 0 = 0x12, 1 = 0x1F, 2 = 0x20.
		unsigned s_layerDraws[ 2 ][ 3 ]{};
		unsigned s_layerSkips[ 3 ]{};
		unsigned s_skyWorlds[ 2 ]{};
		unsigned s_skyMoved = 0;
		DWORD s_layerLogTick = 0;

		int LayerSlot( int layer )
		{
			return layer == 0x12 ? 0 : ( layer == 0x1F ? 1 : ( layer == 0x20 ? 2 : -1 ) );
		}

		void MaybeLogLayers()
		{
			const DWORD now = GetTickCount();
			if( now - s_layerLogTick < 5000 )
			{
				return;
			}
			s_layerLogTick = now;
			PSU_LOG( "Layers 5 s: flat draws sky %u blur %u glare %u | eye draws sky %u blur %u glare %u | skipped blur %u glare %u | "
				"sky world sets flat %u eyes %u moved %u | current layer 0x%X\n",
				s_layerDraws[ 0 ][ 0 ], s_layerDraws[ 0 ][ 1 ], s_layerDraws[ 0 ][ 2 ], s_layerDraws[ 1 ][ 0 ], s_layerDraws[ 1 ][ 1 ],
				s_layerDraws[ 1 ][ 2 ], s_layerSkips[ 1 ], s_layerSkips[ 2 ], s_skyWorlds[ 0 ], s_skyWorlds[ 1 ], s_skyMoved,
				*reinterpret_cast< const int * >( 0x009946C8 ) );
			std::memset( s_layerDraws, 0, sizeof( s_layerDraws ) );
			std::memset( s_layerSkips, 0, sizeof( s_layerSkips ) );
			std::memset( s_skyWorlds, 0, sizeof( s_skyWorlds ) );
			s_skyMoved = 0;
		}
	}
#endif

#ifdef _DEBUG
	namespace
	{
		// Every draw site seen in an eye pass, with the layer it was first seen on (site bisect).
		struct EyeSite
		{
			void *ret;
			int layer;
		};
		constexpr int kMaxEyeSites = 256;
		EyeSite s_eyeSites[ kMaxEyeSites ];
		int s_eyeSiteCount = 0;
	}
#endif

	void StereoRenderer::CountLayerDraw( void *ret )
	{
#ifdef _DEBUG
		if( s_inEyePass && s_eyeSiteCount < kMaxEyeSites )
		{
			bool seen = false;
			for( int i = 0; i < s_eyeSiteCount && !seen; ++i )
			{
				seen = s_eyeSites[ i ].ret == ret;
			}
			if( !seen )
			{
				s_eyeSites[ s_eyeSiteCount++ ] = { ret, *reinterpret_cast< const int * >( 0x009946C8 ) };
			}
		}
		const int slot = LayerSlot( *reinterpret_cast< const int * >( 0x009946C8 ) );
		if( slot >= 0 )
		{
			++s_layerDraws[ s_inEyePass ? 1 : 0 ][ slot ];
		}
		MaybeLogLayers();
#endif
	}

#ifdef _DEBUG
	namespace
	{
		void *s_bisectSite = nullptr;
		int s_bisectIndex = -1;
	}
#endif

	const char *StereoRenderer::StepSiteBisect( int direction )
	{
#ifdef _DEBUG
		// Every draw site seen in the eye passes so far.
		const int count = s_eyeSiteCount;
		static char s_text[ 80 ];
		if( count == 0 )
		{
			s_bisectIndex = -1;
			s_bisectSite = nullptr;
			sprintf_s( s_text, "Site bisect: no eye draw sites seen yet" );
			return s_text;
		}
		s_bisectIndex += direction;
		if( s_bisectIndex < -1 )
		{
			s_bisectIndex = count - 1;
		}
		if( s_bisectIndex >= count )
		{
			s_bisectIndex = -1;
		}
		s_bisectSite = s_bisectIndex >= 0 ? s_eyeSites[ s_bisectIndex ].ret : nullptr;
		if( s_bisectSite )
		{
			sprintf_s( s_text, "Site bisect %d/%d: hiding %p (layer 0x%X)", s_bisectIndex + 1, count, s_bisectSite,
				s_eyeSites[ s_bisectIndex ].layer );
		}
		else
		{
			sprintf_s( s_text, "Site bisect: nothing hidden" );
		}
		PSU_LOG( "%s\n", s_text );
		return s_text;
#else
		( void )direction;
		return "";
#endif
	}

	bool StereoRenderer::SkipEyeEffectDraw( void *ret )
	{
		if( !s_inEyePass )
		{
			return false;
		}
#ifdef _DEBUG
		if( s_bisectSite && ret == s_bisectSite )
		{
			return true;
		}
#endif
		// PSU_DrawLayer keeps the layer it is drawing at 0x009946C8.
		const int layer = *reinterpret_cast< const int * >( 0x009946C8 );
		const auto &vr = Config::Get().vr;
		// The glare's full-screen blend over the eye (0x004B6331): the glare layer (0x20) fills that
		// image from the flat picture, so in VR its glows float off the enemies and show through the
		// weapon. The layer itself keeps running (hiding it left the image stale: a frozen eye).
		const bool glareBlend = reinterpret_cast< uintptr_t >( ret ) == 0x004B6331;
		const bool skip = ( layer == 0x12 && !vr.m_eyeSky ) || ( layer == 0x1F && !vr.m_eyeBlur ) ||
			( layer == 0x20 && !vr.m_eyeGlare ) || ( glareBlend && !vr.m_eyeGlare );
#ifdef _DEBUG
		if( skip )
		{
			++s_layerSkips[ 0 ];
		}
#endif
		return skip;
	}

	void StereoRenderer::NoteVertexConstants( unsigned startRegister, const float *data, unsigned vector4Count, void *ret )
	{
#ifdef _DEBUG
		if( !data )
		{
			return;
		}
		for( unsigned i = 0; i < vector4Count && startRegister + i < kTrackedConstants; ++i )
		{
			std::memcpy( s_vsConst[ startRegister + i ], data + i * 4, sizeof( float ) * 4 );
			s_vsConstRet[ startRegister + i ] = ret;
		}
#else
		( void )startRegister; ( void )data; ( void )vector4Count; ( void )ret;
#endif
	}

	void StereoRenderer::NoteShaderDraw( IDirect3DDevice9 *device, unsigned primitiveCount, void *ret )
	{
#ifdef _DEBUG
		static void *s_sites[ 48 ]{};
		static unsigned s_count = 0;
		if( !s_inEyePass || !device || s_count >= 48 )
		{
			return;
		}
		IDirect3DVertexShader9 *vs = nullptr;
		device->GetVertexShader( &vs );
		if( !vs )
		{
			return;
		}
		vs->Release();
		for( unsigned i = 0; i < s_count; ++i )
		{
			if( s_sites[ i ] == ret )
			{
				return;
			}
		}
		s_sites[ s_count++ ] = ret;
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
		PSU_LOG( "Stereo: shader draw ret=%p vs=%p prims=%u tex=%ux%u | camera state %d\n", ret, static_cast< void * >( vs ),
			primitiveCount, texW, texH, VRCamera::CameraState() );
		for( unsigned r = 0; r < 12; ++r )
		{
			const float *c = s_vsConst[ r ];
			PSU_LOG( "  c%u = %.4f %.4f %.4f %.4f (set by %p)\n", r, c[ 0 ], c[ 1 ], c[ 2 ], c[ 3 ], s_vsConstRet[ r ] );
		}
#else
		( void )device; ( void )primitiveCount; ( void )ret;
#endif
	}

	void StereoRenderer::NoteNearCameraDraw( IDirect3DDevice9 *device, unsigned primitiveType, unsigned primitiveCount,
		const void *verts, unsigned stride, void *ret )
	{
#ifdef _DEBUG
		static void *s_sites[ 64 ]{};
		static unsigned s_count = 0;
		if( !s_inEyePass || !device || !verts || stride < 12 || s_count >= 64 )
		{
			return;
		}
		StereoRenderer &self = Get();
		if( !self.m_haveInvGameView )
		{
			return;
		}
		for( unsigned i = 0; i < s_count; ++i )
		{
			if( s_sites[ i ] == ret )
			{
				return;
			}
		}
		DWORD fvf = 0;
		if( FAILED( device->GetFVF( &fvf ) ) || ( fvf & D3DFVF_POSITION_MASK ) == D3DFVF_XYZRHW || fvf == 0 )
		{
			return;
		}
		const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
		if( vertCount < 3 || vertCount > 20000 )
		{
			return;
		}
		D3DMATRIX world{}, toCamera{};
		device->GetTransform( D3DTS_WORLD, &world );
		MulMatrix( world, self.m_gameView, toCamera );
		float mn[ 3 ] = { 1.0e30f, 1.0e30f, 1.0e30f }, mx[ 3 ] = { -1.0e30f, -1.0e30f, -1.0e30f };
		float nearest = 1.0e30f;
		const auto *bytes = static_cast< const unsigned char * >( verts );
		for( unsigned i = 0; i < vertCount; ++i )
		{
			const float *v = reinterpret_cast< const float * >( bytes + i * stride );
			const float in[ 4 ] = { v[ 0 ], v[ 1 ], v[ 2 ], 1.0f };
			float c[ 4 ]{};
			MulVec4Mat( in, toCamera, c );
			for( int k = 0; k < 3; ++k )
			{
				mn[ k ] = ( std::min )( mn[ k ], c[ k ] );
				mx[ k ] = ( std::max )( mx[ k ], c[ k ] );
			}
			nearest = ( std::min )( nearest, std::sqrt( c[ 0 ] * c[ 0 ] + c[ 1 ] * c[ 1 ] + c[ 2 ] * c[ 2 ] ) );
		}
		// Within 2 m (20 units) of the camera, or a box around it on every axis.
		const bool around = mn[ 0 ] < 0.0f && mx[ 0 ] > 0.0f && mn[ 1 ] < 0.0f && mx[ 1 ] > 0.0f && mn[ 2 ] < 0.0f && mx[ 2 ] > 0.0f;
		if( nearest > 20.0f && !around )
		{
			return;
		}
		s_sites[ s_count++ ] = ret;
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
		DWORD blend = 0, zEnable = 0, zWrite = 0;
		device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
		device->GetRenderState( D3DRS_ZENABLE, &zEnable );
		device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
		PSU_LOG( "Stereo: 3D near camera ret=%p verts=%u fvf=0x%X tex=%ux%u blend=%lu z=%lu/%lu nearest %.1f%s | camera box (%.1f %.1f %.1f)-(%.1f %.1f %.1f) | world t (%.1f %.1f %.1f) | camera state %d\n",
			ret, vertCount, fvf, texW, texH, blend, zEnable, zWrite, nearest, around ? " AROUND" : "", mn[ 0 ], mn[ 1 ], mn[ 2 ], mx[ 0 ],
			mx[ 1 ], mx[ 2 ], world._41, world._42, world._43, VRCamera::CameraState() );
#else
		( void )device; ( void )primitiveType; ( void )primitiveCount; ( void )verts; ( void )stride; ( void )ret;
#endif
	}

	void StereoRenderer::NoteTechnique()
	{
#ifdef _DEBUG
		s_techTick = GetTickCount();
#endif
	}

	bool StereoRenderer::SkipEyeHitSpark( IDirect3DDevice9 *device, unsigned primitiveCount )
	{
		if( !s_inEyePass || !device || primitiveCount != 2 || *reinterpret_cast< const int * >( 0x009946C8 ) != 0x0F ||
			!Config::Get().vr.m_enabled )
		{
			return false;
		}
		DWORD blend = 0, src = 0, dst = 0;
		device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
		device->GetRenderState( D3DRS_SRCBLEND, &src );
		device->GetRenderState( D3DRS_DESTBLEND, &dst );
		if( !blend || src != D3DBLEND_ONE || dst != D3DBLEND_ONE )
		{
			return false;
		}
		IDirect3DBaseTexture9 *texture = nullptr;
		if( FAILED( device->GetTexture( 0, &texture ) ) || !texture )
		{
			return false;
		}
		IDirect3DTexture9 *tex2d = nullptr;
		texture->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex2d ) );
		texture->Release();
		if( !tex2d )
		{
			return false;
		}
		D3DSURFACE_DESC desc{};
		tex2d->GetLevelDesc( 0, &desc );
		tex2d->Release();
		return desc.Width <= 32 && desc.Height <= 32;
	}

	void StereoRenderer::NoteLayerDraw( IDirect3DDevice9 *device, const char *api, unsigned primitiveType, unsigned primitiveCount, void *ret )
	{
#ifdef _DEBUG
		constexpr int kLayer = 0x0F;
		if( !s_inEyePass || !device || *reinterpret_cast< const int * >( 0x009946C8 ) != kLayer )
		{
			return;
		}
		DWORD fvf = 0, blend = 0, src = 0, dst = 0, zEnable = 0, zWrite = 0;
		device->GetFVF( &fvf );
		device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
		device->GetRenderState( D3DRS_SRCBLEND, &src );
		device->GetRenderState( D3DRS_DESTBLEND, &dst );
		device->GetRenderState( D3DRS_ZENABLE, &zEnable );
		device->GetRenderState( D3DRS_ZWRITEENABLE, &zWrite );
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
		struct Seen { void *ret; unsigned texW, texH, prims; DWORD fvf; };
		static Seen s_seen[ 128 ];
		static unsigned s_count = 0;
		for( unsigned i = 0; i < s_count; ++i )
		{
			if( s_seen[ i ].ret == ret && s_seen[ i ].texW == texW && s_seen[ i ].texH == texH && s_seen[ i ].prims == primitiveCount &&
				s_seen[ i ].fvf == fvf )
			{
				return;
			}
		}
		if( s_count >= 128 )
		{
			return;
		}
		s_seen[ s_count++ ] = { ret, texW, texH, primitiveCount, fvf };
		PSU_LOG( "VRLayer0F: %s ret=%p type=%u prims=%u fvf=0x%X tex=%ux%u blend=%lu %lu/%lu z=%lu zw=%lu\n", api, ret, primitiveType,
			primitiveCount, fvf, texW, texH, blend, src, dst, zEnable, zWrite );
#else
		( void )device; ( void )api; ( void )primitiveType; ( void )primitiveCount; ( void )ret;
#endif
	}

	void StereoRenderer::NoteEffectDraw( IDirect3DDevice9 *device, int where, unsigned primitiveType, unsigned primitiveCount,
		const void *verts, unsigned stride, void *ret )
	{
#ifdef _DEBUG
		if( !device || s_effectLogs >= kMaxEffectLogs || !Config::Get().vr.m_enabled )
		{
			return;
		}
		DWORD fvf = 0;
		IDirect3DVertexShader9 *vs = nullptr;
		device->GetVertexShader( &vs );
		if( vs )
		{
			vs->Release();
			return;
		}
		if( FAILED( device->GetFVF( &fvf ) ) || ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return;
		}
		unsigned texW = 0, texH = 0;
		int texRT = -1;
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
				texRT = ( desc.Usage & D3DUSAGE_RENDERTARGET ) ? 1 : 0;
				tex2d->Release();
			}
		}
		const DWORD now = GetTickCount();
		const bool tech = s_techTick && now - s_techTick < 2000;
		EffectKind *kind = nullptr;
		for( unsigned i = 0; i < s_effectKindCount; ++i )
		{
			EffectKind &k = s_effectKinds[ i ];
			if( k.ret == ret && k.texW == texW && k.texH == texH && k.where == where )
			{
				kind = &k;
				break;
			}
		}
		if( !kind && s_effectKindCount < kMaxEffectKinds )
		{
			kind = &s_effectKinds[ s_effectKindCount ];
			*kind = { ret, texW, texH, where, 0 };
			const unsigned id = s_effectKindCount++;
			char box[ 512 ] = "(vertex buffer)";
			const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
			if( verts && stride >= 16 && vertCount > 0 && vertCount < 4096 )
			{
				float minX = 1.0e30f, minY = 1.0e30f, maxX = -1.0e30f, maxY = -1.0e30f;
				float minZ = 1.0e30f, maxZ = -1.0e30f, minW = 1.0e30f, maxW = -1.0e30f;
				const auto *bytes = static_cast< const unsigned char * >( verts );
				for( unsigned i = 0; i < vertCount; ++i )
				{
					const float *v = reinterpret_cast< const float * >( bytes + i * stride );
					minX = ( std::min )( minX, v[ 0 ] ); maxX = ( std::max )( maxX, v[ 0 ] );
					minY = ( std::min )( minY, v[ 1 ] ); maxY = ( std::max )( maxY, v[ 1 ] );
					minZ = ( std::min )( minZ, v[ 2 ] ); maxZ = ( std::max )( maxZ, v[ 2 ] );
					minW = ( std::min )( minW, v[ 3 ] ); maxW = ( std::max )( maxW, v[ 3 ] );
				}
				_snprintf_s( box, _TRUNCATE, "box (%.0f,%.0f)-(%.0f,%.0f) z %.4f..%.4f rhw %.4f..%.4f", minX, minY, maxX, maxY, minZ, maxZ,
					minW, maxW );
			}
			DWORD blend = 0, src = 0, dst = 0, zEnable = 0;
			device->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
			device->GetRenderState( D3DRS_SRCBLEND, &src );
			device->GetRenderState( D3DRS_DESTBLEND, &dst );
			device->GetRenderState( D3DRS_ZENABLE, &zEnable );
			static const char *const kWhere[] = { "eye pass", "pass B/UI", "other" };
			++s_effectLogs;
			PSU_LOG( "VREffect: new #%u %s%s ret=%p prim=%u x%u fvf=0x%X tex=%ux%u rt=%d blend=%lu %lu/%lu z=%lu %s | camera state %d\n",
				id, kWhere[ where < 0 || where > 2 ? 2 : where ], tech ? " TECH" : "", ret, primitiveType, primitiveCount, fvf,
				texW, texH, texRT, blend, src, dst, zEnable, box, VRCamera::CameraState() );
		}
		if( kind )
		{
			++kind->count;
		}
		// Every 10 s: how often each kind was drawn (only kinds drawn in that time).
		if( now - s_effectSummaryTick >= 10000 )
		{
			s_effectSummaryTick = now;
			char line[ 512 ];
			int len = sprintf_s( line, "VREffect: last 10 s:" );
			for( unsigned i = 0; i < s_effectKindCount && len < 440; ++i )
			{
				if( s_effectKinds[ i ].count )
				{
					len += _snprintf_s( line + len, sizeof( line ) - len, _TRUNCATE, " #%u=%u", i, s_effectKinds[ i ].count );
					s_effectKinds[ i ].count = 0;
				}
			}
			++s_effectLogs;
			PSU_LOG( "%s\n", line );
		}
#else
		( void )device; ( void )where; ( void )primitiveType; ( void )primitiveCount; ( void )verts; ( void )stride; ( void )ret;
#endif
	}

	bool StereoRenderer::RecenterCameraAnchored( const D3DMATRIX &world, D3DMATRIX &out, void *ret )
	{
#ifdef _DEBUG
		if( *reinterpret_cast< const int * >( 0x009946C8 ) == 0x12 )
		{
			++s_skyWorlds[ s_inEyePass ? 1 : 0 ];
		}
#endif
		if( !s_inEyePass )
		{
			return false;
		}
		StereoRenderer &self = Get();
		const int eye = s_passEyeIndex & 1;
		if( !self.m_haveInvGameView || !self.m_haveEyePosition[ eye ] )
		{
			return false;
		}
		// The sky layer (PSU_DrawLayer keeps the layer being drawn at 0x009946C8): world matrices are
		// in game camera space, so moving the model by the eye's offset from the camera means the eye
		// sees it from the camera's spot with its own rotation (a skybox: infinitely far).
		constexpr int kSkyLayer = 0x12;
		if( Config::Get().vr.m_skyAtEye && *reinterpret_cast< const int * >( 0x009946C8 ) == kSkyLayer )
		{
			out = world;
			out._41 += self.m_invEyeView[ eye ]._41;
			out._42 += self.m_invEyeView[ eye ]._42;
			out._43 += self.m_invEyeView[ eye ]._43;
#ifdef _DEBUG
			++s_skyMoved;
#endif
			return true;
		}
		if( !Config::Get().vr.m_recenterSky )
		{
			return false;
		}
		// The game camera's position: the inverse view's translation.
		const float dx = world._41 - self.m_invGameView._41;
		const float dy = world._42 - self.m_invGameView._42;
		const float dz = world._43 - self.m_invGameView._43;
		constexpr float kAnchoredUnits = 0.05f;  // exactly at the camera, give or take float noise
		if( dx * dx + dy * dy + dz * dz > kAnchoredUnits * kAnchoredUnits )
		{
#ifdef _DEBUG
			// Diagnostics: models within 20 units (2 m) of the camera, once per draw site, with
			// where they sit in camera space (overlays placed just in front of the camera).
			static void *s_nearSites[ 64 ]{};
			static unsigned s_nearCount = 0;
			if( dx * dx + dy * dy + dz * dz < 400.0f && s_nearCount < 64 )
			{
				bool seen = false;
				for( unsigned i = 0; i < s_nearCount; ++i )
				{
					seen = seen || s_nearSites[ i ] == ret;
				}
				if( !seen )
				{
					s_nearSites[ s_nearCount++ ] = ret;
					D3DMATRIX inCamera{};
					MulMatrix( world, self.m_gameView, inCamera );
					PSU_LOG( "Stereo: model near the camera ret=%p, in camera [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f] | camera state %d\n",
						ret, inCamera._11, inCamera._12, inCamera._13, inCamera._21, inCamera._22, inCamera._23, inCamera._31,
						inCamera._32, inCamera._33, inCamera._41, inCamera._42, inCamera._43, VRCamera::CameraState() );
				}
			}
#endif
			return false;
		}
		// Camera-locked: world -> game camera space, enlarged across the view (the headset sees
		// far wider than the game's camera), then from this eye into the world.
		constexpr float kWiden = 3.0f;
		D3DMATRIX inCamera{}, widened{};
		MulMatrix( world, self.m_gameView, inCamera );
		widened = inCamera;
		for( int c = 0; c < 4; ++c )
		{
			widened.m[ c ][ 0 ] *= kWiden;
			widened.m[ c ][ 1 ] *= kWiden;
		}
		MulMatrix( widened, self.m_invEyeView[ eye ], out );
#ifdef _DEBUG
		static void *s_lockedSites[ 32 ]{};
		static unsigned s_lockedCount = 0;
		bool lockedSeen = s_lockedCount >= 32;
		for( unsigned i = 0; i < s_lockedCount; ++i )
		{
			lockedSeen = lockedSeen || s_lockedSites[ i ] == ret;
		}
		if( !lockedSeen )
		{
			s_lockedSites[ s_lockedCount++ ] = ret;
			const float scale = std::sqrt( world._11 * world._11 + world._12 * world._12 + world._13 * world._13 );
			PSU_LOG( "Stereo: camera-locked model (scale %.1f) kept on eye %d, ret=%p | in camera [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f]\n",
				scale, eye, ret, inCamera._11, inCamera._12, inCamera._13, inCamera._21, inCamera._22, inCamera._23,
				inCamera._31, inCamera._32, inCamera._33, inCamera._41, inCamera._42, inCamera._43 );
		}
#else
		( void )ret;
#endif
		return true;
	}

	const void *StereoRenderer::ReprojectSpriteIfNeeded( IDirect3DDevice9 *device, unsigned primitiveType,
		unsigned primitiveCount, const void *verts, unsigned stride, void *ret )
	{
		// The game's 2D sprite draw; HUD pieces come through it too, but those are drawn in pass B
		// (not an eye pass) or carry no projected depth.
		constexpr uintptr_t kSpriteDraw = 0x00747248;
#ifdef _DEBUG
		// A projected-depth XYZRHW draw in an eye pass from any other site: a sprite we fail to recognise?
		if( s_inEyePass && device && verts && stride >= 16 && reinterpret_cast< uintptr_t >( ret ) != kSpriteDraw )
		{
			DWORD dfvf = 0;
			const float *v0 = static_cast< const float * >( verts );
			if( SUCCEEDED( device->GetFVF( &dfvf ) ) && ( dfvf & D3DFVF_POSITION_MASK ) == D3DFVF_XYZRHW && v0[ 2 ] > 0.0f &&
				v0[ 2 ] < 1.0f && v0[ 3 ] > 0.0f )
			{
				static void *s_seen[ 64 ]{};
				static unsigned s_seenCount = 0;
				bool seen = s_seenCount >= 64;
				for( unsigned i = 0; i < s_seenCount && !seen; ++i )
				{
					seen = s_seen[ i ] == ret;
				}
				if( !seen )
				{
					s_seen[ s_seenCount++ ] = ret;
					PSU_LOG( "Stereo: eye XYZRHW draw with projected z from ret=%p (not the sprite site) prims=%u z=%.3f\n", ret,
						primitiveCount, v0[ 2 ] );
				}
			}
		}
#endif
		if( !s_inEyePass || !device || !verts || reinterpret_cast< uintptr_t >( ret ) != kSpriteDraw ||
			!Config::Get().vr.m_reprojectSprites || stride < 16 || s_gameW < 1 || s_gameH < 1 || s_eyeW < 1 || s_eyeH < 1 )
		{
			return nullptr;
		}
		StereoRenderer &self = Get();
		if( !self.m_haveEyeMatrices || !self.m_haveInvGameView )
		{
			return nullptr;
		}
		DWORD fvf = 0;
		if( FAILED( device->GetFVF( &fvf ) ) || ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return nullptr;
		}
		const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
		if( vertCount < 3 || vertCount * stride > sizeof( s_spriteBuf ) )
		{
			return nullptr;
		}
		// Projected depth only (HUD sprites use z 0); skip garbage (off-screen sentinels).
		const auto *bytes = static_cast< const unsigned char * >( verts );
		for( unsigned i = 0; i < vertCount; ++i )
		{
			const float *v = reinterpret_cast< const float * >( bytes + i * stride );
			if( !( v[ 2 ] > 0.0f && v[ 2 ] < 1.0f ) || !std::isfinite( v[ 0 ] ) || !std::isfinite( v[ 1 ] ) ||
				std::fabs( v[ 0 ] ) > 1.0e5f || std::fabs( v[ 1 ] ) > 1.0e5f || !( v[ 3 ] > 0.0f ) )
			{
				return nullptr;
			}
		}
		const int eye = s_passEyeIndex & 1;
		const float gw = static_cast< float >( s_gameW );
		const float gh = static_cast< float >( s_gameH );
		const float ew = static_cast< float >( s_eyeW );
		const float eh = static_cast< float >( s_eyeH );
		std::memcpy( s_spriteBuf, verts, vertCount * stride );
		bool behind = false;
		for( unsigned i = 0; i < vertCount; ++i )
		{
			float *xyzw = reinterpret_cast< float * >( s_spriteBuf + i * stride );
			// Flat screen -> game camera -> world (homogeneous) -> this eye.
			const float w = 1.0f / xyzw[ 3 ];
			const float clip[ 4 ] = { ( ( xyzw[ 0 ] / gw ) * 2.0f - 1.0f ) * w, ( 1.0f - ( xyzw[ 1 ] / gh ) * 2.0f ) * w, xyzw[ 2 ] * w, w };
			float cam[ 4 ]{}, world[ 4 ]{}, view[ 4 ]{}, eyeClip[ 4 ]{};
			MulVec4Mat( clip, self.m_invGameProj, cam );
			MulVec4Mat( cam, self.m_invGameView, world );
			MulVec4Mat( world, self.m_eyeView[ eye ], view );
			MulVec4Mat( view, self.m_eyeProj[ eye ], eyeClip );
			if( !( eyeClip[ 3 ] > 1.0e-6f ) )
			{
				behind = true;
				break;
			}
			const float invW = 1.0f / eyeClip[ 3 ];
			xyzw[ 0 ] = ( eyeClip[ 0 ] * invW * 0.5f + 0.5f ) * ew;
			xyzw[ 1 ] = ( 0.5f - eyeClip[ 1 ] * invW * 0.5f ) * eh;
			xyzw[ 2 ] = eyeClip[ 2 ] * invW;
			xyzw[ 3 ] = invW;
		}
		if( behind )
		{
			// Behind this eye: nothing to draw (all vertices off screen).
			for( unsigned i = 0; i < vertCount; ++i )
			{
				float *xyzw = reinterpret_cast< float * >( s_spriteBuf + i * stride );
				xyzw[ 0 ] = -100000.0f;
				xyzw[ 1 ] = -100000.0f;
				xyzw[ 2 ] = 0.5f;
				xyzw[ 3 ] = 1.0f;
			}
		}
#ifdef _DEBUG
		static unsigned s_logs = 0;
		if( s_logs < 20 )
		{
			++s_logs;
			PSU_LOG( "Stereo: sprite re-projected for eye %d (%u verts%s)\n", eye, vertCount, behind ? ", behind the eye" : "" );
		}
#endif
		return s_spriteBuf;
	}

	const void *StereoRenderer::StretchScreenQuadIfNeeded( IDirect3DDevice9 *device, unsigned primitiveType,
		unsigned primitiveCount, const void *verts, unsigned stride, void *ret )
	{
		if( !verts || stride < 16 || stride > kMaxStretchStride || !device )
		{
			return nullptr;
		}
		if( !s_inEyePass || s_gameW < 1 || s_gameH < 1 || s_eyeW < 1 || s_eyeH < 1 )
		{
			return nullptr;
		}
		if( s_gameW == s_eyeW && s_gameH == s_eyeH )
		{
			return nullptr;
		}
		if( !EyeColorTargetBound( device ) )
		{
			return nullptr;
		}

		DWORD fvf = 0;
		if( FAILED( device->GetFVF( &fvf ) ) || ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return nullptr;
		}

		unsigned vertCount = 0;
		switch( static_cast< D3DPRIMITIVETYPE >( primitiveType ) )
		{
			case D3DPT_TRIANGLELIST: vertCount = primitiveCount * 3; break;
			case D3DPT_TRIANGLESTRIP:
			case D3DPT_TRIANGLEFAN: vertCount = primitiveCount + 2; break;
			case D3DPT_LINELIST: vertCount = primitiveCount * 2; break;
			case D3DPT_LINESTRIP: vertCount = primitiveCount + 1; break;
			case D3DPT_POINTLIST: vertCount = primitiveCount; break;
			default: return nullptr;
		}
		if( vertCount < 3 || vertCount > kMaxStretchVerts )
		{
			return nullptr;
		}
		if( vertCount * stride > sizeof( s_stretchQuadBuf ) )
		{
			return nullptr;
		}

		float minX = 1.0e30f, minY = 1.0e30f, maxX = -1.0e30f, maxY = -1.0e30f;
		const auto *bytes = static_cast< const unsigned char * >( verts );
		for( unsigned i = 0; i < vertCount; ++i )
		{
			const float *xy = reinterpret_cast< const float * >( bytes + i * stride );
			minX = ( std::min )( minX, xy[ 0 ] );
			maxX = ( std::max )( maxX, xy[ 0 ] );
			minY = ( std::min )( minY, xy[ 1 ] );
			maxY = ( std::max )( maxY, xy[ 1 ] );
		}
		const float boxW = maxX - minX;
		const float boxH = maxY - minY;
		const bool fullscreen = boxW >= 0.8f * static_cast< float >( s_gameW ) && boxH >= 0.8f * static_cast< float >( s_gameH );

		// Dressing room background (camera state 5): tiles of plain textures (512x512 seen),
		// any size, fixed in the room.
		if( VRCamera::CameraState() == 5 )
		{
			bool plainTexture = false;
			IDirect3DBaseTexture9 *stage0 = nullptr;
			if( SUCCEEDED( device->GetTexture( 0, &stage0 ) ) && stage0 )
			{
				IDirect3DTexture9 *tex9 = nullptr;
				stage0->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast< void ** >( &tex9 ) );
				stage0->Release();
				if( tex9 )
				{
					D3DSURFACE_DESC desc{};
					tex9->GetLevelDesc( 0, &desc );
					plainTexture = ( desc.Usage & D3DUSAGE_RENDERTARGET ) == 0;
					tex9->Release();
				}
			}
			float center[ 3 ]{};
			float orientation[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };
			if( plainTexture )
			{
				// Only the dressing room draws these tiles in camera state 5 (NPC talk doesn't).
				VRCamera::NoteDressingRoom();
			}
			if( plainTexture && VRCamera::HasReference() )
			{
				VRCamera::PanelPose( kBackdropDistance, 0.0f, center, orientation );
				std::memcpy( s_stretchQuadBuf, verts, vertCount * stride );
				bool mapped = true;
				for( unsigned i = 0; i < vertCount && mapped; ++i )
				{
					float *xyzw = reinterpret_cast< float * >( s_stretchQuadBuf + i * stride );
					float sx = 0.0f, sy = 0.0f, rhw = 1.0f;
					mapped = MapBackdropVertex( xyzw[ 0 ], xyzw[ 1 ], center, orientation, sx, sy, rhw );
					xyzw[ 0 ] = sx;
					xyzw[ 1 ] = sy;
					xyzw[ 2 ] = 0.9999f;  // behind the character and the menu
					xyzw[ 3 ] = rhw;
				}
				if( mapped )
				{
					static bool s_logged = false;
					if( !s_logged )
					{
						s_logged = true;
						PSU_LOG( "Stereo: dressing room background fixed in the room (%u verts)\n", vertCount );
					}
					return s_stretchQuadBuf;
				}
				// A corner behind the eye (looking far to the side): drop the tile rather than
				// drawing it head-locked. All vertices off screen = nothing rasterised.
				for( unsigned i = 0; i < vertCount; ++i )
				{
					float *xyzw = reinterpret_cast< float * >( s_stretchQuadBuf + i * stride );
					xyzw[ 0 ] = -100000.0f;
					xyzw[ 1 ] = -100000.0f;
					xyzw[ 2 ] = 0.9999f;
					xyzw[ 3 ] = 1.0f;
				}
				return s_stretchQuadBuf;
			}
		}

		// Eye-pass tint: only near-fullscreen quads.
		if( !fullscreen )
		{
			return nullptr;
		}

		const float sx = static_cast< float >( s_eyeW ) / static_cast< float >( s_gameW );
		const float sy = static_cast< float >( s_eyeH ) / static_cast< float >( s_gameH );
		std::memcpy( s_stretchQuadBuf, verts, vertCount * stride );
		for( unsigned i = 0; i < vertCount; ++i )
		{
			float *xy = reinterpret_cast< float * >( s_stretchQuadBuf + i * stride );
			xy[ 0 ] *= sx;
			xy[ 1 ] *= sy;
		}

		if( s_stretchQuadLogCount < 8 )
		{
			++s_stretchQuadLogCount;
			PSU_LOG( "StereoStretchQuad: game=%ux%u eye=%ux%u verts=%u box=%.0fx%.0f ret=%p\n",
				s_gameW, s_gameH, s_eyeW, s_eyeH, vertCount, boxW, boxH, ret );
		}
		return s_stretchQuadBuf;
	}

	bool StereoRenderer::DrawNameplateToEyes( IDirect3DDevice9 *device, unsigned primitiveType,
		unsigned primitiveCount, const void *verts, unsigned stride )
	{
		if( s_inNameplateDraw || s_inEyePass || !device || !verts || !m_hasFrame || !m_haveEyeMatrices )
		{
			return false;
		}
		if( stride < 16 || stride > kMaxStretchStride )
		{
			return false;
		}
		const unsigned vertCount = VertCountForPrimitive( primitiveType, primitiveCount );
		if( vertCount < 3 || vertCount > kMaxStretchVerts || vertCount * stride > sizeof( s_nameplateVertBuf ) )
		{
			return false;
		}
		D3D11Bridge *bridge = System::Get().StereoBridge();
		if( !bridge || m_lastEyeW < 1 || m_lastEyeH < 1 || m_lastGameW < 1 || m_lastGameH < 1 )
		{
			return false;
		}

		DWORD fvf = 0;
		device->GetFVF( &fvf );
		if( ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW )
		{
			return false;
		}
		bool onMonitor = false;

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

		device->SetRenderState( D3DRS_ZENABLE, FALSE );
		device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
		device->SetDepthStencilSurface( nullptr );

		const float gw = static_cast< float >( m_lastGameW );
		const float gh = static_cast< float >( m_lastGameH );
		const float ew = static_cast< float >( m_lastEyeW );
		const float eh = static_cast< float >( m_lastEyeH );

		s_inNameplateDraw = true;
		for( int eye = 0; eye < 2; ++eye )
		{
			IDirect3DSurface9 *eyeSurf = bridge->EyeSurface( eye );
			if( !eyeSurf )
			{
				continue;
			}

			std::memcpy( s_nameplateVertBuf, verts, vertCount * stride );
			for( unsigned i = 0; i < vertCount; ++i )
			{
				float *xyzw = reinterpret_cast< float * >( s_nameplateVertBuf + i * stride );
				const float sx = xyzw[ 0 ];
				const float sy = xyzw[ 1 ];
				const float sz = xyzw[ 2 ];
				float rhw = xyzw[ 3 ];
				if( rhw > -1.0e-8f && rhw < 1.0e-8f )
				{
					continue;
				}
				const float w = 1.0f / rhw;
				const float ndcX = ( sx / gw ) * 2.0f - 1.0f;
				const float ndcY = 1.0f - ( sy / gh ) * 2.0f;
				const float clipIn[ 4 ] = { ndcX * w, ndcY * w, sz * w, w };
				float cam[ 4 ]{};
				MulVec4Mat( clipIn, m_invGameProj, cam );
				float viewClip[ 4 ]{};
				MulVec4Mat( cam, m_eyeView[ eye ], viewClip );
				float eyeClip[ 4 ]{};
				MulVec4Mat( viewClip, m_eyeProj[ eye ], eyeClip );
				if( eyeClip[ 3 ] > -1.0e-8f && eyeClip[ 3 ] < 1.0e-8f )
				{
					continue;
				}
				const float invW = 1.0f / eyeClip[ 3 ];
				xyzw[ 0 ] = ( eyeClip[ 0 ] * invW * 0.5f + 0.5f ) * ew;
				xyzw[ 1 ] = ( 0.5f - eyeClip[ 1 ] * invW * 0.5f ) * eh;
				xyzw[ 2 ] = eyeClip[ 2 ] * invW;
				xyzw[ 3 ] = invW;
			}

			// Grow around the remapped AABB center so tags stay above the avatar.
			float uiScale = Config::Get().vr.m_uiScale;
			if( uiScale < 1.0f )
			{
				uiScale = 1.0f;
			}
			else if( uiScale > 2.5f )
			{
				uiScale = 2.5f;
			}
			if( uiScale > 1.001f )
			{
				float minX = 1.0e30f, minY = 1.0e30f, maxX = -1.0e30f, maxY = -1.0e30f;
				for( unsigned i = 0; i < vertCount; ++i )
				{
					const float *xy = reinterpret_cast< const float * >( s_nameplateVertBuf + i * stride );
					minX = ( std::min )( minX, xy[ 0 ] );
					maxX = ( std::max )( maxX, xy[ 0 ] );
					minY = ( std::min )( minY, xy[ 1 ] );
					maxY = ( std::max )( maxY, xy[ 1 ] );
				}
				const float cx = ( minX + maxX ) * 0.5f;
				const float cy = ( minY + maxY ) * 0.5f;
				for( unsigned i = 0; i < vertCount; ++i )
				{
					float *xy = reinterpret_cast< float * >( s_nameplateVertBuf + i * stride );
					xy[ 0 ] = cx + ( xy[ 0 ] - cx ) * uiScale;
					xy[ 1 ] = cy + ( xy[ 1 ] - cy ) * uiScale;
				}
			}

			device->SetRenderTarget( 0, eyeSurf );
			D3DVIEWPORT9 eyeVp{ 0, 0, m_lastEyeW, m_lastEyeH, 0.0f, 1.0f };
			device->SetViewport( &eyeVp );
			RECT scissor{ 0, 0, static_cast< LONG >( m_lastEyeW ), static_cast< LONG >( m_lastEyeH ) };
			device->SetScissorRect( &scissor );
			device->DrawPrimitiveUP( static_cast< D3DPRIMITIVETYPE >( primitiveType ),
				primitiveCount, s_nameplateVertBuf, stride );

			// flatMirrorEye: the monitor shows the left eye, so the plate goes where that eye has it,
			// through the same crop, instead of the flat camera's place.
			const LONG srcW = m_mirrorSrc.right - m_mirrorSrc.left;
			const LONG srcH = m_mirrorSrc.bottom - m_mirrorSrc.top;
			if( eye == 0 && m_mirrorActive && savedRt && savedRt == m_mirrorTarget && srcW > 0 && srcH > 0 )
			{
				const float sx = static_cast< float >( m_mirrorDst.right - m_mirrorDst.left ) / srcW;
				const float sy = static_cast< float >( m_mirrorDst.bottom - m_mirrorDst.top ) / srcH;
				for( unsigned i = 0; i < vertCount; ++i )
				{
					float *xy = reinterpret_cast< float * >( s_nameplateVertBuf + i * stride );
					xy[ 0 ] = m_mirrorDst.left + ( xy[ 0 ] - m_mirrorSrc.left ) * sx;
					xy[ 1 ] = m_mirrorDst.top + ( xy[ 1 ] - m_mirrorSrc.top ) * sy;
				}
				device->SetRenderTarget( 0, savedRt );
				const D3DVIEWPORT9 monitorVp{ static_cast< DWORD >( m_mirrorDst.left ), static_cast< DWORD >( m_mirrorDst.top ),
					static_cast< DWORD >( m_mirrorDst.right - m_mirrorDst.left ), static_cast< DWORD >( m_mirrorDst.bottom - m_mirrorDst.top ),
					0.0f, 1.0f };
				device->SetViewport( &monitorVp );
				device->SetScissorRect( &m_mirrorDst );
				onMonitor = SUCCEEDED( device->DrawPrimitiveUP( static_cast< D3DPRIMITIVETYPE >( primitiveType ),
					primitiveCount, s_nameplateVertBuf, stride ) );
			}
		}
		s_inNameplateDraw = false;

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

		if( !m_loggedNameplate )
		{
			m_loggedNameplate = true;
			PSU_LOG( "VR: Pass B nameplates/bubbles remapped to stereo eyes\n" );
		}
		return onMonitor;
	}

	bool StereoRenderer::SceneAllowsStereo()
	{
		const auto &vr = Config::Get().vr;
		if( !vr.m_enabled || !vr.m_stereo || vr.m_paused )
		{
			return false;
		}
		const unsigned sceneMode = *reinterpret_cast< unsigned * >( 0x00B90EA8 );
		return vr.m_stereoMode == 0 || sceneMode == vr.m_stereoMode;
	}

	bool StereoRenderer::CanRenderEyes( IDirect3DDevice9 *device ) const
	{
		EyePose views[ 2 ];
		EyePose viewSpace[ 2 ];
		return device && SceneAllowsStereo() && System::Get().StereoBridge() && System::Get().NextViews( views, viewSpace );
	}

	bool StereoRenderer::MirrorLeftEye( IDirect3DDevice9 *device )
	{
		D3D11Bridge *bridge = System::Get().StereoBridge();
		IDirect3DSurface9 *eye = bridge ? bridge->EyeSurface( 0 ) : nullptr;
		if( !device || !eye || m_lastEyeW == 0 || m_lastEyeH == 0 )
		{
			return false;
		}
		IDirect3DSurface9 *gameTarget = nullptr;
		if( FAILED( device->GetRenderTarget( 0, &gameTarget ) ) || !gameTarget )
		{
			return false;
		}
		D3DVIEWPORT9 vp{};
		device->GetViewport( &vp );
		if( vp.Width == 0 || vp.Height == 0 )
		{
			vp.X = vp.Y = 0;
			vp.Width = m_lastGameW;
			vp.Height = m_lastGameH;
		}
		// Centre crop of the eye image to the viewport's aspect (the eye buffer is near square).
		RECT src{ 0, 0, static_cast< LONG >( m_lastEyeW ), static_cast< LONG >( m_lastEyeH ) };
		const float targetAspect = static_cast< float >( vp.Width ) / static_cast< float >( vp.Height );
		const float eyeAspect = static_cast< float >( m_lastEyeW ) / static_cast< float >( m_lastEyeH );
		if( eyeAspect < targetAspect )
		{
			const LONG h = static_cast< LONG >( m_lastEyeW / targetAspect );
			src.top = ( static_cast< LONG >( m_lastEyeH ) - h ) / 2;
			src.bottom = src.top + h;
		}
		else
		{
			const LONG w = static_cast< LONG >( m_lastEyeH * targetAspect );
			src.left = ( static_cast< LONG >( m_lastEyeW ) - w ) / 2;
			src.right = src.left + w;
		}
		const RECT dst{ static_cast< LONG >( vp.X ), static_cast< LONG >( vp.Y ),
			static_cast< LONG >( vp.X + vp.Width ), static_cast< LONG >( vp.Y + vp.Height ) };
		const HRESULT hr = device->StretchRect( eye, &src, gameTarget, &dst, D3DTEXF_LINEAR );
		m_mirrorTarget = gameTarget;  // identity only, not kept alive
		gameTarget->Release();
		m_mirrorActive = SUCCEEDED( hr );
		m_mirrorSrc = src;
		m_mirrorDst = dst;

		// Pass B (nameplates, HUD) depth-tests against what the flat pass would have left.
		IDirect3DSurface9 *gameDepth = nullptr;
		DWORD depthFlags = 0;
		if( SUCCEEDED( device->GetDepthStencilSurface( &gameDepth ) ) && gameDepth )
		{
			D3DSURFACE_DESC depthDesc{};
			gameDepth->GetDesc( &depthDesc );
			depthFlags = D3DCLEAR_ZBUFFER | ( HasStencil( depthDesc.Format ) ? D3DCLEAR_STENCIL : 0 );
			device->Clear( 0, nullptr, depthFlags, 0, 1.0f, 0 );
			gameDepth->Release();
		}

		if( !m_loggedMirror || FAILED( hr ) )
		{
			static unsigned s_failLogs = 0;
			if( !m_loggedMirror || s_failLogs++ < 8 )
			{
				PSU_LOG( "Stereo: monitor mirrors the left eye, src [%ld %ld %ld %ld] of %ux%u -> viewport %u,%u %ux%u (game RT %ux%u), "
					"depth clear 0x%X, hr 0x%08X\n", src.left, src.top, src.right, src.bottom, m_lastEyeW, m_lastEyeH,
					vp.X, vp.Y, vp.Width, vp.Height, m_lastGameW, m_lastGameH, depthFlags, static_cast< unsigned >( hr ) );
			}
			m_loggedMirror = true;
		}
		return SUCCEEDED( hr );
	}

	bool StereoRenderer::RenderEyes( IDirect3DDevice9 *device, Pass pass )
	{
		const auto &vr = Config::Get().vr;
		if( !device || !pass || !SceneAllowsStereo() )
		{
			return false;
		}

		EyePose views[ 2 ];
		EyePose viewSpace[ 2 ];
		D3D11Bridge *bridge = System::Get().StereoBridge();
		if( !bridge || !System::Get().NextViews( views, viewSpace ) )
		{
			return false;
		}
		if( !VRCamera::HasReference() || VRCamera::ConsumeRecenterRequest() )
		{
			VRCamera::Recenter( views );
		}

		IDirect3DSurface9 *gameTarget = nullptr;
		IDirect3DSurface9 *gameDepth = nullptr;
		if( FAILED( device->GetRenderTarget( 0, &gameTarget ) ) || !gameTarget )
		{
			return false;
		}
		bool drawn = false;
		device->GetDepthStencilSurface( &gameDepth );

		D3DSURFACE_DESC targetDesc{};
		gameTarget->GetDesc( &targetDesc );
		D3DFORMAT depthFormat = D3DFMT_D24S8;
		if( gameDepth )
		{
			D3DSURFACE_DESC depthDesc{};
			gameDepth->GetDesc( &depthDesc );
			depthFormat = depthDesc.Format;
		}

		// Prefer the OpenXR eye swapchain size so FOV maps 1:1 (stretching a 16:9
		// game RT into a near-square eye buffer warps stereo and looks like ghosts).
		UINT eyeW = targetDesc.Width;
		UINT eyeH = targetDesc.Height;
		uint32_t xrW = 0, xrH = 0;
		if( System::Get().EyeSwapchainSize( xrW, xrH ) )
		{
			eyeW = xrW;
			eyeH = xrH;
		}

		if( bridge->EnsureEyeTargets( device, eyeW, eyeH, depthFormat ) )
		{
			D3DVIEWPORT9 gameViewport{};
			D3DMATRIX gameView{};
			D3DMATRIX gameProj{};
			device->GetViewport( &gameViewport );
			device->GetTransform( D3DTS_VIEW, &gameView );
			device->GetTransform( D3DTS_PROJECTION, &gameProj );

			const float unitsPerMeter = VRCamera::kGameUnitsPerMeter * ( vr.m_worldScale > 0.01f ? vr.m_worldScale : 1.0f );
			VRCamera::CaptureRenderCamera();
			const bool firstPerson = VRCamera::FirstPersonActive();
			// Bracket the local player's draw callback so its head can be cut in the eyes.
			VRPlayerDraw::Update( firstPerson );
			if( firstPerson )
			{
				VRCamera::UpdateFirstPersonAnchor( views );
			}
			const DWORD clearFlags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | ( HasStencil( depthFormat ) ? D3DCLEAR_STENCIL : 0 );

			m_gameProj = gameProj;
			m_haveEyeMatrices = InvertMatrix( gameProj, m_invGameProj );
			m_haveInvGameView = InvertMatrix( gameView, m_invGameView );
			m_gameView = gameView;
			const float vignette = vr.m_comfortVignette ? UpdateVignetteStrength() : 0.0f;

			// Free aim hides the local player from the layer draw; keep it for the eyes.
			VRPlayerDraw::BeginEyePasses();
			for( int eye = 0; eye < 2; ++eye )
			{
				VRPerf::Scope timer( eye ? VRPerf::Stage::EyeRight : VRPerf::Stage::EyeLeft );
				const VRPerf::Gpu gpuPass = eye ? VRPerf::Gpu::EyeRight : VRPerf::Gpu::EyeLeft;
				VRPerf::GpuBegin( device, gpuPass );
				D3DMATRIX eyeView{};
				D3DMATRIX eyeProj{};
				if( firstPerson )
				{
					VRCamera::BuildEyeFirstPerson( views[ eye ], unitsPerMeter, gameProj, eyeView, eyeProj );
				}
				else
				{
					VRCamera::BuildEye( views[ eye ], unitsPerMeter, gameProj, eyeView, eyeProj );
				}
				m_eyeView[ eye ] = eyeView;
				m_eyeProj[ eye ] = eyeProj;
				m_haveEyePosition[ eye ] = InvertMatrix( eyeView, m_invEyeView[ eye ] );

				IDirect3DSurface9 *eyeSurface = bridge->EyeSurface( eye );
				device->SetRenderTarget( 0, eyeSurface );
				device->SetDepthStencilSurface( bridge->EyeDepth() );
				D3DVIEWPORT9 eyeViewport{ 0, 0, eyeW, eyeH, 0.0f, 1.0f };
				device->SetViewport( &eyeViewport );
				device->Clear( 0, nullptr, clearFlags, D3DCOLOR_XRGB( 0, 0, 0 ), 1.0f, 0 );
				device->SetTransform( D3DTS_VIEW, &eyeView );
				device->SetTransform( D3DTS_PROJECTION, &eyeProj );

				SetPassEye( views[ eye ] );
				s_passEyeIndex = eye;
				BeginEyePass( eyeSurface, eyeW, eyeH, targetDesc.Width, targetDesc.Height );
				pass();
				EndEyePass();
				// After the eye pass, so the screen-quad hooks leave them alone.
				VRControllers::DrawMarkers( device, views[ eye ], eyeW, eyeH );
				DrawVignette( device, eyeW, eyeH, vignette );
				VRPerf::GpuEnd( device, gpuPass );

				m_poses[ eye ] = views[ eye ];
				m_viewSpacePoses[ eye ] = viewSpace[ eye ];
			}
			VRPlayerDraw::EndEyePasses();

			device->SetRenderTarget( 0, gameTarget );
			device->SetDepthStencilSurface( gameDepth );
			device->SetViewport( &gameViewport );
			device->SetTransform( D3DTS_VIEW, &gameView );
			device->SetTransform( D3DTS_PROJECTION, &gameProj );
			m_lastEyeW = eyeW;
			m_lastEyeH = eyeH;
			m_lastGameW = targetDesc.Width;
			m_lastGameH = targetDesc.Height;
			m_hasFrame = true;
			drawn = true;
			VRHud::Get().BeginStereoFrame( device, targetDesc.Width, targetDesc.Height );

			if( !m_loggedFirst )
			{
				m_loggedFirst = true;
				PSU_LOG( "Stereo: first eye pair %ux%u (game RT %ux%u), %.1f units/m, skipScreenQuads=%d, fovL=[%.2f %.2f %.2f %.2f]\n",
					eyeW, eyeH, targetDesc.Width, targetDesc.Height, unitsPerMeter,
					vr.m_stereoSkipScreenQuads ? 1 : 0,
					views[ 0 ].fovAngleLeft, views[ 0 ].fovAngleRight, views[ 0 ].fovAngleUp, views[ 0 ].fovAngleDown );
			}
		}

		gameTarget->Release();
		if( gameDepth )
		{
			gameDepth->Release();
		}
		return drawn;
	}
}
