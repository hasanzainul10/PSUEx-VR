#include "D3D9ExCompat.h"

#include "D3D9ExUpgrade.h"
#include "VRPerf.h"
#include "../Misc/Log.h"
#include "detours.h"

#include <atomic>
#include <d3d9.h>
#include <d3dx9.h>

namespace PSUVR
{
	using CreateTexture_t = HRESULT( APIENTRY * )(
		IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
		IDirect3DTexture9 **, HANDLE * );
	using CreateVolumeTexture_t = HRESULT( APIENTRY * )(
		IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
		IDirect3DVolumeTexture9 **, HANDLE * );
	using CreateCubeTexture_t = HRESULT( APIENTRY * )(
		IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
		IDirect3DCubeTexture9 **, HANDLE * );
	using CreateVertexBuffer_t = HRESULT( APIENTRY * )(
		IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL,
		IDirect3DVertexBuffer9 **, HANDLE * );
	using CreateIndexBuffer_t = HRESULT( APIENTRY * )(
		IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL,
		IDirect3DIndexBuffer9 **, HANDLE * );
	using SetTexture_t = HRESULT( APIENTRY * )(
		IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 * );

	using D3DXCreateTexture_t = HRESULT( WINAPI * )(
		LPDIRECT3DDEVICE9, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, LPDIRECT3DTEXTURE9 * );
	using D3DXCreateTextureFromFileInMemoryEx_t = HRESULT( WINAPI * )(
		LPDIRECT3DDEVICE9, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
		DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO *, PALETTEENTRY *, LPDIRECT3DTEXTURE9 * );
	using D3DXCreateTextureFromFileExA_t = HRESULT( WINAPI * )(
		LPDIRECT3DDEVICE9, LPCSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
		DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO *, PALETTEENTRY *, LPDIRECT3DTEXTURE9 * );
	using D3DXCreateTextureFromFileExW_t = HRESULT( WINAPI * )(
		LPDIRECT3DDEVICE9, LPCWSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
		DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO *, PALETTEENTRY *, LPDIRECT3DTEXTURE9 * );
	using D3DXCreateTextureFromFileA_t = HRESULT( WINAPI * )(
		LPDIRECT3DDEVICE9, LPCSTR, LPDIRECT3DTEXTURE9 * );
	using D3DXCreateTextureFromFileW_t = HRESULT( WINAPI * )(
		LPDIRECT3DDEVICE9, LPCWSTR, LPDIRECT3DTEXTURE9 * );
	using D3DXCreateTextureFromFileInMemory_t = HRESULT( WINAPI * )(
		LPDIRECT3DDEVICE9, LPCVOID, UINT, LPDIRECT3DTEXTURE9 * );

	static CreateTexture_t s_origCreateTexture = nullptr;
	static CreateVolumeTexture_t s_origCreateVolumeTexture = nullptr;
	static CreateCubeTexture_t s_origCreateCubeTexture = nullptr;
	static CreateVertexBuffer_t s_origCreateVertexBuffer = nullptr;
	static CreateIndexBuffer_t s_origCreateIndexBuffer = nullptr;
	static SetTexture_t s_origSetTexture = nullptr;

	// One orig set per hooked D3DX module (game uses _33, PSUEx uses _43).
	struct D3DXOrigs
	{
		HMODULE mod = nullptr;
		D3DXCreateTexture_t create = nullptr;
		D3DXCreateTextureFromFileInMemoryEx_t fromMemEx = nullptr;
		D3DXCreateTextureFromFileExA_t fromFileExA = nullptr;
		D3DXCreateTextureFromFileExW_t fromFileExW = nullptr;
		D3DXCreateTextureFromFileA_t fromFileA = nullptr;
		D3DXCreateTextureFromFileW_t fromFileW = nullptr;
		D3DXCreateTextureFromFileInMemory_t fromMem = nullptr;
	};

	static D3DXOrigs s_d3dx[ 4 ]{};
	static int s_d3dxCount = 0;

	static bool s_dynamicTextures = true;
	static bool s_createDetoured = false;
	static void *s_patchedVtables[ 8 ]{};
	static int s_patchedVtableCount = 0;

	static std::atomic< unsigned > s_remapTexture{ 0 };
	static std::atomic< unsigned > s_remapVolume{ 0 };
	static std::atomic< unsigned > s_remapCube{ 0 };
	static std::atomic< unsigned > s_remapVB{ 0 };
	static std::atomic< unsigned > s_remapIB{ 0 };
	static std::atomic< unsigned > s_remapD3DX{ 0 };
	static std::atomic< unsigned > s_d3dxCalls{ 0 };
	static std::atomic< unsigned > s_allCreates{ 0 };
	static std::atomic< unsigned > s_failLogged{ 0 };
	static std::atomic< unsigned > s_badSetTexture{ 0 };
	static std::atomic< bool > s_statsLogged{ false };

	static constexpr unsigned kMaxFailLogs = 64;
	static constexpr unsigned kMaxRemapDetailLogs = 16;

	static bool IsDxtFormat( D3DFORMAT format )
	{
		switch( format )
		{
		case D3DFMT_DXT1:
		case D3DFMT_DXT2:
		case D3DFMT_DXT3:
		case D3DFMT_DXT4:
		case D3DFMT_DXT5:
			return true;
		default:
			return false;
		}
	}

	// DDS header FourCC at byte 84 ("DDS " + DDSURFACEDESC2.ddspf.dwFourCC).
	static bool MemoryLooksLikeDxt( LPCVOID src, UINT size )
	{
		if( !src || size < 128 )
		{
			return false;
		}
		const auto *b = static_cast< const BYTE * >( src );
		if( b[ 0 ] != 'D' || b[ 1 ] != 'D' || b[ 2 ] != 'S' || b[ 3 ] != ' ' )
		{
			return false;
		}
		const DWORD fourCC = *reinterpret_cast< const DWORD * >( b + 84 );
		return fourCC == MAKEFOURCC( 'D', 'X', 'T', '1' ) ||
			fourCC == MAKEFOURCC( 'D', 'X', 'T', '2' ) ||
			fourCC == MAKEFOURCC( 'D', 'X', 'T', '3' ) ||
			fourCC == MAKEFOURCC( 'D', 'X', 'T', '4' ) ||
			fourCC == MAKEFOURCC( 'D', 'X', 'T', '5' );
	}

	// Ex rejects MANAGED. DXT must stay non-DYNAMIC; lockable formats (fonts/UI
	// atlases written with LockRect) need DEFAULT|DYNAMIC or glyphs stay blank.
	static void RemapManagedTexture( DWORD &usage, D3DPOOL &pool, D3DFORMAT format, bool forceDynamic )
	{
		if( pool != D3DPOOL_MANAGED )
		{
			return;
		}
		pool = D3DPOOL_DEFAULT;
		const bool dxt = IsDxtFormat( format );
		if( !dxt && s_dynamicTextures && ( forceDynamic || format != D3DFMT_UNKNOWN ) )
		{
			usage &= ~static_cast< DWORD >( D3DUSAGE_AUTOGENMIPMAP );
			usage |= D3DUSAGE_DYNAMIC;
		}
	}

	static void RemapManagedBuffer( DWORD &usage, D3DPOOL &pool )
	{
		if( pool != D3DPOOL_MANAGED )
		{
			return;
		}
		pool = D3DPOOL_DEFAULT;
		usage |= D3DUSAGE_DYNAMIC;
	}

	// Phase 4 probe: remapped buffers carry this private-data tag (their size); a Lock hook on the
	// VB / IB vtables (index 11) counts locks on tagged buffers only.
	static const GUID kRemapTag = { 0x5c1d7a40, 0x2b9e, 0x4f61, { 0x9a, 0x3c, 0x71, 0x0e, 0x84, 0x2d, 0x55, 0xb3 } };
	using BufferLock_t = HRESULT( APIENTRY * )( IDirect3DResource9 *, UINT, UINT, void **, DWORD );
	static BufferLock_t s_origVBLock = nullptr;
	static BufferLock_t s_origIBLock = nullptr;

	static void CountLock( IDirect3DResource9 *buffer, UINT size )
	{
		UINT length = 0;
		DWORD tagSize = sizeof( length );
		if( SUCCEEDED( buffer->GetPrivateData( kRemapTag, &length, &tagSize ) ) )
		{
			PSUVR::VRPerf::Count( PSUVR::VRPerf::Counter::BufferLock );
			PSUVR::VRPerf::Count( PSUVR::VRPerf::Counter::BufferLockKB, ( size ? size : length ) / 1024u );
		}
	}

	static HRESULT APIENTRY Hook_VBLock( IDirect3DResource9 *buffer, UINT offset, UINT size, void **data, DWORD flags )
	{
		CountLock( buffer, size );
		return s_origVBLock( buffer, offset, size, data, flags );
	}

	static HRESULT APIENTRY Hook_IBLock( IDirect3DResource9 *buffer, UINT offset, UINT size, void **data, DWORD flags )
	{
		CountLock( buffer, size );
		return s_origIBLock( buffer, offset, size, data, flags );
	}

	// Tags a remapped buffer and patches its vtable's Lock once (VB and IB have their own vtable).
	static void TagRemappedBuffer( IDirect3DResource9 *buffer, UINT length, BufferLock_t &orig, BufferLock_t hook )
	{
		if( !buffer )
		{
			return;
		}
		buffer->SetPrivateData( kRemapTag, &length, sizeof( length ), 0 );
		PSUVR::VRPerf::Count( PSUVR::VRPerf::Counter::BufferRemap );
		PSUVR::VRPerf::Count( PSUVR::VRPerf::Counter::BufferRemapKB, length / 1024u );
		void **vTable = *reinterpret_cast< void *** >( buffer );
		if( vTable[ 11 ] == reinterpret_cast< void * >( hook ) )
		{
			return;
		}
		DWORD oldProtect = 0;
		if( VirtualProtect( &vTable[ 11 ], sizeof( void * ), PAGE_EXECUTE_READWRITE, &oldProtect ) )
		{
			if( !orig )
			{
				orig = reinterpret_cast< BufferLock_t >( vTable[ 11 ] );
			}
			vTable[ 11 ] = reinterpret_cast< void * >( hook );
			VirtualProtect( &vTable[ 11 ], sizeof( void * ), oldProtect, &oldProtect );
			PSU_LOG( "D3D9ExCompat: buffer Lock counter on vtable %p\n", vTable );
		}
	}

	static void MaybeLogRemap( const char *kind, unsigned count, DWORD usage, D3DPOOL pool )
	{
		if( count <= kMaxRemapDetailLogs )
		{
			PSU_LOG( "D3D9ExCompat: remapped %s #%u -> pool=%u usage=0x%X\n",
				kind, count, static_cast< unsigned >( pool ), usage );
		}
	}

	static void LogCreateFail(
		const char *kind, HRESULT hr, DWORD usage, D3DFORMAT format, D3DPOOL pool )
	{
		const unsigned n = ++s_failLogged;
		if( n > kMaxFailLogs )
		{
			return;
		}
		PSU_LOG(
			"D3D9ExCompat: %s FAILED hr=0x%08X pool=%u usage=0x%X format=%d (%u/%u)\n",
			kind, hr, static_cast< unsigned >( pool ), usage, static_cast< int >( format ),
			n, kMaxFailLogs );
	}

	static HRESULT APIENTRY Hook_CreateTexture(
		IDirect3DDevice9 *device, UINT width, UINT height, UINT levels, DWORD usage,
		D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture, HANDLE *shared )
	{
		++s_allCreates;
		const bool remapped = ( pool == D3DPOOL_MANAGED );
		RemapManagedTexture( usage, pool, format, true );
		if( remapped )
		{
			MaybeLogRemap( "CreateTexture", ++s_remapTexture, usage, pool );
		}
		const HRESULT hr = s_origCreateTexture(
			device, width, height, levels, usage, format, pool, texture, shared );
		if( FAILED( hr ) )
		{
			LogCreateFail( "CreateTexture", hr, usage, format, pool );
			if( texture )
			{
				*texture = nullptr;
			}
		}
		return hr;
	}

	static HRESULT APIENTRY Hook_CreateVolumeTexture(
		IDirect3DDevice9 *device, UINT width, UINT height, UINT depth, UINT levels,
		DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **texture,
		HANDLE *shared )
	{
		++s_allCreates;
		const bool remapped = ( pool == D3DPOOL_MANAGED );
		RemapManagedTexture( usage, pool, format, true );
		if( remapped )
		{
			MaybeLogRemap( "CreateVolumeTexture", ++s_remapVolume, usage, pool );
		}
		const HRESULT hr = s_origCreateVolumeTexture(
			device, width, height, depth, levels, usage, format, pool, texture, shared );
		if( FAILED( hr ) )
		{
			LogCreateFail( "CreateVolumeTexture", hr, usage, format, pool );
			if( texture )
			{
				*texture = nullptr;
			}
		}
		return hr;
	}

	static HRESULT APIENTRY Hook_CreateCubeTexture(
		IDirect3DDevice9 *device, UINT edgeLength, UINT levels, DWORD usage,
		D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **texture, HANDLE *shared )
	{
		++s_allCreates;
		const bool remapped = ( pool == D3DPOOL_MANAGED );
		RemapManagedTexture( usage, pool, format, true );
		if( remapped )
		{
			MaybeLogRemap( "CreateCubeTexture", ++s_remapCube, usage, pool );
		}
		const HRESULT hr = s_origCreateCubeTexture(
			device, edgeLength, levels, usage, format, pool, texture, shared );
		if( FAILED( hr ) )
		{
			LogCreateFail( "CreateCubeTexture", hr, usage, format, pool );
			if( texture )
			{
				*texture = nullptr;
			}
		}
		return hr;
	}

	static HRESULT APIENTRY Hook_CreateVertexBuffer(
		IDirect3DDevice9 *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
		IDirect3DVertexBuffer9 **buffer, HANDLE *shared )
	{
		++s_allCreates;
		const bool remapped = ( pool == D3DPOOL_MANAGED );
		RemapManagedBuffer( usage, pool );
		if( remapped )
		{
			MaybeLogRemap( "CreateVertexBuffer", ++s_remapVB, usage, pool );
		}
		const HRESULT hr = s_origCreateVertexBuffer(
			device, length, usage, fvf, pool, buffer, shared );
		if( FAILED( hr ) )
		{
			LogCreateFail( "CreateVertexBuffer", hr, usage, D3DFMT_UNKNOWN, pool );
			if( buffer )
			{
				*buffer = nullptr;
			}
		}
		else if( remapped && buffer )
		{
			TagRemappedBuffer( *buffer, length, s_origVBLock, Hook_VBLock );
		}
		return hr;
	}

	static HRESULT APIENTRY Hook_CreateIndexBuffer(
		IDirect3DDevice9 *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
		IDirect3DIndexBuffer9 **buffer, HANDLE *shared )
	{
		++s_allCreates;
		const bool remapped = ( pool == D3DPOOL_MANAGED );
		RemapManagedBuffer( usage, pool );
		if( remapped )
		{
			MaybeLogRemap( "CreateIndexBuffer", ++s_remapIB, usage, pool );
		}
		const HRESULT hr = s_origCreateIndexBuffer(
			device, length, usage, format, pool, buffer, shared );
		if( FAILED( hr ) )
		{
			LogCreateFail( "CreateIndexBuffer", hr, usage, format, pool );
			if( buffer )
			{
				*buffer = nullptr;
			}
		}
		else if( remapped && buffer )
		{
			TagRemappedBuffer( *buffer, length, s_origIBLock, Hook_IBLock );
		}
		return hr;
	}

	static bool IsPlausibleComPointer( const void *p )
	{
		const auto addr = reinterpret_cast< uintptr_t >( p );
		if( addr < 0x10000 || ( addr & 1 ) )
		{
			return false;
		}
		// The recurring garbage pointer from dumps.
		if( ( addr & 0xFFFF0000 ) == 0xB9190000 )
		{
			return false;
		}
		__try
		{
			const void *vt = *reinterpret_cast< void *const * >( p );
			const auto vtAddr = reinterpret_cast< uintptr_t >( vt );
			return vtAddr >= 0x10000 && ( vtAddr & 1 ) == 0;
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			return false;
		}
	}

	static HRESULT APIENTRY Hook_SetTexture(
		IDirect3DDevice9 *device, DWORD stage, IDirect3DBaseTexture9 *texture )
	{
		if( texture && !IsPlausibleComPointer( texture ) )
		{
			const unsigned n = ++s_badSetTexture;
			if( n <= 32 )
			{
				PSU_LOG( "D3D9ExCompat: blocked bad SetTexture stage=%u tex=%p (%u)\n",
					stage, texture, n );
			}
			return s_origSetTexture( device, stage, nullptr );
		}
		return s_origSetTexture( device, stage, texture );
	}

#define DEFINE_D3DX_HOOKS( N ) \
	static HRESULT WINAPI Hook##N##_D3DXCreateTexture( \
		LPDIRECT3DDEVICE9 device, UINT width, UINT height, UINT levels, DWORD usage, \
		D3DFORMAT format, D3DPOOL pool, LPDIRECT3DTEXTURE9 *texture ) \
	{ \
		++s_d3dxCalls; \
		const bool remapped = ( pool == D3DPOOL_MANAGED ); \
		RemapManagedTexture( usage, pool, format, true ); \
		if( remapped ) MaybeLogRemap( "D3DXCreateTexture", ++s_remapD3DX, usage, pool ); \
		const HRESULT hr = s_d3dx[ N ].create( device, width, height, levels, usage, format, pool, texture ); \
		if( FAILED( hr ) ) LogCreateFail( "D3DXCreateTexture", hr, usage, format, pool ); \
		return hr; \
	} \
	static HRESULT WINAPI Hook##N##_D3DXFromMemEx( \
		LPDIRECT3DDEVICE9 device, LPCVOID src, UINT srcSize, UINT width, UINT height, \
		UINT mipLevels, DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mipFilter, \
		D3DCOLOR colorKey, D3DXIMAGE_INFO *info, PALETTEENTRY *palette, LPDIRECT3DTEXTURE9 *texture ) \
	{ \
		++s_d3dxCalls; \
		const bool remapped = ( pool == D3DPOOL_MANAGED ); \
		const bool dxt = IsDxtFormat( format ) || MemoryLooksLikeDxt( src, srcSize ); \
		RemapManagedTexture( usage, pool, dxt ? D3DFMT_DXT1 : ( format == D3DFMT_UNKNOWN ? D3DFMT_A8R8G8B8 : format ), !dxt ); \
		if( remapped ) MaybeLogRemap( "D3DXFromMemEx", ++s_remapD3DX, usage, pool ); \
		const HRESULT hr = s_d3dx[ N ].fromMemEx( device, src, srcSize, width, height, mipLevels, usage, format, \
			pool, filter, mipFilter, colorKey, info, palette, texture ); \
		if( FAILED( hr ) ) LogCreateFail( "D3DXFromMemEx", hr, usage, format, pool ); \
		return hr; \
	} \
	static HRESULT WINAPI Hook##N##_D3DXFromFileExA( \
		LPDIRECT3DDEVICE9 device, LPCSTR src, UINT width, UINT height, UINT mipLevels, \
		DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mipFilter, \
		D3DCOLOR colorKey, D3DXIMAGE_INFO *info, PALETTEENTRY *palette, LPDIRECT3DTEXTURE9 *texture ) \
	{ \
		++s_d3dxCalls; \
		const bool remapped = ( pool == D3DPOOL_MANAGED ); \
		RemapManagedTexture( usage, pool, format, !IsDxtFormat( format ) ); \
		if( remapped ) MaybeLogRemap( "D3DXFromFileExA", ++s_remapD3DX, usage, pool ); \
		const HRESULT hr = s_d3dx[ N ].fromFileExA( device, src, width, height, mipLevels, usage, format, pool, \
			filter, mipFilter, colorKey, info, palette, texture ); \
		if( FAILED( hr ) ) LogCreateFail( "D3DXFromFileExA", hr, usage, format, pool ); \
		return hr; \
	} \
	static HRESULT WINAPI Hook##N##_D3DXFromFileExW( \
		LPDIRECT3DDEVICE9 device, LPCWSTR src, UINT width, UINT height, UINT mipLevels, \
		DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mipFilter, \
		D3DCOLOR colorKey, D3DXIMAGE_INFO *info, PALETTEENTRY *palette, LPDIRECT3DTEXTURE9 *texture ) \
	{ \
		++s_d3dxCalls; \
		const bool remapped = ( pool == D3DPOOL_MANAGED ); \
		RemapManagedTexture( usage, pool, format, !IsDxtFormat( format ) ); \
		if( remapped ) MaybeLogRemap( "D3DXFromFileExW", ++s_remapD3DX, usage, pool ); \
		const HRESULT hr = s_d3dx[ N ].fromFileExW( device, src, width, height, mipLevels, usage, format, pool, \
			filter, mipFilter, colorKey, info, palette, texture ); \
		if( FAILED( hr ) ) LogCreateFail( "D3DXFromFileExW", hr, usage, format, pool ); \
		return hr; \
	} \
	static HRESULT WINAPI Hook##N##_D3DXFromFileA( \
		LPDIRECT3DDEVICE9 device, LPCSTR src, LPDIRECT3DTEXTURE9 *texture ) \
	{ \
		++s_d3dxCalls; \
		DWORD usage = s_dynamicTextures ? D3DUSAGE_DYNAMIC : 0; \
		MaybeLogRemap( "D3DXFromFileA->Ex", ++s_remapD3DX, usage, D3DPOOL_DEFAULT ); \
		HRESULT hr; \
		if( s_d3dx[ N ].fromFileExA ) \
		{ \
			hr = s_d3dx[ N ].fromFileExA( device, src, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
				usage, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
				0, nullptr, nullptr, texture ); \
			if( FAILED( hr ) && usage != 0 ) \
			{ \
				usage = 0; \
				hr = s_d3dx[ N ].fromFileExA( device, src, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
					0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
					0, nullptr, nullptr, texture ); \
			} \
		} \
		else \
		{ \
			hr = s_d3dx[ N ].fromFileA( device, src, texture ); \
		} \
		if( FAILED( hr ) ) LogCreateFail( "D3DXFromFileA", hr, usage, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT ); \
		return hr; \
	} \
	static HRESULT WINAPI Hook##N##_D3DXFromFileW( \
		LPDIRECT3DDEVICE9 device, LPCWSTR src, LPDIRECT3DTEXTURE9 *texture ) \
	{ \
		++s_d3dxCalls; \
		DWORD usage = s_dynamicTextures ? D3DUSAGE_DYNAMIC : 0; \
		MaybeLogRemap( "D3DXFromFileW->Ex", ++s_remapD3DX, usage, D3DPOOL_DEFAULT ); \
		HRESULT hr; \
		if( s_d3dx[ N ].fromFileExW ) \
		{ \
			hr = s_d3dx[ N ].fromFileExW( device, src, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
				usage, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
				0, nullptr, nullptr, texture ); \
			if( FAILED( hr ) && usage != 0 ) \
			{ \
				hr = s_d3dx[ N ].fromFileExW( device, src, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
					0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
					0, nullptr, nullptr, texture ); \
			} \
		} \
		else \
		{ \
			hr = s_d3dx[ N ].fromFileW( device, src, texture ); \
		} \
		if( FAILED( hr ) ) LogCreateFail( "D3DXFromFileW", hr, usage, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT ); \
		return hr; \
	} \
	static HRESULT WINAPI Hook##N##_D3DXFromMem( \
		LPDIRECT3DDEVICE9 device, LPCVOID src, UINT srcSize, LPDIRECT3DTEXTURE9 *texture ) \
	{ \
		++s_d3dxCalls; \
		const bool dxt = MemoryLooksLikeDxt( src, srcSize ); \
		DWORD usage = ( !dxt && s_dynamicTextures ) ? D3DUSAGE_DYNAMIC : 0; \
		MaybeLogRemap( "D3DXFromMem->Ex", ++s_remapD3DX, usage, D3DPOOL_DEFAULT ); \
		HRESULT hr; \
		if( s_d3dx[ N ].fromMemEx ) \
		{ \
			hr = s_d3dx[ N ].fromMemEx( device, src, srcSize, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
				usage, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
				0, nullptr, nullptr, texture ); \
			if( FAILED( hr ) && usage != 0 ) \
			{ \
				hr = s_d3dx[ N ].fromMemEx( device, src, srcSize, D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
					0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, \
					0, nullptr, nullptr, texture ); \
			} \
		} \
		else \
		{ \
			hr = s_d3dx[ N ].fromMem( device, src, srcSize, texture ); \
		} \
		if( FAILED( hr ) ) LogCreateFail( "D3DXFromMem", hr, usage, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT ); \
		return hr; \
	}

	DEFINE_D3DX_HOOKS( 0 )
	DEFINE_D3DX_HOOKS( 1 )
	DEFINE_D3DX_HOOKS( 2 )
	DEFINE_D3DX_HOOKS( 3 )

#undef DEFINE_D3DX_HOOKS

	struct D3DXHookFns
	{
		PVOID create;
		PVOID fromMemEx;
		PVOID fromFileExA;
		PVOID fromFileExW;
		PVOID fromFileA;
		PVOID fromFileW;
		PVOID fromMem;
	};

	static const D3DXHookFns s_hookFns[ 4 ] = {
		{ Hook0_D3DXCreateTexture, Hook0_D3DXFromMemEx, Hook0_D3DXFromFileExA, Hook0_D3DXFromFileExW,
		  Hook0_D3DXFromFileA, Hook0_D3DXFromFileW, Hook0_D3DXFromMem },
		{ Hook1_D3DXCreateTexture, Hook1_D3DXFromMemEx, Hook1_D3DXFromFileExA, Hook1_D3DXFromFileExW,
		  Hook1_D3DXFromFileA, Hook1_D3DXFromFileW, Hook1_D3DXFromMem },
		{ Hook2_D3DXCreateTexture, Hook2_D3DXFromMemEx, Hook2_D3DXFromFileExA, Hook2_D3DXFromFileExW,
		  Hook2_D3DXFromFileA, Hook2_D3DXFromFileW, Hook2_D3DXFromMem },
		{ Hook3_D3DXCreateTexture, Hook3_D3DXFromMemEx, Hook3_D3DXFromFileExA, Hook3_D3DXFromFileExW,
		  Hook3_D3DXFromFileA, Hook3_D3DXFromFileW, Hook3_D3DXFromMem },
	};

	static bool PatchDeviceVtable( IDirect3DDevice9 *device )
	{
		void **vTable = *reinterpret_cast< void *** >( device );
		for( int i = 0; i < s_patchedVtableCount; ++i )
		{
			if( s_patchedVtables[ i ] == vTable )
			{
				return true;
			}
		}

		PSU_LOG( "D3D9ExCompat: patching device vtable %p CreateTexture[23]=%p SetTexture[65]=%p\n",
			vTable, vTable[ 23 ], vTable[ 65 ] );

		DWORD oldProtect = 0;
		if( !VirtualProtect( vTable, sizeof( void * ) * 70, PAGE_EXECUTE_READWRITE, &oldProtect ) )
		{
			PSU_LOG( "D3D9ExCompat: VirtualProtect(vtable) failed %lu\n", GetLastError() );
			return false;
		}

		if( !s_origCreateTexture )
		{
			s_origCreateTexture = reinterpret_cast< CreateTexture_t >( vTable[ 23 ] );
			s_origCreateVolumeTexture = reinterpret_cast< CreateVolumeTexture_t >( vTable[ 24 ] );
			s_origCreateCubeTexture = reinterpret_cast< CreateCubeTexture_t >( vTable[ 25 ] );
			s_origCreateVertexBuffer = reinterpret_cast< CreateVertexBuffer_t >( vTable[ 26 ] );
			s_origCreateIndexBuffer = reinterpret_cast< CreateIndexBuffer_t >( vTable[ 27 ] );
			s_origSetTexture = reinterpret_cast< SetTexture_t >( vTable[ 65 ] );
		}
		else if( vTable[ 23 ] != Hook_CreateTexture &&
			vTable[ 23 ] != reinterpret_cast< void * >( s_origCreateTexture ) )
		{
			// After Detours, slot may still hold the original absolute address (now a JMP).
			PSU_LOG( "D3D9ExCompat: note CreateTexture slot %p (orig/tramp %p) — still patching\n",
				vTable[ 23 ], s_origCreateTexture );
		}

		vTable[ 23 ] = reinterpret_cast< void * >( Hook_CreateTexture );
		vTable[ 24 ] = reinterpret_cast< void * >( Hook_CreateVolumeTexture );
		vTable[ 25 ] = reinterpret_cast< void * >( Hook_CreateCubeTexture );
		vTable[ 26 ] = reinterpret_cast< void * >( Hook_CreateVertexBuffer );
		vTable[ 27 ] = reinterpret_cast< void * >( Hook_CreateIndexBuffer );
		vTable[ 65 ] = reinterpret_cast< void * >( Hook_SetTexture );

		VirtualProtect( vTable, sizeof( void * ) * 70, oldProtect, &oldProtect );

		if( s_patchedVtableCount < 8 )
		{
			s_patchedVtables[ s_patchedVtableCount++ ] = vTable;
		}

		// Also Detour the CreateTexture implementation so callers that bypass a
		// stale vtable copy still hit the MANAGED remap.
		if( !s_createDetoured && s_origCreateTexture )
		{
			DetourTransactionBegin();
			DetourUpdateThread( GetCurrentThread() );
			DetourAttach( &( PVOID & )s_origCreateTexture, Hook_CreateTexture );
			DetourAttach( &( PVOID & )s_origCreateVolumeTexture, Hook_CreateVolumeTexture );
			DetourAttach( &( PVOID & )s_origCreateCubeTexture, Hook_CreateCubeTexture );
			DetourAttach( &( PVOID & )s_origCreateVertexBuffer, Hook_CreateVertexBuffer );
			DetourAttach( &( PVOID & )s_origCreateIndexBuffer, Hook_CreateIndexBuffer );
			const LONG err = DetourTransactionCommit();
			if( err == NO_ERROR )
			{
				s_createDetoured = true;
				PSU_LOG( "D3D9ExCompat: Create* implementation Detours attached\n" );
			}
			else
			{
				PSU_LOG( "D3D9ExCompat: Create* Detours failed %ld\n", err );
			}
		}

		// Probe: if this CreateTexture path works, allCreates becomes > 0.
		IDirect3DTexture9 *probe = nullptr;
		const HRESULT probeHr = device->CreateTexture(
			4, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &probe, nullptr );
		PSU_LOG( "D3D9ExCompat: probe CreateTexture hr=0x%08X tex=%p allCreates=%u\n",
			probeHr, probe, s_allCreates.load() );
		if( probe )
		{
			probe->Release();
		}

		PSU_LOG( "D3D9ExCompat: vtable patched (%d devices)\n", s_patchedVtableCount );
		return true;
	}

	static bool InstallD3DXHooksForModule( HMODULE mod, const char *name )
	{
		if( !mod || s_d3dxCount >= 4 )
		{
			return false;
		}
		for( int i = 0; i < s_d3dxCount; ++i )
		{
			if( s_d3dx[ i ].mod == mod )
			{
				return true;
			}
		}

		const int idx = s_d3dxCount;
		D3DXOrigs &o = s_d3dx[ idx ];
		o.mod = mod;
		o.create = reinterpret_cast< D3DXCreateTexture_t >( GetProcAddress( mod, "D3DXCreateTexture" ) );
		o.fromMemEx = reinterpret_cast< D3DXCreateTextureFromFileInMemoryEx_t >(
			GetProcAddress( mod, "D3DXCreateTextureFromFileInMemoryEx" ) );
		o.fromFileExA = reinterpret_cast< D3DXCreateTextureFromFileExA_t >(
			GetProcAddress( mod, "D3DXCreateTextureFromFileExA" ) );
		o.fromFileExW = reinterpret_cast< D3DXCreateTextureFromFileExW_t >(
			GetProcAddress( mod, "D3DXCreateTextureFromFileExW" ) );
		o.fromFileA = reinterpret_cast< D3DXCreateTextureFromFileA_t >(
			GetProcAddress( mod, "D3DXCreateTextureFromFileA" ) );
		o.fromFileW = reinterpret_cast< D3DXCreateTextureFromFileW_t >(
			GetProcAddress( mod, "D3DXCreateTextureFromFileW" ) );
		o.fromMem = reinterpret_cast< D3DXCreateTextureFromFileInMemory_t >(
			GetProcAddress( mod, "D3DXCreateTextureFromFileInMemory" ) );

		if( !o.create && !o.fromMemEx && !o.fromFileExA && !o.fromFileA )
		{
			PSU_LOG( "D3D9ExCompat: no D3DX create exports in %s\n", name );
			o.mod = nullptr;
			return false;
		}

		DetourTransactionBegin();
		DetourUpdateThread( GetCurrentThread() );
		int attached = 0;
		auto attach = [ & ]( PVOID *orig, PVOID hook )
		{
			if( *orig )
			{
				DetourAttach( orig, hook );
				++attached;
			}
		};
		attach( &( PVOID & )o.create, s_hookFns[ idx ].create );
		attach( &( PVOID & )o.fromMemEx, s_hookFns[ idx ].fromMemEx );
		attach( &( PVOID & )o.fromFileExA, s_hookFns[ idx ].fromFileExA );
		attach( &( PVOID & )o.fromFileExW, s_hookFns[ idx ].fromFileExW );
		attach( &( PVOID & )o.fromFileA, s_hookFns[ idx ].fromFileA );
		attach( &( PVOID & )o.fromFileW, s_hookFns[ idx ].fromFileW );
		attach( &( PVOID & )o.fromMem, s_hookFns[ idx ].fromMem );

		const LONG err = DetourTransactionCommit();
		if( err != NO_ERROR || attached == 0 )
		{
			PSU_LOG( "D3D9ExCompat: D3DX Detour failed %ld attached=%d in %s\n", err, attached, name );
			o = {};
			return false;
		}

		++s_d3dxCount;
		PSU_LOG( "D3D9ExCompat: D3DX hooks installed in %s (slot %d, %d exports)\n",
			name, idx, attached );
		return true;
	}

	static void InstallD3DXHooks()
	{
		const char *names[] = {
			"d3dx9_33.dll", "D3DX9_33.dll", // game
			"D3DX9_43.dll", "d3dx9_43.dll", "D3dx9d_43.dll", // PSUEx
			"d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll",
			"d3dx9_39.dll", "d3dx9_38.dll", "d3dx9_37.dll",
			"d3dx9_36.dll", "d3dx9_35.dll", "d3dx9_34.dll",
			"d3dx9_32.dll", "d3dx9_31.dll", "d3dx9_30.dll"
		};

		for( const char *name : names )
		{
			if( HMODULE mod = GetModuleHandleA( name ) )
			{
				InstallD3DXHooksForModule( mod, name );
			}
		}
		// Ensure both common modules are loaded/hooked.
		if( HMODULE mod = LoadLibraryA( "d3dx9_33.dll" ) )
		{
			InstallD3DXHooksForModule( mod, "d3dx9_33.dll" );
		}
		if( HMODULE mod = LoadLibraryA( "D3DX9_43.dll" ) )
		{
			InstallD3DXHooksForModule( mod, "D3DX9_43.dll" );
		}
		if( s_d3dxCount == 0 )
		{
			PSU_LOG( "D3D9ExCompat: WARNING no D3DX modules hooked\n" );
		}
	}

	void LogD3D9ExCompatStats()
	{
		PSU_LOG(
			"D3D9ExCompat stats: allCreates=%u d3dxCalls=%u tex=%u vol=%u cube=%u vb=%u ib=%u d3dxRemap=%u fails=%u badSetTex=%u\n",
			s_allCreates.load(), s_d3dxCalls.load(), s_remapTexture.load(), s_remapVolume.load(),
			s_remapCube.load(), s_remapVB.load(), s_remapIB.load(), s_remapD3DX.load(),
			s_failLogged.load(), s_badSetTexture.load() );
		s_statsLogged.store( true );
	}

	bool InstallD3D9ExCompatHooks( IDirect3DDevice9 *device )
	{
		if( !IsD3D9ExActive() || !device )
		{
			return false;
		}

		D3DCAPS9 caps{};
		if( SUCCEEDED( device->GetDeviceCaps( &caps ) ) )
		{
			s_dynamicTextures = ( caps.Caps2 & D3DCAPS2_DYNAMICTEXTURES ) != 0;
		}

		InstallD3DXHooks();
		const bool ok = PatchDeviceVtable( device );
		PSU_LOG( "D3D9ExCompat: Caps2_DYNAMICTEXTURES=%d vtablePatch=%d d3dxModules=%d\n",
			( int )s_dynamicTextures, ( int )ok, s_d3dxCount );
		return ok;
	}
}
