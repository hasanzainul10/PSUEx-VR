#include "VRDisplay.h"
#include "VRSetupDisplay.h"

#include "../../PSUExConfig.h"
#include "../../PSUVR/VRBindings.h"
#include "../../PSUVR/VRCamera.h"
#include "../../PSUVR/VRHaptics.h"
#include "../../PSUVR/VRHud.h"
#include "../../PSUVR/VRSystem.h"
#include "../../PSUVR/VRUiPointer.h"

#include <cfloat>
#include <cstdio>

VRDisplay::VRDisplay() : IFWinCtrl( "VRDisplay", false )
{
	m_context.inGame = true;
	m_context.save = []()
	{
		Config::Get().SaveToFile();
		return true;
	};
	m_context.reload = []()
	{
		return Config::Get().ReloadVRFromFile();
	};
	m_context.runSetup = [this]()
	{
		m_bEnabled = false;
		VRSetupDisplay::RequestOpen();
	};
	m_context.headHeight = []( float &meters )
	{
		return PSUVR::System::Get().HeadHeightAboveFloor( meters );
	};
	m_context.testVibration = []( int test )
	{
		PSUVR::VRHaptics::PlayBoth( test == 0 ? PSUVR::VRHaptics::Effect::TestStrength : PSUVR::VRHaptics::Effect::TestLength );
	};
	m_context.openToolkit = [this]()
	{
		m_bEnabled = false;
		PSUVR::VRBindings::OpenToolkitMenu();
	};
}

void VRDisplay::DrawToast()
{
	float alpha = 0.0f;
	const char *text = PSUVR::VRBindings::CurrentToast( alpha );
	if( !text )
	{
		return;
	}
	// Large and top-centre so it reads on the HUD panel in the headset.
	ImFont *font = ImGui::GetFont();
	const float size = ImGui::GetFontSize() * 2.5f;
	const ImVec2 textSize = font->CalcTextSizeA( size, FLT_MAX, 0.0f, text );
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const ImVec2 pos( ( display.x - textSize.x ) * 0.5f, display.y * 0.12f );
	const float pad = size * 0.4f;
	ImDrawList *draw = ImGui::GetForegroundDrawList();
	draw->AddRectFilled( ImVec2( pos.x - pad, pos.y - pad * 0.5f ),
		ImVec2( pos.x + textSize.x + pad, pos.y + textSize.y + pad * 0.5f ),
		ImGui::GetColorU32( ImVec4( 0.0f, 0.0f, 0.0f, 0.6f * alpha ) ), pad * 0.5f );
	draw->AddText( font, size, pos, ImGui::GetColorU32( ImVec4( 1.0f, 1.0f, 1.0f, alpha ) ), text );
}

void VRDisplay::Render()
{
	if( PSUVR::VRBindings::ConsumeSettingsRequest() )
	{
		Toggle();
	}
	// Circle on the right controller closes the panel while it points at it.
	if( PSUVR::VRUiPointer::ConsumeCloseRequest() && m_bEnabled )
	{
		m_bEnabled = false;
	}
	PSUVR::VRUiPointer::SetWindowOpen( m_bEnabled );

	// In the headset the OS cursor isn't visible; let ImGui draw one on the panel.
	const bool inHeadset = PSUVR::VRHud::Get().Active();
	ImGui::GetIO().MouseDrawCursor = ( inHeadset || PSUVR::VRUiPointer::Wanted() ) && m_bEnabled;

	DrawToast();
	if( !m_bEnabled )
	{
		return;
	}

	const auto &vr = Config::Get().vr;
	m_context.status.clear();
	char line[ 160 ];
	std::snprintf( line, sizeof( line ), "VR %s, runtime %s%s", vr.m_enabled ? "on" : "off (enable + restart)",
		vr.m_runtime.c_str(), vr.m_paused ? ", PAUSED" : "" );
	m_context.status.emplace_back( line );
	std::snprintf( line, sizeof( line ), "Stereo %s, first person %s, turning via %s",
		inHeadset ? "active" : "inactive",
		PSUVR::VRCamera::FirstPersonActive() ? "active" : "inactive",
		PSUVR::VRCamera::TurnFromStick() ? "XInput" : "game camera" );
	m_context.status.emplace_back( line );

	// Opens in the middle of the view, sized to the text scale (bigger text needs a bigger window).
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const float textScale = vr.m_panelTextScale < 1.0f ? 1.0f : ( vr.m_panelTextScale > 2.5f ? 2.5f : vr.m_panelTextScale );
	ImVec2 size( 560.0f * textScale, 680.0f * textScale );
	size.x = size.x > display.x * 0.9f ? display.x * 0.9f : size.x;
	size.y = size.y > display.y * 0.9f ? display.y * 0.9f : size.y;
	ImGui::SetNextWindowPos( ImVec2( display.x * 0.5f, display.y * 0.5f ), ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
	ImGui::SetNextWindowSize( size, ImGuiCond_Appearing );
	if( ImGui::Begin( "VR Settings", &m_bEnabled ) )
	{
		ImGui::SetWindowFontScale( textScale );
		PSUVR::DrawVRSettingsPanel( Config::Get().vr, m_context );
	}
	ImGui::End();
}
