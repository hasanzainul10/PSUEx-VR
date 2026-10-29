#include "VRSettings.h"

#include "../Json.hpp"

#include <Windows.h>
#include <Xinput.h>

using json = nlohmann::json;

namespace
{
	// One list of (member, json key) so load and save can't drift apart.
#define PSUEX_VR_FIELDS( X ) \
	X( m_enabled, "enabled" ) \
	X( m_forceD3D9Ex, "forceD3D9Ex" ) \
	X( m_d3d9exOnly, "d3d9exOnly" ) \
	X( m_solidColorPrototype, "solidColorPrototype" ) \
	X( m_runtime, "runtime" ) \
	X( m_cameraMode, "cameraMode" ) \
	X( m_inputMode, "inputMode" ) \
	X( m_backgroundInput, "backgroundInput" ) \
	X( m_backgroundKeyboard, "backgroundKeyboard" ) \
	X( m_worldScale, "worldScale" ) \
	X( m_ipdScale, "ipdScale" ) \
	X( m_hudDistance, "hudDistance" ) \
	X( m_hudSize, "hudSize" ) \
	X( m_uiScale, "uiScale" ) \
	X( m_hudWorldPanel, "hudWorldPanel" ) \
	X( m_hudMapBlackAlpha, "hudMapBlackAlpha" ) \
	X( m_hudMapMeshes, "hudMapMeshes" ) \
	X( m_hudMapFit, "hudMapFit" ) \
	X( m_hudMapFitScale, "hudMapFitScale" ) \
	X( m_hudFollow, "hudFollow" ) \
	X( m_menuBackdrop, "menuBackdrop" ) \
	X( m_hudOverlays, "hudOverlays" ) \
	X( m_toolkitOnHud, "toolkitOnHud" ) \
	X( m_toolkitPointer, "toolkitPointer" ) \
	X( m_hudFollowAngle, "hudFollowAngle" ) \
	X( m_hudDrop, "hudDrop" ) \
	X( m_fpEyeHeight, "fpEyeHeight" ) \
	X( m_fpEyeForward, "fpEyeForward" ) \
	X( m_fpEyeFollowsNeck, "fpEyeFollowsNeck" ) \
	X( m_bodyRestPose, "bodyRestPose" ) \
	X( m_bodyFitHeight, "bodyFitHeight" ) \
	X( m_bodyKeepLegAnim, "bodyKeepLegAnim" ) \
	X( m_bodyIk, "bodyIk" ) \
	X( m_bodyProceduralLegs, "bodyProceduralLegs" ) \
	X( m_snapTurnDegrees, "snapTurnDegrees" ) \
	X( m_fpTurnSpeed, "fpTurnSpeed" ) \
	X( m_fpMoveDirection, "fpMoveDirection" ) \
	X( m_fpBody, "fpBody" ) \
	X( m_fpNeckDrop, "fpNeckDrop" ) \
	X( m_fpLeanLimit, "fpLeanLimit" ) \
	X( m_fpCrouchLimit, "fpCrouchLimit" ) \
	X( m_tpDistance, "tpDistance" ) \
	X( m_tpHeight, "tpHeight" ) \
	X( m_tpZoomSpeed, "tpZoomSpeed" ) \
	X( m_tpMinDistance, "tpMinDistance" ) \
	X( m_tpMaxDistance, "tpMaxDistance" ) \
	X( m_tpOrbitDegrees, "tpOrbitDegrees" ) \
	X( m_tpSnapTurn, "tpSnapTurn" ) \
	X( m_tpTurnSpeed, "tpTurnSpeed" ) \
	X( m_tpPitch, "tpPitch" ) \
	X( m_tpTiltSpeed, "tpTiltSpeed" ) \
	X( m_tpLbCamera, "tpLbCamera" ) \
	X( m_fpLbCamera, "fpLbCamera" ) \
	X( m_lbWindowSeconds, "lbWindowSeconds" ) \
	X( m_snapTurn, "snapTurn" ) \
	X( m_comfortVignette, "comfortVignette" ) \
	X( m_showControllers, "showControllers" ) \
	X( m_uiPointer, "uiPointer" ) \
	X( m_controllerLabels, "controllerLabels" ) \
	X( m_controllerLabelsGrip, "controllerLabelsGrip" ) \
	X( m_controllerLabelsHoldSeconds, "controllerLabelsHoldSeconds" ) \
	X( m_controllerLabelsSize, "controllerLabelsSize" ) 	X( m_setupDone, "setupDone" ) \
	X( m_roomWalk, "roomWalk" ) \
	X( m_roomWalkStart, "roomWalkStart" ) \
	X( m_emulateControllers, "emulateControllers" ) \
	X( m_simScript, "simScript" ) \
	X( m_handsFollow, "handsFollow" ) \
	X( m_armStretch, "armStretch" ) \
	X( m_handsAttack, "handsAttack" ) \
	X( m_handsPhotonArtAnimation, "handsPhotonArtAnimation" ) \
	X( m_handCalibrated, "handCalibrated" ) \
	X( m_handRotL0, "handRotL0" ) \
	X( m_handRotL1, "handRotL1" ) \
	X( m_handRotL2, "handRotL2" ) \
	X( m_handRotL3, "handRotL3" ) \
	X( m_handRotR0, "handRotR0" ) \
	X( m_handRotR1, "handRotR1" ) \
	X( m_handRotR2, "handRotR2" ) \
	X( m_handRotR3, "handRotR3" ) \
	X( m_handPitch, "handPitch" ) \
	X( m_handRoll, "handRoll" ) \
	X( m_motionProfile, "motionProfile" ) \
	X( m_comboHaptic, "comboHaptic" ) \
	X( m_autoDpad, "autoDpad" ) \
	X( m_swingAttack, "swingAttack" ) \
	X( m_paGesture, "paGesture" ) \
	X( m_gripPhotonArt, "gripPhotonArt" ) \
	X( m_triangleChatL2, "triangleChatL2" ) \
	X( m_strafeBody, "strafeBody" ) \
	X( m_strafeBodyBlend, "strafeBodyBlend" ) \
	X( m_attackFaceLook, "attackFaceLook" ) \
	X( m_attackFaceTolerance, "attackFaceTolerance" ) \
	X( m_controllerAim, "controllerAim" ) \
	X( m_aimInvertPitch, "aimInvertPitch" ) \
	X( m_aimDirect, "aimDirect" ) \
	X( m_reticleOnShot, "reticleOnShot" ) \
	X( m_gunPointsAtAim, "gunPointsAtAim" ) \
	X( m_aimLeash, "aimLeash" ) \
	X( m_aimLeashMeters, "aimLeashMeters" ) \
	X( m_gunAdjustHandgunPitch, "gunAdjustHandgunPitch" ) \
	X( m_gunAdjustHandgunYaw, "gunAdjustHandgunYaw" ) \
	X( m_gunAdjustBowPitch, "gunAdjustBowPitch" ) \
	X( m_gunAdjustBowYaw, "gunAdjustBowYaw" ) \
	X( m_gunAdjustRiflePitch, "gunAdjustRiflePitch" ) \
	X( m_gunAdjustRifleYaw, "gunAdjustRifleYaw" ) \
	X( m_techWeaponStill, "techWeaponStill" ) \
	X( m_freeAimNoNearHide, "freeAimNoNearHide" ) \
	X( m_freeAimNoPartCull, "freeAimNoPartCull" ) \
	X( m_reticleSize, "reticleSize" ) \
	X( m_swingSpeed, "swingSpeed" ) \
	X( m_comboBuffer, "comboBuffer" ) \
	X( m_comboBufferJustAttack, "comboBufferJustAttack" ) \
	X( m_speechToText, "speechToText" ) \
	X( m_speechModel, "speechModel" ) \
	X( m_speechChat, "speechChat" ) \
	X( m_voiceCommands, "voiceCommands" ) \
	X( m_voiceItems, "voiceItems" ) \
	X( m_voiceItemScript, "voiceItemScript" ) \
	X( m_voiceItemButtons, "voiceItemButtons" ) \
	X( m_speechAlwaysOn, "speechAlwaysOn" ) \
	X( m_voiceRoute, "voiceRoute" ) \
	X( m_vignetteStrength, "vignetteStrength" ) \
	X( m_stereo, "stereo" ) \
	X( m_stereoMode, "stereoMode" ) \
	X( m_stereoSkipScreenQuads, "stereoSkipScreenQuads" ) \
	X( m_reprojectSprites, "reprojectSprites" ) \
	X( m_recenterSky, "recenterSky" ) \
	X( m_skyAtEye, "skyAtEye" ) \
	X( m_eyeBlur, "eyeBlur" ) \
	X( m_eyeGlare, "eyeGlare" ) \
	X( m_eyeSky, "eyeSky" ) \
	X( m_stereoPanel, "stereoPanel" ) \
	X( m_debugEyeShift, "debugEyeShift" ) \
	X( m_debugDoublePass, "debugDoublePass" ) \
	X( m_perfOverlay, "perfOverlay" ) \
	X( m_fastPointerChecks, "fastPointerChecks" ) \
	X( m_perfSkipFlatCopy, "perfSkipFlatCopy" ) \
	X( m_perfCopyBlits, "perfCopyBlits" ) \
	X( m_perfHudLockRanges, "perfHudLockRanges" ) \
	X( m_perfLogBatching, "perfLogBatching" ) \
	X( m_flatMirrorEye, "flatMirrorEye" )

	void Read( const json &node, const char *key, bool &value )
	{
		if( node.contains( key ) && node[ key ].is_boolean() )
			value = node[ key ].get< bool >();
	}

	void Read( const json &node, const char *key, float &value )
	{
		if( node.contains( key ) && node[ key ].is_number() )
			value = node[ key ].get< float >();
	}

	void Read( const json &node, const char *key, unsigned &value )
	{
		if( node.contains( key ) && node[ key ].is_number() )
			value = node[ key ].get< unsigned >();
	}

	void Read( const json &node, const char *key, std::string &value )
	{
		if( node.contains( key ) && node[ key ].is_string() )
			value = node[ key ].get< std::string >();
	}

	// Stable JSON keys for the bindings (labels are for display only).
	const char *const kActionKeys[ kVRActionCount ] = {
		"recenter", "toggleCameraMode", "toggleSettings", "togglePause", "toggleSnapTurn", "toggleHudFollow", "toggleToolkitMenu" };
}

const char *VRActionLabel( VRAction action )
{
	switch( action )
	{
		case VRAction::Recenter: return "Recenter";
		case VRAction::ToggleCameraMode: return "Switch first / third person";
		case VRAction::ToggleSettings: return "Show / hide VR settings";
		case VRAction::TogglePause: return "Pause VR";
		case VRAction::ToggleSnapTurn: return "Snap / smooth turn";
		case VRAction::ToggleHudFollow: return "Body-locked HUD on / off";
		case VRAction::ToggleToolkitMenu: return "Open Rogue's Toolkit menu";
		default: return "?";
	}
}

std::array< VRBinding, kVRActionCount > DefaultVRBindings()
{
	std::array< VRBinding, kVRActionCount > bindings{};
	// Ctrl+F-keys: the game itself uses F1-F7 (main menu shortcuts) and others.
	bindings[ static_cast< int >( VRAction::Recenter ) ] = { VK_F9, XINPUT_GAMEPAD_X, kVRModCtrl };
	bindings[ static_cast< int >( VRAction::ToggleCameraMode ) ] = { VK_F7, XINPUT_GAMEPAD_Y, kVRModCtrl };
	bindings[ static_cast< int >( VRAction::ToggleSettings ) ] = { VK_F10, XINPUT_GAMEPAD_START, kVRModCtrl };
	bindings[ static_cast< int >( VRAction::TogglePause ) ] = { VK_F8, 0, kVRModCtrl };
	// The toolkit's own key is plain F6; Ctrl+F6 here sends it. Back+A on the pad.
	bindings[ static_cast< int >( VRAction::ToggleToolkitMenu ) ] = { VK_F6, XINPUT_GAMEPAD_A, kVRModCtrl };
	return bindings;
}

bool LoadVRSettingsJson( VRSettings &settings, const std::string &jsonText )
{
	const json node = json::parse( jsonText, nullptr, false );
	if( node.is_discarded() || !node.is_object() )
	{
		return false;
	}
#define PSUEX_VR_READ( member, key ) Read( node, key, settings.member );
	PSUEX_VR_FIELDS( PSUEX_VR_READ )
#undef PSUEX_VR_READ

	if( node.contains( "bindings" ) && node[ "bindings" ].is_object() )
	{
		const json &bindings = node[ "bindings" ];
		for( int i = 0; i < kVRActionCount; ++i )
		{
			if( !bindings.contains( kActionKeys[ i ] ) || !bindings[ kActionKeys[ i ] ].is_object() )
			{
				continue;
			}
			const json &entry = bindings[ kActionKeys[ i ] ];
			if( entry.contains( "key" ) && entry[ "key" ].is_number_integer() )
				settings.m_bindings[ i ].key = entry[ "key" ].get< int >();
			// Older files have no "mods": plain key.
			settings.m_bindings[ i ].mods = 0;
			if( entry.contains( "mods" ) && entry[ "mods" ].is_number_integer() )
				settings.m_bindings[ i ].mods = static_cast< uint8_t >( entry[ "mods" ].get< int >() );
			if( entry.contains( "pad" ) && entry[ "pad" ].is_number_integer() )
				settings.m_bindings[ i ].pad = static_cast< uint16_t >( entry[ "pad" ].get< int >() );
		}
	}
	return true;
}

std::string SaveVRSettingsJson( const VRSettings &settings )
{
	json node = json::object();
#define PSUEX_VR_WRITE( member, key ) node[ key ] = settings.member;
	PSUEX_VR_FIELDS( PSUEX_VR_WRITE )
#undef PSUEX_VR_WRITE

	json bindings = json::object();
	for( int i = 0; i < kVRActionCount; ++i )
	{
		bindings[ kActionKeys[ i ] ] = { { "key", settings.m_bindings[ i ].key }, { "mods", settings.m_bindings[ i ].mods },
			{ "pad", settings.m_bindings[ i ].pad } };
	}
	node[ "bindings" ] = bindings;
	return node.dump( 4 );
}
