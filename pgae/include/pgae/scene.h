#pragma once

// Scene manifest (scenes.json) + stem loading. The Stem Production Specification owns the
// musical material; this file only says which stems make up which scene. Stems are
// preloaded and fully decoded at scene load (real-time rule 4: no disk streaming in v1 —
// loops are short by design, so the memory budget holds).

#include "pgae/mapping.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::pgae {

struct SceneStem {
  StemRole role = StemRole::Bed;
  std::string file; // relative to the manifest's directory
};

struct Scene {
  std::string id;
  std::string key; // musical key (informative; the material itself encodes it)
  std::vector<SceneStem> stems;
};

struct SceneManifest {
  std::string schema_version;
  std::string default_scene;
  std::vector<Scene> scenes;

  const Scene* find(std::string_view id) const;
};

// Strict parse + shape validation. Unknown roles, duplicate roles within a scene, or a
// default_scene that doesn't exist are errors — a bad manifest must fail at load, not
// render silence at runtime.
std::optional<SceneManifest> parse_scene_manifest(std::string_view json, std::string* error);

// Decoded, ready-to-play stems for one scene: mono f32, all at the same sample rate.
// Indexed by StemRole; a role absent from the scene has an empty buffer (renders silent).
struct SceneAssets {
  uint32_t sample_rate = 0;
  std::array<std::vector<float>, kStemRoleCount> stems{};
};

// Reads and decodes every stem in the scene via miniaudio (WAV/FLAC), downmixed to mono
// f32 at the file's native rate. All stems must share one sample rate.
std::optional<SceneAssets> load_scene_assets(const SceneManifest& manifest,
                                             std::string_view scene_id,
                                             const std::string& manifest_dir, std::string* error);

} // namespace prism::pgae
