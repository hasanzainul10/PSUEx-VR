#pragma once

namespace PSUVR
{
	// Named controller vibrations. Each effect is a short pattern of pulses (amplitude, length,
	// frequency); a hand plays one pattern at a time, and an effect of lower priority than the one
	// still playing on that hand is dropped. vr.hapticStrength scales every amplitude,
	// vr.hapticUi turns the UI effects off.
	namespace VRHaptics
	{
		enum class Effect
		{
			// UI (vr.hapticUi)
			Tick,         // a step while dragging the panel
			Click,        // UI pointer click
			Grab,         // panel drag starts
			// Feedback
			Confirm,      // VR chord / toggle / setting changed
			AttackSent,   // a gesture or voice command pressed attack / shoot / cast
			ComboWindow,  // Just Attack window opened (scale 1.6 before the last hit)
			Holster,      // holster zone fired
			Countdown,    // calibration countdown, once per second
			Done,         // calibration captured
			// Game events (VRGameHaptics)
			Hit,          // our attack landed (vr.hapticHits)
			HeavyHit,     // our Just Attack / Photon Art landed
			Hurt,         // we lost HP (vr.hapticHurt; scale grows with the share of max HP lost)
			// Panel tests: what the controllers can render
			TestStrength, // 0.2, 0.4, 0.6, 0.8, 1.0 for 0.1 s each, 0.6 s apart
			TestLength,   // 0.8 for 10, 20, 40, 80 ms, 0.6 s apart
			Count
		};

		// Plays `effect` on `hand` (0 left, 1 right); `scale` multiplies its amplitudes. The first
		// pulse goes out at once, later ones from Update.
		void Play( int hand, Effect effect, float scale = 1.0f );
		void PlayBoth( Effect effect, float scale = 1.0f );
		// Once per frame after input: sends the patterns' pending pulses.
		void Update();
		const char *Name( Effect effect );
	}
}
