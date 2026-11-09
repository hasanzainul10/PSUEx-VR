#pragma once

// Pose math shared by the first-person arms (VRHands) and the body IK (VRBodyIk). Poses are the
// model-space bone matrices the game builds (0x0074B760): one row-major D3DX 4x4 per bone, row
// vectors (v' = v M), translation in the last row.

#include <d3dx9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace PSUVR
{
	namespace Skeleton
	{
		// PSU human skeleton (28 bones; bone records 0x90 bytes, parent at +6). Controller
		// hand 0 drives the character's left arm.
		inline constexpr int kBoneCount = 28;
		inline constexpr size_t kRecordSize = 0x90;
		inline constexpr size_t kRecordParent = 0x06;
		struct Arm
		{
			int shoulder, upper, fore, hand;
		};
		inline constexpr Arm kArms[ 2 ] = { { 14, 15, 16, 17 }, { 19, 20, 21, 22 } };
		// Legs as two-bone chains: hip node, thigh, calf, foot.
		inline constexpr Arm kLegs[ 2 ] = { { 2, 3, 4, 5 }, { 2, 6, 7, 8 } };
		inline constexpr int kPelvisBone = 1;
		// Waist (above the hips): its subtree is the upper body. Neck: its subtree is the head.
		inline constexpr int kWaistBone = 9;
		inline constexpr int kChestBone = 10;
		inline constexpr int kShoulderLevelBone = 11;
		inline constexpr int kNeckBone = 12;
		inline constexpr int kHeadBone = 13;
		// Parents of the 28-bone character skeleton (VRBones log; the same on every model seen,
		// CAST included). The rest pose and body IK index bones directly, so they need this map.
		inline constexpr int kBoneParents[ kBoneCount ] = { -1, 0, 1, 2, 3, 4, 2, 6, 7, 1, 9, 10, 11, 12, 11, 14, 15, 16, 17, 11, 19,
			20, 21, 22, 10, 10, 9, 0 };
		// The model faces +Z (left side is +X, up +Y).
		inline const D3DXVECTOR3 kModelForward( 0.0f, 0.0f, 1.0f );
		inline constexpr float kPi = 3.14159265f;

		inline D3DXVECTOR3 Position( const float *out, int bone )
		{
			const float *m = out + bone * 16;
			return { m[ 12 ], m[ 13 ], m[ 14 ] };
		}

		inline int Parent( const uint8_t *records, int bone )
		{
			return *reinterpret_cast< const short * >( records + bone * kRecordSize + kRecordParent );
		}

		inline bool Below( const uint8_t *records, int bone, int root )
		{
			for( int guard = 0; bone >= 0 && guard < kBoneCount; ++guard )
			{
				if( bone == root )
				{
					return true;
				}
				bone = Parent( records, bone );
			}
			return false;
		}

		inline D3DXVECTOR3 Normalized( const D3DXVECTOR3 &v, const D3DXVECTOR3 &fallback )
		{
			const float len = D3DXVec3Length( &v );
			return len > 1.0e-5f ? v / len : fallback;
		}

		inline float WrapPi( float a )
		{
			while( a > kPi ) a -= 2.0f * kPi;
			while( a < -kPi ) a += 2.0f * kPi;
			return a;
		}

		inline D3DXVECTOR3 ToWorld( const D3DXMATRIX &world, const D3DXVECTOR3 &v )
		{
			D3DXVECTOR3 r;
			D3DXVec3TransformCoord( &r, &v, &world );
			return r;
		}

		inline D3DXVECTOR3 RotateByQuat( const float q[ 4 ], const D3DXVECTOR3 &v )
		{
			// v + 2w(u x v) + 2(u x (u x v)), u = q.xyz
			const D3DXVECTOR3 u( q[ 0 ], q[ 1 ], q[ 2 ] );
			D3DXVECTOR3 t;
			D3DXVec3Cross( &t, &u, &v );
			t *= 2.0f;
			D3DXVECTOR3 c;
			D3DXVec3Cross( &c, &u, &t );
			return v + t * q[ 3 ] + c;
		}

		// Sets the translation of rotation-only m so that it turns about pivot.
		inline void SetPivot( D3DXMATRIX &m, const D3DXVECTOR3 &pivot )
		{
			D3DXVECTOR3 moved;
			D3DXVec3TransformNormal( &moved, &pivot, &m );
			m( 3, 0 ) = pivot.x - moved.x;
			m( 3, 1 ) = pivot.y - moved.y;
			m( 3, 2 ) = pivot.z - moved.z;
		}

		// Row-vector matrix rotating unit a onto unit b about pivot (v' = (v - p) R + p).
		inline D3DXMATRIX RotationAbout( const D3DXVECTOR3 &a, const D3DXVECTOR3 &b, const D3DXVECTOR3 &pivot )
		{
			D3DXVECTOR3 k;
			D3DXVec3Cross( &k, &a, &b );
			const float c = D3DXVec3Dot( &a, &b );
			// Column form R = I + K + K^2 / (1 + c), K = skew(a x b); valid away from c = -1.
			float r[ 3 ][ 3 ] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			if( c > -0.999f )
			{
				const float K[ 3 ][ 3 ] = { { 0, -k.z, k.y }, { k.z, 0, -k.x }, { -k.y, k.x, 0 } };
				const float f = 1.0f / ( 1.0f + c );
				for( int i = 0; i < 3; ++i )
				{
					for( int j = 0; j < 3; ++j )
					{
						float k2 = 0.0f;
						for( int n = 0; n < 3; ++n )
						{
							k2 += K[ i ][ n ] * K[ n ][ j ];
						}
						r[ i ][ j ] += K[ i ][ j ] + k2 * f;
					}
				}
			}
			// Row-vector form is the transpose.
			D3DXMATRIX m;
			D3DXMatrixIdentity( &m );
			for( int i = 0; i < 3; ++i )
			{
				for( int j = 0; j < 3; ++j )
				{
					m( i, j ) = r[ j ][ i ];
				}
			}
			SetPivot( m, pivot );
			return m;
		}

		// Rotate unit a toward unit b by at most limit (radians), about pivot.
		inline D3DXMATRIX LimitedTurn( const D3DXVECTOR3 &a, const D3DXVECTOR3 &b, float limit, const D3DXVECTOR3 &pivot )
		{
			const float c = std::clamp( D3DXVec3Dot( &a, &b ), -1.0f, 1.0f );
			const float angle = std::acos( c );
			D3DXVECTOR3 to = b;
			if( angle > limit && angle > 1.0e-4f )
			{
				const float t = limit / angle;
				to = Normalized( a * ( 1.0f - t ) + b * t, b );
			}
			return RotationAbout( a, to, pivot );
		}

		// Yaw (about +Y) by angle about pivot; +angle turns +Z toward +X (the game's yaw).
		inline D3DXMATRIX YawAbout( float angle, const D3DXVECTOR3 &pivot )
		{
			D3DXMATRIX m;
			D3DXMatrixRotationY( &m, angle );
			SetPivot( m, pivot );
			return m;
		}

		// 3x3 with unit rows (axes), no translation.
		inline D3DXMATRIX Axes( const D3DXMATRIX &m )
		{
			D3DXMATRIX r;
			D3DXMatrixIdentity( &r );
			for( int i = 0; i < 3; ++i )
			{
				D3DXVECTOR3 row( m( i, 0 ), m( i, 1 ), m( i, 2 ) );
				const float len = D3DXVec3Length( &row );
				row = len > 1.0e-6f ? row / len : row;
				r( i, 0 ) = row.x;
				r( i, 1 ) = row.y;
				r( i, 2 ) = row.z;
			}
			return r;
		}

		// Rotation taking the frame (forward a, up roughly aUp) onto (forward b, up roughly bUp).
		inline D3DXMATRIX FrameRotation( const D3DXVECTOR3 &a, const D3DXVECTOR3 &aUp, const D3DXVECTOR3 &b, const D3DXVECTOR3 &bUp )
		{
			const auto basis = []( const D3DXVECTOR3 &forward, const D3DXVECTOR3 &up ) {
				const D3DXVECTOR3 f = Normalized( forward, kModelForward );
				D3DXVECTOR3 side;
				D3DXVec3Cross( &side, &up, &f );
				side = Normalized( side, D3DXVECTOR3( 1.0f, 0.0f, 0.0f ) );
				D3DXVECTOR3 u;
				D3DXVec3Cross( &u, &f, &side );
				D3DXMATRIX m;
				D3DXMatrixIdentity( &m );
				const D3DXVECTOR3 rows[ 3 ] = { side, u, f };
				for( int i = 0; i < 3; ++i )
				{
					m( i, 0 ) = rows[ i ].x;
					m( i, 1 ) = rows[ i ].y;
					m( i, 2 ) = rows[ i ].z;
				}
				return m;
			};
			// Rows are axes: A^T B maps each row of A onto the same row of B.
			D3DXMATRIX aT, r;
			const D3DXMATRIX am = basis( a, aUp );
			D3DXMatrixTranspose( &aT, &am );
			const D3DXMATRIX bm = basis( b, bUp );
			D3DXMatrixMultiply( &r, &aT, &bm );
			return r;
		}

		// The part t (0-1) of rotation r (no translation), turning about pivot.
		inline D3DXMATRIX PartialAbout( const D3DXMATRIX &r, float t, const D3DXVECTOR3 &pivot )
		{
			D3DXQUATERNION q, partial;
			const D3DXQUATERNION identity( 0.0f, 0.0f, 0.0f, 1.0f );
			D3DXQuaternionRotationMatrix( &q, &r );
			D3DXQuaternionSlerp( &partial, &identity, &q, t );
			D3DXMATRIX m;
			D3DXMatrixRotationQuaternion( &m, &partial );
			SetPivot( m, pivot );
			return m;
		}

		// Angle of rotation r (no translation), radians.
		inline float RotationAngle( const D3DXMATRIX &r )
		{
			const float c = ( r._11 + r._22 + r._33 - 1.0f ) * 0.5f;
			return std::acos( std::clamp( c, -1.0f, 1.0f ) );
		}

		inline void ApplyToSubtree( float *out, const uint8_t *records, int root, const D3DXMATRIX &delta )
		{
			for( int b = 0; b < kBoneCount; ++b )
			{
				if( Below( records, b, root ) )
				{
					D3DXMATRIX *m = reinterpret_cast< D3DXMATRIX * >( out + b * 16 );
					D3DXMatrixMultiply( m, m, &delta );
				}
			}
		}

		// Two-bone IK: arm.upper -> arm.fore -> arm.hand reaches for target, the elbow bends toward pole
		// (a direction; only its part across the reach counts). Legs use it too (kLegs).
		inline void SolveArm( float *out, const uint8_t *records, const Arm &arm, const D3DXVECTOR3 &target,
			const D3DXVECTOR3 &pole )
		{
			const D3DXVECTOR3 s = Position( out, arm.upper );
			const D3DXVECTOR3 e = Position( out, arm.fore );
			const D3DXVECTOR3 h = Position( out, arm.hand );
			const D3DXVECTOR3 se = e - s;
			const D3DXVECTOR3 eh = h - e;
			const float a = D3DXVec3Length( &se );
			const float b = D3DXVec3Length( &eh );
			if( a < 1.0e-3f || b < 1.0e-3f )
			{
				return;
			}
			D3DXVECTOR3 toTarget = target - s;
			float dist = D3DXVec3Length( &toTarget );
			const D3DXVECTOR3 dir = Normalized( toTarget, Normalized( h - s, { 0.0f, -1.0f, 0.0f } ) );
			const float minReach = std::fabs( a - b ) + 1.0e-3f;
			const float maxReach = ( a + b ) * 0.999f;
			dist = dist < minReach ? minReach : ( dist > maxReach ? maxReach : dist );

			// Elbow in the plane of the reach and the pole.
			D3DXVECTOR3 bend = pole - dir * D3DXVec3Dot( &pole, &dir );
			bend = Normalized( bend, Normalized( se - dir * D3DXVec3Dot( &se, &dir ), { 0.0f, -1.0f, 0.0f } ) );
			const float cosA = ( a * a + dist * dist - b * b ) / ( 2.0f * a * dist );
			const float clamped = cosA < -1.0f ? -1.0f : ( cosA > 1.0f ? 1.0f : cosA );
			const float sinA = std::sqrt( 1.0f - clamped * clamped );
			const D3DXVECTOR3 elbow = s + dir * ( a * clamped ) + bend * ( a * sinA );
			const D3DXVECTOR3 wrist = s + dir * dist;

			// Upper arm: shoulder -> elbow; then the forearm: elbow -> wrist.
			ApplyToSubtree( out, records, arm.upper,
				RotationAbout( Normalized( se, dir ), Normalized( elbow - s, dir ), s ) );
			const D3DXVECTOR3 e1 = Position( out, arm.fore );
			const D3DXVECTOR3 h1 = Position( out, arm.hand );
			ApplyToSubtree( out, records, arm.fore,
				RotationAbout( Normalized( h1 - e1, dir ), Normalized( wrist - e1, dir ), e1 ) );
		}
	}
}
