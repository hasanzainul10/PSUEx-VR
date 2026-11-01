#include "VRHolster.h"

#include "VRControllers.h"
#include "VRSystem.h"
#include "../PSUExConfig.h"

#include <cmath>

namespace PSUVR
{
	namespace VRHolster
	{
		namespace
		{
			struct V3 { float x, y, z; };

			// q * v for a unit quaternion (x, y, z, w).
			V3 Rotate( const float q[ 4 ], const V3 &v )
			{
				const float x = q[ 0 ], y = q[ 1 ], z = q[ 2 ], w = q[ 3 ];
				const float tx = 2.0f * ( y * v.z - z * v.y );
				const float ty = 2.0f * ( z * v.x - x * v.z );
				const float tz = 2.0f * ( x * v.y - y * v.x );
				return { v.x + w * tx + ( y * tz - z * ty ), v.y + w * ty + ( z * tx - x * tz ), v.z + w * tz + ( x * ty - y * tx ) };
			}

			// Offsets in meters for a 1.7 m head height.
			constexpr float kRefHeadHeight = 1.7f;

			struct Frame
			{
				V3 head;
				V3 forward;   // level head yaw
				V3 right;
				float orientation[ 4 ];
				float scale;
			};

			bool GetFrame( Frame &frame )
			{
				EyePose views[ 2 ];
				EyePose viewSpace[ 2 ];
				if( !System::Get().NextViews( views, viewSpace ) )
				{
					return false;
				}
				frame.head = { ( views[ 0 ].position[ 0 ] + views[ 1 ].position[ 0 ] ) * 0.5f,
					( views[ 0 ].position[ 1 ] + views[ 1 ].position[ 1 ] ) * 0.5f,
					( views[ 0 ].position[ 2 ] + views[ 1 ].position[ 2 ] ) * 0.5f };
				for( int i = 0; i < 4; ++i )
				{
					frame.orientation[ i ] = views[ 0 ].orientation[ i ];
				}
				// Head yaw: the look direction on the floor plane; looking straight up or down uses
				// the head's up vector pointing the other way (face-down = up points forward).
				V3 f = Rotate( frame.orientation, { 0.0f, 0.0f, -1.0f } );
				float len = std::sqrt( f.x * f.x + f.z * f.z );
				if( len < 0.3f )
				{
					const V3 u = Rotate( frame.orientation, { 0.0f, 1.0f, 0.0f } );
					const float sign = f.y < 0.0f ? 1.0f : -1.0f;
					f = { u.x * sign, 0.0f, u.z * sign };
					len = std::sqrt( f.x * f.x + f.z * f.z );
				}
				if( len < 1.0e-3f )
				{
					return false;
				}
				frame.forward = { f.x / len, 0.0f, f.z / len };
				frame.right = { -frame.forward.z, 0.0f, frame.forward.x };
				float height = 0.0f;
				frame.scale = 1.0f;
				if( System::Get().HeadHeightAboveFloor( height ) && height > 0.8f )
				{
					frame.scale = height / kRefHeadHeight;
					frame.scale = frame.scale < 0.6f ? 0.6f : ( frame.scale > 1.3f ? 1.3f : frame.scale );
				}
				return true;
			}
		}

		bool ZoneShape( Zone zone, float center[ 3 ], float axes[ 3 ][ 3 ], float half[ 3 ] )
		{
			Frame f;
			if( zone == Zone::None || !GetFrame( f ) )
			{
				return false;
			}
			const auto &vr = Config::Get().vr;
			// Level body frame (head yaw): right, up, forward. The mouth uses the head's own axes.
			V3 r = f.right, u{ 0.0f, 1.0f, 0.0f }, fw = f.forward;
			// Centre as meters (right, up, forward) from the head at 1.7 m, then half extents.
			float c[ 3 ], h[ 3 ], ratio;
			switch( zone )
			{
				case Zone::Head:
					// Behind and above the head, reaching back past the neck and forward to the shoulders.
					c[ 0 ] = 0.0f; c[ 1 ] = 0.12f; c[ 2 ] = -0.20f;
					h[ 0 ] = 0.28f; h[ 1 ] = 0.30f; h[ 2 ] = 0.30f;
					ratio = vr.m_holsterHeadRadius / 0.22f;
					break;
				case Zone::Waist:
					// Small of the back: wider, taller, reaching well behind the body.
					c[ 0 ] = 0.0f; c[ 1 ] = -0.72f; c[ 2 ] = -0.28f;
					h[ 0 ] = 0.34f; h[ 1 ] = 0.28f; h[ 2 ] = 0.30f;
					ratio = vr.m_holsterWaistRadius / 0.22f;
					break;
				case Zone::Mouth:
				{
					// Face and down to the chest, reaching out in front.
					r = Rotate( f.orientation, { 1.0f, 0.0f, 0.0f } );
					u = Rotate( f.orientation, { 0.0f, 1.0f, 0.0f } );
					fw = Rotate( f.orientation, { 0.0f, 0.0f, -1.0f } );
					c[ 0 ] = 0.0f; c[ 1 ] = -0.20f; c[ 2 ] = 0.14f;
					h[ 0 ] = 0.20f; h[ 1 ] = 0.25f; h[ 2 ] = 0.20f;
					ratio = vr.m_holsterMouthRadius / 0.13f;
					break;
				}
				default:
					return false;
			}
			const float k = f.scale * ( ratio < 0.2f ? 0.2f : ratio );
			const V3 o{ r.x * c[ 0 ] * f.scale + u.x * c[ 1 ] * f.scale + fw.x * c[ 2 ] * f.scale,
				r.y * c[ 0 ] * f.scale + u.y * c[ 1 ] * f.scale + fw.y * c[ 2 ] * f.scale,
				r.z * c[ 0 ] * f.scale + u.z * c[ 1 ] * f.scale + fw.z * c[ 2 ] * f.scale };
			center[ 0 ] = f.head.x + o.x;
			center[ 1 ] = f.head.y + o.y;
			center[ 2 ] = f.head.z + o.z;
			const V3 dirs[ 3 ] = { r, u, fw };
			for( int i = 0; i < 3; ++i )
			{
				axes[ i ][ 0 ] = dirs[ i ].x;
				axes[ i ][ 1 ] = dirs[ i ].y;
				axes[ i ][ 2 ] = dirs[ i ].z;
				half[ i ] = h[ i ] * k;
			}
			return true;
		}

		Zone ZoneAtHand( int hand )
		{
			const ControllerState &c = VRControllers::State( hand );
			if( !c.active || !c.gripValid )
			{
				return Zone::None;
			}
			Zone best = Zone::None;
			float bestDistance = 1.0f;
			for( Zone zone : { Zone::Mouth, Zone::Head, Zone::Waist } )
			{
				float center[ 3 ], axes[ 3 ][ 3 ], half[ 3 ];
				if( !ZoneShape( zone, center, axes, half ) )
				{
					continue;
				}
				const float d[ 3 ] = { c.gripPosition[ 0 ] - center[ 0 ], c.gripPosition[ 1 ] - center[ 1 ], c.gripPosition[ 2 ] - center[ 2 ] };
				float sum = 0.0f;
				for( int i = 0; i < 3; ++i )
				{
					const float local = d[ 0 ] * axes[ i ][ 0 ] + d[ 1 ] * axes[ i ][ 1 ] + d[ 2 ] * axes[ i ][ 2 ];
					const float n = half[ i ] > 0.01f ? local / half[ i ] : 99.0f;
					sum += n * n;
				}
				const float distance = std::sqrt( sum );
				if( distance < bestDistance )
				{
					bestDistance = distance;
					best = zone;
				}
			}
			return best;
		}

		const char *Name( Zone zone )
		{
			switch( zone )
			{
				case Zone::Head: return "head";
				case Zone::Waist: return "waist";
				case Zone::Mouth: return "mouth";
				default: return "none";
			}
		}
	}
}
