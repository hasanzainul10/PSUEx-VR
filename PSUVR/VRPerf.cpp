#include "VRPerf.h"

#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <imgui.h>

#include <d3d9.h>

#include <algorithm>
#include <cstdio>

namespace PSUVR
{
	namespace VRPerf
	{
		namespace
		{
			constexpr int kStages = static_cast< int >( Stage::Count );
			constexpr int kGpu = static_cast< int >( Gpu::Count );
			// GPU queries are read this many frames late at most; older unread slots are dropped.
			constexpr int kSlots = 4;

			const char *const kStageNames[ kStages ] = {
				"3D (monitor)", "Left eye", "Right eye", "2D / HUD", "Overlay", "Capture", "Present",
				"Submit", "  WaitFrame", "  GPU wait", "  Updates", "    Input", "    Controllers", "    UI pointer", "    Synthetic pad",
				"      Combo feedback", "      Combat", "      Free aim", "      Room walk", "    Label update",
				"    Speech", "  Stereo submit", "  Swapchain wait", "  D3D11 blit",
				"  HUD quads", "  Label cards", "  xrEndFrame" };
			const char *const kStageKeys[ kStages ] = {
				"flat", "eyeL", "eyeR", "passB", "overlay", "capture", "present", "submit", "waitFrame", "gpuWait",
				"updates", "input", "ctrl", "uiPtr", "pad", "padCombo", "padCombat", "padAim", "padRoom",
				"lblUpd", "speech", "stereo", "swapWait", "blit", "hudQuads", "labels", "endFrame" };
			const char *const kGpuKeys[ kGpu ] = { "flat", "eyeL", "eyeR", "passB" };
			// Which CPU row shows each GPU pass in the overlay.
			const Stage kGpuRow[ kGpu ] = { Stage::FlatPass, Stage::EyeLeft, Stage::EyeRight, Stage::PassB };

			bool s_enabled = false;
			int64_t s_qpcFrequency = 0;
			int64_t s_lastFrame = 0;
			double s_cur[ kStages ]{};
			double s_displayMs = 0.0;
			bool s_stereoThisFrame = false;

			struct Window
			{
				unsigned frames = 0;
				double frameSum = 0.0, frameMax = 0.0;
				unsigned overBudget = 0;
				unsigned stereo = 0;
				double sum[ kStages ]{}, max[ kStages ]{};
				unsigned gpuCount[ kGpu ]{};
				double gpuSum[ kGpu ]{}, gpuMax[ kGpu ]{};
				unsigned gpuFrameCount = 0;
				double gpuFrameSum = 0.0, gpuFrameMax = 0.0;
				int64_t start = 0;

				void AddFrame( double frameMs, const double *stages, bool stereoFrame, double budgetMs )
				{
					++frames;
					frameSum += frameMs;
					frameMax = ( std::max )( frameMax, frameMs );
					if( budgetMs > 0.0 && frameMs > budgetMs * 1.1 )
					{
						++overBudget;
					}
					if( stereoFrame )
					{
						++stereo;
					}
					for( int i = 0; i < kStages; ++i )
					{
						sum[ i ] += stages[ i ];
						max[ i ] = ( std::max )( max[ i ], stages[ i ] );
					}
				}

				void AddGpu( int pass, double ms )
				{
					++gpuCount[ pass ];
					gpuSum[ pass ] += ms;
					gpuMax[ pass ] = ( std::max )( gpuMax[ pass ], ms );
				}

				void AddGpuFrame( double ms )
				{
					++gpuFrameCount;
					gpuFrameSum += ms;
					gpuFrameMax = ( std::max )( gpuFrameMax, ms );
				}
			};

			// The overlay shows the last finished 1 s window; the log gets 5 s windows.
			Window s_overlayWindow;
			Window s_shown;
			bool s_haveShown = false;
			Window s_logWindow;

			struct GpuSlot
			{
				IDirect3DQuery9 *disjoint = nullptr;
				IDirect3DQuery9 *frequency = nullptr;
				IDirect3DQuery9 *frame[ 2 ]{};
				IDirect3DQuery9 *pass[ kGpu ][ 2 ]{};
				bool used[ kGpu ]{};
				bool open = false;
				bool pending = false;
			};

			GpuSlot s_slots[ kSlots ];
			int s_slot = 0;
			IDirect3DDevice9 *s_device = nullptr;
			bool s_gpuFailed = false;

			int64_t Now()
			{
				LARGE_INTEGER value{};
				QueryPerformanceCounter( &value );
				return value.QuadPart;
			}

			double Ms( int64_t ticks )
			{
				if( !s_qpcFrequency )
				{
					LARGE_INTEGER frequency{};
					QueryPerformanceFrequency( &frequency );
					s_qpcFrequency = frequency.QuadPart ? frequency.QuadPart : 1;
				}
				return static_cast< double >( ticks ) * 1000.0 / static_cast< double >( s_qpcFrequency );
			}

			void ReleaseQuery( IDirect3DQuery9 *&query )
			{
				if( query )
				{
					query->Release();
					query = nullptr;
				}
			}

			void ReleaseSlots()
			{
				for( GpuSlot &slot : s_slots )
				{
					ReleaseQuery( slot.disjoint );
					ReleaseQuery( slot.frequency );
					ReleaseQuery( slot.frame[ 0 ] );
					ReleaseQuery( slot.frame[ 1 ] );
					for( auto &pair : slot.pass )
					{
						ReleaseQuery( pair[ 0 ] );
						ReleaseQuery( pair[ 1 ] );
					}
					slot = GpuSlot{};
				}
			}

			bool CreateSlots( IDirect3DDevice9 *device )
			{
				for( GpuSlot &slot : s_slots )
				{
					bool ok = SUCCEEDED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMPDISJOINT, &slot.disjoint ) ) &&
						SUCCEEDED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMPFREQ, &slot.frequency ) ) &&
						SUCCEEDED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &slot.frame[ 0 ] ) ) &&
						SUCCEEDED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &slot.frame[ 1 ] ) );
					for( int pass = 0; ok && pass < kGpu; ++pass )
					{
						ok = SUCCEEDED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &slot.pass[ pass ][ 0 ] ) ) &&
							SUCCEEDED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &slot.pass[ pass ][ 1 ] ) );
					}
					if( !ok )
					{
						ReleaseSlots();
						return false;
					}
				}
				return true;
			}

			bool EnsureQueries( IDirect3DDevice9 *device )
			{
				if( s_gpuFailed || !device )
				{
					return false;
				}
				if( s_device == device )
				{
					return true;
				}
				ReleaseSlots();
				s_slot = 0;
				s_device = device;
				if( !CreateSlots( device ) )
				{
					s_gpuFailed = true;
					PSU_LOG( "Perf: GPU timestamp queries are not available; showing CPU times only\n" );
					return false;
				}
				return true;
			}

			bool ReadTimestamp( IDirect3DQuery9 *query, UINT64 &value )
			{
				return query->GetData( &value, sizeof( value ), 0 ) == S_OK;
			}

			// Reads a closed slot without flushing. False while the GPU hasn't reached it yet.
			bool TryReadSlot( GpuSlot &slot )
			{
				BOOL disjoint = TRUE;
				UINT64 frequency = 0;
				if( slot.disjoint->GetData( &disjoint, sizeof( disjoint ), 0 ) != S_OK ||
					slot.frequency->GetData( &frequency, sizeof( frequency ), 0 ) != S_OK )
				{
					return false;
				}
				UINT64 frameStart = 0, frameEnd = 0;
				if( !ReadTimestamp( slot.frame[ 0 ], frameStart ) || !ReadTimestamp( slot.frame[ 1 ], frameEnd ) )
				{
					return false;
				}
				double passMs[ kGpu ]{};
				for( int pass = 0; pass < kGpu; ++pass )
				{
					if( !slot.used[ pass ] )
					{
						continue;
					}
					UINT64 begin = 0, end = 0;
					if( !ReadTimestamp( slot.pass[ pass ][ 0 ], begin ) || !ReadTimestamp( slot.pass[ pass ][ 1 ], end ) )
					{
						return false;
					}
					passMs[ pass ] = end > begin ? static_cast< double >( end - begin ) * 1000.0 / static_cast< double >( frequency ) : 0.0;
				}
				if( disjoint || !frequency )
				{
					return true; // finished, but the clock changed: drop it
				}
				for( int pass = 0; pass < kGpu; ++pass )
				{
					if( slot.used[ pass ] )
					{
						s_overlayWindow.AddGpu( pass, passMs[ pass ] );
						s_logWindow.AddGpu( pass, passMs[ pass ] );
					}
				}
				const double frameMs = frameEnd > frameStart ?
					static_cast< double >( frameEnd - frameStart ) * 1000.0 / static_cast< double >( frequency ) : 0.0;
				s_overlayWindow.AddGpuFrame( frameMs );
				s_logWindow.AddGpuFrame( frameMs );
				return true;
			}

			void OpenSlot( IDirect3DDevice9 *device )
			{
				GpuSlot &slot = s_slots[ s_slot ];
				if( slot.pending && !TryReadSlot( slot ) )
				{
					// Still not finished kSlots frames later: drop it rather than wait.
				}
				slot.pending = false;
				for( bool &used : slot.used )
				{
					used = false;
				}
				slot.disjoint->Issue( D3DISSUE_BEGIN );
				slot.frame[ 0 ]->Issue( D3DISSUE_END );
				slot.open = true;
				( void )device;
			}

			void LogWindow( const Window &w, double seconds )
			{
				if( !w.frames )
				{
					return;
				}
				char text[ 2048 ];
				int length = std::snprintf( text, sizeof( text ),
					"Perf %.0f s: %u frames, %.1f fps, frame avg %.2f max %.2f ms (headset %.0f Hz, %u%% over budget, stereo %u%%, fast %c%c%c%c%c, layer %s, res %.2f) | CPU",
					seconds, w.frames, w.frames / seconds, w.frameSum / w.frames, w.frameMax,
					s_displayMs > 0.0 ? 1000.0 / s_displayMs : 0.0, w.overBudget * 100u / w.frames, w.stereo * 100u / w.frames,
					// Phase 1 toggles: F flat copy skipped, C plain copies, H HUD lock ranges, L log batching;
					// phase 2: M monitor mirrors the left eye.
					Config::Get().vr.m_perfSkipFlatCopy ? 'F' : '-', Config::Get().vr.m_perfCopyBlits ? 'C' : '-',
					Config::Get().vr.m_perfHudLockRanges ? 'H' : '-', Config::Get().vr.m_perfLogBatching ? 'L' : '-',
					Config::Get().vr.m_flatMirrorEye ? 'M' : '-',
					// phase 3: stereo layer space and eye resolution scale.
					Config::Get().vr.m_stereoLayerSpace == "local" ? "local" : "view", Config::Get().vr.m_eyeResolutionScale );
				for( int i = 0; i < kStages && length > 0 && length < static_cast< int >( sizeof( text ) ); ++i )
				{
					length += std::snprintf( text + length, sizeof( text ) - length, " %s %.2f/%.2f", kStageKeys[ i ],
						w.sum[ i ] / w.frames, w.max[ i ] );
				}
				if( length > 0 && length < static_cast< int >( sizeof( text ) ) )
				{
					length += std::snprintf( text + length, sizeof( text ) - length, " | GPU" );
				}
				for( int pass = 0; pass < kGpu && length > 0 && length < static_cast< int >( sizeof( text ) ); ++pass )
				{
					if( w.gpuCount[ pass ] )
					{
						length += std::snprintf( text + length, sizeof( text ) - length, " %s %.2f/%.2f", kGpuKeys[ pass ],
							w.gpuSum[ pass ] / w.gpuCount[ pass ], w.gpuMax[ pass ] );
					}
				}
				if( w.gpuFrameCount && length > 0 && length < static_cast< int >( sizeof( text ) ) )
				{
					std::snprintf( text + length, sizeof( text ) - length, " frame %.2f/%.2f",
						w.gpuFrameSum / w.gpuFrameCount, w.gpuFrameMax );
				}
				PSU_LOG( "%s\n", text );
			}
		}

		bool Enabled()
		{
			return s_enabled;
		}

		Scope::Scope( Stage stage ) : m_stage( stage ), m_start( s_enabled ? Now() : 0 )
		{
		}

		Scope::~Scope()
		{
			if( m_start && s_enabled )
			{
				s_cur[ static_cast< int >( m_stage ) ] += Ms( Now() - m_start );
			}
		}

		int64_t Begin()
		{
			return s_enabled ? Now() : 0;
		}

		void End( Stage stage, int64_t start )
		{
			if( start && s_enabled )
			{
				s_cur[ static_cast< int >( stage ) ] += Ms( Now() - start );
			}
		}

		void GpuBegin( IDirect3DDevice9 *device, Gpu pass )
		{
			if( !s_enabled || !EnsureQueries( device ) )
			{
				return;
			}
			GpuSlot &slot = s_slots[ s_slot ];
			if( !slot.open )
			{
				OpenSlot( device );
			}
			slot.pass[ static_cast< int >( pass ) ][ 0 ]->Issue( D3DISSUE_END );
		}

		void GpuEnd( IDirect3DDevice9 *device, Gpu pass )
		{
			if( !s_enabled || s_gpuFailed || device != s_device )
			{
				return;
			}
			GpuSlot &slot = s_slots[ s_slot ];
			if( !slot.open )
			{
				return;
			}
			slot.pass[ static_cast< int >( pass ) ][ 1 ]->Issue( D3DISSUE_END );
			slot.used[ static_cast< int >( pass ) ] = true;
		}

		void BeforePresent( IDirect3DDevice9 *device )
		{
			// Close an open slot even when the overlay was just turned off.
			if( s_gpuFailed || !s_device || device != s_device )
			{
				return;
			}
			GpuSlot &slot = s_slots[ s_slot ];
			if( !slot.open )
			{
				return;
			}
			slot.frame[ 1 ]->Issue( D3DISSUE_END );
			slot.disjoint->Issue( D3DISSUE_END );
			slot.frequency->Issue( D3DISSUE_END );
			slot.open = false;
			slot.pending = true;
			s_slot = ( s_slot + 1 ) % kSlots;
		}

		void EndFrame()
		{
			const auto &vr = Config::Get().vr;
			const bool enabled = vr.m_enabled && vr.m_perfOverlay;
			const int64_t now = Now();
			if( enabled != s_enabled )
			{
				s_enabled = enabled;
				s_lastFrame = 0;
				s_overlayWindow = Window{};
				s_logWindow = Window{};
				s_haveShown = false;
			}
			if( !s_enabled )
			{
				return;
			}

			if( s_lastFrame )
			{
				const double frameMs = Ms( now - s_lastFrame );
				s_overlayWindow.AddFrame( frameMs, s_cur, s_stereoThisFrame, s_displayMs );
				s_logWindow.AddFrame( frameMs, s_cur, s_stereoThisFrame, s_displayMs );
			}
			s_lastFrame = now;
			for( double &value : s_cur )
			{
				value = 0.0;
			}
			s_stereoThisFrame = false;

			for( GpuSlot &slot : s_slots )
			{
				if( slot.pending && slot.disjoint && TryReadSlot( slot ) )
				{
					slot.pending = false;
				}
			}

			if( !s_overlayWindow.start )
			{
				s_overlayWindow.start = now;
			}
			if( !s_logWindow.start )
			{
				s_logWindow.start = now;
			}
			if( Ms( now - s_overlayWindow.start ) >= 1000.0 )
			{
				s_shown = s_overlayWindow;
				s_haveShown = true;
				s_overlayWindow = Window{};
				s_overlayWindow.start = now;
			}
			const double logMs = Ms( now - s_logWindow.start );
			if( logMs >= 5000.0 )
			{
				LogWindow( s_logWindow, logMs / 1000.0 );
				s_logWindow = Window{};
				s_logWindow.start = now;
			}
		}

		void SetDisplayPeriod( int64_t nanoseconds )
		{
			s_displayMs = static_cast< double >( nanoseconds ) / 1.0e6;
		}

		void NoteStereoFrame()
		{
			s_stereoThisFrame = true;
		}

		void DrawOverlay()
		{
			if( !s_enabled )
			{
				return;
			}
			ImGui::SetNextWindowPos( ImVec2( 10.0f, 10.0f ), ImGuiCond_FirstUseEver );
			ImGui::SetNextWindowBgAlpha( 0.7f );
			const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
				ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
				ImGuiWindowFlags_NoSavedSettings;
			if( ImGui::Begin( "PSUEx performance", nullptr, flags ) )
			{
				if( !s_haveShown || !s_shown.frames )
				{
					ImGui::TextUnformatted( "Measuring..." );
				}
				else
				{
					const Window &w = s_shown;
					const double fps = w.frames * 1000.0 / ( std::max )( 1.0, w.frameSum );
					ImGui::Text( "%.1f fps  frame %.2f ms (max %.2f)", fps, w.frameSum / w.frames, w.frameMax );
					if( s_displayMs > 0.0 )
					{
						ImGui::Text( "Headset %.0f Hz (%.2f ms)  over budget %u%%  stereo %u%%", 1000.0 / s_displayMs,
							s_displayMs, w.overBudget * 100u / w.frames, w.stereo * 100u / w.frames );
					}
					if( w.gpuFrameCount )
					{
						ImGui::Text( "GPU frame %.2f ms (max %.2f)", w.gpuFrameSum / w.gpuFrameCount, w.gpuFrameMax );
					}
					else if( s_gpuFailed )
					{
						ImGui::TextUnformatted( "GPU timing not available" );
					}
					if( ImGui::BeginTable( "perf_stages", 5, ImGuiTableFlags_SizingFixedFit ) )
					{
						ImGui::TableSetupColumn( "ms" );
						ImGui::TableSetupColumn( "CPU" );
						ImGui::TableSetupColumn( "max" );
						ImGui::TableSetupColumn( "GPU" );
						ImGui::TableSetupColumn( "max" );
						ImGui::TableHeadersRow();
						for( int i = 0; i < kStages; ++i )
						{
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex( 0 );
							ImGui::TextUnformatted( kStageNames[ i ] );
							ImGui::TableSetColumnIndex( 1 );
							ImGui::Text( "%.2f", w.sum[ i ] / w.frames );
							ImGui::TableSetColumnIndex( 2 );
							ImGui::Text( "%.2f", w.max[ i ] );
							for( int pass = 0; pass < kGpu; ++pass )
							{
								if( kGpuRow[ pass ] == static_cast< Stage >( i ) && w.gpuCount[ pass ] )
								{
									ImGui::TableSetColumnIndex( 3 );
									ImGui::Text( "%.2f", w.gpuSum[ pass ] / w.gpuCount[ pass ] );
									ImGui::TableSetColumnIndex( 4 );
									ImGui::Text( "%.2f", w.gpuMax[ pass ] );
								}
							}
						}
						ImGui::EndTable();
					}
				}
			}
			ImGui::End();
		}
	}
}
