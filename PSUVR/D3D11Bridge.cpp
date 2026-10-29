#include "D3D11Bridge.h"

#include "VRPerf.h"

#include "../Misc/Log.h"

#include <d3d9.h>
#include <d3d11.h>

namespace PSUVR
{
	bool D3D11Bridge::WaitGpu( IDirect3DDevice9 *d3d9 )
	{
		if( !d3d9 )
		{
			return false;
		}
		if( !m_eventQuery )
		{
			if( FAILED( d3d9->CreateQuery( D3DQUERYTYPE_EVENT, &m_eventQuery ) ) || !m_eventQuery )
			{
				return false;
			}
		}
		if( FAILED( m_eventQuery->Issue( D3DISSUE_END ) ) )
		{
			return false;
		}
		VRPerf::Scope timer( VRPerf::Stage::GpuWait );
		// Bounded wait — an infinite GetData loop freezes the game (not responding).
		const DWORD start = GetTickCount();
		for( ;; )
		{
			const HRESULT qhr = m_eventQuery->GetData( nullptr, 0, D3DGETDATA_FLUSH );
			if( qhr == S_OK )
			{
				return true;
			}
			if( qhr != S_FALSE )
			{
				return false;
			}
			if( ( GetTickCount() - start ) > 8 )
			{
				return true; // proceed; better a rare tear than a hard hang
			}
			Sleep( 0 );
		}
	}

	bool D3D11Bridge::RecreateShared( IDirect3DDevice9 *d3d9, ID3D11Device *d3d11, UINT width, UINT height )
	{
		Shutdown();
		m_d3d11 = d3d11;
		m_width = width;
		m_height = height;
		m_sharedHandle = nullptr;

		IDirect3DDevice9Ex *d3d9Ex = nullptr;
		HRESULT hr = d3d9->QueryInterface( __uuidof( IDirect3DDevice9Ex ), reinterpret_cast< void ** >( &d3d9Ex ) );
		if( FAILED( hr ) || !d3d9Ex )
		{
			PSU_LOG( "D3D11Bridge: device is not D3D9Ex (QI failed 0x%08X)\n", hr );
			return false;
		}

		hr = d3d9Ex->CreateTexture(
			width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
			&m_d3d9Texture, &m_sharedHandle );
		d3d9Ex->Release();

		if( FAILED( hr ) || !m_d3d9Texture || !m_sharedHandle )
		{
			PSU_LOG( "D3D11Bridge: CreateTexture(shared) failed 0x%08X\n", hr );
			Shutdown();
			return false;
		}

		hr = m_d3d9Texture->GetSurfaceLevel( 0, &m_d3d9Surface );
		if( FAILED( hr ) || !m_d3d9Surface )
		{
			PSU_LOG( "D3D11Bridge: GetSurfaceLevel failed 0x%08X\n", hr );
			Shutdown();
			return false;
		}

		hr = d3d11->OpenSharedResource( m_sharedHandle, __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &m_d3d11Texture ) );
		if( FAILED( hr ) || !m_d3d11Texture )
		{
			PSU_LOG( "D3D11Bridge: OpenSharedResource failed 0x%08X\n", hr );
			Shutdown();
			return false;
		}

		d3d9->CreateQuery( D3DQUERYTYPE_EVENT, &m_eventQuery );

		PSU_LOG( "D3D11Bridge: shared %ux%u handle=%p d3d11=%p\n", width, height, m_sharedHandle, m_d3d11Texture );
		return true;
	}

	bool D3D11Bridge::Init( IDirect3DDevice9 *d3d9, ID3D11Device *d3d11, UINT preferredWidth, UINT preferredHeight )
	{
		if( !d3d9 || !d3d11 )
		{
			return false;
		}

		UINT width = preferredWidth;
		UINT height = preferredHeight;
		if( width == 0 || height == 0 )
		{
			IDirect3DSurface9 *bb = nullptr;
			HRESULT hr = d3d9->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb );
			if( FAILED( hr ) || !bb )
			{
				PSU_LOG( "D3D11Bridge: GetBackBuffer failed 0x%08X\n", hr );
				return false;
			}
			D3DSURFACE_DESC desc{};
			bb->GetDesc( &desc );
			bb->Release();
			width = desc.Width;
			height = desc.Height;
		}

		if( width == 0 || height == 0 )
		{
			PSU_LOG( "D3D11Bridge: target size is zero\n" );
			return false;
		}

		return RecreateShared( d3d9, d3d11, width, height );
	}

	bool D3D11Bridge::EnsureSize( IDirect3DDevice9 *d3d9, UINT width, UINT height )
	{
		if( !d3d9 || !m_d3d11 || width == 0 || height == 0 )
		{
			return false;
		}
		if( Ready() && m_width == width && m_height == height )
		{
			return true;
		}
		return RecreateShared( d3d9, m_d3d11, width, height );
	}

	bool D3D11Bridge::CreateSharedTarget( IDirect3DDevice9 *d3d9, UINT width, UINT height, SharedTarget &out )
	{
		IDirect3DDevice9Ex *d3d9Ex = nullptr;
		if( FAILED( d3d9->QueryInterface( __uuidof( IDirect3DDevice9Ex ), reinterpret_cast< void ** >( &d3d9Ex ) ) ) || !d3d9Ex )
		{
			return false;
		}
		HANDLE shared = nullptr;
		HRESULT hr = d3d9Ex->CreateTexture( width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
			&out.texture9, &shared );
		d3d9Ex->Release();
		if( FAILED( hr ) || !out.texture9 || !shared )
		{
			PSU_LOG( "D3D11Bridge: eye CreateTexture(shared) failed 0x%08X\n", hr );
			ReleaseSharedTarget( out );
			return false;
		}
		if( FAILED( out.texture9->GetSurfaceLevel( 0, &out.surface9 ) ) || !out.surface9 )
		{
			ReleaseSharedTarget( out );
			return false;
		}
		hr = m_d3d11->OpenSharedResource( shared, __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &out.texture11 ) );
		if( FAILED( hr ) || !out.texture11 )
		{
			PSU_LOG( "D3D11Bridge: eye OpenSharedResource failed 0x%08X\n", hr );
			ReleaseSharedTarget( out );
			return false;
		}
		return true;
	}

	void D3D11Bridge::ReleaseSharedTarget( SharedTarget &target )
	{
		if( target.texture11 ) { target.texture11->Release(); target.texture11 = nullptr; }
		if( target.surface9 ) { target.surface9->Release(); target.surface9 = nullptr; }
		if( target.texture9 ) { target.texture9->Release(); target.texture9 = nullptr; }
	}

	void D3D11Bridge::ShutdownEyes()
	{
		for( auto &eye : m_eyes )
		{
			ReleaseSharedTarget( eye );
		}
		if( m_eyeDepth )
		{
			m_eyeDepth->Release();
			m_eyeDepth = nullptr;
		}
		m_eyeWidth = 0;
		m_eyeHeight = 0;
		m_eyeDepthFormat = D3DFMT_UNKNOWN;
	}

	void D3D11Bridge::ShutdownHud()
	{
		ReleaseSharedTarget( m_hudUser );
		ReleaseSharedTarget( m_hudWorld );
		m_hudWidth = 0;
		m_hudHeight = 0;
	}

	bool D3D11Bridge::EnsureHudTargets( IDirect3DDevice9 *d3d9, UINT width, UINT height )
	{
		if( !d3d9 || !m_d3d11 || width == 0 || height == 0 )
		{
			return false;
		}
		if( m_hudUser.texture11 && m_hudWorld.texture11 && m_hudWidth == width && m_hudHeight == height )
		{
			return true;
		}
		ShutdownHud();
		if( !CreateSharedTarget( d3d9, width, height, m_hudUser ) ||
			!CreateSharedTarget( d3d9, width, height, m_hudWorld ) )
		{
			ShutdownHud();
			return false;
		}
		m_hudWidth = width;
		m_hudHeight = height;
		PSU_LOG( "D3D11Bridge: HUD targets %ux%u (user + world)\n", width, height );
		return true;
	}

	bool D3D11Bridge::EnsureLabelTarget( IDirect3DDevice9 *d3d9, UINT width, UINT height )
	{
		if( m_label.texture11 )
		{
			return true;
		}
		if( !d3d9 || !m_d3d11 || !CreateSharedTarget( d3d9, width, height, m_label ) )
		{
			ReleaseSharedTarget( m_label );
			return false;
		}
		PSU_LOG( "D3D11Bridge: label target %ux%u\n", width, height );
		return true;
	}

	bool D3D11Bridge::EnsureEyeTargets( IDirect3DDevice9 *d3d9, UINT width, UINT height, D3DFORMAT depthFormat )
	{
		if( !d3d9 || !m_d3d11 || width == 0 || height == 0 )
		{
			return false;
		}
		if( m_eyeDepth && m_eyes[ 1 ].texture11 && m_eyeWidth == width && m_eyeHeight == height && m_eyeDepthFormat == depthFormat )
		{
			return true;
		}

		ShutdownEyes();
		for( auto &eye : m_eyes )
		{
			if( !CreateSharedTarget( d3d9, width, height, eye ) )
			{
				ShutdownEyes();
				return false;
			}
		}
		const HRESULT hr = d3d9->CreateDepthStencilSurface( width, height, depthFormat, D3DMULTISAMPLE_NONE, 0, FALSE, &m_eyeDepth, nullptr );
		if( FAILED( hr ) || !m_eyeDepth )
		{
			PSU_LOG( "D3D11Bridge: eye depth %ux%u fmt=%u failed 0x%08X\n", width, height, ( unsigned )depthFormat, hr );
			ShutdownEyes();
			return false;
		}
		m_eyeWidth = width;
		m_eyeHeight = height;
		m_eyeDepthFormat = depthFormat;
		PSU_LOG( "D3D11Bridge: eye targets %ux%u depth fmt=%u\n", width, height, ( unsigned )depthFormat );
		return true;
	}

	void D3D11Bridge::Shutdown()
	{
		ShutdownEyes();
		ShutdownHud();
		ReleaseSharedTarget( m_label );
		if( m_eventQuery )
		{
			m_eventQuery->Release();
			m_eventQuery = nullptr;
		}
		if( m_d3d11Texture )
		{
			m_d3d11Texture->Release();
			m_d3d11Texture = nullptr;
		}
		if( m_d3d9Surface )
		{
			m_d3d9Surface->Release();
			m_d3d9Surface = nullptr;
		}
		if( m_d3d9Texture )
		{
			m_d3d9Texture->Release();
			m_d3d9Texture = nullptr;
		}
		m_sharedHandle = nullptr;
		m_width = 0;
		m_height = 0;
		m_d3d11 = nullptr;
	}

	bool D3D11Bridge::BlitBackBuffer( IDirect3DDevice9 *d3d9 )
	{
		if( !d3d9 || !m_d3d9Surface )
		{
			return false;
		}

		IDirect3DSurface9 *bb = nullptr;
		HRESULT hr = d3d9->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb );
		if( FAILED( hr ) || !bb )
		{
			return false;
		}

		hr = d3d9->StretchRect( bb, nullptr, m_d3d9Surface, nullptr, D3DTEXF_LINEAR );
		bb->Release();
		if( FAILED( hr ) )
		{
			static bool s_logged = false;
			if( !s_logged )
			{
				s_logged = true;
				PSU_LOG( "D3D11Bridge: StretchRect failed 0x%08X\n", hr );
			}
			return false;
		}

		if( !WaitGpu( d3d9 ) )
		{
			static bool s_logged = false;
			if( !s_logged )
			{
				s_logged = true;
				PSU_LOG( "D3D11Bridge: GPU fence wait failed\n" );
			}
			return false;
		}
		return true;
	}
}
