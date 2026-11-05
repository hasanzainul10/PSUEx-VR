#include "VRTrackers.h"

#include "VRCamera.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace PSUVR
{
	namespace VRTrackers
	{
		namespace
		{
			const char *const kRoleNames[ kTrackerRoleCount ] = { "waist", "chest", "left foot", "right foot", "left knee",
				"right knee", "left elbow", "right elbow" };

			// One-euro filter on one value: the cutoff rises with speed, so slow motion is smoothed and
			// fast motion keeps up (Casiez et al. 2012).
			struct OneEuro
			{
				bool primed = false;
				float value = 0.0f;
				float speed = 0.0f;

				static float Alpha( float cutoff, float dt )
				{
					const float tau = 1.0f / ( 6.2831853f * cutoff );
					return 1.0f / ( 1.0f + tau / dt );
				}

				float Filter( float x, float dt, float minCutoff, float beta )
				{
					if( !primed || dt <= 0.0f )
					{
						primed = true;
						value = x;
						speed = 0.0f;
						return x;
					}
					const float rawSpeed = ( x - value ) / dt;
					speed += Alpha( 1.0f, dt ) * ( rawSpeed - speed );
					const float cutoff = minCutoff + beta * std::fabs( speed );
					value += Alpha( cutoff, dt ) * ( x - value );
					return value;
				}
			};

			struct Track
			{
				TrackerState raw;
				Pose pose;
				OneEuro axis[ 3 ];
				bool seen = false;          // was ever usable this session
				bool haveGood = false;      // pose holds a good sample
				float lostFor = 0.0f;
			};

			Track s_tracks[ kTrackerRoleCount ];
			LARGE_INTEGER s_lastTime{};
			DWORD s_lastLog = 0;

			float SecondsSinceLast()
			{
				LARGE_INTEGER now, freq;
				QueryPerformanceCounter( &now );
				QueryPerformanceFrequency( &freq );
				const float dt = s_lastTime.QuadPart ? static_cast< float >( now.QuadPart - s_lastTime.QuadPart ) / freq.QuadPart : 0.0f;
				s_lastTime = now;
				return std::clamp( dt, 0.0f, 0.1f );
			}

			// Normalized lerp toward b along the short way.
			void Nlerp( float a[ 4 ], const float b[ 4 ], float t )
			{
				const float dot = a[ 0 ] * b[ 0 ] + a[ 1 ] * b[ 1 ] + a[ 2 ] * b[ 2 ] + a[ 3 ] * b[ 3 ];
				const float sign = dot < 0.0f ? -1.0f : 1.0f;
				float len = 0.0f;
				for( int i = 0; i < 4; ++i )
				{
					a[ i ] += ( sign * b[ i ] - a[ i ] ) * t;
					len += a[ i ] * a[ i ];
				}
				len = std::sqrt( len );
				if( len < 1.0e-6f )
				{
					std::copy( b, b + 4, a );
					return;
				}
				for( int i = 0; i < 4; ++i )
				{
					a[ i ] /= len;
				}
			}

			void Log()
			{
				char line[ 512 ];
				int len = sprintf_s( line, "VRTrackers:" );
				bool any = false;
				for( int role = 0; role < kTrackerRoleCount && len < 440; ++role )
				{
					const Track &t = s_tracks[ role ];
					if( !t.raw.active && !t.seen )
					{
						continue;
					}
					any = true;
					if( t.pose.valid && !t.pose.held )
					{
						len += sprintf_s( line + len, sizeof( line ) - len, " %s %s (%.2f %.2f %.2f)", kRoleNames[ role ],
							t.raw.tracked ? "ok" : "inferred", t.pose.position[ 0 ], t.pose.position[ 1 ], t.pose.position[ 2 ] );
						if( t.pose.floorValid )
						{
							len += sprintf_s( line + len, sizeof( line ) - len, " floor %.2f", t.pose.floorHeight );
						}
						len += sprintf_s( line + len, sizeof( line ) - len, ";" );
					}
					else
					{
						len += sprintf_s( line + len, sizeof( line ) - len, " %s lost %.0f ms;", kRoleNames[ role ], t.lostFor * 1000.0f );
					}
				}
				if( !any )
				{
					len += sprintf_s( line + len, sizeof( line ) - len, " none active (roles set in SteamVR > Manage Trackers?)" );
				}
				PSU_LOG( "%s\n", line );
			}
		}

		void Update( IVRRuntime *runtime )
		{
			const auto &vr = Config::Get().vr;
			const float dt = SecondsSinceLast();
			if( !vr.m_trackersEnabled || !runtime )
			{
				for( Track &t : s_tracks )
				{
					t = {};
				}
				return;
			}
			// Smoothing 0 = raw (high cutoff), 1 = heavy (low cutoff, slow response to speed).
			const float smooth = std::clamp( vr.m_trackerSmoothing, 0.0f, 1.0f );
			const float minCutoff = 12.0f * std::pow( 0.04f, smooth );  // 12 Hz .. ~0.5 Hz
			const float beta = 2.0f * ( 1.0f - 0.9f * smooth );
			const float hold = ( std::max )( vr.m_trackerLostHold, 0.0f );
			for( int role = 0; role < kTrackerRoleCount; ++role )
			{
				Track &t = s_tracks[ role ];
				runtime->GetTrackerState( static_cast< TrackerRole >( role ), t.raw );
				if( t.raw.active && t.raw.valid )
				{
					const bool wasLost = !t.haveGood || t.pose.held || !t.pose.valid;
					if( wasLost )
					{
						// Back from a loss: no smoothing across the gap.
						for( OneEuro &f : t.axis )
						{
							f.primed = false;
						}
						std::copy( t.raw.orientation, t.raw.orientation + 4, t.pose.orientation );
					}
					for( int i = 0; i < 3; ++i )
					{
						t.pose.position[ i ] = smooth > 0.0f ? t.axis[ i ].Filter( t.raw.position[ i ], dt, minCutoff, beta ) : t.raw.position[ i ];
					}
					const float k = smooth > 0.0f ? OneEuro::Alpha( minCutoff * 2.0f, ( std::max )( dt, 1.0e-3f ) ) : 1.0f;
					Nlerp( t.pose.orientation, t.raw.orientation, k );
					t.pose.floorValid = t.raw.floorValid;
					t.pose.floorHeight = t.raw.floorHeight;
					t.pose.valid = true;
					t.pose.held = false;
					t.pose.lostSeconds = 0.0f;
					t.lostFor = 0.0f;
					t.haveGood = true;
					if( !t.seen )
					{
						t.seen = true;
						PSU_LOG( "VRTrackers: %s found\n", kRoleNames[ role ] );
					}
				}
				else
				{
					t.lostFor += dt;
					t.pose.lostSeconds = t.lostFor;
					t.pose.held = t.haveGood && t.lostFor <= hold;
					t.pose.valid = t.pose.held;
				}
			}
			if( GetTickCount() - s_lastLog >= 3000 )
			{
				s_lastLog = GetTickCount();
				Log();
			}
		}

		bool Get( TrackerRole role, Pose &out )
		{
			const int index = static_cast< int >( role );
			if( index < 0 || index >= kTrackerRoleCount )
			{
				out = {};
				return false;
			}
			out = s_tracks[ index ].pose;
			return out.valid;
		}

		bool GetWorld( TrackerRole role, float world[ 3 ] )
		{
			Pose pose;
			return Get( role, pose ) && VRCamera::BodyTrackingToWorld( pose.position, world );
		}

		bool Any()
		{
			for( const Track &t : s_tracks )
			{
				if( t.pose.valid )
				{
					return true;
				}
			}
			return false;
		}

		const char *RoleName( TrackerRole role )
		{
			const int index = static_cast< int >( role );
			return index >= 0 && index < kTrackerRoleCount ? kRoleNames[ index ] : "?";
		}

		std::string StatusLine()
		{
			if( !Config::Get().vr.m_trackersEnabled )
			{
				return {};
			}
			std::string line;
			for( int role = 0; role < kTrackerRoleCount; ++role )
			{
				const Track &t = s_tracks[ role ];
				if( !t.raw.active && !t.seen )
				{
					continue;
				}
				line += line.empty() ? "" : ", ";
				line += kRoleNames[ role ];
				line += t.pose.valid && !t.pose.held ? ( t.raw.tracked ? " ok" : " inferred" ) : " lost";
			}
			return line.empty() ? "No trackers with a role (set roles in SteamVR > Manage Trackers)" : line;
		}
	}
}
