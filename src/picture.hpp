#pragma once

// Picture, OCR and graph internals of picture.cpp, exposed for the tests in
// tests/native/graphs.cpp. Each function keeps the name and behavior of the
// Python reference function it was ported from.

#include "braille.hpp"
#include "imaging.hpp"

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace braille {

struct Box {
    int left = 0, top = 0, right = 0, bottom = 0;
    auto operator<=>(const Box&) const = default;
};

// One hOCR element: its class and title attributes and its children. Word
// elements (class ocrx_word) also keep their text content.
struct Hocr {
    std::string kind, title, text;
    std::vector<Hocr> children;
};

Hocr parse_hocr(std::string_view xml);
Box layout_box(const Hocr& node);

struct VerticalLabel {
    Box box;
    std::string text;
    int angle = 90;
    double score = 0;
};

struct OcrWord {
    Box box;
    int confidence = 0;
    std::string text;
};

struct FoundGraph {
    Box plot;
    Graphic graph;
    std::vector<Box> erased;
};

// Test seams. When empty, layout runs Tesseract, and vertical_text and regions
// use the functions below.
struct PictureHooks {
    std::function<Hocr(const imaging::Bitmap& rgb, const std::string& language, int psm)> layout;
    std::function<std::vector<VerticalLabel>(const Image& image, const std::string& language)> vertical_text;
    std::function<std::vector<Box>(const Image& image, const Hocr& document)> regions;
};
PictureHooks& picture_hooks();

Image to_image(const imaging::Bitmap& bitmap);
imaging::Bitmap gray_bitmap(const Image& image);
imaging::Bitmap color_bitmap(const Image& image);

Hocr tesseract_layout(const imaging::Bitmap& rgb, const std::string& language, int psm = 3);
std::vector<VerticalLabel> vertical_text(const Image& image, const std::string& language);
std::vector<Box> picture_regions(const Image& image, const Hocr& document);
bool framed_text(const Image& image, Box box, const std::vector<OcrWord>& words);
std::optional<FoundGraph> graph_picture(const Image& image, Box region, const Hocr& document,
                                        const std::string& language);
bool bar_chart(const Image& image);
int otsu_threshold(const imaging::Bitmap& image);

} // namespace braille
