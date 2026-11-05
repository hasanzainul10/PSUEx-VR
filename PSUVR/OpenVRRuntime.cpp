#include "OpenVRRuntime.h"

#include "../Misc/Log.h"
#include "../Json.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

// Header only: we never call openvr_api.dll's VR_* exports, only vrclient.dll's factory.
#include "../third_party/openvr/openvr.h"

namespace PSUVR
{
	namespace OpenVRTrackerSource
	{
		namespace
		{
			// vrclient.dll's core (openvr src/ivrclientcore.h): what openvr_api.dll's VR_Init uses.
			class IVRClientCore
			{
			public:
				virtual vr::EVRInitError Init( vr::EVRApplicationType eApplicationType, const char *pStartupInfo ) = 0;
				virtual void Cleanup() = 0;
				virtual vr::EVRInitError IsInterfaceVersionValid( const char *pchInterfaceVersion ) = 0;
				virtual void *GetGenericInterface( const char *pchNameAndVersion, vr::EVRInitError *peError ) = 0;
				virtual bool BIsHmdPresent() = 0;
				virtual const char *GetEnglishStringForHmdError( vr::EVRInitError eError ) = 0;
				virtual const char *GetIDForVRInitError( vr::EVRInitError eError ) = 0;
			};
			typedef void *( __cdecl *ClientCoreFactory_t )( const char *pInterfaceName, int *pReturnCode );
			const char *const kClientCoreVersion = "IVRClientCore_003";

			const char *const kRoleNames[ kTrackerRoleCount ] = { "waist", "chest", "left foot", "right foot", "left knee",
				"right knee", "left elbow", "right elbow" };

			bool s_tried = false;
			bool s_connected = false;
			bool s_ownModule = false;   // we loaded vrclient.dll (not shared with the OpenXR runtime)
			bool s_weInited = false;    // our Init call succeeded (Cleanup at disconnect)
			HMODULE s_module = nullptr;
			IVRClientCore *s_core = nullptr;
			vr::IVRSystem *s_system = nullptr;
			vr::IVRSettings *s_settings = nullptr;

			vr::TrackedDevicePose_t s_poses[ vr::k_unMaxTrackedDeviceCount ]{};
			int s_roleDevice[ kTrackerRoleCount ];
			std::string s_deviceLabel[ vr::k_unMaxTrackedDeviceCount ]; // last logged mapping per device
			int s_trackerCount = 0;
			DWORD s_lastScan = 0;
			float s_vsyncToPhotons = 0.0f;
			float s_frameSeconds = 1.0f / 90.0f;

			std::string Lower( std::string s )
			{
				std::transform( s.begin(), s.end(), s.begin(), []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
				return s;
			}

			// "TrackerRole_LeftFoot", "standable_tracker_foot_left", "Foot Left", "Hips" -> role; -1 = none.
			int RoleFromText( const std::string &text )
			{
				const std::string s = Lower( text );
				if( s.empty() )
				{
					return -1;
				}
				const bool left = s.find( "left" ) != std::string::npos;
				const bool right = s.find( "right" ) != std::string::npos;
				auto has = [ &s ]( const char *word ) { return s.find( word ) != std::string::npos; };
				if( has( "waist" ) || has( "hip" ) || has( "pelvis" ) )
				{
					return static_cast< int >( TrackerRole::Waist );
				}
				if( has( "chest" ) || has( "spine" ) )
				{
					return static_cast< int >( TrackerRole::Chest );
				}
				const bool sided = left != right;
				if( sided && ( has( "foot" ) || has( "feet" ) || has( "ankle" ) ) )
				{
					return static_cast< int >( left ? TrackerRole::LeftFoot : TrackerRole::RightFoot );
				}
				if( sided && has( "knee" ) )
				{
					return static_cast< int >( left ? TrackerRole::LeftKnee : TrackerRole::RightKnee );
				}
				if( sided && has( "elbow" ) )
				{
					return static_cast< int >( left ? TrackerRole::LeftElbow : TrackerRole::RightElbow );
				}
				return -1;
			}

			std::string StringProp( vr::TrackedDeviceIndex_t device, vr::ETrackedDeviceProperty prop )
			{
				char value[ 256 ] = "";
				vr::ETrackedPropertyError err = vr::TrackedProp_Success;
				s_system->GetStringTrackedDeviceProperty( device, prop, value, sizeof( value ), &err );
				return err == vr::TrackedProp_Success ? value : "";
			}

			// SteamVR > Manage Trackers role: steamvr.vrsettings "trackers" section, key "/devices/<system>/<serial>".
			std::string SteamVrRole( const std::string &system, const std::string &serial )
			{
				if( !s_settings || serial.empty() )
				{
					return {};
				}
				char value[ 128 ] = "";
				vr::EVRSettingsError err = vr::VRSettingsError_None;
				const std::string key = "/devices/" + system + "/" + serial;
				s_settings->GetString( "trackers", key.c_str(), value, sizeof( value ), &err );
				return err == vr::VRSettingsError_None ? value : "";
			}

			void Scan()
			{
				for( int &d : s_roleDevice )
				{
					d = -1;
				}
				s_trackerCount = 0;
				vr::ETrackedPropertyError err = vr::TrackedProp_Success;
				const float freq = s_system->GetFloatTrackedDeviceProperty( vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &err );
				if( err == vr::TrackedProp_Success && freq > 30.0f )
				{
					s_frameSeconds = 1.0f / freq;
				}
				const float photons = s_system->GetFloatTrackedDeviceProperty( vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float, &err );
				s_vsyncToPhotons = err == vr::TrackedProp_Success ? photons : 0.0f;

				for( vr::TrackedDeviceIndex_t device = 1; device < vr::k_unMaxTrackedDeviceCount; ++device )
				{
					if( s_system->GetTrackedDeviceClass( device ) != vr::TrackedDeviceClass_GenericTracker )
					{
						if( !s_deviceLabel[ device ].empty() )
						{
							PSU_LOG( "OpenVR: tracker #%u gone\n", device );
							s_deviceLabel[ device ].clear();
						}
						continue;
					}
					++s_trackerCount;
					const std::string serial = StringProp( device, vr::Prop_SerialNumber_String );
					const std::string type = StringProp( device, vr::Prop_ControllerType_String );
					const std::string system = StringProp( device, vr::Prop_TrackingSystemName_String );
					const std::string steamRole = SteamVrRole( system, serial );
					// The role set in SteamVR wins, then the input profile's type, then the serial (Standable: "Foot Left").
					int role = RoleFromText( steamRole );
					const char *from = "steamvr role";
					if( role < 0 )
					{
						role = RoleFromText( type );
						from = "type";
					}
					if( role < 0 )
					{
						role = RoleFromText( serial );
						from = "serial";
					}
					if( role >= 0 && s_roleDevice[ role ] < 0 )
					{
						s_roleDevice[ role ] = static_cast< int >( device );
					}
					else if( role >= 0 )
					{
						from = "duplicate role, ignored";
					}
					char label[ 512 ];
					sprintf_s( label, "OpenVR: tracker #%u serial '%s' type '%s' system '%s' steamvr role '%s' -> %s (%s)\n", device,
						serial.c_str(), type.c_str(), system.c_str(), steamRole.c_str(), role >= 0 ? kRoleNames[ role ] : "no role",
						role >= 0 ? from : "unknown name" );
					if( s_deviceLabel[ device ] != label )
					{
						s_deviceLabel[ device ] = label;
						PSU_LOG( "%s", label );
					}
				}
			}

			void MatrixToPose( const vr::HmdMatrix34_t &m, float position[ 3 ], float q[ 4 ] )
			{
				position[ 0 ] = m.m[ 0 ][ 3 ];
				position[ 1 ] = m.m[ 1 ][ 3 ];
				position[ 2 ] = m.m[ 2 ][ 3 ];
				const float trace = m.m[ 0 ][ 0 ] + m.m[ 1 ][ 1 ] + m.m[ 2 ][ 2 ];
				if( trace > 0.0f )
				{
					const float s = 0.5f / std::sqrt( trace + 1.0f );
					q[ 3 ] = 0.25f / s;
					q[ 0 ] = ( m.m[ 2 ][ 1 ] - m.m[ 1 ][ 2 ] ) * s;
					q[ 1 ] = ( m.m[ 0 ][ 2 ] - m.m[ 2 ][ 0 ] ) * s;
					q[ 2 ] = ( m.m[ 1 ][ 0 ] - m.m[ 0 ][ 1 ] ) * s;
				}
				else if( m.m[ 0 ][ 0 ] > m.m[ 1 ][ 1 ] && m.m[ 0 ][ 0 ] > m.m[ 2 ][ 2 ] )
				{
					const float s = 2.0f * std::sqrt( 1.0f + m.m[ 0 ][ 0 ] - m.m[ 1 ][ 1 ] - m.m[ 2 ][ 2 ] );
					q[ 3 ] = ( m.m[ 2 ][ 1 ] - m.m[ 1 ][ 2 ] ) / s;
					q[ 0 ] = 0.25f * s;
					q[ 1 ] = ( m.m[ 0 ][ 1 ] + m.m[ 1 ][ 0 ] ) / s;
					q[ 2 ] = ( m.m[ 0 ][ 2 ] + m.m[ 2 ][ 0 ] ) / s;
				}
				else if( m.m[ 1 ][ 1 ] > m.m[ 2 ][ 2 ] )
				{
					const float s = 2.0f * std::sqrt( 1.0f + m.m[ 1 ][ 1 ] - m.m[ 0 ][ 0 ] - m.m[ 2 ][ 2 ] );
					q[ 3 ] = ( m.m[ 0 ][ 2 ] - m.m[ 2 ][ 0 ] ) / s;
					q[ 0 ] = ( m.m[ 0 ][ 1 ] + m.m[ 1 ][ 0 ] ) / s;
					q[ 1 ] = 0.25f * s;
					q[ 2 ] = ( m.m[ 1 ][ 2 ] + m.m[ 2 ][ 1 ] ) / s;
				}
				else
				{
					const float s = 2.0f * std::sqrt( 1.0f + m.m[ 2 ][ 2 ] - m.m[ 0 ][ 0 ] - m.m[ 1 ][ 1 ] );
					q[ 3 ] = ( m.m[ 1 ][ 0 ] - m.m[ 0 ][ 1 ] ) / s;
					q[ 0 ] = ( m.m[ 0 ][ 2 ] + m.m[ 2 ][ 0 ] ) / s;
					q[ 1 ] = ( m.m[ 1 ][ 2 ] + m.m[ 2 ][ 1 ] ) / s;
					q[ 2 ] = 0.25f * s;
				}
				const float len = std::sqrt( q[ 0 ] * q[ 0 ] + q[ 1 ] * q[ 1 ] + q[ 2 ] * q[ 2 ] + q[ 3 ] * q[ 3 ] );
				for( int i = 0; i < 4 && len > 1.0e-6f; ++i )
				{
					q[ i ] /= len;
				}
			}

			// SteamVR's runtime folder from %LOCALAPPDATA%\openvr\openvrpaths.vrpath (what openvr_api.dll reads).
			std::string RuntimeVrClientPath()
			{
				char local[ MAX_PATH ] = "";
				if( !GetEnvironmentVariableA( "LOCALAPPDATA", local, MAX_PATH ) )
				{
					return {};
				}
				std::ifstream file( std::string( local ) + "\\openvr\\openvrpaths.vrpath" );
				if( !file )
				{
					return {};
				}
				try
				{
					const nlohmann::json j = nlohmann::json::parse( file );
					const auto &runtime = j.at( "runtime" );
					if( runtime.is_array() && !runtime.empty() )
					{
						return runtime[ 0 ].get< std::string >() + "\\bin\\vrclient.dll";
					}
				}
				catch( ... )
				{
				}
				return {};
			}
		}

		bool Connect()
		{
			if( s_tried )
			{
				return s_connected;
			}
			s_tried = true;
			for( int &d : s_roleDevice )
			{
				d = -1;
			}

			// SteamVR's OpenXR runtime is vrclient.dll itself, so it is normally loaded already.
			s_module = GetModuleHandleA( "vrclient.dll" );
			if( !s_module )
			{
				const std::string path = RuntimeVrClientPath();
				s_module = path.empty() ? nullptr : LoadLibraryA( path.c_str() );
				s_ownModule = s_module != nullptr;
				PSU_LOG( "OpenVR: vrclient.dll not loaded in the process; loading '%s': %s\n", path.c_str(), s_module ? "ok" : "failed" );
			}
			else
			{
				PSU_LOG( "OpenVR: vrclient.dll already loaded (SteamVR's OpenXR runtime), sharing it\n" );
			}
			if( !s_module )
			{
				PSU_LOG( "OpenVR: no side connection (no vrclient.dll; SteamVR not the runtime?)\n" );
				return false;
			}
			const auto factory = reinterpret_cast< ClientCoreFactory_t >( GetProcAddress( s_module, "VRClientCoreFactory" ) );
			int rc = 0;
			s_core = factory ? static_cast< IVRClientCore * >( factory( kClientCoreVersion, &rc ) ) : nullptr;
			if( !s_core )
			{
				PSU_LOG( "OpenVR: VRClientCoreFactory(%s) failed (%d)\n", kClientCoreVersion, rc );
				return false;
			}

			// Shared core: the OpenXR runtime may have initialized it already, then the interfaces
			// are there without a second Init (least intrusive). Else Init as a Background app.
			vr::EVRInitError err = vr::VRInitError_None;
			if( !s_ownModule )
			{
				s_system = static_cast< vr::IVRSystem * >( s_core->GetGenericInterface( vr::IVRSystem_Version, &err ) );
				PSU_LOG( "OpenVR: %s without Init: %s (%d)\n", vr::IVRSystem_Version, s_system ? "ok" : "unavailable", ( int )err );
			}
			if( !s_system )
			{
				for( const vr::EVRApplicationType type : { vr::VRApplication_Background, vr::VRApplication_Other } )
				{
					err = s_core->Init( type, nullptr );
					PSU_LOG( "OpenVR: Init(%s) -> %s (%d)\n", type == vr::VRApplication_Background ? "Background" : "Other",
						err == vr::VRInitError_None ? "ok" : s_core->GetIDForVRInitError( err ), ( int )err );
					if( err == vr::VRInitError_None )
					{
						s_weInited = true;
						break;
					}
				}
				s_system = static_cast< vr::IVRSystem * >( s_core->GetGenericInterface( vr::IVRSystem_Version, &err ) );
			}
			if( !s_system )
			{
				PSU_LOG( "OpenVR: %s unavailable (%d): in-process connection failed, needs the helper exe\n", vr::IVRSystem_Version, ( int )err );
				Disconnect();
				return false;
			}
			s_settings = static_cast< vr::IVRSettings * >( s_core->GetGenericInterface( vr::IVRSettings_Version, &err ) );
			PSU_LOG( "OpenVR: side connection up (%s, settings %s)\n", vr::IVRSystem_Version, s_settings ? "ok" : "unavailable" );
			s_connected = true;
			Scan();
			s_lastScan = GetTickCount();
			PSU_LOG( "OpenVR: %d generic trackers\n", s_trackerCount );
			return true;
		}

		void Disconnect()
		{
			// Never Cleanup a core the OpenXR runtime shares: that would tear down its connection.
			if( s_core && s_weInited && s_ownModule )
			{
				s_core->Cleanup();
			}
			s_core = nullptr;
			s_system = nullptr;
			s_settings = nullptr;
			s_connected = false;
			s_weInited = false;
			if( s_module && s_ownModule )
			{
				FreeLibrary( s_module );
			}
			s_module = nullptr;
			s_ownModule = false;
		}

		bool Connected()
		{
			return s_connected;
		}

		void Poll( float extraSeconds )
		{
			if( !s_connected )
			{
				return;
			}
			if( GetTickCount() - s_lastScan >= 2000 )
			{
				s_lastScan = GetTickCount();
				Scan();
			}
			// Same prediction as openvr samples: to the next vsync, plus vsync-to-photons, plus the caller's extra.
			float sinceVsync = 0.0f;
			uint64_t frame = 0;
			s_system->GetTimeSinceLastVsync( &sinceVsync, &frame );
			const float predict = std::clamp( s_frameSeconds - sinceVsync + s_vsyncToPhotons + extraSeconds, 0.0f, 0.1f );
			s_system->GetDeviceToAbsoluteTrackingPose( vr::TrackingUniverseStanding, predict, s_poses, vr::k_unMaxTrackedDeviceCount );
		}

		bool Get( TrackerRole role, TrackerState &out )
		{
			out = {};
			const int index = static_cast< int >( role );
			if( !s_connected || index < 0 || index >= kTrackerRoleCount || s_roleDevice[ index ] < 0 )
			{
				return false;
			}
			const vr::TrackedDevicePose_t &p = s_poses[ s_roleDevice[ index ] ];
			out.active = p.bDeviceIsConnected;
			out.valid = p.bDeviceIsConnected && p.bPoseIsValid;
			out.tracked = out.valid && p.eTrackingResult == vr::TrackingResult_Running_OK;
			out.fromOpenVR = true;
			if( out.valid )
			{
				MatrixToPose( p.mDeviceToAbsoluteTracking, out.position, out.orientation );
				out.floorValid = true;
				out.floorHeight = out.position[ 1 ];
			}
			return out.active;
		}

		bool GetHmd( float position[ 3 ], float orientation[ 4 ] )
		{
			if( !s_connected )
			{
				return false;
			}
			const vr::TrackedDevicePose_t &p = s_poses[ vr::k_unTrackedDeviceIndex_Hmd ];
			if( !p.bPoseIsValid )
			{
				return false;
			}
			MatrixToPose( p.mDeviceToAbsoluteTracking, position, orientation );
			return true;
		}

		int TrackerCount()
		{
			return s_connected ? s_trackerCount : 0;
		}
	}
}
