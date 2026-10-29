#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace PSUVR
{
	// Frame timing (vr.perfOverlay): CPU time per stage of the frame, GPU time per render pass
	// (D3D9 timestamp queries, read back a few frames late so they never stall), shown in a small
	// overlay and logged as "Perf 5 s:" every 5 seconds. Off: every call returns at once.
	namespace VRPerf
	{
		// CPU stages. Present / Capture / Submit are whole hooks; WaitFrame and GpuWait run
		// inside Submit / Capture and are shown on their own too.
		enum class Stage : int
		{
			FlatPass,  // the game's 3D pass for the monitor
			EyeLeft,
			EyeRight,
			PassB,     // 2D / HUD layers (includes the HUD replay)
			Overlay,   // ImGui, HUD panel copy, label cards (EndScene)
			Capture,   // before Present: back-buffer copy for the headset
			Present,   // the game's own Present
			Submit,    // after Present: OpenXR frame (includes WaitFrame and GpuWait)
			WaitFrame, // xrWaitFrame
			GpuWait,   // waiting for the GPU before the headset reads the frame
			// Inside Submit:
			Updates,       // input, controllers, synthetic pad, labels, speech
			// Inside Updates:
			Input,          // UpdateInput (xrSyncActions, controller poses)
			Controllers,    // VRControllers::Update
			UiPointer,      // VRUiPointer::Update
			Pad,            // VRSyntheticPad::Update
			PadCombo,       // its ComboFeedback (PSUCombat::Read)
			PadCombat,      // its Combat (roles, swings, guns)
			PadAim,         // its FreeAim + AimModeFollow
			PadRoomWalk,    // its VRCamera::RoomWalkStick
			LabelsUpdate,   // VRControllerLabels::Update
			Speech,         // VRSpeech::Update
			StereoSubmit,  // SubmitStereo (eye swapchains, blits, layer)
			SwapchainWait, // xrAcquireSwapchainImage + xrWaitSwapchainImage (all swapchains)
			Blit,          // D3D11 copies into swapchain images (all swapchains)
			HudQuads,      // SubmitHudQuads
			Labels,        // VRControllerLabels::Submit
			EndFrame,      // xrEndFrame
			Count
		};

		// GPU passes.
		enum class Gpu : int
		{
			FlatPass,
			EyeLeft,
			EyeRight,
			PassB,
			Count
		};

		// True while vr.enabled and vr.perfOverlay (read once per frame in EndFrame).
		bool Enabled();

		// RAII CPU timer for one stage; adds to the stage's time this frame.
		class Scope
		{
		public:
			explicit Scope( Stage stage );
			~Scope();
			Scope( const Scope & ) = delete;
			Scope &operator=( const Scope & ) = delete;

		private:
			Stage m_stage;
			int64_t m_start;
		};

		// For functions that can't hold a Scope (__try): start = Begin(), then End( stage, start ).
		int64_t Begin();
		void End( Stage stage, int64_t start );

		// GPU timestamps around a pass (same device each frame).
		void GpuBegin( IDirect3DDevice9 *device, Gpu pass );
		void GpuEnd( IDirect3DDevice9 *device, Gpu pass );

		// In the Present hook, before the game's Present: closes this frame's GPU queries.
		void BeforePresent( IDirect3DDevice9 *device );
		// At the end of the Present hook: frame time, reads finished GPU queries, logs.
		void EndFrame();

		// The headset's frame period from xrWaitFrame (nanoseconds).
		void SetDisplayPeriod( int64_t nanoseconds );
		// This frame went to the headset in stereo.
		void NoteStereoFrame();

		// Draws the overlay window (inside the ImGui frame).
		void DrawOverlay();
	}
}
