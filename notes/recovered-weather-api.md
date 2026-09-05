# Weather divergence diagnosis

Read-only inspection of the two running cities on 2026-09-05 confirms that
their native weather timelines differ. This is not merely a HUD formatting
problem or a clock offset.

Host PID 42272: 34 timeline entries, current index 0. First weather GUID
6c7ae7fe42b16c9bcf47f2d0b6d2de2d resolves to `-20_NoFog`.
Client PID 60632: 38 entries, current index 0. First weather GUID
5b9ceea1a5eba2be4f4eb6f6c7efc499 resolves to `-30_NoFog`.
Both first events begin at native tick 402653184000. Later event times and
weather identities differ too. Aligning clocks alone cannot fix this.

Verified native layout (supported executable only):

- Global weather-system pointer RVA 2B687A0, vtable RVA 1E35390.
- Timeline pointer +118, count +120; 16-byte entries: int64 native ticks,
  followed by a local weather-entry pointer.
- Selected timeline definitions +130; current timeline index +148.
- Current weather-entry pointer +150; previous weather +158.
- Last change time +160; citizen-effects last resolve time +168.
- Weather entry starts with a local ASCII name pointer and a 16-byte GUID at +8.

Offsets are corroborated by named RTTI registrations around RVA 127C671
through 127C8C9 (`WeatherTimeline`, `SelectedWeatherTimelineEntries`,
`WeatherTimelineIndex`, `CurrentWeather`, `PreviousWeather`, timestamps).
`Inspect-Runtime.py PID BASE weather unused` prints the first twelve events
read-only using portable names/GUIDs rather than process-specific pointers.

The current start protocol transfers mapIndex only, so independently created
weather schedules remain independent. It is not yet established whether the
different selection originates from RNG, difficulty, or both. A production
fix needs host-authoritative weather schedule/selection (including future
generation), portable GUID resolution on clients, and native notification /
callback rescheduling. Replacing only the displayed Celsius number or writing
the CurrentWeather pointer would leave effects and future events inconsistent.

No native weather mutation or weather synchronization is implemented by this
diagnostic change. Campaign launch remains a separate pending task.
