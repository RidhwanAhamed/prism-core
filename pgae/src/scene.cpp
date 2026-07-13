#include "pgae/scene.h"

#include "psv/detail/json_value.h"

#include "miniaudio.h"

#include <utility>

namespace prism::pgae {

using psv::detail::JValue;

const Scene* SceneManifest::find(std::string_view id) const {
  for (const Scene& scene : scenes) {
    if (scene.id == id) {
      return &scene;
    }
  }
  return nullptr;
}

namespace {

std::optional<StemRole> role_from_string(std::string_view s) {
  for (size_t i = 0; i < kStemRoleCount; ++i) {
    if (s == kStemRoleNames[i]) {
      return static_cast<StemRole>(i);
    }
  }
  return std::nullopt;
}

const JValue* require(const JValue& obj, const char* key, JValue::Type type) {
  const JValue* v = obj.find(key);
  return v && v->type == type ? v : nullptr;
}

} // namespace

std::optional<SceneManifest> parse_scene_manifest(std::string_view json, std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<SceneManifest> {
    if (error) {
      *error = why;
    }
    return std::nullopt;
  };

  JValue root;
  std::string parse_error;
  if (!psv::detail::parse_json(json, root, parse_error)) {
    return fail(parse_error);
  }
  if (root.type != JValue::Type::Object) {
    return fail("manifest is not a JSON object");
  }

  SceneManifest manifest;

  const JValue* version = require(root, "schema_version", JValue::Type::String);
  if (!version) {
    return fail("schema_version is missing or not a string");
  }
  manifest.schema_version = version->string;

  const JValue* default_scene = require(root, "default_scene", JValue::Type::String);
  if (!default_scene) {
    return fail("default_scene is missing or not a string");
  }
  manifest.default_scene = default_scene->string;

  const JValue* scenes = require(root, "scenes", JValue::Type::Array);
  if (!scenes || scenes->array.empty()) {
    return fail("scenes is missing, not an array, or empty");
  }

  for (const JValue& scene_value : scenes->array) {
    if (scene_value.type != JValue::Type::Object) {
      return fail("scene entry is not an object");
    }
    Scene scene;
    const JValue* id = require(scene_value, "id", JValue::Type::String);
    if (!id) {
      return fail("scene.id is missing or not a string");
    }
    scene.id = id->string;
    if (const JValue* key = require(scene_value, "key", JValue::Type::String)) {
      scene.key = key->string;
    }
    const JValue* stems = require(scene_value, "stems", JValue::Type::Array);
    if (!stems || stems->array.empty()) {
      return fail("scene \"" + scene.id + "\" has no stems");
    }
    bool seen[kStemRoleCount] = {};
    for (const JValue& stem_value : stems->array) {
      if (stem_value.type != JValue::Type::Object) {
        return fail("stem entry is not an object");
      }
      const JValue* role = require(stem_value, "role", JValue::Type::String);
      const JValue* file = require(stem_value, "file", JValue::Type::String);
      if (!role || !file) {
        return fail("stem entry needs string \"role\" and \"file\"");
      }
      const auto parsed_role = role_from_string(role->string);
      if (!parsed_role) {
        return fail("unknown stem role \"" + role->string + "\"");
      }
      if (seen[static_cast<size_t>(*parsed_role)]) {
        return fail("duplicate stem role \"" + role->string + "\" in scene \"" + scene.id + "\"");
      }
      seen[static_cast<size_t>(*parsed_role)] = true;
      scene.stems.push_back({*parsed_role, file->string});
    }
    manifest.scenes.push_back(std::move(scene));
  }

  if (!manifest.find(manifest.default_scene)) {
    return fail("default_scene \"" + manifest.default_scene + "\" does not exist");
  }
  return manifest;
}

std::optional<SceneAssets> load_scene_assets(const SceneManifest& manifest,
                                             std::string_view scene_id,
                                             const std::string& manifest_dir, std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<SceneAssets> {
    if (error) {
      *error = why;
    }
    return std::nullopt;
  };

  const Scene* scene = manifest.find(scene_id);
  if (!scene) {
    return fail("scene \"" + std::string(scene_id) + "\" not in manifest");
  }

  SceneAssets assets;
  for (const SceneStem& stem : scene->stems) {
    const std::string path = manifest_dir + "/" + stem.file;

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 0); // mono, native rate
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) {
      return fail("cannot open stem " + path);
    }
    if (assets.sample_rate == 0) {
      assets.sample_rate = decoder.outputSampleRate;
    } else if (assets.sample_rate != decoder.outputSampleRate) {
      ma_decoder_uninit(&decoder);
      return fail("stem sample rates differ (" + path + ")");
    }

    std::vector<float>& frames = assets.stems[static_cast<size_t>(stem.role)];
    float chunk[4096];
    for (;;) {
      ma_uint64 frames_read = 0;
      const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk, 4096, &frames_read);
      frames.insert(frames.end(), chunk, chunk + frames_read);
      if (result != MA_SUCCESS || frames_read < 4096) {
        break;
      }
    }
    ma_decoder_uninit(&decoder);

    if (frames.empty()) {
      return fail("stem decoded to zero frames: " + path);
    }
  }
  return assets;
}

} // namespace prism::pgae
