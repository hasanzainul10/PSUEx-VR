#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace PSUVR
{
	// Knows when the game is drawing the local player's character, so first person can
	// cut its head away (or hide it) in the eye passes. Each object in the draw layers is
	// drawn by one callback (node +0x10) called from PSU_DrawLayer 0x00560830; the local
	// player's node gets a thunk that brackets the original callback.
	namespace VRPlayerDraw
	{
		// Once per stereo frame before the eye passes. firstPerson = first person is active;
		// thirdPersonBody = third-person body IK wants the model; neither restores every patched callback.
		void Update( bool firstPerson, bool thirdPersonBody );

		// Inside the local player's draw callback.
		bool DrawingLocalPlayer();
		// The local player's TDivModel, seen at its last draw; null when not tracked
		// (neither mode, first-person vr.fpBody "full", no player).
		const void *PlayerModel();
		// Counts Update calls (one per stereo frame).
		unsigned Frame();
		// Around the eye passes. PSU_DrawLayer only calls a node whose object has none of the
		// bits 0x223 in its 16-bit flags at +8; free aim sets one on the local player, so the
		// character, hands and weapons vanish. With the arms on the controllers the bits are
		// cleared for the eye passes and restored after.
		void BeginEyePasses();
		void EndEyePasses();
		// Inside the local player's draw in free aim with the arms following: the game's
		// near-camera hide (0x005089C0) / part culling (0x00562070) is ignored
		// (vr.freeAimNoNearHide / vr.freeAimNoPartCull).
		bool IgnoreNearHide();
		bool IgnorePartCull();

		// Right after 0x0074B760 wrote a model's bone-indexed, model-space bone matrices
		// (out = model +0x64 buffer). The loaded player character takes this path.
		void OnBonePose( float *out, const uint8_t *skeleton );
	}

	// Wraps one D3D draw in the device hooks. While the local player is drawn in a
	// first-person eye pass: vr.fpBody "headless" clips everything above the neck with
	// user clip plane 0, "hidden" skips the draw, "full" leaves it alone.
	class LocalPlayerDrawScope
	{
	public:
		explicit LocalPlayerDrawScope( IDirect3DDevice9 *device );
		~LocalPlayerDrawScope();

		LocalPlayerDrawScope( const LocalPlayerDrawScope & ) = delete;
		LocalPlayerDrawScope &operator=( const LocalPlayerDrawScope & ) = delete;

		bool Skip() const { return m_skip; }

	private:
		IDirect3DDevice9 *m_device = nullptr;  // set only when the clip plane was applied
		bool m_skip = false;
		unsigned long m_savedClipEnable = 0;
		float m_savedPlane[ 4 ]{};
	};
}
