#pragma once

#include "braille.hpp"

#include <functional>

namespace braille {

void read_raster(const fs::path& path, int max_pages, int max_pixels,
                 const std::function<void(int, Image)>& process_page);

}
