#pragma once

#include <set>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace sdk {
  template<typename Input>
  nlohmann::json parse_audio_descriptor(Input &input) {
    using Json = nlohmann::json;
    bool duplicate = false;
    std::set<std::string> ids;
    auto json = Json::parse(input, [&](int depth, Json::parse_event_t event, Json &value) {
      // The DOM alone would silently keep the last value for a repeated key.
      if(depth == 1 && event == Json::parse_event_t::key && !duplicate)
        duplicate = !ids.insert(value.get_ref<const std::string &>()).second;
      return true;
    });
    if(!json.is_object()) throw std::runtime_error("Expected a JSON object mapping cue IDs to WAV paths.");
    if(duplicate) throw std::runtime_error("Audio cue IDs must be unique.");
    return json;
  }

  // Validate each entry as it is consumed so runtime errors retain cue order.
  inline const std::string &audio_cue_path(const std::string &id, const nlohmann::json &value) {
    if(id.empty()) throw std::runtime_error("Audio cue IDs must not be empty.");
    if(!value.is_string()) throw std::runtime_error("Expected a WAV path for cue '" + id + "'.");
    return value.get_ref<const std::string &>();
  }
}
