#pragma once

#include <cstdint>

namespace PSUVR
{
	// Speech to text and voice commands (vr.speechToText, left Square held in the field).
	// Recognition runs in the 64-bit helper PSUExSpeech.exe (Moonshine), started on demand and
	// fed BEGIN / END over its stdin; its transcript comes back as a line on stdout. The text is
	// either a voice command (vr.voiceCommands: a technique on the equipped weapon) or is typed
	// into the game's own chat box: the chat key and Enter go in as local key presses
	// (InjectKeys, from the game's keyboard poll) and the characters as WM_CHAR, so the game
	// sends it the normal way. No packets are touched.
	namespace VRSpeech
	{
		// Once per frame on the VR update thread, after VRSyntheticPad::Update.
		void Update();
		// Left Square pressed / released in the field.
		void BeginTalk();
		void EndTalk();
		bool Talking();
		// Cycles where speech goes: command -> chat -> auto (command while a weapon is out in the
		// field). Writes vr.voiceRoute; the VR settings panel's Save keeps it.
		void CycleRoute();

		// Called from the game's keyboard poll (0x007738E0): adds the key codes the chat sender is
		// holding to the held-key list. keys has room for 14; returns the new count.
		unsigned InjectKeys( uint8_t *keys, unsigned count );
	}
}
