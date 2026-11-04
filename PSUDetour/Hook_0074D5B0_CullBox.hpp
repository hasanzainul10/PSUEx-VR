/*
* Box vs camera frustum (caller 0x0074DEB0: centre at object +0x70, half extents +0x84..+0x8C):
* the box axes are the matrix rows (model -> camera) scaled by the half extents, tested against
* the same planes as the sphere test 0x0074DA40, same return codes. In the eye passes
* VREyeCull tests against the eye's frustum instead (vr.eyeFrustumCull).
*/

#pragma once

#include "../PSUEx.h"
#include "../PSUVR/VREyeCull.h"

namespace sub_74D5B0
{
	typedef uint32_t( __cdecl *function_t )( const float *center, float ex, float ey, float ez, const float *matrix );
	function_t pOriginal = reinterpret_cast< function_t >( 0x0074D5B0 );

	uint32_t __cdecl Hook_74D5B0( const float *center, float ex, float ey, float ez, const float *matrix )
	{
		uint32_t result = 0;
		if( PSUVR::VREyeCull::Box( center, ex, ey, ez, matrix, result ) )
		{
#ifdef _DEBUG
			PSUVR::VREyeCull::NoteCompare( pOriginal( center, ex, ey, ez, matrix ), result );
#endif
			return result;
		}
		return pOriginal( center, ex, ey, ez, matrix );
	}

	void Hook()
	{
		DetourAttach( &( PVOID & )pOriginal, reinterpret_cast< PVOID >( Hook_74D5B0 ) );
	}
}
