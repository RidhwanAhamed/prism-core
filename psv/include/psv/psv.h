#pragma once

// Prism State Vector — the contract (PRISM-SPEC-PSV-001 v1.0.0, spec §4–6).
// This module is the only thing that crosses the PCE/PGAE firewall.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace prism::psv {

// The spec version this implementation emits (spec §4.3).
inline constexpr const char* kSchemaVersion = "1.0.0";

// One semantic dimension (spec §4.1–4.2): bounded value, per-dimension trust.
struct Dimension {
  double value = 0.5;      // [0,1]; 0.5 is neutral/baseline (spec §3.1)
  double confidence = 0.0; // [0,1]; 0 = pure prior / unknown (spec §4.2)

  // Spec §8.1 — the confidence weighting every consumer must apply: blend
  // toward neutral as confidence falls. At confidence 0 the dimension is
  // inert; at 1 it exerts full influence.
  double effective() const { return 0.5 + (value - 0.5) * confidence; }
};

// Adapter profile that produced the vector (spec §4.3).
enum class Vertical { Aqademiq, Venues, Automotive };

const char* to_string(Vertical v);
std::optional<Vertical> vertical_from_string(std::string_view s);

// Known mode hints (spec §9). mode_hint stays a plain string in StateVector:
// consumers must pass through / ignore unknown hints gracefully (spec §10),
// so the type must be able to carry values this build has never heard of.
namespace mode_hint {
inline constexpr const char* kDeepWork = "deep_work";
inline constexpr const char* kReview = "review";
inline constexpr const char* kCreative = "creative";
inline constexpr const char* kAdmin = "admin";
inline constexpr const char* kWindDown = "wind_down";
inline constexpr const char* kVenueEnergize = "venue_energize";
inline constexpr const char* kVenueSustain = "venue_sustain";
inline constexpr const char* kVenueSettle = "venue_settle";
// The three above name a DIRECTION of travel (lift, hold, settle). Prism Venues
// also lets a manager pin one of six named operating moods outright, so those
// need hints of their own — a direction cannot express "evening warmth". Purely
// additive per spec §10; every one fits RtStateVector::mode_hint[24].
inline constexpr const char* kVenueMorningCalm = "venue_morning_calm";
inline constexpr const char* kVenueDaytimeFlow = "venue_daytime_flow";
inline constexpr const char* kVenueAfternoonLift = "venue_afternoon_lift";
inline constexpr const char* kVenueEveningWarmth = "venue_evening_warmth";
inline constexpr const char* kVenuePeak = "venue_peak";
inline constexpr const char* kVenueWindDown = "venue_wind_down";
inline constexpr const char* kDriveFocus = "drive_focus";
inline constexpr const char* kDriveEnergize = "drive_energize";
inline constexpr const char* kDriveCalm = "drive_calm";
} // namespace mode_hint

// The vector itself (spec §4). v1 carries exactly these four dimensions;
// future dimensions arrive additively in a MINOR version (spec §10).
struct StateVector {
  std::string schema_version = kSchemaVersion;
  Vertical vertical = Vertical::Aqademiq;
  std::optional<int64_t> sequence;      // recommended; monotonic (spec §4.3)
  int64_t update_timestamp_ms = 0;      // required; epoch ms (spec §4.3)
  std::optional<std::string> mode_hint; // nullopt == null == no hint (spec §9)
  Dimension arousal;
  Dimension valence;
  Dimension cognitive_load;
  Dimension readiness;
};

// Exact field-for-field equality (doubles compared bit-for-bit); used by
// round-trip tests and consumers that dedup on content.
bool operator==(const StateVector& a, const StateVector& b);
inline bool operator!=(const StateVector& a, const StateVector& b) {
  return !(a == b);
}

// Spec §6 — the neutral / cold-start vector: every value 0.5 at confidence 0,
// no mode hint. Also the fallback under prolonged staleness (spec §7.3).
StateVector neutral(Vertical vertical, int64_t update_timestamp_ms);

} // namespace prism::psv
