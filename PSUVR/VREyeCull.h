#pragma once

#include <cstdint>

struct _D3DMATRIX;

namespace PSUVR
{
	// The game culls meshes and terrain against its flat camera (sphere test 0x0074DA40, box test
	// 0x0074D5B0: side planes 0x00BA8BC0..0x00BA8BDC from the flat projection, near / far
	// 0x00BA91A4 / 0x00BA91A0). The headset's view is wider and turns away from the game camera,
	// so things popped in at the edges. In the eye passes (vr.eyeFrustumCull) the tests use the
	// eye's own frustum: the game's camera-space matrix times the eye VIEW and PROJECTION.
	namespace VREyeCull
	{
		// Around each eye pass, with the matrices the eye pass put on the device. Begin also
		// snapshots the game's plane globals: when they change (shadow light camera, any other
		// camera the game sets) the game's own test runs until they match again.
		void BeginEye( const _D3DMATRIX &view, const _D3DMATRIX &proj );
		void End();

		// The game's return codes: 0x10 outside, 2 fully inside, else partial (4 crosses the near
		// plane, 8 the far plane, 0 only a side). False = run the game's test.
		// Sphere: centre (model space), radius, model -> camera matrix (row vectors).
		bool Sphere( const float *center, float radius, const float *matrix, uint32_t &result );
		// Box: centre, half extents along the model axes, model -> camera matrix.
		bool Box( const float *center, float ex, float ey, float ez, const float *matrix, uint32_t &result );

		// Debug: one test the game also ran (gameResult) with our result, for the 3 s log.
		void NoteCompare( uint32_t gameResult, uint32_t ourResult );
	}
}
