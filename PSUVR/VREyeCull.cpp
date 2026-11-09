#include "VREyeCull.h"

#include "StereoRenderer.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <Windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstring>

namespace PSUVR
{
	namespace VREyeCull
	{
		namespace
		{
			// Side planes (4 x {cos, sin}) the game built from its projection (0x0074B200).
			constexpr uintptr_t kGamePlanes = 0x00BA8BC0;
			constexpr size_t kGamePlaneBytes = 8 * sizeof( float );
			// The perspective path (0x00BA9398 == 1 is the orthographic one).
			constexpr uintptr_t kGameOrtho = 0x00BA9398;

			enum { kLeft, kRight, kBottom, kTop, kNear, kFar, kPlaneCount };

			struct Plane
			{
				float x, y, z, d;
			};

			Plane s_planes[ kPlaneCount ]{};
			float s_gamePlanes[ 8 ]{};
			bool s_active = false;

			// Debug counts (3 s).
			unsigned s_tests = 0, s_fallback = 0, s_drawnGameCulled = 0, s_culledGameDrawn = 0;
			DWORD s_lastLog = 0;

			bool Usable()
			{
				return s_active && StereoRenderer::InEyePass() &&
					std::memcmp( reinterpret_cast< const void * >( kGamePlanes ), s_gamePlanes, kGamePlaneBytes ) == 0 &&
					*reinterpret_cast< const int * >( kGameOrtho ) != 1;
			}

			// Clip space (D3D): -w <= x, y <= w, 0 <= z <= w, for clip = [p 1] * C. Plane = a column
			// combination of C, in the game's camera space, normalized to distances.
			Plane Column( const D3DMATRIX &c, int j, float sign, int k )
			{
				Plane p{ c.m[ 0 ][ k ] + sign * ( j >= 0 ? c.m[ 0 ][ j ] : 0.0f ),
					c.m[ 1 ][ k ] + sign * ( j >= 0 ? c.m[ 1 ][ j ] : 0.0f ),
					c.m[ 2 ][ k ] + sign * ( j >= 0 ? c.m[ 2 ][ j ] : 0.0f ),
					c.m[ 3 ][ k ] + sign * ( j >= 0 ? c.m[ 3 ][ j ] : 0.0f ) };
				const float len = std::sqrt( p.x * p.x + p.y * p.y + p.z * p.z );
				if( len > 1e-6f )
				{
					p.x /= len;
					p.y /= len;
					p.z /= len;
					p.d /= len;
				}
				return p;
			}

			// camera = [p 1] * matrix (row vectors, as 0x007463F0).
			void ToCamera( const float *p, const float *m, float out[ 3 ] )
			{
				out[ 0 ] = p[ 0 ] * m[ 0 ] + p[ 1 ] * m[ 4 ] + p[ 2 ] * m[ 8 ] + m[ 12 ];
				out[ 1 ] = p[ 0 ] * m[ 1 ] + p[ 1 ] * m[ 5 ] + p[ 2 ] * m[ 9 ] + m[ 13 ];
				out[ 2 ] = p[ 0 ] * m[ 2 ] + p[ 1 ] * m[ 6 ] + p[ 2 ] * m[ 10 ] + m[ 14 ];
			}

			// radius[ i ] = how far the volume reaches across plane i.
			uint32_t Classify( const float c[ 3 ], const float radius[ kPlaneCount ] )
			{
				uint32_t partial = 0;
				bool side = false;
				for( int i = 0; i < kPlaneCount; ++i )
				{
					const Plane &p = s_planes[ i ];
					const float dist = p.x * c[ 0 ] + p.y * c[ 1 ] + p.z * c[ 2 ] + p.d;
					if( dist < -radius[ i ] )
					{
						return 0x10;
					}
					if( dist < radius[ i ] )
					{
						if( i == kNear )
						{
							partial |= 4;
						}
						else if( i == kFar )
						{
							partial |= 8;
						}
						else
						{
							side = true;
						}
					}
				}
				return partial || side ? partial : 2;
			}
		}

		void BeginEye( const D3DMATRIX &view, const D3DMATRIX &proj )
		{
			s_active = false;
			if( !Config::Get().vr.m_eyeFrustumCull )
			{
				return;
			}
			D3DMATRIX c{};
			for( int i = 0; i < 4; ++i )
			{
				for( int j = 0; j < 4; ++j )
				{
					float sum = 0.0f;
					for( int k = 0; k < 4; ++k )
					{
						sum += view.m[ i ][ k ] * proj.m[ k ][ j ];
					}
					c.m[ i ][ j ] = sum;
				}
			}
			s_planes[ kLeft ] = Column( c, 0, 1.0f, 3 );
			s_planes[ kRight ] = Column( c, 0, -1.0f, 3 );
			s_planes[ kBottom ] = Column( c, 1, 1.0f, 3 );
			s_planes[ kTop ] = Column( c, 1, -1.0f, 3 );
			s_planes[ kNear ] = Column( c, -1, 0.0f, 2 );
			s_planes[ kFar ] = Column( c, 2, -1.0f, 3 );
			std::memcpy( s_gamePlanes, reinterpret_cast< const void * >( kGamePlanes ), kGamePlaneBytes );
			s_active = true;
		}

		void End()
		{
			s_active = false;
#ifdef _DEBUG
			if( s_tests + s_fallback > 0 && GetTickCount() - s_lastLog >= 3000 )
			{
				s_lastLog = GetTickCount();
				PSU_LOG( "VREyeCull: %u eye tests, %u fell back to the game's (other camera) | drawn but game-culled %u, "
					"culled but game-drawn %u\n", s_tests, s_fallback, s_drawnGameCulled, s_culledGameDrawn );
				s_tests = s_fallback = s_drawnGameCulled = s_culledGameDrawn = 0;
			}
#endif
		}

		bool Sphere( const float *center, float radius, const float *matrix, uint32_t &result )
		{
			if( !s_active )
			{
				return false;
			}
			if( !Usable() || !center || !matrix )
			{
				++s_fallback;
				return false;
			}
			++s_tests;
			float c[ 3 ];
			ToCamera( center, matrix, c );
			const float r[ kPlaneCount ] = { radius, radius, radius, radius, radius, radius };
			result = Classify( c, r );
			return true;
		}

		bool Box( const float *center, float ex, float ey, float ez, const float *matrix, uint32_t &result )
		{
			if( !s_active )
			{
				return false;
			}
			if( !Usable() || !center || !matrix )
			{
				++s_fallback;
				return false;
			}
			++s_tests;
			float c[ 3 ];
			ToCamera( center, matrix, c );
			// Box axis k in camera space = row k of the matrix, scaled by its half extent (as the game).
			const float e[ 3 ] = { ex, ey, ez };
			float r[ kPlaneCount ];
			for( int i = 0; i < kPlaneCount; ++i )
			{
				const Plane &p = s_planes[ i ];
				float reach = 0.0f;
				for( int k = 0; k < 3; ++k )
				{
					reach += std::fabs( e[ k ] * ( matrix[ k * 4 + 0 ] * p.x + matrix[ k * 4 + 1 ] * p.y + matrix[ k * 4 + 2 ] * p.z ) );
				}
				r[ i ] = reach;
			}
			result = Classify( c, r );
			return true;
		}

		void NoteCompare( uint32_t gameResult, uint32_t ourResult )
		{
			const bool gameOut = gameResult == 0x10;
			const bool ourOut = ourResult == 0x10;
			if( gameOut && !ourOut )
			{
				++s_drawnGameCulled;
			}
			else if( ourOut && !gameOut )
			{
				++s_culledGameDrawn;
			}
		}
	}
}
