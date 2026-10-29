#pragma once

#include "VRRuntime.h"

#include <d3d11.h>
#include <string>

namespace PSUVR
{
	// Phase 0 prototype: opens an OpenXR session with D3D11 and clears both eyes
	// to a solid color each frame. No game stereo yet.
	class OpenXRRuntime final : public IVRRuntime
	{
	public:
		bool Init( ID3D11Device *d3d11 ) override;
		void Shutdown() override;
		bool BeginFrame() override;
		bool AcquireSwapchains( ID3D11Texture2D **left, ID3D11Texture2D **right ) override;
		bool EndFrame() override;
		bool GetEyePose( int eye, EyePose &out ) override;
		const char *Name() const override { return "OpenXR"; }
		bool GetControllerState( int hand, ControllerState &out ) override;
		void Haptic( int hand, float amplitude, float seconds ) override;
		bool GetUiQuad( UiQuad &out ) override;
		bool GetHeadHeightAboveFloor( float &meters ) override;

		// Sync controller actions and locate the controllers for the next views' display
		// time (after BeginFrame, with LocateNextViews).
		void UpdateInput();

		// Poll events without xrWaitFrame (safe to call before Present).
		void PumpEvents();
		bool SessionRunning() const { return m_sessionRunning; }

		// Clears acquired swapchain images to RGB and releases them.
		bool ClearEyes( float r, float g, float b );

		// Copies a D3D11 texture into the panel swapchain for a mono quad layer (virtual screen).
		bool SubmitSharedTexture( ID3D11Texture2D *src );

		// Town HUD: head-locked user visor + world-locked kiosk (alpha quads over stereo).
		// userPosition/userOrientation: LOCAL-space pose of the user HUD (body-locked);
		// nullptr keeps it head-locked in VIEW space.
		bool SubmitHudQuads( ID3D11Texture2D *userTex, float userDistanceM, float userHeightM,
			ID3D11Texture2D *worldTex, float worldDistanceM, float worldHeightM,
			const float worldPosition[ 3 ], const float worldOrientation[ 4 ],
			const float userPosition[ 3 ] = nullptr, const float userOrientation[ 4 ] = nullptr );

		// Controller label cards: tex holds the left card in its left half and the right card in
		// its right half; each visible card is a square quad (widthM) at its LOCAL pose, on top.
		bool SubmitHandLabels( ID3D11Texture2D *tex, const float position[ 2 ][ 3 ], const float orientation[ 2 ][ 4 ],
			float widthM, const bool visible[ 2 ] );

		// Recreate the panel swapchain to match the game back-buffer.
		bool ResizeSwapchains( int32_t width, int32_t height );

		void SetVirtualScreenPlacement( float distanceMeters, float heightMeters );

		// Panel in LOCAL space (stereo mode) instead of head-locked VIEW space.
		void SetPanelWorldPose( bool enable, const float position[ 3 ], const float orientation[ 4 ] );

		// Eye poses predicted for the frame after the current one (after BeginFrame).
		// localOut: LOCAL space — used to build D3D VIEW (head on top of the game camera).
		// viewSpaceOut: VIEW space — used when submitting the projection layer so the
		// compositor does not reproject game pixels as if they were real-world depth
		// (that mismatch shows up as sky/water ghosting under SteamVR motion smoothing).
		bool LocateNextViews( EyePose localOut[ 2 ], EyePose viewSpaceOut[ 2 ] );

		// Blits per-eye renders into the eye swapchains. fov comes from localPoses;
		// the projection layer is submitted head-locked using viewSpacePoses.
		bool SubmitStereo( ID3D11Texture2D *const eyes[ 2 ], const EyePose localPoses[ 2 ],
			const EyePose viewSpacePoses[ 2 ] );

		int32_t SwapchainWidth() const { return m_panelWidth; }
		int32_t SwapchainHeight() const { return m_panelHeight; }
		int32_t EyeSwapchainWidth() const { return m_swapchainWidth; }
		int32_t EyeSwapchainHeight() const { return m_swapchainHeight; }

	private:
		bool LoadLoader();
		bool CreateInstance();
		bool CreateSession( ID3D11Device *d3d11 );
		bool CreateSwapchains( int32_t width, int32_t height );
		void DestroySwapchains();
		bool CreateActions();
		void DestroyActions();
		void LogInteractionProfiles();
		bool CreatePanelSwapchain( int32_t width, int32_t height );
		void DestroyPanelSwapchain();
		void DestroyVisorSwapchain();
		void DestroyLabelSwapchain();
		bool CreateSwapchain( int32_t width, int32_t height, uint64_t &handle, void *&images, uint32_t &imageCount );
		bool BlitIntoSwapchain( uint64_t swapchain, void *images, ID3D11Texture2D *src );
		int64_t PickSwapchainFormat( uint64_t sessionHandle );
		bool EnsureBlitPipeline();
		bool BlitTexture( ID3D11Texture2D *src, ID3D11Texture2D *dst );

		HMODULE m_loader = nullptr;
		bool m_ready = false;

		ID3D11VertexShader *m_blitVS = nullptr;
		ID3D11PixelShader *m_blitPS = nullptr;           // passthrough (UNORM RTV)
		ID3D11PixelShader *m_blitPSLinearize = nullptr;  // sRGB→linear before sRGB RTV write
		ID3D11SamplerState *m_blitSampler = nullptr;
		ID3D11RasterizerState *m_blitRS = nullptr;
		bool m_loggedBlitRtv = false;
		bool m_loggedBlitCopy = false;

		// Opaque OpenXR handles stored as uint64 to avoid requiring headers at
		// every include site; implementation uses real Xr* types.
		uint64_t m_instance = 0;
		uint64_t m_systemId = 0;
		uint64_t m_session = 0;
		uint64_t m_space = 0;     // LOCAL — stereo projection
		uint64_t m_viewSpace = 0; // VIEW — head-locked virtual screen quad
		uint64_t m_stageSpace = 0; // STAGE — floor level, when the runtime has one (eye height)
		uint64_t m_swapchain[ 2 ]{};
		int32_t m_swapchainWidth = 0;
		int32_t m_swapchainHeight = 0;
		int64_t m_swapchainFormat = 0;
		uint32_t m_swapchainImageCount[ 2 ]{};
		void *m_swapchainImages[ 2 ]{}; // XrSwapchainImageD3D11KHR*
		ID3D11Device *m_device = nullptr;
		ID3D11DeviceContext *m_context = nullptr;
		std::string m_lastError;
		bool m_frameStarted = false;
		bool m_sessionRunning = false;
		bool m_shouldRender = false;
		bool m_clearedThisFrame = false;
		bool m_submitQuadLayer = false;
		bool m_submitUserHud = false;
		bool m_submitWorldHud = false;
		bool m_submitStereo = false;
		// The eyes were cleared this frame (ClearEyes): submit them as a backdrop projection
		// layer under the panels, so the runtime never shows its own environment / boundary.
		bool m_submitBackdrop = false;
		int m_sessionState = 1; // XR_SESSION_STATE_IDLE
		int64_t m_displayTime = 0;
		int64_t m_displayPeriod = 0;
		uint32_t m_acquiredIndex[ 2 ]{};
		bool m_loggedSubmitFormats = false;
		float m_quadDistance = 2.0f;
		float m_quadHeight = 1.2f;
		float m_userHudDistance = 2.5f;
		float m_userHudHeight = 1.4f;
		bool m_userHudLocal = false;
		float m_userHudPosition[ 3 ]{};
		float m_userHudOrientation[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };
		float m_worldHudHeight = 1.35f;

		uint64_t m_panelSwapchain = 0;
		void *m_panelImages = nullptr; // XrSwapchainImageD3D11KHR*
		uint32_t m_panelImageCount = 0;
		int32_t m_panelWidth = 0;
		int32_t m_panelHeight = 0;
		bool m_panelWorldLocked = false;
		float m_panelPosition[ 3 ]{};
		float m_panelOrientation[ 4 ]{ 0.0f, 0.0f, 0.0f, 1.0f };

		// Second panel for the head-locked personal HUD (same size as game BB / world HUD).
		uint64_t m_visorSwapchain = 0;
		void *m_visorImages = nullptr;
		uint32_t m_visorImageCount = 0;

		// Controller label cards (created on first use, at the texture's size).
		uint64_t m_labelSwapchain = 0;
		void *m_labelImages = nullptr;
		uint32_t m_labelImageCount = 0;
		int32_t m_labelWidth = 0;
		int32_t m_labelHeight = 0;
		bool m_submitLabels[ 2 ]{};
		float m_labelPosition[ 2 ][ 3 ]{};
		float m_labelOrientation[ 2 ][ 4 ]{};
		float m_labelSize = 0.14f;

		// Motion controllers (OpenXR actions).
		uint64_t m_actionSet = 0;
		uint64_t m_actGripPose = 0;
		uint64_t m_actAimPose = 0;
		uint64_t m_actTrigger = 0;
		uint64_t m_actGrip = 0;
		uint64_t m_actStick = 0;
		uint64_t m_actStickClick = 0;
		uint64_t m_actPrimary = 0;
		uint64_t m_actSecondary = 0;
		uint64_t m_actMenu = 0;
		uint64_t m_actHaptic = 0;
		uint64_t m_handPath[ 2 ]{};
		uint64_t m_gripSpace[ 2 ]{};
		uint64_t m_aimSpace[ 2 ]{};
		ControllerState m_controllers[ 2 ]{};

		// The layer that carried the overlay in the last EndFrame (user HUD in stereo, the
		// back-buffer panel otherwise), in LOCAL space.
		UiQuad m_uiQuad{};
		bool m_uiQuadValid = false;

		EyePose m_submittedPoses[ 2 ]{};      // FOV (+ debug); layer uses view-space poses
		EyePose m_submittedViewSpacePoses[ 2 ]{};
	};
}
