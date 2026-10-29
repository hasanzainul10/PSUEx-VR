#include "OpenXRRuntime.h"

#include "VRPerf.h"

#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstring>

#include <d3dcompiler.h>

#pragma comment( lib, "openxr_loader.lib" )
#pragma comment( lib, "d3d11.lib" )
#pragma comment( lib, "dxgi.lib" )
#pragma comment( lib, "d3dcompiler.lib" )

namespace PSUVR
{
	static const char *
	XrResultString( XrResult r )
	{
		switch( r )
		{
			case XR_SUCCESS: return "XR_SUCCESS";
			case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
			case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
			case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
			case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
			case XR_ERROR_SESSION_NOT_READY: return "XR_ERROR_SESSION_NOT_READY";
			case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
			case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
			case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
			default: return "XR_ERROR";
		}
	}

	bool OpenXRRuntime::LoadLoader()
	{
		return true;
	}

	bool OpenXRRuntime::CreateInstance()
	{
		const char *exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

		XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
		std::strncpy( ci.applicationInfo.applicationName, "PSUEx", XR_MAX_APPLICATION_NAME_SIZE );
		ci.applicationInfo.applicationVersion = 1;
		std::strncpy( ci.applicationInfo.engineName, "PSUEx", XR_MAX_ENGINE_NAME_SIZE );
		ci.applicationInfo.engineVersion = 1;
		// Headers are 1.1.x; older loaders (and many runtimes) only accept 1.0.
		ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
		ci.enabledExtensionCount = 1;
		ci.enabledExtensionNames = exts;

		XrInstance instance = XR_NULL_HANDLE;
		XrResult r = xrCreateInstance( &ci, &instance );
		if( XR_FAILED( r ) )
		{
			m_lastError = XrResultString( r );
			PSU_LOG( "OpenXR: xrCreateInstance failed: %s (0x%08X)\n", m_lastError.c_str(), ( unsigned )r );
			return false;
		}
		m_instance = static_cast< uint64_t >( instance  );

		XrSystemGetInfo sysInfo{ XR_TYPE_SYSTEM_GET_INFO };
		sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XrSystemId systemId = XR_NULL_SYSTEM_ID;
		r = xrGetSystem( instance, &sysInfo, &systemId );
		if( XR_FAILED( r ) )
		{
			m_lastError = XrResultString( r );
			PSU_LOG( "OpenXR: xrGetSystem failed: %s (start SteamVR / headset runtime)\n", m_lastError.c_str() );
			return false;
		}
		m_systemId = static_cast< uint64_t >( systemId );
		PSU_LOG( "OpenXR: instance+system ok\n" );
		return true;
	}

	bool OpenXRRuntime::CreateSession( ID3D11Device *d3d11 )
	{
		XrInstance instance = static_cast< XrInstance >( m_instance  );
		XrSystemId systemId = static_cast< XrSystemId >( m_systemId );

		PFN_xrGetD3D11GraphicsRequirementsKHR pfnReqs = nullptr;
		xrGetInstanceProcAddr( instance, "xrGetD3D11GraphicsRequirementsKHR",
			reinterpret_cast< PFN_xrVoidFunction * >( &pfnReqs ) );
		if( !pfnReqs )
		{
			PSU_LOG( "OpenXR: missing xrGetD3D11GraphicsRequirementsKHR\n" );
			return false;
		}

		XrGraphicsRequirementsD3D11KHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		XrResult r = pfnReqs( instance, systemId, &reqs );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: graphics requirements failed %s\n", XrResultString( r ) );
			return false;
		}

		XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
		binding.device = d3d11;

		XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
		sci.next = &binding;
		sci.systemId = systemId;

		XrSession session = XR_NULL_HANDLE;
		r = xrCreateSession( instance, &sci, &session );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrCreateSession failed %s\n", XrResultString( r ) );
			return false;
		}
		m_session = static_cast< uint64_t >( session  );

		XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
		XrSpace space = XR_NULL_HANDLE;
		r = xrCreateReferenceSpace( session, &spaceInfo, &space );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrCreateReferenceSpace(LOCAL) failed %s\n", XrResultString( r ) );
			return false;
		}
		m_space = static_cast< uint64_t >( space );

		XrReferenceSpaceCreateInfo viewInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
		XrSpace viewSpace = XR_NULL_HANDLE;
		r = xrCreateReferenceSpace( session, &viewInfo, &viewSpace );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrCreateReferenceSpace(VIEW) failed %s\n", XrResultString( r ) );
			return false;
		}
		m_viewSpace = static_cast< uint64_t >( viewSpace );

		// STAGE (floor at y = 0) only for the setup wizard's eye height; optional.
		uint32_t spaceCount = 0;
		if( XR_SUCCEEDED( xrEnumerateReferenceSpaces( session, 0, &spaceCount, nullptr ) ) && spaceCount )
		{
			std::vector< XrReferenceSpaceType > types( spaceCount );
			if( XR_SUCCEEDED( xrEnumerateReferenceSpaces( session, spaceCount, &spaceCount, types.data() ) ) &&
				std::find( types.begin(), types.end(), XR_REFERENCE_SPACE_TYPE_STAGE ) != types.end() )
			{
				XrReferenceSpaceCreateInfo stageInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
				stageInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
				stageInfo.poseInReferenceSpace.orientation.w = 1.0f;
				XrSpace stageSpace = XR_NULL_HANDLE;
				r = xrCreateReferenceSpace( session, &stageInfo, &stageSpace );
				if( XR_SUCCEEDED( r ) )
				{
					m_stageSpace = static_cast< uint64_t >( stageSpace );
				}
				else
				{
					PSU_LOG( "OpenXR: xrCreateReferenceSpace(STAGE) failed %s\n", XrResultString( r ) );
				}
			}
		}
		PSU_LOG( "OpenXR: STAGE space %s\n", m_stageSpace ? "available" : "not available" );
		return true;
	}

	// ---- Motion controllers -------------------------------------------------------

	static XrPath
	ToPath( XrInstance instance, const char *text )
	{
		XrPath path = XR_NULL_PATH;
		xrStringToPath( instance, text, &path );
		return path;
	}

	static XrAction
	MakeAction( XrActionSet set, XrActionType type, const char *name, const char *localized,
		const XrPath hands[ 2 ] )
	{
		XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
		info.actionType = type;
		strcpy_s( info.actionName, name );
		strcpy_s( info.localizedActionName, localized );
		info.countSubactionPaths = 2;
		info.subactionPaths = hands;
		XrAction action = XR_NULL_HANDLE;
		const XrResult r = xrCreateAction( set, &info, &action );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrCreateAction(%s) failed %s\n", name, XrResultString( r ) );
		}
		return action;
	}

	bool OpenXRRuntime::CreateActions()
	{
		const XrInstance instance = static_cast< XrInstance >( m_instance );
		const XrSession session = static_cast< XrSession >( m_session );

		XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
		strcpy_s( setInfo.actionSetName, "gameplay" );
		strcpy_s( setInfo.localizedActionSetName, "Gameplay" );
		XrActionSet set = XR_NULL_HANDLE;
		XrResult r = xrCreateActionSet( instance, &setInfo, &set );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrCreateActionSet failed %s (no motion controllers)\n", XrResultString( r ) );
			return false;
		}
		m_actionSet = static_cast< uint64_t >( set );

		const XrPath hands[ 2 ] = { ToPath( instance, "/user/hand/left" ), ToPath( instance, "/user/hand/right" ) };
		m_handPath[ 0 ] = static_cast< uint64_t >( hands[ 0 ] );
		m_handPath[ 1 ] = static_cast< uint64_t >( hands[ 1 ] );

		const XrAction gripPose = MakeAction( set, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand pose", hands );
		const XrAction aimPose = MakeAction( set, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Pointer pose", hands );
		const XrAction trigger = MakeAction( set, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", hands );
		const XrAction grip = MakeAction( set, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", hands );
		const XrAction stick = MakeAction( set, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick", hands );
		const XrAction stickClick = MakeAction( set, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click", hands );
		const XrAction primary = MakeAction( set, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "A / X", hands );
		const XrAction secondary = MakeAction( set, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "B / Y", hands );
		const XrAction menu = MakeAction( set, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", hands );
		const XrAction haptic = MakeAction( set, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration", hands );
		if( !gripPose || !aimPose || !trigger || !grip || !stick || !stickClick || !primary || !secondary || !menu || !haptic )
		{
			return false;
		}
		m_actGripPose = static_cast< uint64_t >( gripPose );
		m_actAimPose = static_cast< uint64_t >( aimPose );
		m_actTrigger = static_cast< uint64_t >( trigger );
		m_actGrip = static_cast< uint64_t >( grip );
		m_actStick = static_cast< uint64_t >( stick );
		m_actStickClick = static_cast< uint64_t >( stickClick );
		m_actPrimary = static_cast< uint64_t >( primary );
		m_actSecondary = static_cast< uint64_t >( secondary );
		m_actMenu = static_cast< uint64_t >( menu );
		m_actHaptic = static_cast< uint64_t >( haptic );

		// Suggested bindings. SteamVR maps other controllers (PS VR2 Sense, ...) onto one of
		// these, and the player can remap them in SteamVR's controller bindings.
		struct Binding { XrAction action; const char *path; };
		const auto suggest = [ & ]( const char *profile, std::initializer_list< Binding > list )
		{
			std::vector< XrActionSuggestedBinding > bindings;
			for( const Binding &b : list )
			{
				bindings.push_back( { b.action, ToPath( instance, b.path ) } );
			}
			XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
			suggested.interactionProfile = ToPath( instance, profile );
			suggested.suggestedBindings = bindings.data();
			suggested.countSuggestedBindings = static_cast< uint32_t >( bindings.size() );
			const XrResult hr = xrSuggestInteractionProfileBindings( instance, &suggested );
			if( XR_FAILED( hr ) )
			{
				PSU_LOG( "OpenXR: bindings for %s rejected %s (0x%08X)\n", profile, XrResultString( hr ), ( unsigned )hr );
			}
		};

		suggest( "/interaction_profiles/khr/simple_controller", {
			{ gripPose, "/user/hand/left/input/grip/pose" }, { gripPose, "/user/hand/right/input/grip/pose" },
			{ aimPose, "/user/hand/left/input/aim/pose" }, { aimPose, "/user/hand/right/input/aim/pose" },
			{ trigger, "/user/hand/left/input/select/click" }, { trigger, "/user/hand/right/input/select/click" },
			{ menu, "/user/hand/left/input/menu/click" }, { menu, "/user/hand/right/input/menu/click" },
			{ haptic, "/user/hand/left/output/haptic" }, { haptic, "/user/hand/right/output/haptic" } } );

		suggest( "/interaction_profiles/oculus/touch_controller", {
			{ gripPose, "/user/hand/left/input/grip/pose" }, { gripPose, "/user/hand/right/input/grip/pose" },
			{ aimPose, "/user/hand/left/input/aim/pose" }, { aimPose, "/user/hand/right/input/aim/pose" },
			{ trigger, "/user/hand/left/input/trigger/value" }, { trigger, "/user/hand/right/input/trigger/value" },
			{ grip, "/user/hand/left/input/squeeze/value" }, { grip, "/user/hand/right/input/squeeze/value" },
			{ stick, "/user/hand/left/input/thumbstick" }, { stick, "/user/hand/right/input/thumbstick" },
			{ stickClick, "/user/hand/left/input/thumbstick/click" }, { stickClick, "/user/hand/right/input/thumbstick/click" },
			{ primary, "/user/hand/left/input/x/click" }, { primary, "/user/hand/right/input/a/click" },
			{ secondary, "/user/hand/left/input/y/click" }, { secondary, "/user/hand/right/input/b/click" },
			{ menu, "/user/hand/left/input/menu/click" },
			{ haptic, "/user/hand/left/output/haptic" }, { haptic, "/user/hand/right/output/haptic" } } );

		suggest( "/interaction_profiles/valve/index_controller", {
			{ gripPose, "/user/hand/left/input/grip/pose" }, { gripPose, "/user/hand/right/input/grip/pose" },
			{ aimPose, "/user/hand/left/input/aim/pose" }, { aimPose, "/user/hand/right/input/aim/pose" },
			{ trigger, "/user/hand/left/input/trigger/value" }, { trigger, "/user/hand/right/input/trigger/value" },
			{ grip, "/user/hand/left/input/squeeze/value" }, { grip, "/user/hand/right/input/squeeze/value" },
			{ stick, "/user/hand/left/input/thumbstick" }, { stick, "/user/hand/right/input/thumbstick" },
			{ stickClick, "/user/hand/left/input/thumbstick/click" }, { stickClick, "/user/hand/right/input/thumbstick/click" },
			{ primary, "/user/hand/left/input/a/click" }, { primary, "/user/hand/right/input/a/click" },
			{ secondary, "/user/hand/left/input/b/click" }, { secondary, "/user/hand/right/input/b/click" },
			{ haptic, "/user/hand/left/output/haptic" }, { haptic, "/user/hand/right/output/haptic" } } );

		suggest( "/interaction_profiles/htc/vive_controller", {
			{ gripPose, "/user/hand/left/input/grip/pose" }, { gripPose, "/user/hand/right/input/grip/pose" },
			{ aimPose, "/user/hand/left/input/aim/pose" }, { aimPose, "/user/hand/right/input/aim/pose" },
			{ trigger, "/user/hand/left/input/trigger/value" }, { trigger, "/user/hand/right/input/trigger/value" },
			{ grip, "/user/hand/left/input/squeeze/click" }, { grip, "/user/hand/right/input/squeeze/click" },
			{ stick, "/user/hand/left/input/trackpad" }, { stick, "/user/hand/right/input/trackpad" },
			{ stickClick, "/user/hand/left/input/trackpad/click" }, { stickClick, "/user/hand/right/input/trackpad/click" },
			{ menu, "/user/hand/left/input/menu/click" }, { menu, "/user/hand/right/input/menu/click" },
			{ haptic, "/user/hand/left/output/haptic" }, { haptic, "/user/hand/right/output/haptic" } } );

		for( int hand = 0; hand < 2; ++hand )
		{
			XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
			spaceInfo.subactionPath = hands[ hand ];
			spaceInfo.poseInActionSpace.orientation.w = 1.0f;
			XrSpace space = XR_NULL_HANDLE;
			spaceInfo.action = gripPose;
			if( XR_SUCCEEDED( xrCreateActionSpace( session, &spaceInfo, &space ) ) )
			{
				m_gripSpace[ hand ] = static_cast< uint64_t >( space );
			}
			space = XR_NULL_HANDLE;
			spaceInfo.action = aimPose;
			if( XR_SUCCEEDED( xrCreateActionSpace( session, &spaceInfo, &space ) ) )
			{
				m_aimSpace[ hand ] = static_cast< uint64_t >( space );
			}
		}

		XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		attach.countActionSets = 1;
		attach.actionSets = &set;
		r = xrAttachSessionActionSets( session, &attach );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrAttachSessionActionSets failed %s\n", XrResultString( r ) );
			return false;
		}
		PSU_LOG( "OpenXR: controller actions ready\n" );
		return true;
	}

	void OpenXRRuntime::DestroyActions()
	{
		for( int hand = 0; hand < 2; ++hand )
		{
			if( m_gripSpace[ hand ] ) { xrDestroySpace( static_cast< XrSpace >( m_gripSpace[ hand ] ) ); m_gripSpace[ hand ] = 0; }
			if( m_aimSpace[ hand ] ) { xrDestroySpace( static_cast< XrSpace >( m_aimSpace[ hand ] ) ); m_aimSpace[ hand ] = 0; }
			m_controllers[ hand ] = {};
		}
		// Destroying the set destroys its actions.
		if( m_actionSet )
		{
			xrDestroyActionSet( static_cast< XrActionSet >( m_actionSet ) );
			m_actionSet = 0;
		}
		m_actGripPose = m_actAimPose = m_actTrigger = m_actGrip = m_actStick = 0;
		m_actStickClick = m_actPrimary = m_actSecondary = m_actMenu = m_actHaptic = 0;
	}

	void OpenXRRuntime::LogInteractionProfiles()
	{
		if( !m_actionSet )
		{
			return;
		}
		const XrInstance instance = static_cast< XrInstance >( m_instance );
		const XrSession session = static_cast< XrSession >( m_session );
		static const char *const kHandNames[ 2 ] = { "left", "right" };
		for( int hand = 0; hand < 2; ++hand )
		{
			XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
			if( XR_FAILED( xrGetCurrentInteractionProfile( session, static_cast< XrPath >( m_handPath[ hand ] ), &state ) ) )
			{
				continue;
			}
			char text[ XR_MAX_PATH_LENGTH ] = "(none)";
			uint32_t length = 0;
			if( state.interactionProfile != XR_NULL_PATH )
			{
				xrPathToString( instance, state.interactionProfile, sizeof( text ), &length, text );
			}
			PSU_LOG( "OpenXR: %s hand profile %s\n", kHandNames[ hand ], text );
		}
	}

	static void
	CopyPose( const XrPosef &pose, float position[ 3 ], float orientation[ 4 ] )
	{
		position[ 0 ] = pose.position.x;
		position[ 1 ] = pose.position.y;
		position[ 2 ] = pose.position.z;
		orientation[ 0 ] = pose.orientation.x;
		orientation[ 1 ] = pose.orientation.y;
		orientation[ 2 ] = pose.orientation.z;
		orientation[ 3 ] = pose.orientation.w;
	}

	void OpenXRRuntime::UpdateInput()
	{
		if( !m_actionSet || !m_frameStarted )
		{
			return;
		}
		const XrSession session = static_cast< XrSession >( m_session );
		XrActiveActionSet active{ static_cast< XrActionSet >( m_actionSet ), XR_NULL_PATH };
		XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		const XrResult syncHr = xrSyncActions( session, &sync );
		if( XR_FAILED( syncHr ) || syncHr == XR_SESSION_NOT_FOCUSED )
		{
			// Not focused (SteamVR dashboard open): no controller input.
			m_controllers[ 0 ] = {};
			m_controllers[ 1 ] = {};
			return;
		}

		// Same time as the next views, so hands and eyes line up.
		const XrTime time = m_displayTime + m_displayPeriod;
		const XrSpace base = static_cast< XrSpace >( m_space );
		constexpr XrSpaceLocationFlags kPoseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		for( int hand = 0; hand < 2; ++hand )
		{
			ControllerState &c = m_controllers[ hand ];
			c = {};

			XrActionStateGetInfo get{ XR_TYPE_ACTION_STATE_GET_INFO };
			get.subactionPath = static_cast< XrPath >( m_handPath[ hand ] );

			XrActionStatePose poseState{ XR_TYPE_ACTION_STATE_POSE };
			get.action = static_cast< XrAction >( m_actGripPose );
			c.active = XR_SUCCEEDED( xrGetActionStatePose( session, &get, &poseState ) ) && poseState.isActive;

			if( m_gripSpace[ hand ] )
			{
				XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
				XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
				location.next = &velocity;
				if( XR_SUCCEEDED( xrLocateSpace( static_cast< XrSpace >( m_gripSpace[ hand ] ), base, time, &location ) ) )
				{
					c.gripValid = ( location.locationFlags & kPoseValid ) == kPoseValid;
					if( c.gripValid )
					{
						CopyPose( location.pose, c.gripPosition, c.gripOrientation );
					}
					if( velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT )
					{
						c.velocityValid = true;
						c.linearVelocity[ 0 ] = velocity.linearVelocity.x;
						c.linearVelocity[ 1 ] = velocity.linearVelocity.y;
						c.linearVelocity[ 2 ] = velocity.linearVelocity.z;
					}
					if( velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT )
					{
						c.angularVelocity[ 0 ] = velocity.angularVelocity.x;
						c.angularVelocity[ 1 ] = velocity.angularVelocity.y;
						c.angularVelocity[ 2 ] = velocity.angularVelocity.z;
					}
				}
			}
			if( m_aimSpace[ hand ] )
			{
				XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
				if( XR_SUCCEEDED( xrLocateSpace( static_cast< XrSpace >( m_aimSpace[ hand ] ), base, time, &location ) ) )
				{
					c.aimValid = ( location.locationFlags & kPoseValid ) == kPoseValid;
					if( c.aimValid )
					{
						CopyPose( location.pose, c.aimPosition, c.aimOrientation );
					}
				}
			}

			const auto readFloat = [ & ]( uint64_t action ) -> float
			{
				XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
				get.action = static_cast< XrAction >( action );
				return XR_SUCCEEDED( xrGetActionStateFloat( session, &get, &state ) ) && state.isActive ? state.currentState : 0.0f;
			};
			const auto readBool = [ & ]( uint64_t action ) -> bool
			{
				XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
				get.action = static_cast< XrAction >( action );
				return XR_SUCCEEDED( xrGetActionStateBoolean( session, &get, &state ) ) && state.isActive && state.currentState;
			};
			c.trigger = readFloat( m_actTrigger );
			c.grip = readFloat( m_actGrip );
			XrActionStateVector2f stickState{ XR_TYPE_ACTION_STATE_VECTOR2F };
			get.action = static_cast< XrAction >( m_actStick );
			if( XR_SUCCEEDED( xrGetActionStateVector2f( session, &get, &stickState ) ) && stickState.isActive )
			{
				c.stick[ 0 ] = stickState.currentState.x;
				c.stick[ 1 ] = stickState.currentState.y;
			}
			if( c.trigger > 0.5f ) c.buttons |= kControllerTrigger;
			if( c.grip > 0.5f ) c.buttons |= kControllerGrip;
			if( readBool( m_actStickClick ) ) c.buttons |= kControllerStick;
			if( readBool( m_actPrimary ) ) c.buttons |= kControllerPrimary;
			if( readBool( m_actSecondary ) ) c.buttons |= kControllerSecondary;
			if( readBool( m_actMenu ) ) c.buttons |= kControllerMenu;
		}
	}

	bool OpenXRRuntime::GetControllerState( int hand, ControllerState &out )
	{
		if( hand < 0 || hand > 1 )
		{
			out = {};
			return false;
		}
		out = m_controllers[ hand ];
		return out.active;
	}

	void OpenXRRuntime::Haptic( int hand, float amplitude, float seconds )
	{
		if( !m_actHaptic || hand < 0 || hand > 1 || !m_sessionRunning )
		{
			return;
		}
		XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
		vibration.amplitude = amplitude < 0.0f ? 0.0f : ( amplitude > 1.0f ? 1.0f : amplitude );
		vibration.duration = static_cast< XrDuration >( ( seconds > 0.0f ? seconds : 0.0f ) * 1.0e9 );
		vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
		XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
		info.action = static_cast< XrAction >( m_actHaptic );
		info.subactionPath = static_cast< XrPath >( m_handPath[ hand ] );
		xrApplyHapticFeedback( static_cast< XrSession >( m_session ), &info,
			reinterpret_cast< const XrHapticBaseHeader * >( &vibration ) );
	}

	int64_t OpenXRRuntime::PickSwapchainFormat( uint64_t sessionHandle )
	{
		XrSession session = static_cast< XrSession >( sessionHandle );
		uint32_t formatCount = 0;
		xrEnumerateSwapchainFormats( session, 0, &formatCount, nullptr );
		std::vector< int64_t > formats( formatCount );
		if( formatCount )
		{
			xrEnumerateSwapchainFormats( session, formatCount, &formatCount, formats.data() );
		}

		for( uint32_t i = 0; i < formatCount; ++i )
		{
			PSU_LOG( "OpenXR: listed format[%u]=%lld\n", i, ( long long )formats[ i ] );
		}

		// D3D9 A8R8G8B8 shared surfaces open as B8G8R8A8_* — prefer that family.
		const int64_t preferred[] = {
			DXGI_FORMAT_B8G8R8A8_UNORM,
			DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
			DXGI_FORMAT_B8G8R8A8_TYPELESS,
			DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			DXGI_FORMAT_R8G8B8A8_TYPELESS,
		};
		for( int64_t want : preferred )
		{
			for( int64_t have : formats )
			{
				if( have == want )
				{
					PSU_LOG( "OpenXR: picked swapchain format %lld\n", ( long long )want );
					return want;
				}
			}
		}

		if( !formats.empty() )
		{
			PSU_LOG( "OpenXR: using first listed format %lld\n", ( long long )formats[ 0 ] );
			return formats[ 0 ];
		}
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	}

	void OpenXRRuntime::DestroySwapchains()
	{
		for( int eye = 0; eye < 2; ++eye )
		{
			if( m_swapchainImages[ eye ] )
			{
				delete[] reinterpret_cast< XrSwapchainImageD3D11KHR * >( m_swapchainImages[ eye ] );
				m_swapchainImages[ eye ] = nullptr;
			}
			m_swapchainImageCount[ eye ] = 0;
			if( m_swapchain[ eye ] )
			{
				xrDestroySwapchain( static_cast< XrSwapchain >( m_swapchain[ eye ] ) );
				m_swapchain[ eye ] = 0;
			}
		}
		m_swapchainWidth = 0;
		m_swapchainHeight = 0;
	}

	bool OpenXRRuntime::CreateSwapchains( int32_t width, int32_t height )
	{
		XrSession session = static_cast< XrSession >( m_session );
		XrInstance instance = static_cast< XrInstance >( m_instance );
		XrSystemId systemId = static_cast< XrSystemId >( m_systemId );

		uint32_t viewCount = 0;
		xrEnumerateViewConfigurationViews( instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr );
		std::vector< XrViewConfigurationView > views( viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW } );
		xrEnumerateViewConfigurationViews( instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data() );
		if( viewCount < 2 )
		{
			PSU_LOG( "OpenXR: expected stereo views, got %u\n", viewCount );
			return false;
		}

		// Virtual screen: match game BB when provided. Avoid 2K+ shared D3D9 RTs (device loss / black).
		if( width < 1 || height < 1 )
		{
			width = static_cast< int32_t >( views[ 0 ].recommendedImageRectWidth );
			height = static_cast< int32_t >( views[ 0 ].recommendedImageRectHeight );
			// eyeResolutionScale; even sizes.
			width = static_cast< int32_t >( width * m_eyeScale + 0.5f ) & ~1;
			height = static_cast< int32_t >( height * m_eyeScale + 0.5f ) & ~1;
		}
		// Clamp to runtime limits.
		const int32_t maxW = static_cast< int32_t >( views[ 0 ].maxImageRectWidth );
		const int32_t maxH = static_cast< int32_t >( views[ 0 ].maxImageRectHeight );
		if( width < 1 ) width = 1;
		if( height < 1 ) height = 1;
		if( maxW > 0 && width > maxW ) width = maxW;
		if( maxH > 0 && height > maxH ) height = maxH;

		m_swapchainFormat = PickSwapchainFormat( m_session );
		m_swapchainWidth = width;
		m_swapchainHeight = height;

		DestroySwapchains();
		m_swapchainWidth = width;
		m_swapchainHeight = height;

		for( int eye = 0; eye < 2; ++eye )
		{
			if( !CreateSwapchain( width, height, m_swapchain[ eye ], m_swapchainImages[ eye ], m_swapchainImageCount[ eye ] ) )
			{
				DestroySwapchains();
				return false;
			}
		}

		PSU_LOG( "OpenXR: eye swapchains %dx%d fmt=%lld\n",
			m_swapchainWidth, m_swapchainHeight, ( long long )m_swapchainFormat );
		return true;
	}

	bool OpenXRRuntime::CreateSwapchain( int32_t width, int32_t height, uint64_t &handle, void *&images, uint32_t &imageCount )
	{
		XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		sci.format = m_swapchainFormat;
		sci.sampleCount = 1;
		sci.width = static_cast< uint32_t >( width );
		sci.height = static_cast< uint32_t >( height );
		sci.faceCount = 1;
		sci.arraySize = 1;
		sci.mipCount = 1;

		XrSwapchain swapchain = XR_NULL_HANDLE;
		XrResult r = xrCreateSwapchain( static_cast< XrSession >( m_session ), &sci, &swapchain );
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrCreateSwapchain failed %s (0x%08X) fmt=%lld %dx%d\n",
				XrResultString( r ), ( unsigned )r, ( long long )m_swapchainFormat, width, height );
			return false;
		}
		handle = static_cast< uint64_t >( swapchain );

		uint32_t count = 0;
		xrEnumerateSwapchainImages( swapchain, 0, &count, nullptr );
		auto *list = new XrSwapchainImageD3D11KHR[ count ];
		for( uint32_t i = 0; i < count; ++i )
		{
			list[ i ] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
		}
		xrEnumerateSwapchainImages( swapchain, count, &count, reinterpret_cast< XrSwapchainImageBaseHeader * >( list ) );
		imageCount = count;
		images = list;
		return true;
	}

	bool OpenXRRuntime::CreatePanelSwapchain( int32_t width, int32_t height )
	{
		DestroyPanelSwapchain();
		DestroyVisorSwapchain();
		if( !CreateSwapchain( width, height, m_panelSwapchain, m_panelImages, m_panelImageCount ) )
		{
			return false;
		}
		if( !CreateSwapchain( width, height, m_visorSwapchain, m_visorImages, m_visorImageCount ) )
		{
			DestroyPanelSwapchain();
			DestroyVisorSwapchain();
			return false;
		}
		m_panelWidth = width;
		m_panelHeight = height;
		PSU_LOG( "OpenXR: panel+visor swapchains %dx%d\n", width, height );
		return true;
	}

	void OpenXRRuntime::DestroyPanelSwapchain()
	{
		if( m_panelImages )
		{
			delete[] reinterpret_cast< XrSwapchainImageD3D11KHR * >( m_panelImages );
			m_panelImages = nullptr;
		}
		m_panelImageCount = 0;
		if( m_panelSwapchain )
		{
			xrDestroySwapchain( static_cast< XrSwapchain >( m_panelSwapchain ) );
			m_panelSwapchain = 0;
		}
		m_panelWidth = 0;
		m_panelHeight = 0;
	}

	void OpenXRRuntime::DestroyVisorSwapchain()
	{
		if( m_visorImages )
		{
			delete[] reinterpret_cast< XrSwapchainImageD3D11KHR * >( m_visorImages );
			m_visorImages = nullptr;
		}
		m_visorImageCount = 0;
		if( m_visorSwapchain )
		{
			xrDestroySwapchain( static_cast< XrSwapchain >( m_visorSwapchain ) );
			m_visorSwapchain = 0;
		}
	}

	void OpenXRRuntime::DestroyLabelSwapchain()
	{
		if( m_labelImages )
		{
			delete[] reinterpret_cast< XrSwapchainImageD3D11KHR * >( m_labelImages );
			m_labelImages = nullptr;
		}
		m_labelImageCount = 0;
		if( m_labelSwapchain )
		{
			xrDestroySwapchain( static_cast< XrSwapchain >( m_labelSwapchain ) );
			m_labelSwapchain = 0;
		}
		m_labelWidth = 0;
		m_labelHeight = 0;
	}

	bool OpenXRRuntime::SubmitHandLabels( ID3D11Texture2D *tex, const float position[ 2 ][ 3 ], const float orientation[ 2 ][ 4 ],
		float widthM, const bool visible[ 2 ] )
	{
		if( !tex || !m_context || !m_device || !m_frameStarted || !m_shouldRender || !m_session )
		{
			return false;
		}
		D3D11_TEXTURE2D_DESC desc{};
		tex->GetDesc( &desc );
		if( !m_labelSwapchain || m_labelWidth != static_cast< int32_t >( desc.Width ) || m_labelHeight != static_cast< int32_t >( desc.Height ) )
		{
			DestroyLabelSwapchain();
			if( !CreateSwapchain( static_cast< int32_t >( desc.Width ), static_cast< int32_t >( desc.Height ),
				m_labelSwapchain, m_labelImages, m_labelImageCount ) )
			{
				DestroyLabelSwapchain();
				return false;
			}
			m_labelWidth = static_cast< int32_t >( desc.Width );
			m_labelHeight = static_cast< int32_t >( desc.Height );
			PSU_LOG( "OpenXR: label swapchain %dx%d\n", m_labelWidth, m_labelHeight );
		}
		if( !BlitIntoSwapchain( m_labelSwapchain, m_labelImages, tex ) )
		{
			return false;
		}
		m_context->Flush();
		m_labelSize = widthM;
		for( int hand = 0; hand < 2; ++hand )
		{
			m_submitLabels[ hand ] = visible[ hand ];
			std::memcpy( m_labelPosition[ hand ], position[ hand ], sizeof( m_labelPosition[ hand ] ) );
			std::memcpy( m_labelOrientation[ hand ], orientation[ hand ], sizeof( m_labelOrientation[ hand ] ) );
		}
		return true;
	}

	void OpenXRRuntime::SetEyeResolutionScale( float scale )
	{
		scale = std::clamp( scale, 0.5f, 1.5f );
		if( !m_ready || !m_session || m_frameStarted || std::fabs( scale - m_eyeScale ) < 0.005f )
		{
			if( !m_ready )
			{
				m_eyeScale = scale;  // Init creates them at this size
			}
			m_pendingEyeScale = scale;
			return;
		}
		// Wait until the value stays put for half a second (the slider is dragged through many values).
		const unsigned long long now = GetTickCount64();
		if( std::fabs( scale - m_pendingEyeScale ) >= 0.005f )
		{
			m_pendingEyeScale = scale;
			m_pendingEyeScaleSince = now;
			return;
		}
		if( now - m_pendingEyeScaleSince < 500 || std::fabs( scale - m_failedEyeScale ) < 0.005f )
		{
			return;
		}
		const float previous = m_eyeScale;
		m_eyeScale = scale;
		PSU_LOG( "OpenXR: eye resolution scale %.2f -> %.2f\n", previous, scale );
		if( !CreateSwapchains( 0, 0 ) )
		{
			// Keep VR alive: back to the old size, and don't retry this scale.
			m_failedEyeScale = scale;
			m_eyeScale = previous;
			if( !CreateSwapchains( 0, 0 ) )
			{
				PSU_LOG( "OpenXR: eye swapchains could not be recreated\n" );
			}
		}
	}

	bool OpenXRRuntime::ResizeSwapchains( int32_t width, int32_t height )
	{
		if( !m_ready || !m_session )
		{
			return false;
		}
		if( width == m_panelWidth && height == m_panelHeight && m_panelSwapchain && m_visorSwapchain )
		{
			return true;
		}
		return CreatePanelSwapchain( width, height );
	}

	bool OpenXRRuntime::Init( ID3D11Device *d3d11 )
	{
		Shutdown();
		m_device = d3d11;
		d3d11->GetImmediateContext( &m_context );

		// Eye swapchains at the runtime's recommended size; the panel swapchain is
		// created later at the game back-buffer size (ResizeSwapchains).
		if( !LoadLoader() || !CreateInstance() || !CreateSession( d3d11 ) || !CreateSwapchains( 0, 0 ) )
		{
			Shutdown();
			return false;
		}
		// Without actions the game still runs in VR, just without motion controllers.
		if( !CreateActions() )
		{
			DestroyActions();
		}
		m_ready = true;
		PSU_LOG( "OpenXR: runtime ready\n" );
		return true;
	}

	void OpenXRRuntime::Shutdown()
	{
		m_ready = false;
		m_frameStarted = false;
		m_sessionRunning = false;
		m_loggedSubmitFormats = false;

		DestroyLabelSwapchain();
		DestroyVisorSwapchain();
		DestroyPanelSwapchain();
		DestroySwapchains();
		if( m_blitRS ) { m_blitRS->Release(); m_blitRS = nullptr; }
		if( m_blitSampler ) { m_blitSampler->Release(); m_blitSampler = nullptr; }
		if( m_blitPSLinearize ) { m_blitPSLinearize->Release(); m_blitPSLinearize = nullptr; }
		if( m_blitPS ) { m_blitPS->Release(); m_blitPS = nullptr; }
		if( m_blitVS ) { m_blitVS->Release(); m_blitVS = nullptr; }
		m_loggedBlitRtv = false;
		DestroyActions();
		if( m_stageSpace )
		{
			xrDestroySpace( static_cast< XrSpace >( m_stageSpace ) );
			m_stageSpace = 0;
		}
		if( m_viewSpace )
		{
			xrDestroySpace( static_cast< XrSpace >( m_viewSpace ) );
			m_viewSpace = 0;
		}
		if( m_space )
		{
			xrDestroySpace( static_cast< XrSpace >( m_space ) );
			m_space = 0;
		}
		if( m_session )
		{
			xrDestroySession( static_cast< XrSession >( m_session ) );
			m_session = 0;
		}
		if( m_instance )
		{
			xrDestroyInstance( static_cast< XrInstance >( m_instance ) );
			m_instance = 0;
		}
		if( m_context )
		{
			m_context->Release();
			m_context = nullptr;
		}
		m_device = nullptr;
	}

	void OpenXRRuntime::PumpEvents()
	{
		if( !m_ready || !m_instance )
		{
			return;
		}

		XrSession session = static_cast< XrSession >( m_session );
		XrInstance instance = static_cast< XrInstance >( m_instance );

		XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
		while( xrPollEvent( instance, &ev ) == XR_SUCCESS )
		{
			if( ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED )
			{
				LogInteractionProfiles();
			}
			else if( ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
			{
				auto *changed = reinterpret_cast< XrEventDataSessionStateChanged * >( &ev );
				m_sessionState = static_cast< int >( changed->state );
				PSU_LOG( "OpenXR: session state %d\n", m_sessionState );
				if( changed->state == XR_SESSION_STATE_READY )
				{
					XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					const XrResult beginHr = xrBeginSession( session, &bi );
					if( XR_SUCCEEDED( beginHr ) )
					{
						m_sessionRunning = true;
						PSU_LOG( "OpenXR: xrBeginSession ok\n" );
					}
					else
					{
						PSU_LOG( "OpenXR: xrBeginSession failed %d\n", static_cast< int >( beginHr ) );
					}
				}
				else if( changed->state == XR_SESSION_STATE_STOPPING )
				{
					xrEndSession( session );
					m_sessionRunning = false;
				}
				else if( changed->state == XR_SESSION_STATE_EXITING ||
						 changed->state == XR_SESSION_STATE_LOSS_PENDING )
				{
					m_sessionRunning = false;
				}
			}
			ev = { XR_TYPE_EVENT_DATA_BUFFER };
		}
	}

	bool OpenXRRuntime::BeginFrame()
	{
		if( !m_ready )
		{
			return false;
		}

		PumpEvents();

		if( !m_sessionRunning )
		{
			static bool s_loggedWait = false;
			if( !s_loggedWait && m_sessionState >= XR_SESSION_STATE_READY )
			{
				s_loggedWait = true;
				PSU_LOG( "OpenXR: BeginFrame waiting (sessionRunning=0 state=%d)\n", m_sessionState );
			}
			return false;
		}

		// SteamVR only advances READY → SYNCHRONIZED → VISIBLE after WaitFrame.
		// Gating on VISIBLE deadlocks the session (HMD stays on the SteamVR dashboard).
		XrSession session = static_cast< XrSession >( m_session );

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frameState{ XR_TYPE_FRAME_STATE };
		XrResult waitHr;
		{
			VRPerf::Scope timer( VRPerf::Stage::WaitFrame );
			waitHr = xrWaitFrame( session, &waitInfo, &frameState );
		}
		if( XR_FAILED( waitHr ) )
		{
			static unsigned s_waitFails = 0;
			if( ++s_waitFails <= 8 )
			{
				PSU_LOG( "OpenXR: xrWaitFrame failed %d state=%d\n",
					static_cast< int >( waitHr ), m_sessionState );
			}
			return false;
		}
		m_displayTime = frameState.predictedDisplayTime;
		m_displayPeriod = frameState.predictedDisplayPeriod;
		VRPerf::SetDisplayPeriod( m_displayPeriod );
		m_shouldRender = frameState.shouldRender;
		// Testing with emulated controllers and no headset worn (SteamVR's null HMD stays
		// "synchronized", never visible): render anyway, so the eye passes and everything
		// that depends on them run. The runtime simply doesn't show the frames.
		if( !m_shouldRender && Config::Get().vr.m_emulateControllers && m_sessionState >= XR_SESSION_STATE_SYNCHRONIZED &&
			m_sessionState <= XR_SESSION_STATE_FOCUSED )
		{
			m_shouldRender = true;
		}

		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		if( XR_FAILED( xrBeginFrame( session, &beginInfo ) ) )
		{
			return false;
		}
		m_frameStarted = true;

		static bool s_loggedFrame = false;
		if( !s_loggedFrame )
		{
			s_loggedFrame = true;
			PSU_LOG( "OpenXR: first WaitFrame/BeginFrame ok state=%d shouldRender=%d\n",
				m_sessionState, ( int )m_shouldRender );
		}
		return true;
	}

	bool OpenXRRuntime::AcquireSwapchains( ID3D11Texture2D **left, ID3D11Texture2D **right )
	{
		if( !m_frameStarted || !m_shouldRender )
		{
			return false;
		}
		ID3D11Texture2D **outs[ 2 ] = { left, right };
		for( int eye = 0; eye < 2; ++eye )
		{
			XrSwapchain swapchain = static_cast< XrSwapchain >( m_swapchain[ eye ]  );
			XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			uint32_t index = 0;
			VRPerf::Scope swapTimer( VRPerf::Stage::SwapchainWait );
			if( XR_FAILED( xrAcquireSwapchainImage( swapchain, &ai, &index ) ) )
			{
				return false;
			}
			XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wi.timeout = XR_INFINITE_DURATION;
			if( XR_FAILED( xrWaitSwapchainImage( swapchain, &wi ) ) )
			{
				return false;
			}
			m_acquiredIndex[ eye ] = index;
			auto *images = reinterpret_cast< XrSwapchainImageD3D11KHR * >( m_swapchainImages[ eye ] );
			if( outs[ eye ] )
			{
				*outs[ eye ] = images[ index ].texture;
			}
		}
		return true;
	}

	static DXGI_FORMAT RtvFormatForClear( DXGI_FORMAT fmt, int64_t /*swapchainFmt*/ )
	{
		// D3D9 content is already gamma-encoded. Prefer UNORM views on typeless
		// images so clears/blits are not double-encoded through an sRGB RTV.
		switch( fmt )
		{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_UNKNOWN: return DXGI_FORMAT_B8G8R8A8_UNORM;
			default: return fmt;
		}
	}

	bool OpenXRRuntime::ClearEyes( float r, float g, float b )
	{
		ID3D11Texture2D *left = nullptr;
		ID3D11Texture2D *right = nullptr;
		if( !AcquireSwapchains( &left, &right ) || !m_context )
		{
			return false;
		}

		const float color[ 4 ] = { r, g, b, 1.0f };
		ID3D11Texture2D *eyes[ 2 ] = { left, right };
		int cleared = 0;
		for( int eye = 0; eye < 2; ++eye )
		{
			D3D11_TEXTURE2D_DESC desc{};
			eyes[ eye ]->GetDesc( &desc );
			DXGI_FORMAT rtvFmt = RtvFormatForClear( desc.Format, m_swapchainFormat );

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = rtvFmt;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

			ID3D11RenderTargetView *rtv = nullptr;
			const HRESULT hr = m_device->CreateRenderTargetView( eyes[ eye ], &rtvDesc, &rtv );
			if( SUCCEEDED( hr ) && rtv )
			{
				m_context->ClearRenderTargetView( rtv, color );
				rtv->Release();
				++cleared;
			}
			else
			{
				static unsigned s_rtvFails = 0;
				if( ++s_rtvFails <= 8 )
				{
					PSU_LOG( "OpenXR: ClearEyes CreateRTV failed hr=0x%08X texFmt=%u as %u\n",
						hr, ( unsigned )desc.Format, ( unsigned )rtvFmt );
				}
			}
			XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage( static_cast< XrSwapchain >( m_swapchain[ eye ]  ), &ri );
		}
		m_clearedThisFrame = ( cleared == 2 );
		m_submitBackdrop = m_clearedThisFrame;
		m_submitQuadLayer = false;
		return m_clearedThisFrame;
	}

	void OpenXRRuntime::SetVirtualScreenPlacement( float distanceMeters, float heightMeters )
	{
		if( distanceMeters > 0.1f )
		{
			m_quadDistance = distanceMeters;
		}
		if( heightMeters > 0.1f )
		{
			m_quadHeight = heightMeters;
		}
	}

	bool OpenXRRuntime::EnsureBlitPipeline()
	{
		if( m_blitVS && m_blitPS && m_blitPSLinearize && m_blitSampler && m_blitRS )
		{
			return true;
		}
		if( !m_device )
		{
			return false;
		}

		static const char *kVS = R"(
			struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
			VSOut main( uint id : SV_VertexID ) {
				VSOut o;
				o.uv = float2((id << 1) & 2, id & 2);
				o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
				return o;
			}
		)";
		// Passthrough: D3D9 pixels are already display-encoded; write via UNORM RTV.
		static const char *kPS = R"(
			Texture2D tex : register(t0);
			SamplerState samp : register(s0);
			float4 main( float4 pos : SV_POSITION, float2 uv : TEXCOORD0 ) : SV_TARGET {
				return tex.Sample(samp, uv);
			}
		)";
		// Typed sRGB RTV path: linearize so hardware sRGB encode restores game look.
		static const char *kPSLinearize = R"(
			Texture2D tex : register(t0);
			SamplerState samp : register(s0);
			float3 SrgbToLinear( float3 c ) {
				float3 lo = c / 12.92;
				float3 hi = pow(abs((c + 0.055) / 1.055), 2.4);
				return lerp(hi, lo, step(c, 0.04045));
			}
			float4 main( float4 pos : SV_POSITION, float2 uv : TEXCOORD0 ) : SV_TARGET {
				float4 c = tex.Sample(samp, uv);
				return float4(SrgbToLinear(c.rgb), c.a);
			}
		)";

		ID3DBlob *vsBlob = nullptr;
		ID3DBlob *psBlob = nullptr;
		ID3DBlob *psLinBlob = nullptr;
		ID3DBlob *err = nullptr;
		HRESULT hr = D3DCompile( kVS, strlen( kVS ), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &err );
		if( FAILED( hr ) )
		{
			PSU_LOG( "OpenXR: blit VS compile failed 0x%08X\n", hr );
			if( err ) { PSU_LOG( "%s\n", ( char * )err->GetBufferPointer() ); err->Release(); }
			return false;
		}
		hr = D3DCompile( kPS, strlen( kPS ), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &err );
		if( FAILED( hr ) )
		{
			PSU_LOG( "OpenXR: blit PS compile failed 0x%08X\n", hr );
			if( err ) { PSU_LOG( "%s\n", ( char * )err->GetBufferPointer() ); err->Release(); }
			vsBlob->Release();
			return false;
		}
		hr = D3DCompile( kPSLinearize, strlen( kPSLinearize ), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psLinBlob, &err );
		if( FAILED( hr ) )
		{
			PSU_LOG( "OpenXR: blit linearize PS compile failed 0x%08X\n", hr );
			if( err ) { PSU_LOG( "%s\n", ( char * )err->GetBufferPointer() ); err->Release(); }
			vsBlob->Release();
			psBlob->Release();
			return false;
		}

		m_device->CreateVertexShader( vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_blitVS );
		m_device->CreatePixelShader( psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_blitPS );
		m_device->CreatePixelShader( psLinBlob->GetBufferPointer(), psLinBlob->GetBufferSize(), nullptr, &m_blitPSLinearize );
		vsBlob->Release();
		psBlob->Release();
		psLinBlob->Release();

		D3D11_SAMPLER_DESC samp{};
		samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		m_device->CreateSamplerState( &samp, &m_blitSampler );

		D3D11_RASTERIZER_DESC rs{};
		rs.FillMode = D3D11_FILL_SOLID;
		rs.CullMode = D3D11_CULL_NONE;
		m_device->CreateRasterizerState( &rs, &m_blitRS );

		PSU_LOG( "OpenXR: blit pipeline ready (passthrough + linearize)\n" );
		return m_blitVS && m_blitPS && m_blitPSLinearize && m_blitSampler && m_blitRS;
	}

	static DXGI_FORMAT
	RtvFormatForBlit( DXGI_FORMAT fmt )
	{
		switch( fmt )
		{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_UNKNOWN: return DXGI_FORMAT_B8G8R8A8_UNORM;
			default: return fmt;
		}
	}

	static bool IsSrgbFormat( DXGI_FORMAT fmt )
	{
		return fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	}

	// 8-bit BGRA / RGBA formats that CopySubresourceRegion can copy between (same typeless group).
	static DXGI_FORMAT CopyGroup( DXGI_FORMAT fmt )
	{
		switch( fmt )
		{
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	bool OpenXRRuntime::BlitTexture( ID3D11Texture2D *src, ID3D11Texture2D *dst )
	{
		VRPerf::Scope timer( VRPerf::Stage::Blit );
		if( !src || !dst || !m_context )
		{
			return false;
		}

		D3D11_TEXTURE2D_DESC srcDesc{};
		D3D11_TEXTURE2D_DESC dstDesc{};
		src->GetDesc( &srcDesc );
		dst->GetDesc( &dstDesc );

		// Same size and byte layout: the shader blit only passes the stored gamma bytes through
		// (the linearize + sRGB RTV path round-trips to the same bytes), so a plain copy matches it.
		const DXGI_FORMAT group = CopyGroup( srcDesc.Format );
		if( Config::Get().vr.m_perfCopyBlits && group != DXGI_FORMAT_UNKNOWN && group == CopyGroup( dstDesc.Format ) &&
			srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height &&
			srcDesc.SampleDesc.Count == 1 && dstDesc.SampleDesc.Count == 1 )
		{
			m_context->CopySubresourceRegion( dst, 0, 0, 0, 0, src, 0, nullptr );
			if( !m_loggedBlitCopy )
			{
				m_loggedBlitCopy = true;
				PSU_LOG( "OpenXR: blit as plain copy %ux%u src fmt=%u dst fmt=%u\n",
					srcDesc.Width, srcDesc.Height, ( unsigned )srcDesc.Format, ( unsigned )dstDesc.Format );
			}
			return true;
		}
		if( !EnsureBlitPipeline() )
		{
			return false;
		}

		DXGI_FORMAT srvFmt = srcDesc.Format;
		if( srvFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS ) srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
		if( srvFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS ) srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
		// Sample typed sRGB sources as UNORM so we read the stored gamma bytes.
		if( srvFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ) srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
		if( srvFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ) srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

		DXGI_FORMAT rtvFmt = RtvFormatForBlit( dstDesc.Format );
		const bool typedSrgbDst = IsSrgbFormat( dstDesc.Format );
		// Typeless: always UNORM passthrough (avoids double-encode when OpenXR
		// created the swapchain as *_SRGB but exposed typeless images).
		// Typed sRGB: must use matching sRGB RTV + linearize PS.
		ID3D11PixelShader *ps = m_blitPS;
		if( typedSrgbDst )
		{
			rtvFmt = dstDesc.Format;
			ps = m_blitPSLinearize;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = srvFmt;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = rtvFmt;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

		ID3D11ShaderResourceView *srv = nullptr;
		ID3D11RenderTargetView *rtv = nullptr;
		if( FAILED( m_device->CreateShaderResourceView( src, &srvDesc, &srv ) ) || !srv )
		{
			// Typed sRGB source cannot take a UNORM SRV — fall back to native format.
			srvDesc.Format = srcDesc.Format;
			if( FAILED( m_device->CreateShaderResourceView( src, &srvDesc, &srv ) ) || !srv )
			{
				PSU_LOG( "OpenXR: CreateSRV failed srcFmt=%u\n", ( unsigned )srcDesc.Format );
				return false;
			}
		}
		if( FAILED( m_device->CreateRenderTargetView( dst, &rtvDesc, &rtv ) ) || !rtv )
		{
			PSU_LOG( "OpenXR: CreateRTV failed dstFmt=%u as %u\n",
				( unsigned )dstDesc.Format, ( unsigned )rtvFmt );
			srv->Release();
			return false;
		}

		if( !m_loggedBlitRtv )
		{
			m_loggedBlitRtv = true;
			PSU_LOG( "OpenXR: blit RTV fmt=%u dstTex=%u mode=%s (passthrough UNORM for D3D9)\n",
				( unsigned )rtvFmt, ( unsigned )dstDesc.Format,
				typedSrgbDst ? "linearize+sRGB" : "passthrough" );
		}

		if( !Config::Get().vr.m_perfCopyBlits )
		{
			// The full-screen triangle covers the whole target, so the clear only costs time.
			const float clear[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
			m_context->ClearRenderTargetView( rtv, clear );
		}
		m_context->OMSetRenderTargets( 1, &rtv, nullptr );

		D3D11_VIEWPORT vp{};
		vp.Width = static_cast< float >( dstDesc.Width );
		vp.Height = static_cast< float >( dstDesc.Height );
		vp.MaxDepth = 1.0f;
		m_context->RSSetViewports( 1, &vp );
		m_context->RSSetState( m_blitRS );
		m_context->IASetInputLayout( nullptr );
		m_context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		m_context->VSSetShader( m_blitVS, nullptr, 0 );
		m_context->PSSetShader( ps, nullptr, 0 );
		m_context->PSSetShaderResources( 0, 1, &srv );
		m_context->PSSetSamplers( 0, 1, &m_blitSampler );
		m_context->Draw( 3, 0 );

		ID3D11ShaderResourceView *nullSrv = nullptr;
		m_context->PSSetShaderResources( 0, 1, &nullSrv );
		m_context->OMSetRenderTargets( 0, nullptr, nullptr );

		rtv->Release();
		srv->Release();
		return true;
	}

	bool OpenXRRuntime::SubmitSharedTexture( ID3D11Texture2D *src )
	{
		if( !src || !m_context || !m_device || !m_frameStarted || !m_shouldRender )
		{
			return false;
		}

		// Mono virtual screen: one quad layer, not a stereo projection. Blitting
		// the same BB into both eyes with different poses looks like two copies
		// of the player stitched together.
		if( !m_panelSwapchain || !BlitIntoSwapchain( m_panelSwapchain, m_panelImages, src ) )
		{
			return false;
		}
		m_context->Flush();
		m_clearedThisFrame = true;
		m_submitQuadLayer = true;
		m_submitUserHud = false;
		m_submitWorldHud = false;
		return true;
	}

	bool OpenXRRuntime::SubmitHudQuads( ID3D11Texture2D *userTex, float userDistanceM, float userHeightM,
		ID3D11Texture2D *worldTex, float worldDistanceM, float worldHeightM,
		const float worldPosition[ 3 ], const float worldOrientation[ 4 ],
		const float userPosition[ 3 ], const float userOrientation[ 4 ] )
	{
		if( !m_context || !m_device || !m_frameStarted || !m_shouldRender )
		{
			return false;
		}
		bool any = false;
		if( userTex && m_visorSwapchain && userHeightM > 0.1f )
		{
			if( BlitIntoSwapchain( m_visorSwapchain, m_visorImages, userTex ) )
			{
				m_userHudDistance = ( userDistanceM > 0.1f ) ? userDistanceM : m_quadDistance;
				m_userHudHeight = userHeightM;
				m_userHudLocal = userPosition && userOrientation;
				if( m_userHudLocal )
				{
					std::memcpy( m_userHudPosition, userPosition, sizeof( m_userHudPosition ) );
					std::memcpy( m_userHudOrientation, userOrientation, sizeof( m_userHudOrientation ) );
				}
				m_submitUserHud = true;
				any = true;
			}
		}
		if( worldTex && m_panelSwapchain && worldHeightM > 0.1f && worldPosition && worldOrientation )
		{
			if( BlitIntoSwapchain( m_panelSwapchain, m_panelImages, worldTex ) )
			{
				m_worldHudHeight = worldHeightM;
				m_panelWorldLocked = true;
				std::memcpy( m_panelPosition, worldPosition, sizeof( m_panelPosition ) );
				std::memcpy( m_panelOrientation, worldOrientation, sizeof( m_panelOrientation ) );
				(void)worldDistanceM;
				m_submitWorldHud = true;
				any = true;
			}
		}
		if( any )
		{
			m_context->Flush();
			m_clearedThisFrame = true;
		}
		return any;
	}

	bool OpenXRRuntime::BlitIntoSwapchain( uint64_t swapchainHandle, void *imageList, ID3D11Texture2D *src )
	{
		XrSwapchain swapchain = static_cast< XrSwapchain >( swapchainHandle );
		XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t index = 0;
		XrResult waitResult;
		{
			VRPerf::Scope swapTimer( VRPerf::Stage::SwapchainWait );
			if( XR_FAILED( xrAcquireSwapchainImage( swapchain, &ai, &index ) ) )
			{
				return false;
			}
			XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wi.timeout = XR_INFINITE_DURATION;
			waitResult = xrWaitSwapchainImage( swapchain, &wi );
		}
		if( XR_FAILED( waitResult ) )
		{
			XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage( swapchain, &ri );
			return false;
		}

		auto *images = reinterpret_cast< XrSwapchainImageD3D11KHR * >( imageList );
		ID3D11Texture2D *dst = images[ index ].texture;

		D3D11_TEXTURE2D_DESC srcDesc{};
		D3D11_TEXTURE2D_DESC dstDesc{};
		src->GetDesc( &srcDesc );
		dst->GetDesc( &dstDesc );
		if( !m_loggedSubmitFormats )
		{
			m_loggedSubmitFormats = true;
			PSU_LOG( "OpenXR: first submit blit src=%ux%u fmt=%u dst=%ux%u fmt=%u\n",
				srcDesc.Width, srcDesc.Height, ( unsigned )srcDesc.Format,
				dstDesc.Width, dstDesc.Height, ( unsigned )dstDesc.Format );
		}

		if( !BlitTexture( src, dst ) )
		{
			DXGI_FORMAT rtvFmt = RtvFormatForBlit( dstDesc.Format );
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = rtvFmt;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			ID3D11RenderTargetView *rtv = nullptr;
			if( SUCCEEDED( m_device->CreateRenderTargetView( dst, &rtvDesc, &rtv ) ) && rtv )
			{
				const float teal[ 4 ] = { 0.0f, 0.45f, 0.55f, 1.0f };
				m_context->ClearRenderTargetView( rtv, teal );
				rtv->Release();
			}
		}

		XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage( swapchain, &ri );
		return true;
	}

	bool OpenXRRuntime::SubmitStereo( ID3D11Texture2D *const eyes[ 2 ], const EyePose localPoses[ 2 ],
		const EyePose viewSpacePoses[ 2 ] )
	{
		if( !m_context || !m_frameStarted || !m_shouldRender || !eyes[ 0 ] || !eyes[ 1 ] )
		{
			return false;
		}
		for( int eye = 0; eye < 2; ++eye )
		{
			if( !BlitIntoSwapchain( m_swapchain[ eye ], m_swapchainImages[ eye ], eyes[ eye ] ) )
			{
				return false;
			}
			m_submittedPoses[ eye ] = localPoses[ eye ];
			m_submittedViewSpacePoses[ eye ] = viewSpacePoses[ eye ];
		}
		m_context->Flush();
		m_clearedThisFrame = true;
		m_submitStereo = true;
		return true;
	}

	static void CopyXrViewsToEyePoses( const XrView views[ 2 ], EyePose out[ 2 ] )
	{
		for( int eye = 0; eye < 2; ++eye )
		{
			out[ eye ].position[ 0 ] = views[ eye ].pose.position.x;
			out[ eye ].position[ 1 ] = views[ eye ].pose.position.y;
			out[ eye ].position[ 2 ] = views[ eye ].pose.position.z;
			out[ eye ].orientation[ 0 ] = views[ eye ].pose.orientation.x;
			out[ eye ].orientation[ 1 ] = views[ eye ].pose.orientation.y;
			out[ eye ].orientation[ 2 ] = views[ eye ].pose.orientation.z;
			out[ eye ].orientation[ 3 ] = views[ eye ].pose.orientation.w;
			out[ eye ].fovAngleLeft = views[ eye ].fov.angleLeft;
			out[ eye ].fovAngleRight = views[ eye ].fov.angleRight;
			out[ eye ].fovAngleUp = views[ eye ].fov.angleUp;
			out[ eye ].fovAngleDown = views[ eye ].fov.angleDown;
		}
	}

	static bool LocateViewsInSpace( XrSession session, XrSpace space, XrTime time, XrView outViews[ 2 ] )
	{
		XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = time;
		locateInfo.space = space;
		XrViewState viewState{ XR_TYPE_VIEW_STATE };
		uint32_t viewCount = 0;
		outViews[ 0 ] = { XR_TYPE_VIEW };
		outViews[ 1 ] = { XR_TYPE_VIEW };
		if( XR_FAILED( xrLocateViews( session, &locateInfo, &viewState, 2, &viewCount, outViews ) ) || viewCount < 2 )
		{
			return false;
		}
		const XrViewStateFlags needed = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		return ( viewState.viewStateFlags & needed ) == needed;
	}

	// A layer pose given in VIEW space, in LOCAL space at `time` (head pose * pose).
	static bool ViewPoseInLocal( XrSpace viewSpace, XrSpace localSpace, XrTime time, const XrPosef &inView, XrPosef &out )
	{
		XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
		constexpr XrSpaceLocationFlags kPoseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		if( XR_FAILED( xrLocateSpace( viewSpace, localSpace, time, &head ) ) || ( head.locationFlags & kPoseValid ) != kPoseValid )
		{
			return false;
		}
		const XrQuaternionf &a = head.pose.orientation;
		const XrQuaternionf &b = inView.orientation;
		out.orientation = {
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
		// p rotated by a: p + 2w(u x p) + 2u x (u x p).
		const XrVector3f &p = inView.position;
		const float tx = 2.0f * ( a.y * p.z - a.z * p.y );
		const float ty = 2.0f * ( a.z * p.x - a.x * p.z );
		const float tz = 2.0f * ( a.x * p.y - a.y * p.x );
		out.position = {
			head.pose.position.x + p.x + a.w * tx + ( a.y * tz - a.z * ty ),
			head.pose.position.y + p.y + a.w * ty + ( a.z * tx - a.x * tz ),
			head.pose.position.z + p.z + a.w * tz + ( a.x * ty - a.y * tx ) };
		return true;
	}

	bool OpenXRRuntime::GetUiQuad( UiQuad &out )
	{
		out = m_uiQuadValid ? m_uiQuad : UiQuad{};
		return m_uiQuadValid;
	}

	bool OpenXRRuntime::GetHeadHeightAboveFloor( float &meters )
	{
		meters = 0.0f;
		if( !m_frameStarted || !m_stageSpace || !m_viewSpace )
		{
			return false;
		}
		XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
		constexpr XrSpaceLocationFlags kPositionValid = XR_SPACE_LOCATION_POSITION_VALID_BIT;
		if( XR_FAILED( xrLocateSpace( static_cast< XrSpace >( m_viewSpace ), static_cast< XrSpace >( m_stageSpace ),
				m_displayTime, &head ) ) || !( head.locationFlags & kPositionValid ) )
		{
			return false;
		}
		meters = head.pose.position.y;
		return true;
	}

	bool OpenXRRuntime::LocateNextViews( EyePose localOut[ 2 ], EyePose viewSpaceOut[ 2 ] )
	{
		if( !m_frameStarted || !m_space || !m_viewSpace )
		{
			return false;
		}
		const XrSession session = static_cast< XrSession >( m_session );
		const XrTime time = m_displayTime + m_displayPeriod;
		XrView localViews[ 2 ]{};
		XrView viewViews[ 2 ]{};
		if( !LocateViewsInSpace( session, static_cast< XrSpace >( m_space ), time, localViews ) )
		{
			return false;
		}
		if( !LocateViewsInSpace( session, static_cast< XrSpace >( m_viewSpace ), time, viewViews ) )
		{
			return false;
		}
		CopyXrViewsToEyePoses( localViews, localOut );
		CopyXrViewsToEyePoses( viewViews, viewSpaceOut );
		return true;
	}

	void OpenXRRuntime::SetPanelWorldPose( bool enable, const float position[ 3 ], const float orientation[ 4 ] )
	{
		m_panelWorldLocked = enable;
		if( enable )
		{
			std::memcpy( m_panelPosition, position, sizeof( m_panelPosition ) );
			std::memcpy( m_panelOrientation, orientation, sizeof( m_panelOrientation ) );
		}
	}

	bool OpenXRRuntime::EndFrame()
	{
		if( !m_frameStarted )
		{
			return false;
		}

		XrSession session = static_cast< XrSession >( m_session );
		XrSpace localSpace = static_cast< XrSpace >( m_space );
		XrSpace viewSpace = static_cast< XrSpace >( m_viewSpace );

		XrCompositionLayerProjection projLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		XrCompositionLayerProjectionView projViews[ 2 ]{};
		XrCompositionLayerQuad quadLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
		XrCompositionLayerQuad userHudLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
		XrCompositionLayerQuad worldHudLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
		XrCompositionLayerQuad labelLayers[ 2 ]{ { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
		// Projection + user HUD + world HUD / panel + two controller label cards.
		const XrCompositionLayerBaseHeader *layers[ 5 ]{};
		uint32_t layerCount = 0;
		m_uiQuadValid = false;
		// The overlay's layer, for pointing at it with a controller (VRUiPointer).
		const auto noteUiQuad = [ & ]( const XrCompositionLayerQuad &layer )
		{
			XrPosef pose = layer.pose;
			if( layer.space == viewSpace && !ViewPoseInLocal( viewSpace, localSpace, m_displayTime, layer.pose, pose ) )
			{
				return;
			}
			m_uiQuad.position[ 0 ] = pose.position.x;
			m_uiQuad.position[ 1 ] = pose.position.y;
			m_uiQuad.position[ 2 ] = pose.position.z;
			m_uiQuad.orientation[ 0 ] = pose.orientation.x;
			m_uiQuad.orientation[ 1 ] = pose.orientation.y;
			m_uiQuad.orientation[ 2 ] = pose.orientation.z;
			m_uiQuad.orientation[ 3 ] = pose.orientation.w;
			m_uiQuad.width = layer.size.width;
			m_uiQuad.height = layer.size.height;
			m_uiQuad.pixelWidth = m_panelWidth;
			m_uiQuad.pixelHeight = m_panelHeight;
			m_uiQuadValid = true;
		};

		if( m_shouldRender && m_clearedThisFrame )
		{
			const bool stereo = m_submitStereo;
			const bool clearOnly = !m_submitStereo && !m_submitQuadLayer && !m_submitUserHud && !m_submitWorldHud;
			// Menus / 2D scenes: the cleared eyes go under the panel. With only a quad layer
			// SteamVR draws its own environment, play-area boundary included.
			const bool backdrop = !stereo && m_submitBackdrop;
			if( stereo || clearOnly || backdrop )
			{
				XrView located[ 2 ]{ { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
				// stereoLayerSpace "local": the eyes go out with the LOCAL poses they were rendered
				// with, so the runtime's reprojection corrects head turns at the headset's rate.
				const bool stereoLocal = stereo && Config::Get().vr.m_stereoLayerSpace == "local";
				if( stereo && stereoLocal != m_loggedStereoLocal )
				{
					m_loggedStereoLocal = stereoLocal;
					PSU_LOG( "OpenXR: stereo layer in %s space\n", stereoLocal ? "LOCAL" : "VIEW" );
				}
				if( !stereo )
				{
					LocateViewsInSpace( session, localSpace, m_displayTime, located );
				}

				for( int eye = 0; eye < 2; ++eye )
				{
					projViews[ eye ] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
					if( stereoLocal )
					{
						const EyePose &p = m_submittedPoses[ eye ];
						projViews[ eye ].pose.position = { p.position[ 0 ], p.position[ 1 ], p.position[ 2 ] };
						projViews[ eye ].pose.orientation = { p.orientation[ 0 ], p.orientation[ 1 ], p.orientation[ 2 ], p.orientation[ 3 ] };
						projViews[ eye ].fov = { p.fovAngleLeft, p.fovAngleRight, p.fovAngleUp, p.fovAngleDown };
					}
					else if( stereo )
					{
						// Head-locked (VIEW space): game stereo is not a photo of the
						// play space, so LOCAL-space ATW/ASW warps sky and water into ghosts.
						const EyePose &fov = m_submittedPoses[ eye ];
						const EyePose &p = m_submittedViewSpacePoses[ eye ];
						projViews[ eye ].pose.position = { p.position[ 0 ], p.position[ 1 ], p.position[ 2 ] };
						projViews[ eye ].pose.orientation = { p.orientation[ 0 ], p.orientation[ 1 ], p.orientation[ 2 ], p.orientation[ 3 ] };
						projViews[ eye ].fov = { fov.fovAngleLeft, fov.fovAngleRight, fov.fovAngleUp, fov.fovAngleDown };
					}
					else
					{
						projViews[ eye ].pose = located[ eye ].pose;
						projViews[ eye ].fov = located[ eye ].fov;
					}
					projViews[ eye ].subImage.swapchain = static_cast< XrSwapchain >( m_swapchain[ eye ] );
					projViews[ eye ].subImage.imageRect.offset = { 0, 0 };
					projViews[ eye ].subImage.imageRect.extent = { m_swapchainWidth, m_swapchainHeight };
				}

				projLayer.space = ( stereo && !stereoLocal ) ? viewSpace : localSpace;
				projLayer.viewCount = 2;
				projLayer.views = projViews;
				layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader * >( &projLayer );
			}

			const float aspect = ( m_panelHeight > 0 )
				? ( static_cast< float >( m_panelWidth ) / static_cast< float >( m_panelHeight ) )
				: ( 16.0f / 9.0f );

			if( m_submitUserHud && m_visorSwapchain && ( m_userHudLocal ? localSpace != XR_NULL_HANDLE : viewSpace != XR_NULL_HANDLE ) && layerCount < 3 )
			{
				const float heightM = m_userHudHeight;
				userHudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				userHudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				userHudLayer.subImage.swapchain = static_cast< XrSwapchain >( m_visorSwapchain );
				userHudLayer.subImage.imageRect.offset = { 0, 0 };
				userHudLayer.subImage.imageRect.extent = { m_panelWidth, m_panelHeight };
				static bool s_loggedHudSpace = false;
				if( !s_loggedHudSpace )
				{
					s_loggedHudSpace = true;
					PSU_LOG( "OpenXR: user HUD layer space=%s pos=(%.2f %.2f %.2f) size=%.2fx%.2f\n",
						m_userHudLocal ? "LOCAL" : "VIEW",
						m_userHudPosition[ 0 ], m_userHudPosition[ 1 ], m_userHudPosition[ 2 ], heightM * aspect, heightM );
				}
				if( m_userHudLocal )
				{
					// Body-locked: placed in LOCAL space in front of the body.
					userHudLayer.space = localSpace;
					userHudLayer.pose.position = { m_userHudPosition[ 0 ], m_userHudPosition[ 1 ], m_userHudPosition[ 2 ] };
					userHudLayer.pose.orientation = { m_userHudOrientation[ 0 ], m_userHudOrientation[ 1 ],
						m_userHudOrientation[ 2 ], m_userHudOrientation[ 3 ] };
				}
				else
				{
					userHudLayer.space = viewSpace;
					userHudLayer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
					userHudLayer.pose.position = { 0.0f, 0.0f, -m_userHudDistance };
				}
				userHudLayer.size = { heightM * aspect, heightM };
				layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader * >( &userHudLayer );
				noteUiQuad( userHudLayer );
			}

			if( m_submitWorldHud && m_panelSwapchain && m_panelWorldLocked && layerCount < 3 )
			{
				const float heightM = m_worldHudHeight;
				worldHudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				worldHudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				worldHudLayer.subImage.swapchain = static_cast< XrSwapchain >( m_panelSwapchain );
				worldHudLayer.subImage.imageRect.offset = { 0, 0 };
				worldHudLayer.subImage.imageRect.extent = { m_panelWidth, m_panelHeight };
				worldHudLayer.space = localSpace;
				worldHudLayer.pose.position = { m_panelPosition[ 0 ], m_panelPosition[ 1 ], m_panelPosition[ 2 ] };
				worldHudLayer.pose.orientation = { m_panelOrientation[ 0 ], m_panelOrientation[ 1 ], m_panelOrientation[ 2 ], m_panelOrientation[ 3 ] };
				worldHudLayer.size = { heightM * aspect, heightM };
				layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader * >( &worldHudLayer );
			}
			else if( m_submitQuadLayer && m_panelSwapchain && ( viewSpace || m_panelWorldLocked ) && layerCount < 3 )
			{
				const float heightM = m_quadHeight;
				const float widthM = heightM * aspect;

				quadLayer.layerFlags = 0;
				quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quadLayer.subImage.swapchain = static_cast< XrSwapchain >( m_panelSwapchain );
				quadLayer.subImage.imageRect.offset = { 0, 0 };
				quadLayer.subImage.imageRect.extent = { m_panelWidth, m_panelHeight };
				if( m_panelWorldLocked )
				{
					quadLayer.space = localSpace;
					quadLayer.pose.position = { m_panelPosition[ 0 ], m_panelPosition[ 1 ], m_panelPosition[ 2 ] };
					quadLayer.pose.orientation = { m_panelOrientation[ 0 ], m_panelOrientation[ 1 ], m_panelOrientation[ 2 ], m_panelOrientation[ 3 ] };
				}
				else
				{
					// OpenXR VIEW space looks down -Z.
					quadLayer.space = viewSpace;
					quadLayer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
					quadLayer.pose.position = { 0.0f, 0.0f, -m_quadDistance };
				}
				quadLayer.size = { widthM, heightM };
				layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader * >( &quadLayer );
				// In stereo this panel is a stand-in; the overlay is on the user HUD.
				if( !stereo )
				{
					noteUiQuad( quadLayer );
				}
			}

			// Controller label cards, on top of everything (one swapchain, a half each).
			for( int hand = 0; hand < 2; ++hand )
			{
				if( !m_submitLabels[ hand ] || !m_labelSwapchain || localSpace == XR_NULL_HANDLE || layerCount >= 5 )
				{
					continue;
				}
				XrCompositionLayerQuad &label = labelLayers[ hand ];
				label.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				label.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				label.subImage.swapchain = static_cast< XrSwapchain >( m_labelSwapchain );
				label.subImage.imageRect.offset = { hand * ( m_labelWidth / 2 ), 0 };
				label.subImage.imageRect.extent = { m_labelWidth / 2, m_labelHeight };
				label.space = localSpace;
				label.pose.position = { m_labelPosition[ hand ][ 0 ], m_labelPosition[ hand ][ 1 ], m_labelPosition[ hand ][ 2 ] };
				label.pose.orientation = { m_labelOrientation[ hand ][ 0 ], m_labelOrientation[ hand ][ 1 ],
					m_labelOrientation[ hand ][ 2 ], m_labelOrientation[ hand ][ 3 ] };
				const float aspect = m_labelHeight > 0 ? static_cast< float >( m_labelWidth / 2 ) / static_cast< float >( m_labelHeight ) : 1.0f;
				label.size = { m_labelSize, m_labelSize / ( aspect > 0.01f ? aspect : 1.0f ) };
				layers[ layerCount++ ] = reinterpret_cast< const XrCompositionLayerBaseHeader * >( &label );
			}
		}

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = m_displayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.layerCount = layerCount;
		endInfo.layers = layerCount ? layers : nullptr;

		XrResult r;
		{
			VRPerf::Scope timer( VRPerf::Stage::EndFrame );
			r = xrEndFrame( session, &endInfo );
		}
		m_frameStarted = false;
		m_clearedThisFrame = false;
		m_submitBackdrop = false;
		m_submitQuadLayer = false;
		m_submitUserHud = false;
		m_submitWorldHud = false;
		m_submitStereo = false;
		m_submitLabels[ 0 ] = m_submitLabels[ 1 ] = false;
		if( XR_FAILED( r ) )
		{
			PSU_LOG( "OpenXR: xrEndFrame failed %s\n", XrResultString( r ) );
			return false;
		}
		return true;
	}

	bool OpenXRRuntime::GetEyePose( int /*eye*/, EyePose &out )
	{
		std::memset( &out, 0, sizeof( out ) );
		out.orientation[ 3 ] = 1.0f;
		return m_ready;
	}
}
