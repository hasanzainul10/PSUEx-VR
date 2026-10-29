#pragma once

#include "VRRuntime.h"

#include <d3d9.h>

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct _D3DVIEWPORT9;
typedef struct _D3DVIEWPORT9 D3DVIEWPORT9;
typedef struct tagRECT RECT;

namespace PSUVR
{
	// Synchronized sequential stereo: after the game's own 3D pass, the same pass
	// (0x004FD990) runs once per eye into the bridge's eye targets, in the same tick.
	class StereoRenderer
	{
	public:
		using Pass = void( __cdecl * )();

		static StereoRenderer &Get();

		// True when both eyes were drawn this call.
		bool RenderEyes( IDirect3DDevice9 *device, Pass pass );
		// RenderEyes' preconditions (scene, bridge, views) without drawing anything.
		bool CanRenderEyes( IDirect3DDevice9 *device ) const;
		// After RenderEyes: the left eye, centre-cropped to the game viewport's aspect, onto the game's
		// render target, and the game's depth cleared, so pass B draws the HUD over it (flatMirrorEye).
		bool MirrorLeftEye( IDirect3DDevice9 *device );

		// True from RenderEyes until the frame is submitted.
		bool HasFrame() const { return m_hasFrame; }
		const EyePose *RenderedPoses() const { return m_poses; }
		const EyePose *RenderedViewSpacePoses() const { return m_viewSpacePoses; }
		void ConsumeFrame() { m_hasFrame = false; m_haveEyeMatrices = false; m_mirrorActive = false; }
		// MirrorLeftEye put the left eye on the monitor this frame (nameplates follow it there).
		bool MirrorActive() const { return m_mirrorActive; }

		unsigned EyeWidth() const { return m_lastEyeW; }
		unsigned EyeHeight() const { return m_lastEyeH; }
		unsigned GameWidth() const { return m_lastGameW; }
		unsigned GameHeight() const { return m_lastGameH; }

		// True while pass() runs for an eye. Used by D3D hooks to skip flat-sourced
		// screen-space effect quads that would ghost the mono camera over stereo.
		static bool InEyePass();
		// vr.enabled + vr.stereo and the current scene matches vr.stereoMode (town by default).
		static bool SceneAllowsStereo();
		// True while remapping Pass B nameplates/bubbles onto eye RTs (skip HUD capture).
		static bool InNameplateDraw();

		// Debug diagnostics for screen-space (XYZRHW) effect draws ("VREffect:" log). where: 0 eye
		// pass, 1 pass B / UI, 2 elsewhere. verts may be null (vertex buffer draws). Release: no-op.
		// Debug: 3D vertex-pointer draws in an eye pass that reach within 2 m of the game camera or
		// surround it (camera-attached sheets / boxes), once per draw site ("Stereo: 3D near camera").
		// Debug: vertex constants the game sets (kept per register), and eye-pass draws that use a
		// vertex shader (its matrices are in those constants, not the fixed-function view), once per
		// draw site ("Stereo: shader draw").
		// Eye pass, while the full-screen blur (layer 0x1F) or glare (0x20) draws and it is off for
		// the headset: skip the draw call (the layer still runs, so its render targets stay set up).
		static bool SkipEyeEffectDraw( void *ret );
		// Debug site bisect: step through the screen-space draw sites seen in the eyes, skipping the
		// selected one in the eyes (returns the toast text).
		static const char *StepSiteBisect( int direction );
		// Debug: count a draw call for the layer diagnostics (sky / blur / glare, flat vs eyes).
		static void CountLayerDraw( void *ret );
		static void NoteVertexConstants( unsigned startRegister, const float *data, unsigned vector4Count, void *ret );
		static void NoteShaderDraw( IDirect3DDevice9 *device, unsigned primitiveCount, void *ret );
		static void NoteNearCameraDraw( IDirect3DDevice9 *device, unsigned primitiveType, unsigned primitiveCount,
			const void *verts, unsigned stride, void *ret );
		// Debug: distinct draws made in the eye passes while a given render layer runs (default 0x0F, enemies).
		static void NoteLayerDraw( IDirect3DDevice9 *device, const char *api, unsigned primitiveType, unsigned primitiveCount, void *ret );
		// Eye passes: the enemies' hit-spark billboard (layer 0x0F, 2-triangle additive 32x32) is the cross-shaped flash
		// where a shot lands; it is built for the flat camera, so it is hidden in the eyes.
		static bool SkipEyeHitSpark( IDirect3DDevice9 *device, unsigned primitiveCount );
		static void NoteEffectDraw( IDirect3DDevice9 *device, int where, unsigned primitiveType, unsigned primitiveCount,
			const void *verts, unsigned stride, void *ret );
		// A technique is being cast: effect draws in the next 2 s are tagged "tech".
		static void NoteTechnique();

		// Unproject game XYZRHW (Pass B nameplate/bubble) and draw onto both eye RTs.
		// With the mirror active, also draws it on the monitor at its left-eye place; true when it did.
		bool DrawNameplateToEyes( IDirect3DDevice9 *device, unsigned primitiveType,
			unsigned primitiveCount, const void *verts, unsigned stride );

		// Track RT textures written during the current eye pass (SetRT / StretchRect dest).
		static void OnSetRenderTarget( IDirect3DSurface9 *target );
		static void OnStretchRect( IDirect3DSurface9 *source, IDirect3DSurface9 *dest, void *ret );

		// XYZRHW draws that sample an RT texture not written this eye pass → skip.
		static bool ShouldSkipScreenQuad( IDirect3DDevice9 *device, void *ret, unsigned primitiveCount, bool userPointer );

		// If this DP-UP is a near-full-screen XYZRHW quad sized for the game RT, return a
		// rewritten vertex buffer scaled to the eye RT. Otherwise return nullptr (use original).
		static const void *StretchScreenQuadIfNeeded( IDirect3DDevice9 *device, unsigned primitiveType,
			unsigned primitiveCount, const void *verts, unsigned stride, void *ret );
		// Eye pass: a world sprite the game projected with the flat camera (2D sprite draw with a
		// projected depth), re-projected for the current eye. Null = draw as is.
		// Eye pass: a world matrix that puts a model at the game camera (sky box) is moved to the
		// current eye. True = use `out`.
		static bool RecenterCameraAnchored( const D3DMATRIX &world, D3DMATRIX &out, void *ret );
		static const void *ReprojectSpriteIfNeeded( IDirect3DDevice9 *device, unsigned primitiveType,
			unsigned primitiveCount, const void *verts, unsigned stride, void *ret );

		// True when RT0 is the current eye color target (shadow RTs return false).
		static bool EyeColorTargetBound( IDirect3DDevice9 *device );
		static bool EyeColorTargetIs( IDirect3DSurface9 *surface );
		static void GetEyeViewport( D3DVIEWPORT9 &out );
		static void GetEyeScissor( RECT &out );

	private:
		StereoRenderer() = default;

		static void BeginEyePass( IDirect3DSurface9 *eyeTarget, unsigned eyeW, unsigned eyeH,
			unsigned gameW, unsigned gameH );
		static void EndEyePass();
		static void MarkSurfaceTexture( IDirect3DSurface9 *surface );
		// The eye pose of the next eye pass (dressing room background placement).
		static void SetPassEye( const EyePose &eye );

		bool m_hasFrame = false;
		bool m_loggedFirst = false;
		bool m_loggedMirror = false;
		bool m_mirrorActive = false;
		RECT m_mirrorSrc{};  // left-eye crop
		RECT m_mirrorDst{};  // where it went on the game RT
		const IDirect3DSurface9 *m_mirrorTarget = nullptr;  // that RT (compared only)
		bool m_haveEyeMatrices = false;
		bool m_loggedNameplate = false;
		unsigned m_lastEyeW = 0;
		unsigned m_lastEyeH = 0;
		unsigned m_lastGameW = 0;
		unsigned m_lastGameH = 0;
	public:
		// Game PROJ captured for the current stereo frame (pre-transformed 2D was built with it).
		const D3DMATRIX &GameProj() const { return m_gameProj; }
	private:
		D3DMATRIX m_gameProj{};
		D3DMATRIX m_invGameProj{};
		D3DMATRIX m_invGameView{};
		D3DMATRIX m_gameView{};
		D3DMATRIX m_invEyeView[ 2 ]{};  // each eye in game world
		bool m_haveEyePosition[ 2 ]{};
		bool m_haveInvGameView = false;
		D3DMATRIX m_eyeView[ 2 ]{};
		D3DMATRIX m_eyeProj[ 2 ]{};
		EyePose m_poses[ 2 ]{};
		EyePose m_viewSpacePoses[ 2 ]{};
	};
}
