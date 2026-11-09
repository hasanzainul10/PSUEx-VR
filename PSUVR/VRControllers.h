#pragma once

#include "VRRuntime.h"

#include <cstdint>

struct IDirect3DDevice9;

namespace PSUVR
{
	// Motion controllers, as seen by the rest of PSUVR (runtime-independent).
	// Poses are in the eye poses' space (OpenXR LOCAL, meters) for the next views.
	namespace VRControllers
	{
		// A bullet started here (game world). In free aim the game fires from a fixed point by the
		// character, not from the gun: the reticle is put on that line.
		void NoteShotStart( const float start[ 3 ] );
		// Where free aim shots start (game world): the player plus the offset seen on the last
		// free aim shot (about 1.07 m straight up until then).
		bool ShotStartWorld( float world[ 3 ] );
		// Once per frame after the runtime's input update: latch state and button edges.
		void Update( IVRRuntime *runtime );

		// hand: 0 = left, 1 = right.
		const ControllerState &State( int hand );
		bool Active( int hand );
		bool AnyActive();
		// ControllerButton bits that went down / up this frame.
		uint32_t Pressed( int hand );
		uint32_t Released( int hand );

		// Raw vibration; gameplay and UI use VRHaptics (named effects, strength setting).
		void Haptic( int hand, float amplitude, float seconds, float frequency = 0.0f );

		// Hand markers and pointer rays over the bound eye RT (after the eye pass).
		// eye: the pose and FOV the eye was rendered with.
		void DrawMarkers( IDirect3DDevice9 *device, const EyePose &eye, unsigned width, unsigned height );
	}
}
