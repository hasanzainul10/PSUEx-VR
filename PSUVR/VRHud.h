#pragma once

struct IDirect3DDevice9;
struct _D3DMATRIX;
typedef _D3DMATRIX D3DMATRIX;
struct ID3D11Texture2D;

namespace PSUVR
{
	// Captures town UI onto transparent shared RTs for OpenXR quad layers
	// (head-locked personal HUD + world-locked shop/dialog panel).
	// Avatar nameplates/bubbles are remapped onto stereo eyes separately.
	class VRHud
	{
		struct Aabb { float minX, minY, maxX, maxY; };

	public:
		static VRHud &Get();

		// Pass-B render layer being drawn (0x2B..0x3F), -1 outside pass B. Set by
		// Hook_PassB, which runs the layer loop itself so attribution is exact.
		static void SetPassBLayer( int layer );
		static int PassBLayer() { return s_passBLayer; }

		// Clear both HUD RTs at game resolution. Call once per stereo frame after Pass A.
		void BeginStereoFrame( IDirect3DDevice9 *device, unsigned gameW, unsigned gameH );

		bool Active() const { return m_active; }
		bool Capturing() const { return m_capturing; }
		// While 0x793C40 runs: Pass B DP-UPs are still captured; count them for UI-replay fallback.
		void BeginUiDraw() { m_inUiDraw = true; m_uiPassBCaptures = 0; }
		void EndUiDraw() { m_inUiDraw = false; }
		bool InUiDraw() const { return m_inUiDraw; }
		// The last CapturePassBDraw drew a nameplate on the monitor itself (flatMirrorEye): skip the game's draw.
		bool NameplateOnMonitor() const { return m_nameplateOnMonitor; }
		// Free aim with the gun up: this 2D draw is a piece of the game's reticle (ours
		// replaces it), so it isn't drawn at all.
		bool BlockReticleDraw( IDirect3DDevice9 *device, const void *verts, unsigned primitiveType,
			unsigned primitiveCount, unsigned stride ) const;
		unsigned UiPassBCaptures() const { return m_uiPassBCaptures; }

		// After a 2D DP-UP (Pass B or later UI): eyes / user visor / world kiosk.
		void CapturePassBDraw( IDirect3DDevice9 *device, unsigned primitiveType,
			unsigned primitiveCount, const void *verts, unsigned stride, void *ret );

		// Font / glyph batches: XYZRHW DrawIndexedPrimitive → HUD RTs.
		void CaptureXyzrhwIndexed( IDirect3DDevice9 *device, unsigned primitiveType,
			int baseVertexIndex, unsigned minIndex, unsigned numVertices,
			unsigned startIndex, unsigned primitiveCount, void *ret );

		// Font batches already in system memory (DIP-UP).
		void CaptureXyzrhwIndexedUp( IDirect3DDevice9 *device, unsigned primitiveType,
			unsigned minVertexIndex, unsigned numVertices, unsigned primitiveCount,
			const void *indexData, unsigned indexFormat, const void *verts, unsigned stride, void *ret );

		// Mission map (pass-B layers 0x31 / 0x32) vertex-buffer, indexed and DIP-UP draws, after the
		// game's own draw: replayed onto the visor panel (hudMapMeshes). kind: 0 DP, 1 DIP, 2 DIP-UP.
		void CaptureMapMeshDraw( IDirect3DDevice9 *device, int kind, unsigned primitiveType,
			int baseVertexIndex, unsigned minIndex, unsigned numVertices, unsigned start,
			unsigned primitiveCount, const void *indexData, unsigned indexFormat,
			const void *verts, unsigned stride, void *ret );

		// Fallback when 0x793C40 issued no Pass B DP-UP (no eyes path).
		void CaptureUiDraw( IDirect3DDevice9 *device,
			void *thisPtr, int a4, int vertexCount, void *vertexPtr, int drawType,
			int( __thiscall *drawFn )( void *, int, int, void *, int ) );

		ID3D11Texture2D *UserTexture() const;
		ID3D11Texture2D *WorldTexture() const;
		bool HasUserContent() const { return m_hasUser; }
		// Something else (the PSUEx ImGui overlay) drew into the user HUD RT this frame.
		void MarkUserContent() { m_hasUser = true; }
		bool HasWorldContent() const { return m_hasWorld; }

		void EndStereoFrame() { m_active = false; }

	private:
		VRHud() = default;

		bool EnsureTargets( IDirect3DDevice9 *device, unsigned w, unsigned h );
		void ReplayOnto( IDirect3DDevice9 *device, bool worldLayer,
			void *thisPtr, int a4, int vertexCount, void *vertexPtr, int drawType,
			int( __thiscall *drawFn )( void *, int, int, void *, int ) );
		void ReplayPassBOnto( IDirect3DDevice9 *device, bool worldLayer, unsigned primitiveType,
			unsigned primitiveCount, const void *verts, unsigned stride, bool rememberAabb = true );
		// Expanded font triangle list: route per glyph quad, one replay per layer.
		void ReplayFontBatch( IDirect3DDevice9 *device, const unsigned char *verts,
			unsigned vertCount, unsigned stride, void *ret );
		// true = HudWorld, false = HudUser. Uses sticky world before visor zones.
		bool ClassifyWorldLayer( const void *verts, unsigned vertCount, unsigned stride ) const;
		bool AlreadyCapturedUserAabb( float minX, float minY, float maxX, float maxY ) const;
		void RememberUserAabb( float minX, float minY, float maxX, float maxY );
		void RememberWorldAabb( float minX, float minY, float maxX, float maxY );
		bool NearExistingUserAabb( float cx, float cy, float pad ) const;
		// Current render target is the game-sized frame (not a render-to-texture pass).
		bool TargetsGameFrame( IDirect3DDevice9 *device ) const;
		bool NearExistingWorldAabb( float cx, float cy, float pad ) const;
		static bool IsWorldUiWidget( const void *verts, int vertexCount, unsigned stride,
			unsigned gameW, unsigned gameH );
		// Size/position of a nameplate or speech bubble (mid screen, small).
		static bool IsAvatarRegion( const void *verts, unsigned vertCount, unsigned stride,
			unsigned gameW, unsigned gameH );
		static bool IsAvatarAnchoredPassB( const void *verts, unsigned primitiveType,
			unsigned primitiveCount, unsigned stride, unsigned gameW, unsigned gameH );
		bool IsVisorChrome( float minX, float minY, float maxX, float maxY,
			unsigned gameW, unsigned gameH ) const;

		// Mission map fit: the 2D frame seen this frame (layer 0x30), and the projection that
		// moves the 3D map into it. false = draw unchanged.
		void NoteMapFrame( IDirect3DDevice9 *device, int layer, const void *verts, unsigned vertCount, unsigned stride );
		bool MapFitProjection( IDirect3DDevice9 *device, int layer, D3DMATRIX &fitted ) const;

		static int s_passBLayer;
		Aabb m_mapFrame{};
		bool m_haveMapFrame = false;
		bool m_active = false;
		bool m_capturing = false;
		bool m_inUiDraw = false;
		bool m_nameplateOnMonitor = false;
		unsigned m_uiPassBCaptures = 0;
		bool m_hasUser = false;
		bool m_hasWorld = false;
		bool m_logged = false;
		unsigned m_w = 0;
		unsigned m_h = 0;

		static constexpr unsigned kMaxLayerAabbs = 96;
		Aabb m_userAabbs[ kMaxLayerAabbs ]{};
		unsigned m_userAabbCount = 0;
		Aabb m_worldAabbs[ kMaxLayerAabbs ]{};
		unsigned m_worldAabbCount = 0;
	};
}
