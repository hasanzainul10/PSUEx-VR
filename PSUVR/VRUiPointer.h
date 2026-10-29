#pragma once

#include "VRRuntime.h"

namespace PSUVR
{
	// The right controller as a mouse for the PSUEx overlay (VR settings, ...) in the headset:
	// its aim ray is intersected with the quad layer that shows the overlay, the trigger clicks
	// and the right stick scrolls. While an overlay window wants it, the game gets no
	// controller input (VRSyntheticPad), apart from the both-grip chords.
	namespace VRUiPointer
	{
		// The overlay, once per ImGui frame: whether a window that takes the pointer is open.
		void SetWindowOpen( bool open );
		// The same for the setup wizard (either one open counts).
		void SetWizardOpen( bool open );
		// The Rogue's Toolkit menu is (assumed) open: the pointer also moves and clicks the system mouse.
		void SetToolkitMenuOpen( bool open );
		// True while the pointer drives the overlay (window open, motion input, vr.uiPointer,
		// right controller active), and until every button is let go after the window closes.
		bool Wanted();

		// Once per frame after VRControllers::Update: the ray, the hit and the button edges.
		void Update( IVRRuntime *runtime );
		// Before ImGui::NewFrame (after the platform backend's NewFrame, so this wins): sends
		// the pointer position, the click and the scroll to ImGui.
		void FeedImGui();
		// Circle (right secondary) was pressed while the pointer was wanted: close the window.
		bool ConsumeCloseRequest();

		// The ray to draw (LOCAL, meters): from the aim pose to the hit, or a short stub.
		bool Beam( float from[ 3 ], float to[ 3 ], bool &hit );
	}
}
