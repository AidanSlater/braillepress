#pragma once

#include "braille.hpp"

namespace braille {

Document scanned_score(const fs::path& input, const fs::path& music_review,
                       const fs::path& output, const fs::path& text_output);

} // namespace braille
