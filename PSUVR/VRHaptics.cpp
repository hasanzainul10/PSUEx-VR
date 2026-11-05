#include "VRHaptics.h"

#include "VRControllers.h"
#include "../Misc/Log.h"
#include "../PSUExConfig.h"

#include <windows.h>

namespace PSUVR
{
	namespace VRHaptics
	{
		namespace
		{
			constexpr int kMaxPulses = 5;

			struct Pulse
			{
				float at;         // seconds after the effect starts
				float amplitude;  // 0..1 before vr.hapticStrength
				float seconds;
				float frequency;  // Hz, 0 = the runtime's default
			};

			struct Definition
			{
				const char *name;
				int priority;  // a playing effect of higher priority isn't cut short
				bool ui;
				int count;
				Pulse pulses[ kMaxPulses ];
			};

			// Short weak pulses (0.25 for 20 ms) weren't felt while moving the controllers, so the
			// everyday ones are stronger / longer than the values the call sites used before VRHaptics.
			const Definition kEffects[] = {
				// Click at 0.45 / 30 ms wasn't felt in use, the chord's 0.7 / 60 ms was.
				{ "Tick", 0, true, 1, { { 0.0f, 0.5f, 0.04f, 0.0f } } },
				{ "Click", 0, true, 1, { { 0.0f, 0.7f, 0.06f, 0.0f } } },
				{ "Grab", 0, true, 1, { { 0.0f, 0.75f, 0.07f, 0.0f } } },
				// Two pulses: the first (on the grip press) wasn't felt; the second comes 0.2 s later.
				{ "Confirm", 1, false, 2, { { 0.0f, 0.7f, 0.06f, 0.0f }, { 0.2f, 0.7f, 0.06f, 0.0f } } },
				{ "AttackSent", 1, false, 1, { { 0.0f, 0.5f, 0.05f, 0.0f } } },
				{ "ComboWindow", 2, false, 1, { { 0.0f, 0.5f, 0.03f, 0.0f } } },
				{ "Holster", 3, false, 1, { { 0.0f, 0.9f, 0.12f, 0.0f } } },
				{ "Countdown", 1, false, 1, { { 0.0f, 0.3f, 0.03f, 0.0f } } },
				{ "Done", 3, false, 1, { { 0.0f, 0.8f, 0.15f, 0.0f } } },
				{ "TestStrength", 9, false, 5, { { 0.0f, 0.2f, 0.1f, 0.0f }, { 0.6f, 0.4f, 0.1f, 0.0f },
					{ 1.2f, 0.6f, 0.1f, 0.0f }, { 1.8f, 0.8f, 0.1f, 0.0f }, { 2.4f, 1.0f, 0.1f, 0.0f } } },
				{ "TestLength", 9, false, 4, { { 0.0f, 0.8f, 0.01f, 0.0f }, { 0.6f, 0.8f, 0.02f, 0.0f },
					{ 1.2f, 0.8f, 0.04f, 0.0f }, { 1.8f, 0.8f, 0.08f, 0.0f } } },
			};
			static_assert( sizeof( kEffects ) / sizeof( kEffects[ 0 ] ) == static_cast< size_t >( Effect::Count ),
				"one definition per effect" );

			struct Playing
			{
				const Definition *def = nullptr;
				double start = 0.0;
				double end = 0.0;  // the last pulse's end: until then the priority holds
				float scale = 1.0f;
				int next = 0;
			};
			Playing s_hands[ 2 ];

			double Now()
			{
				static LARGE_INTEGER frequency{};
				if( !frequency.QuadPart )
				{
					QueryPerformanceFrequency( &frequency );
				}
				LARGE_INTEGER now;
				QueryPerformanceCounter( &now );
				return static_cast< double >( now.QuadPart ) / static_cast< double >( frequency.QuadPart );
			}

			float Strength()
			{
				const float strength = Config::Get().vr.m_hapticStrength;
				return strength < 0.0f ? 0.0f : ( strength > 1.5f ? 1.5f : strength );
			}

			void Send( int hand, const Pulse &pulse, float scale )
			{
				float amplitude = pulse.amplitude * scale * Strength();
				amplitude = amplitude > 1.0f ? 1.0f : amplitude;
				if( amplitude > 0.0f )
				{
					VRControllers::Haptic( hand, amplitude, pulse.seconds, pulse.frequency );
				}
			}

			// Sends every pulse of the hand's pattern that is due.
			void Pump( int hand, double now )
			{
				Playing &playing = s_hands[ hand ];
				if( !playing.def )
				{
					return;
				}
				while( playing.next < playing.def->count && now - playing.start >= playing.def->pulses[ playing.next ].at )
				{
					Send( hand, playing.def->pulses[ playing.next ], playing.scale );
					++playing.next;
				}
				if( playing.next >= playing.def->count && now >= playing.end )
				{
					playing.def = nullptr;
				}
			}
		}

		void Play( int hand, Effect effect, float scale )
		{
			const int index = static_cast< int >( effect );
			if( hand < 0 || hand > 1 || index < 0 || index >= static_cast< int >( Effect::Count ) || scale <= 0.0f )
			{
				return;
			}
			const Definition &def = kEffects[ index ];
			if( Strength() <= 0.0f || ( def.ui && !Config::Get().vr.m_hapticUi ) )
			{
				return;
			}
			const double now = Now();
			Playing &playing = s_hands[ hand ];
			if( playing.def && now < playing.end && playing.def->priority > def.priority )
			{
				return;
			}
			const Pulse &last = def.pulses[ def.count - 1 ];
			playing.def = &def;
			playing.start = now;
			playing.end = now + last.at + last.seconds;
			playing.scale = scale;
			playing.next = 0;
#ifdef _DEBUG
			if( !def.ui )
			{
				PSU_LOG( "VRHaptic: %s %s x%.2f\n", def.name, hand == 0 ? "left" : "right", scale );
			}
#endif
			Pump( hand, now );
		}

		void PlayBoth( Effect effect, float scale )
		{
			Play( 0, effect, scale );
			Play( 1, effect, scale );
		}

		void Update()
		{
			const double now = Now();
			Pump( 0, now );
			Pump( 1, now );
		}

		const char *Name( Effect effect )
		{
			const int index = static_cast< int >( effect );
			return index >= 0 && index < static_cast< int >( Effect::Count ) ? kEffects[ index ].name : "?";
		}
	}
}
