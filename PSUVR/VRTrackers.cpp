#include "VRTrackers.h"

#include "VRBindings.h"
#include "VRCamera.h"
#include "VRHaptics.h"
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

			// ---- small vector / quaternion math (x, y, z, w; right-handed, Y up, -Z forward)
			void QMul( const float a[ 4 ], const float b[ 4 ], float out[ 4 ] )
			{
				const float r[ 4 ] = {
					a[ 3 ] * b[ 0 ] + a[ 0 ] * b[ 3 ] + a[ 1 ] * b[ 2 ] - a[ 2 ] * b[ 1 ],
					a[ 3 ] * b[ 1 ] - a[ 0 ] * b[ 2 ] + a[ 1 ] * b[ 3 ] + a[ 2 ] * b[ 0 ],
					a[ 3 ] * b[ 2 ] + a[ 0 ] * b[ 1 ] - a[ 1 ] * b[ 0 ] + a[ 2 ] * b[ 3 ],
					a[ 3 ] * b[ 3 ] - a[ 0 ] * b[ 0 ] - a[ 1 ] * b[ 1 ] - a[ 2 ] * b[ 2 ] };
				std::copy( r, r + 4, out );
			}
			void QConj( const float q[ 4 ], float out[ 4 ] )
			{
				out[ 0 ] = -q[ 0 ]; out[ 1 ] = -q[ 1 ]; out[ 2 ] = -q[ 2 ]; out[ 3 ] = q[ 3 ];
			}
			void QRot( const float q[ 4 ], const float v[ 3 ], float out[ 3 ] )
			{
				const float t[ 3 ] = { 2.0f * ( q[ 1 ] * v[ 2 ] - q[ 2 ] * v[ 1 ] ), 2.0f * ( q[ 2 ] * v[ 0 ] - q[ 0 ] * v[ 2 ] ),
					2.0f * ( q[ 0 ] * v[ 1 ] - q[ 1 ] * v[ 0 ] ) };
				const float r[ 3 ] = {
					v[ 0 ] + q[ 3 ] * t[ 0 ] + q[ 1 ] * t[ 2 ] - q[ 2 ] * t[ 1 ],
					v[ 1 ] + q[ 3 ] * t[ 1 ] + q[ 2 ] * t[ 0 ] - q[ 0 ] * t[ 2 ],
					v[ 2 ] + q[ 3 ] * t[ 2 ] + q[ 0 ] * t[ 1 ] - q[ 1 ] * t[ 0 ] };
				std::copy( r, r + 3, out );
			}
			void QNorm( float q[ 4 ] )
			{
				const float len = std::sqrt( q[ 0 ] * q[ 0 ] + q[ 1 ] * q[ 1 ] + q[ 2 ] * q[ 2 ] + q[ 3 ] * q[ 3 ] );
				if( len < 1.0e-6f ) { q[ 0 ] = q[ 1 ] = q[ 2 ] = 0.0f; q[ 3 ] = 1.0f; return; }
				for( int i = 0; i < 4; ++i ) q[ i ] /= len;
			}
			void QYaw( float yaw, float out[ 4 ] )
			{
				out[ 0 ] = 0.0f; out[ 1 ] = std::sin( yaw * 0.5f ); out[ 2 ] = 0.0f; out[ 3 ] = std::cos( yaw * 0.5f );
			}

			// ---- calibration (STAGE space): joint = s * R * tracker + t, then the tracker's own offset.
			struct Calibration
			{
				bool valid = false;
				float t[ 3 ]{};
				float r[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };
				float s = 1.0f;
				float stature = 0.0f; // meters, 0 = not saved
				bool has[ kTrackerRoleCount ]{};
				float offPos[ kTrackerRoleCount ][ 3 ]{};
				float offRot[ kTrackerRoleCount ][ 4 ]{};
			};
			Calibration s_cal;
			std::string s_calText = "\x01"; // the vr.trackerCal string s_cal was parsed from

			void ParseCalibration( const std::string &text )
			{
				s_calText = text;
				s_cal = {};
				if( text.empty() )
				{
					return;
				}
				size_t start = 0;
				while( start < text.size() )
				{
					size_t end = text.find( ';', start );
					if( end == std::string::npos ) end = text.size();
					const std::string item = text.substr( start, end - start );
					start = end + 1;
					float v[ 8 ];
					int role = -1;
					if( std::sscanf( item.c_str(), " g %f %f %f %f %f %f %f %f", &v[ 0 ], &v[ 1 ], &v[ 2 ], &v[ 3 ], &v[ 4 ], &v[ 5 ], &v[ 6 ], &v[ 7 ] ) == 8 )
					{
						std::copy( v, v + 3, s_cal.t );
						std::copy( v + 3, v + 7, s_cal.r );
						QNorm( s_cal.r );
						s_cal.s = std::clamp( v[ 7 ], 0.5f, 2.0f );
						s_cal.valid = true;
					}
					else if( std::sscanf( item.c_str(), " h %f", &v[ 0 ] ) == 1 )
					{
						s_cal.stature = std::clamp( v[ 0 ], 1.0f, 2.3f );
					}
					else if( std::sscanf( item.c_str(), " r%d %f %f %f %f %f %f %f", &role, &v[ 0 ], &v[ 1 ], &v[ 2 ], &v[ 3 ], &v[ 4 ], &v[ 5 ], &v[ 6 ] ) == 8 &&
						role >= 0 && role < kTrackerRoleCount )
					{
						s_cal.has[ role ] = true;
						std::copy( v, v + 3, s_cal.offPos[ role ] );
						std::copy( v + 3, v + 7, s_cal.offRot[ role ] );
						QNorm( s_cal.offRot[ role ] );
					}
				}
				if( !s_cal.valid )
				{
					s_cal = {};
				}
			}

			// Where each joint is on a person standing straight under the head (STAGE): heights as shares of
			// the stature (eyes at 0.936), sideways (+ = right) and forward offsets from the spine line.
			struct JointModel { float height; float side; float forward; bool absoluteHeight; };
			const JointModel kJoints[ kTrackerRoleCount ] = {
				{ 0.55f, 0.0f, 0.0f, false },     // waist (hips)
				{ 0.72f, 0.0f, 0.03f, false },    // chest
				{ 0.07f, -0.11f, 0.03f, true },   // left ankle (meters above the floor)
				{ 0.07f, 0.11f, 0.03f, true },    // right ankle
				{ 0.285f, -0.10f, 0.02f, false }, // left knee
				{ 0.285f, 0.10f, 0.02f, false },  // right knee
				{ 0.63f, -0.125f, -0.02f, false },// left elbow (side as a share of the stature)
				{ 0.63f, 0.125f, -0.02f, false }, // right elbow
			};

			// Horn's closed form: the rotation that best turns the centred points a onto b (weighted), from
			// the top eigenvector of a 4x4 symmetric matrix (Jacobi sweeps). q = x, y, z, w.
			void HornRotation( const float ( *a )[ 3 ], const float ( *b )[ 3 ], const float *w, int n, float q[ 4 ] )
			{
				double S[ 3 ][ 3 ]{};
				for( int i = 0; i < n; ++i )
					for( int r = 0; r < 3; ++r )
						for( int c = 0; c < 3; ++c )
							S[ r ][ c ] += w[ i ] * a[ i ][ r ] * b[ i ][ c ];
				const double xx = S[ 0 ][ 0 ], xy = S[ 0 ][ 1 ], xz = S[ 0 ][ 2 ], yx = S[ 1 ][ 0 ], yy = S[ 1 ][ 1 ], yz = S[ 1 ][ 2 ],
					zx = S[ 2 ][ 0 ], zy = S[ 2 ][ 1 ], zz = S[ 2 ][ 2 ];
				double N[ 4 ][ 4 ] = {
					{ xx + yy + zz, yz - zy, zx - xz, xy - yx },
					{ yz - zy, xx - yy - zz, xy + yx, zx + xz },
					{ zx - xz, xy + yx, -xx + yy - zz, yz + zy },
					{ xy - yx, zx + xz, yz + zy, -xx - yy + zz } };
				double V[ 4 ][ 4 ]{};
				for( int i = 0; i < 4; ++i ) V[ i ][ i ] = 1.0;
				for( int sweep = 0; sweep < 30; ++sweep )
				{
					for( int p = 0; p < 3; ++p )
						for( int r = p + 1; r < 4; ++r )
						{
							if( std::fabs( N[ p ][ r ] ) < 1.0e-12 ) continue;
							const double theta = 0.5 * std::atan2( 2.0 * N[ p ][ r ], N[ r ][ r ] - N[ p ][ p ] );
							const double c = std::cos( theta ), s = std::sin( theta );
							for( int k = 0; k < 4; ++k )
							{
								const double kp = N[ k ][ p ], kr = N[ k ][ r ];
								N[ k ][ p ] = c * kp - s * kr;
								N[ k ][ r ] = s * kp + c * kr;
							}
							for( int k = 0; k < 4; ++k )
							{
								const double pk = N[ p ][ k ], rk = N[ r ][ k ];
								N[ p ][ k ] = c * pk - s * rk;
								N[ r ][ k ] = s * pk + c * rk;
							}
							for( int k = 0; k < 4; ++k )
							{
								const double kp = V[ k ][ p ], kr = V[ k ][ r ];
								V[ k ][ p ] = c * kp - s * kr;
								V[ k ][ r ] = s * kp + c * kr;
							}
						}
				}
				int best = 0;
				for( int i = 1; i < 4; ++i ) if( N[ i ][ i ] > N[ best ][ best ] ) best = i;
				// Horn's quaternion order is (w, x, y, z).
				q[ 3 ] = static_cast< float >( V[ 0 ][ best ] );
				q[ 0 ] = static_cast< float >( V[ 1 ][ best ] );
				q[ 1 ] = static_cast< float >( V[ 2 ][ best ] );
				q[ 2 ] = static_cast< float >( V[ 3 ][ best ] );
				QNorm( q );
			}

			int s_seenCalibRequest = 0;
			float s_countdown = -1.0f;
			IVRRuntime *s_runtime = nullptr;

			void ToStage( const float stagePos[ 3 ], const float stageRot[ 4 ], const float local[ 3 ], float out[ 3 ] )
			{
				const float d[ 3 ] = { local[ 0 ] - stagePos[ 0 ], local[ 1 ] - stagePos[ 1 ], local[ 2 ] - stagePos[ 2 ] };
				float inv[ 4 ];
				QConj( stageRot, inv );
				QRot( inv, d, out );
			}

			void Capture()
			{
				float stagePos[ 3 ], stageRot[ 4 ], headPos[ 3 ], headRot[ 4 ];
				if( !s_runtime || !s_runtime->GetStagePose( stagePos, stageRot ) || !s_runtime->GetHeadPose( headPos, headRot ) )
				{
					PSU_LOG( "VRTrackerCal: no STAGE space or head pose, not calibrated\n" );
					VRBindings::ShowToast( "Tracker calibration failed (no floor / head)" );
					return;
				}
				// Head and its level forward in STAGE.
				float head[ 3 ], invStage[ 4 ], headStageRot[ 4 ], fwd[ 3 ];
				ToStage( stagePos, stageRot, headPos, head );
				QConj( stageRot, invStage );
				QMul( invStage, headRot, headStageRot );
				const float minusZ[ 3 ] = { 0.0f, 0.0f, -1.0f };
				QRot( headStageRot, minusZ, fwd );
				const float yaw = std::atan2( -fwd[ 0 ], -fwd[ 2 ] );
				const float f[ 3 ] = { -std::sin( yaw ), 0.0f, -std::cos( yaw ) };
				const float r[ 3 ] = { -f[ 2 ], 0.0f, f[ 0 ] };
				float bodyRot[ 4 ];
				QYaw( yaw, bodyRot );
				const float stature = std::clamp( head[ 1 ] / 0.936f, 1.0f, 2.3f );
				// Spine line: under the head, ~8 cm behind the eyes.
				const float base[ 3 ] = { head[ 0 ] - f[ 0 ] * 0.08f, 0.0f, head[ 2 ] - f[ 2 ] * 0.08f };

				float a[ kTrackerRoleCount ][ 3 ], b[ kTrackerRoleCount ][ 3 ], qa[ kTrackerRoleCount ][ 4 ], w[ kTrackerRoleCount ];
				int roleOf[ kTrackerRoleCount ];
				int n = 0;
				const float weight[ kTrackerRoleCount ] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.7f, 0.7f, 0.3f, 0.3f };
				for( int role = 0; role < kTrackerRoleCount; ++role )
				{
					const Pose &p = s_tracks[ role ].pose;
					if( !p.valid || p.held )
					{
						continue;
					}
					ToStage( stagePos, stageRot, p.position, a[ n ] );
					QMul( invStage, p.orientation, qa[ n ] );
					const JointModel &j = kJoints[ role ];
					const float height = j.absoluteHeight ? j.height : j.height * stature;
					const float side = role >= 6 ? j.side * stature : j.side;
					for( int i = 0; i < 3; ++i )
					{
						b[ n ][ i ] = base[ i ] + r[ i ] * side + f[ i ] * j.forward;
					}
					b[ n ][ 1 ] = height;
					w[ n ] = weight[ role ];
					roleOf[ n ] = role;
					++n;
				}
				if( n == 0 )
				{
					PSU_LOG( "VRTrackerCal: no tracker usable, not calibrated\n" );
					VRBindings::ShowToast( "Tracker calibration failed (no trackers)" );
					return;
				}

				// Weighted centroids and the spread of the trackers.
				float wsum = 0.0f, ca[ 3 ]{}, cb[ 3 ]{};
				for( int i = 0; i < n; ++i )
				{
					wsum += w[ i ];
					for( int k = 0; k < 3; ++k ) { ca[ k ] += w[ i ] * a[ i ][ k ]; cb[ k ] += w[ i ] * b[ i ][ k ]; }
				}
				for( int k = 0; k < 3; ++k ) { ca[ k ] /= wsum; cb[ k ] /= wsum; }
				float da[ kTrackerRoleCount ][ 3 ], db[ kTrackerRoleCount ][ 3 ];
				float spreadA = 0.0f, spreadB = 0.0f;
				for( int i = 0; i < n; ++i )
				{
					for( int k = 0; k < 3; ++k ) { da[ i ][ k ] = a[ i ][ k ] - ca[ k ]; db[ i ][ k ] = b[ i ][ k ] - cb[ k ]; }
					spreadA += w[ i ] * ( da[ i ][ 0 ] * da[ i ][ 0 ] + da[ i ][ 1 ] * da[ i ][ 1 ] + da[ i ][ 2 ] * da[ i ][ 2 ] );
					spreadB += w[ i ] * ( db[ i ][ 0 ] * db[ i ][ 0 ] + db[ i ][ 1 ] * db[ i ][ 1 ] + db[ i ][ 2 ] * db[ i ][ 2 ] );
				}

				auto rms = [ & ]( const float q[ 4 ], float s, const float t[ 3 ] )
				{
					float e = 0.0f;
					for( int i = 0; i < n; ++i )
					{
						float p[ 3 ];
						QRot( q, a[ i ], p );
						for( int k = 0; k < 3; ++k )
						{
							const float d = s * p[ k ] + t[ k ] - b[ i ][ k ];
							e += w[ i ] * d * d;
						}
					}
					return std::sqrt( e / wsum );
				};

				Calibration cal;
				cal.valid = true;
				const char *kind = "full";
				if( n >= 3 && spreadA > 0.01f )
				{
					HornRotation( da, db, w, n, cal.r );
					// Scale kept near 1: Standable's space is metric, and a fitted 0.79 (run 2, upright space,
					// noisy joint guesses) shrank every tracker motion by a fifth.
					cal.s = std::clamp( std::sqrt( spreadB / spreadA ), 0.9f, 1.1f );
				}
				else
				{
					// Too few points for a full turn: yaw only.
					float cross = 0.0f, dot = 0.0f;
					for( int i = 0; i < n; ++i )
					{
						cross += w[ i ] * ( da[ i ][ 2 ] * db[ i ][ 0 ] - da[ i ][ 0 ] * db[ i ][ 2 ] );
						dot += w[ i ] * ( da[ i ][ 0 ] * db[ i ][ 0 ] + da[ i ][ 2 ] * db[ i ][ 2 ] );
					}
					QYaw( n >= 2 ? std::atan2( cross, dot ) : 0.0f, cal.r );
					kind = n >= 2 ? "yaw" : "shift";
				}
				float rc[ 3 ];
				QRot( cal.r, ca, rc );
				for( int k = 0; k < 3; ++k ) cal.t[ k ] = cb[ k ] - cal.s * rc[ k ];

				// Keep identity (real trackers on a correct space) unless the fit is clearly better.
				const float identityQ[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
				float identityT[ 3 ];
				for( int k = 0; k < 3; ++k ) identityT[ k ] = cb[ k ] - ca[ k ];
				const float zero[ 3 ] = {};
				const float rmsRaw = rms( identityQ, 1.0f, zero );
				const float rmsShift = rms( identityQ, 1.0f, identityT );
				const float rmsFit = rms( cal.r, cal.s, cal.t );
				if( rmsFit > rmsShift - 0.05f )
				{
					std::copy( identityQ, identityQ + 4, cal.r );
					cal.s = 1.0f;
					// No turn: no shift either (a correct space only needs the per-tracker offsets).
					std::fill( cal.t, cal.t + 3, 0.0f );
					kind = "none";
				}
				const float angle = 2.0f * std::acos( std::clamp( std::fabs( cal.r[ 3 ] ), 0.0f, 1.0f ) ) * 57.29578f;
				PSU_LOG( "VRTrackerCal: %d trackers, stature %.2f m, head (%.2f %.2f %.2f); whole-frame fit %s: turn %.0f deg, scale %.2f, "
					"rms raw %.0f / shift %.0f / fit %.0f cm\n", n, stature, head[ 0 ], head[ 1 ], head[ 2 ], kind, angle, cal.s,
					rmsRaw * 100.0f, rmsShift * 100.0f, rmsFit * 100.0f );

				// Each tracker's offset to its joint, in the tracker's own frame.
				char text[ 1024 ];
				int len = sprintf_s( text, "g %.4f %.4f %.4f %.5f %.5f %.5f %.5f %.4f", cal.t[ 0 ], cal.t[ 1 ], cal.t[ 2 ], cal.r[ 0 ], cal.r[ 1 ],
					cal.r[ 2 ], cal.r[ 3 ], cal.s );
				len += sprintf_s( text + len, sizeof( text ) - len, ";h %.3f", stature );
				for( int i = 0; i < n; ++i )
				{
					float p[ 3 ], q[ 4 ], inv[ 4 ], d[ 3 ], off[ 3 ], offRot[ 4 ];
					QRot( cal.r, a[ i ], p );
					for( int k = 0; k < 3; ++k ) p[ k ] = cal.s * p[ k ] + cal.t[ k ];
					QMul( cal.r, qa[ i ], q );
					QConj( q, inv );
					for( int k = 0; k < 3; ++k ) d[ k ] = b[ i ][ k ] - p[ k ];
					QRot( inv, d, off );
					QMul( inv, bodyRot, offRot );
					const float dist = std::sqrt( d[ 0 ] * d[ 0 ] + d[ 1 ] * d[ 1 ] + d[ 2 ] * d[ 2 ] );
					PSU_LOG( "VRTrackerCal: %s offset %.0f cm\n", kRoleNames[ roleOf[ i ] ], dist * 100.0f );
					len += sprintf_s( text + len, sizeof( text ) - len, ";r%d %.4f %.4f %.4f %.5f %.5f %.5f %.5f", roleOf[ i ], off[ 0 ], off[ 1 ],
						off[ 2 ], offRot[ 0 ], offRot[ 1 ], offRot[ 2 ], offRot[ 3 ] );
				}
				Config::Get().vr.m_trackerCal = text;
				ParseCalibration( text );
				Config::Get().SaveToFile();
				VRBindings::ShowToast( "Tracker calibration saved" );
			}

			// The joint a tracker stands for, LOCAL (needs a calibration with this role and STAGE).
			void UpdateJoint( int role, Pose &p )
			{
				p.jointValid = false;
				float stagePos[ 3 ], stageRot[ 4 ];
				if( !p.valid || !s_cal.valid || !s_cal.has[ role ] || !s_runtime || !s_runtime->GetStagePose( stagePos, stageRot ) )
				{
					return;
				}
				float invStage[ 4 ], a[ 3 ], qa[ 4 ], pg[ 3 ], qg[ 4 ], off[ 3 ], js[ 3 ], jq[ 4 ];
				ToStage( stagePos, stageRot, p.position, a );
				QConj( stageRot, invStage );
				QMul( invStage, p.orientation, qa );
				QRot( s_cal.r, a, pg );
				for( int k = 0; k < 3; ++k ) pg[ k ] = s_cal.s * pg[ k ] + s_cal.t[ k ];
				QMul( s_cal.r, qa, qg );
				QRot( qg, s_cal.offPos[ role ], off );
				for( int k = 0; k < 3; ++k ) js[ k ] = pg[ k ] + off[ k ];
				QMul( qg, s_cal.offRot[ role ], jq );
				// STAGE -> LOCAL
				QRot( stageRot, js, p.joint );
				for( int k = 0; k < 3; ++k ) p.joint[ k ] += stagePos[ k ];
				QMul( stageRot, jq, p.jointOrientation );
				QNorm( p.jointOrientation );
				p.jointFloorHeight = js[ 1 ];
				p.jointValid = true;
			}
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
				char line[ 1024 ];
				int len = sprintf_s( line, "VRTrackers:" );
				bool any = false;
				for( int role = 0; role < kTrackerRoleCount && len < 900; ++role )
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
						if( t.pose.jointValid )
						{
							len += sprintf_s( line + len, sizeof( line ) - len, " joint (%.2f %.2f %.2f)", t.pose.joint[ 0 ], t.pose.joint[ 1 ],
								t.pose.joint[ 2 ] );
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
					len += sprintf_s( line + len, sizeof( line ) - len, " none active (OpenXR: roles in SteamVR > Manage Trackers? OpenVR: see 'OpenVR:' lines)" );
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
						PSU_LOG( "VRTrackers: %s found (%s)\n", kRoleNames[ role ], t.raw.fromOpenVR ? "OpenVR" : "OpenXR" );
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
			s_runtime = runtime;
			if( vr.m_trackerCal != s_calText )
			{
				ParseCalibration( vr.m_trackerCal );
			}
			for( int role = 0; role < kTrackerRoleCount; ++role )
			{
				UpdateJoint( role, s_tracks[ role ].pose );
			}
			// Calibration: 5 s countdown (a buzz each second), then capture.
			if( vr.m_trackerCalibRequest != s_seenCalibRequest )
			{
				s_seenCalibRequest = vr.m_trackerCalibRequest;
				s_countdown = 5.0f;
				VRHaptics::PlayBoth( VRHaptics::Effect::Countdown );
				PSU_LOG( "VRTrackerCal: stand straight, arms down, look ahead: 5 s\n" );
			}
			if( s_countdown > 0.0f )
			{
				const float before = s_countdown;
				s_countdown -= dt;
				if( s_countdown <= 0.0f )
				{
					s_countdown = -1.0f;
					Capture();
					VRHaptics::PlayBoth( VRHaptics::Effect::Done );
				}
				else if( std::floor( before ) != std::floor( s_countdown ) )
				{
					VRHaptics::PlayBoth( VRHaptics::Effect::Countdown );
				}
			}
			if( GetTickCount() - s_lastLog >= 3000 )
			{
				s_lastLog = GetTickCount();
				Log();
			}
		}

		bool Calibrated()
		{
			return s_cal.valid;
		}

		float CalibratedStature()
		{
			return s_cal.valid ? s_cal.stature : 0.0f;
		}

		float CalibratedJointHeight( TrackerRole role )
		{
			const int index = static_cast< int >( role );
			if( index < 0 || index >= kTrackerRoleCount )
			{
				return 0.0f;
			}
			const JointModel &j = kJoints[ index ];
			return j.absoluteHeight ? j.height : j.height * s_cal.stature;
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
			bool openvr = false;
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
				openvr = openvr || t.raw.fromOpenVR;
			}
			if( openvr )
			{
				line += " (OpenVR)";
			}
			return line.empty() ? "No trackers with a role (set roles in SteamVR > Manage Trackers)" : line;
		}
	}
}
