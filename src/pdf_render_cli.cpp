#include "braille.hpp"

#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    using namespace braille;
    try {
        if (argc != 6)
            throw std::runtime_error("usage: pdf-render INPUT PAGE DPI MAX_PIXELS OUTPUT.pgm");
        int page = integer(argv[2], "page"), dpi = integer(argv[3], "dpi"), limit = integer(argv[4], "pixel limit");
        if (page < 1 || dpi < 72 || dpi > 600 || limit < 1)
            throw std::runtime_error("Expected a positive page and pixel limit, and 72 to 600 DPI");
        auto image = render_pdf_page(argv[1], page, dpi, limit);
        auto pgm = "P5\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
        pgm.append(reinterpret_cast<const char*>(image.pixels.data()), image.pixels.size());
        write(argv[5], pgm);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PDF rendering failed: " << error.what() << '\n';
        return 2;
    }
}
