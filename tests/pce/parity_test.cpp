#include <gtest/gtest.h>

#include "iso8601.h"
#include "psv/detail/json_value.h"
#include "pce/pce.h"
#include "psv/psv.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// THE acceptance bar for Task 2 (build plan): replaying every golden trace's input records
// through the C++ PCE reproduces the probe's PSV trajectory within 1e-6 per dimension per
// tick. Trace format and replay procedure: the probe's docs/fixture-capture.md; the
// TypeScript reference replayer is prism-engine-probe/src/pce/replay.ts.

namespace pce = prism::pce;
namespace psv = prism::psv;
using prism::psv::detail::JValue;

namespace {

constexpr double kTolerance = 1e-6;

std::vector<JValue> read_records(const std::filesystem::path& file) {
  std::ifstream in(file);
  EXPECT_TRUE(in.good()) << file;
  std::vector<JValue> records;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    JValue record;
    std::string error;
    EXPECT_TRUE(prism::psv::detail::parse_json(line, record, error))
        << file << ": " << error << "\n"
        << line;
    records.push_back(std::move(record));
  }
  return records;
}

std::string type_of(const JValue& r) {
  const JValue* t = r.find("type");
  return t && t->type == JValue::Type::String ? t->string : "";
}

int64_t int_field(const JValue& r, const char* name) {
  const JValue* v = r.find(name);
  // number_is_integer is required: an exponent/decimal token leaves JValue::integer at 0,
  // which would silently replay a wrong instant (found by adversarial review).
  EXPECT_TRUE(v && v->type == JValue::Type::Number && v->number_is_integer)
      << "field " << name << " missing or not a plain-integer token";
  return v && v->number_is_integer ? v->integer : 0;
}

pce::Priority priority_from(const JValue& task) {
  const JValue* p = task.find("priority");
  if (p && p->type == JValue::Type::String) {
    if (p->string == "low") {
      return pce::Priority::Low;
    }
    if (p->string == "high") {
      return pce::Priority::High;
    }
  }
  return pce::Priority::Medium; // missing/unknown → medium, like the probe
}

// Compare one emission against the fixture's recorded psv record.
void expect_matches(const psv::StateVector& emitted, const JValue& record, size_t index) {
  SCOPED_TRACE("psv record #" + std::to_string(index));
  EXPECT_EQ(emitted.update_timestamp_ms, int_field(record, "t"));
  const JValue* seq = record.find("seq");
  ASSERT_TRUE(seq != nullptr);
  if (seq->type == JValue::Type::Number) {
    ASSERT_TRUE(seq->number_is_integer);
    ASSERT_TRUE(emitted.sequence.has_value());
    EXPECT_EQ(*emitted.sequence, seq->integer);
  } else {
    EXPECT_FALSE(emitted.sequence.has_value());
  }
  const JValue* mode_hint = record.find("mode_hint");
  ASSERT_TRUE(mode_hint != nullptr);
  EXPECT_EQ(mode_hint->type, JValue::Type::Null);
  EXPECT_FALSE(emitted.mode_hint.has_value());

  const JValue* state = record.find("state");
  ASSERT_TRUE(state && state->type == JValue::Type::Object);
  const std::pair<const char*, const psv::Dimension*> dims[] = {
      {"arousal", &emitted.arousal},
      {"valence", &emitted.valence},
      {"cognitive_load", &emitted.cognitive_load},
      {"readiness", &emitted.readiness},
  };
  for (const auto& [name, dim] : dims) {
    SCOPED_TRACE(name);
    const JValue* d = state->find(name);
    ASSERT_TRUE(d && d->type == JValue::Type::Object);
    const JValue* value = d->find("value");
    const JValue* confidence = d->find("confidence");
    ASSERT_TRUE(value && value->type == JValue::Type::Number);
    ASSERT_TRUE(confidence && confidence->type == JValue::Type::Number);
    EXPECT_NEAR(dim->value, value->number, kTolerance);
    EXPECT_NEAR(dim->confidence, confidence->number, kTolerance);
  }
}

void replay_fixture(const std::filesystem::path& file) {
  const std::vector<JValue> records = read_records(file);
  ASSERT_FALSE(records.empty()) << file;

  // fixture_start supplies the engine configuration and timezone the trace was captured
  // under.
  const JValue* fixture_start = nullptr;
  for (const JValue& r : records) {
    if (type_of(r) == "fixture_start") {
      fixture_start = &r;
      break;
    }
  }
  ASSERT_TRUE(fixture_start != nullptr) << file << ": no fixture_start record";
  ASSERT_EQ(int_field(*fixture_start, "fixture_version"), 1) << file;

  const JValue* engine_config = fixture_start->find("engine");
  ASSERT_TRUE(engine_config && engine_config->type == JValue::Type::Object) << file;

  pce::PceOptions options;
  options.cadence_ms = int_field(*engine_config, "cadence_ms");
  const JValue* delta = engine_config->find("significant_delta");
  ASSERT_TRUE(delta && delta->type == JValue::Type::Number) << file;
  options.significant_delta = delta->number;
  const JValue* vertical = engine_config->find("vertical");
  ASSERT_TRUE(vertical && vertical->type == JValue::Type::String) << file;
  const auto vert = psv::vertical_from_string(vertical->string);
  ASSERT_TRUE(vert.has_value()) << file;
  options.vertical = *vert;
  options.tz_offset_min = static_cast<int>(int_field(*fixture_start, "tz_offset_min"));

  pce::Pce engine(options);

  // Apply records strictly in file order (the capture writer guarantees inputs precede
  // the tick they feed). Every emission must be matched by the immediately following psv
  // record, and every psv record must have a matching emission.
  std::optional<psv::StateVector> pending;
  size_t matched = 0;
  size_t psv_records = 0;

  for (const JValue& record : records) {
    const std::string type = type_of(record);
    if (type == "input") {
      const JValue* kind = record.find("kind");
      ASSERT_TRUE(kind && kind->type == JValue::Type::String) << file;
      const int64_t t = int_field(record, "t");
      if (kind->string == "idle") {
        engine.report_idle(t, int_field(record, "idle_ms"));
      } else if (kind->string == "app_switch") {
        engine.report_app_switch(t);
      } else if (kind->string == "tasks") {
        const JValue* tasks = record.find("tasks");
        ASSERT_TRUE(tasks && tasks->type == JValue::Type::Array) << file;
        std::vector<pce::TaskDeadline> deadlines;
        for (const JValue& task : tasks->array) {
          const JValue* due = task.find("due");
          ASSERT_TRUE(due && due->type == JValue::Type::String) << file;
          const auto due_ms = prism::test::parse_iso8601_ms(due->string);
          if (!due_ms) {
            // Mirror the probe (deadline-proximity.ts): a task whose due does not parse
            // is SKIPPED, not an error — a fixture captured against a typo'd tasks.json
            // must replay the same way in both implementations.
            continue;
          }
          deadlines.push_back({*due_ms, priority_from(task)});
        }
        engine.report_task_deadlines(std::move(deadlines));
      } else {
        FAIL() << file << ": unknown input kind " << kind->string;
      }
    } else if (type == "pce_start") {
      EXPECT_FALSE(pending.has_value()) << file << ": unmatched emission before pce_start";
      pending = engine.start(int_field(record, "t"));
    } else if (type == "tick") {
      EXPECT_FALSE(pending.has_value())
          << file << ": C++ engine emitted where the probe did not (before this tick)";
      pending = engine.evaluate(int_field(record, "t"));
    } else if (type == "psv") {
      ++psv_records;
      ASSERT_TRUE(pending.has_value()) << file << ": probe emitted psv record #" << psv_records
                                       << " but the C++ engine stayed silent";
      expect_matches(*pending, record, psv_records);
      pending.reset();
      ++matched;
    }
    // session_start / fixture_start / checkpoint: no replay action.
  }

  EXPECT_FALSE(pending.has_value()) << file << ": trailing unmatched C++ emission";
  EXPECT_EQ(matched, psv_records) << file;
  EXPECT_GT(matched, 0u) << file << ": fixture contained no PSV trajectory";
}

} // namespace

TEST(GoldenTraceParity, EveryFixtureReproducesTheProbeTrajectory) {
  const std::filesystem::path golden_dir(PRISM_GOLDEN_DIR);
  ASSERT_TRUE(std::filesystem::exists(golden_dir)) << golden_dir;

  size_t fixtures = 0;
  for (const auto& entry : std::filesystem::directory_iterator(golden_dir)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("fixture-", 0) != 0 || entry.path().extension() != ".jsonl") {
      continue; // session-*.jsonl are historical PSV-only logs; not replayable
    }
    ++fixtures;
    SCOPED_TRACE(name);
    replay_fixture(entry.path());
  }
  // The suite must never silently pass because the fixtures went missing.
  EXPECT_GE(fixtures, 5u) << "expected the scripted + live smoke fixtures in tests/golden/";
}
