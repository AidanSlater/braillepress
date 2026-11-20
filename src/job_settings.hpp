#pragma once

#include "braille.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace braille {

// Service conversion settings with defaults applied; unknown keys and out-of-range values throw.
nlohmann::json job_settings(const nlohmann::json& values);

// Converter options for validated settings. ranges uses the IPP form "1-3,5".
Options job_options(const nlohmann::json& config, const std::string& ranges);

} // namespace braille
