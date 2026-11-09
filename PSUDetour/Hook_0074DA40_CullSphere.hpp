/*
* Sphere vs camera frustum (meshes / terrain; callers 0x0074DCD0, 0x0074DF00, 0x0074E060):
* centre * matrix (model -> camera, row vectors) against the side planes 0x00BA8BC0..0x00BA8BDC
* and near / far 0x00BA91A4 / 0x00BA91A0. Returns 0x10 outside, 2 inside, else partial.
* In the eye passes VREyeCull tests against the eye's frustum instead (vr.eyeFrustumCull).
*/

#pragma once

#include "../PSUEx.h"
#include "../PSUVR/VREyeCull.h"

namespace sub_74DA40
{
	typedef uint32_t( __cdecl *function_t )( const float *center, float radius, const float *matrix );
	function_t pOriginal = reinterpret_cast< function_t >( 0x0074DA40 );

	uint32_t __cdecl Hook_74DA40( const float *center, float radius, const float *matrix )
	{
		uint32_t result = 0;
		if( PSUVR::VREyeCull::Sphere( center, radius, matrix, result ) )
		{
#ifdef _DEBUG
			PSUVR::VREyeCull::NoteCompare( pOriginal( center, radius, matrix ), result );
#endif
			return result;
		}
		return pOriginal( center, radius, matrix );
	}

	void Hook()
	{
		DetourAttach( &( PVOID & )pOriginal, reinterpret_cast< PVOID >( Hook_74DA40 ) );
	}
}
