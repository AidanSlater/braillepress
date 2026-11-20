#include "raster.hpp"

#include <cups/raster.h>
#include <fcntl.h>
#include <unistd.h>

#include <memory>
#include <stdexcept>

namespace braille {

void read_raster(const fs::path& path, int max_pages, int max_pixels,
                 const std::function<void(int, Image)>& process_page) {
    struct File {
        int fd;
        ~File() { if (fd >= 0) close(fd); }
    } file{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.fd < 0)
        throw std::runtime_error("Cannot open raster job");
    std::unique_ptr<cups_raster_t, decltype(&cupsRasterClose)> raster(cupsRasterOpen(file.fd, CUPS_RASTER_READ), cupsRasterClose);
    constexpr auto invalid = "Invalid, truncated, unsupported, or oversized raster job";
    if (!raster)
        throw std::runtime_error(invalid);
    int number = 0;
    cups_page_header2_t h{};
    while (cupsRasterReadHeader2(raster.get(), &h)) {
        if (++number > max_pages || !h.cupsWidth || !h.cupsHeight || std::uint64_t(h.cupsWidth) * h.cupsHeight > static_cast<unsigned>(max_pixels)
            || h.cupsColorOrder != CUPS_ORDER_CHUNKED || h.cupsNumColors != 1
            || (h.cupsBitsPerPixel != 1 && h.cupsBitsPerPixel != 8) || h.cupsBitsPerPixel != h.cupsBitsPerColor
            || (h.cupsColorSpace != CUPS_CSPACE_W && h.cupsColorSpace != CUPS_CSPACE_SW && h.cupsColorSpace != CUPS_CSPACE_K)
            || h.cupsBytesPerLine < (std::uint64_t(h.cupsWidth) * h.cupsBitsPerPixel + 7) / 8 || h.cupsBytesPerLine > h.cupsWidth + 4096)
            throw std::runtime_error(invalid);
        Image image{int(h.cupsWidth), int(h.cupsHeight), std::vector<std::uint8_t>(std::size_t(h.cupsWidth) * h.cupsHeight)};
        std::vector<unsigned char> line(h.cupsBytesPerLine);
        for (unsigned y = 0; y < h.cupsHeight; ++y) {
            if (cupsRasterReadPixels(raster.get(), line.data(), h.cupsBytesPerLine) != h.cupsBytesPerLine)
                throw std::runtime_error(invalid);
            for (unsigned x = 0; x < h.cupsWidth; ++x) {
                unsigned v = h.cupsBitsPerPixel == 8 ? line[x] : ((line[x / 8] >> (7 - x % 8)) & 1) * 255;
                image.pixels[y * h.cupsWidth + x] = h.cupsColorSpace == CUPS_CSPACE_K ? 255 - v : v;
            }
        }
        process_page(number, std::move(image));
    }
    auto err = cupsRasterErrorString();
    if (!number || (err && *err))
        throw std::runtime_error(invalid);
}

}
