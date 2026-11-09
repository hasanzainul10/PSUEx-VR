---
paths:
  - "PSUVR/**"
  - "PSUOverlay/**"
  - "PSUDetour/Hook_TCameraTaskPlay.hpp"
  - "PSUExConfig.*"
---

# PSUEx VR Module

The roadmap and architecture are in `docs/VR_ROADMAP.md`. Read it before starting VR work, and tick its progress checklist when a step is done.

## Hard constraints

- Everything is gated on `vr.enabled` in `config.json`. With VR off, flat-screen PSUEx must behave exactly as before.
- `vr.cameraMode` (thirdPerson/firstPerson) and `vr.inputMode` (gamepad/motion) are independent. Never write code that assumes one implies the other. All four combinations must work.
- Runtimes sit behind `IVRRuntime`. OpenXR is the default and OpenVR is the second backend. No runtime-specific calls outside `PSUVR/OpenXRRuntime.cpp` and `PSUVR/OpenVRRuntime.cpp`.
- Frames reach the headset only through the D3D9Ex-to-D3D11 shared-surface bridge (`PSUVR/D3D11Bridge.cpp`). Never submit D3D9 textures to a runtime, and never read frames back through the CPU.
- Stereo is synchronized sequential rendering: both eyes are drawn in the same game tick. Alternating eyes across frames is not allowed.
- VR only changes rendering and local input. Motion-controller actions turn into normal gamepad input through the DirectInput hook, never into packets.

## Matrices

The game's view and projection matrices live at `0x00A85C30` and `0x00A85B30` (D3D row-major, left-handed). OpenXR poses are right-handed. Convert in exactly one place (`VRCamera`), not at each use.
