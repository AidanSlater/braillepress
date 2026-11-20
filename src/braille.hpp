#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace braille {

namespace fs = std::filesystem;
using Cells = std::u32string;
using Lines = std::vector<Cells>;
using Pages = std::vector<Lines>;
constexpr char32_t blank = U'⠀';

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // 8-bit luminance, as Pillow's convert("L")
    std::vector<std::uint8_t> color;   // optional RGB triples (white-composited) when the source has color

    int at(int x, int y) const { return pixels[y * width + x]; }
    Image crop(int left, int top, int right, int bottom) const;
};

struct Paragraph {
    enum Kind { body, list, heading, note, row, entry };
    std::string text;
    int first = 2;
    int rest = 0;
    Kind kind = body;
    int level = 1;
};

struct Tick {
    double position;
    std::string text;
};

struct Graphic {
    Image image;
    std::string caption;
    std::vector<Tick> x_labels, y_labels;
    std::string x_title, y_title;
    std::vector<std::string> notes;

    explicit Graphic(Image pixels, std::string label = {})
        : image(std::move(pixels)), caption(std::move(label)) {}
    bool graph() const;
};

struct Music {
    Cells signature;
    Lines measures;
    std::string text, tempo;
    Cells metronome;
};

using Document = std::vector<std::variant<Paragraph, Graphic, Music>>;

enum class OCR { off, automatic, force };

struct Options {
    std::string language = "en", table, ocr_language;
    int width = 40, height = 25, first_indent = 2, list_indent = 4;
    int image_width = 0, image_height = 0;
    std::optional<int> threshold;
    std::string image_mode = "outline";
    OCR ocr = OCR::off;
    bool ocr_embedded = true, render_picture_pages = false;
    bool images = true, images_only = false, page_numbers = false;
    int max_pages = 100000, max_pixels = 100000000, ocr_dpi = 300;
    std::vector<std::pair<int, int>> ranges;

    std::string translation_table() const;
    std::string recognition_language() const;
    bool selects(int page) const;
    void validate() const;
};

struct Imported {
    Document blocks;
    std::vector<std::variant<int, std::string>> recognized_pages;
};

std::string read(const fs::path& path);
void write(const fs::path& path, std::string_view bytes);
Cells unicode(std::string_view utf8);
std::string utf8(std::u32string_view text);
std::string strip(std::string_view text);
std::vector<std::string> split(std::string_view text, char delimiter);
std::string join(const std::vector<std::string>& strings, std::string_view separator);
// Quote text for messages exactly as Python's repr() does, as the Python reference did.
std::string repr(std::string_view text);
std::string extension(const fs::path& path);
int integer(std::string_view text, std::string_view label);

Cells translate(std::string text, const std::string& table = "en-ueb-g1.ctb");
Lines wrap(const Cells& cells, int width, int first = 0, int rest = 0);
Document text_document(std::string_view text, const Options& options);
Imported import_document(const fs::path& path, const Options& options);
Document word_document(const fs::path& path, const Options& options);
std::string list_number(int number, const std::string& format);
Document music_document(const fs::path& path);
Document music_document_xml(std::string_view bytes);
std::string review(const Document& document);
Pages paginate(const Document& document, const Options& options);
std::string serialize(const Pages& pages);
Pages parse_cells(std::string_view bytes, const Options& options, bool six_dots);
void preview(const Pages& pages, const fs::path& path, int width, int height);

bool image_extension(std::string_view suffix);
Image read_image(std::string_view bytes, int max_pixels);
int pdf_page_count(const fs::path& path);
Image render_pdf_page(const fs::path& path, int page, int dpi, int max_pixels);
Image resize_image(const Image& image, int width, int height);
Image transform_image(const Image& image, double degrees, bool flip_h, bool flip_v, int max_pixels);
Lines picture_cells(const Image& image, int width, int height, const Options& options);
Document scan_picture(const Image& image, const Options& options, bool embedded = false);
std::vector<std::string> describe_graph(const Graphic& graphic);
Lines graph_lines(const Graphic& graphic, const Lines& caption, int width, int height,
                  const Options& options, std::vector<std::string>& key);

} // namespace braille
