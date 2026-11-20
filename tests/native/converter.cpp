#include "braille.hpp"
#include "mupdf.hpp"

#include <zip.h>
#include <leptonica/allheaders.h>
#include <iostream>
#include <functional>
#include <regex>

namespace {
using namespace braille;

void check(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

void expect_error(const std::function<void()>& test, const std::string& word) {
    try { test(); } catch (const std::exception& error) {
        check(std::string(error.what()).find(word) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("Expected error mentioning " + word);
}

void literary(const fs::path& fixtures) {
    Options options;
    for (auto suffix : {".txt", ".docx", ".pdf"}) {
        auto input = fixtures / (std::string("simple") + suffix);
        auto blocks = import_document(input, options).blocks;
        check(serialize(paginate(blocks, options)) == read(fixtures / "simple.expected.brl"), input.string());
        check(review(blocks) == "Cat\n\ndog\n\n123\n", input.string() + " review");
    }
    options.width = 12;
    options.height = 3;
    for (auto suffix : {".txt", ".docx"}) {
        auto input = fixtures / (std::string("layout") + suffix);
        check(serialize(paginate(import_document(input, options).blocks, options)) ==
            read(fixtures / "layout.expected.brl"), input.string());
    }
    check(translate("Hello, world!") == U"⠠⠓⠑⠇⠇⠕⠂⠀⠺⠕⠗⠇⠙⠖", "UEB greeting");
    check(translate("and the", "en-ueb-g2.ctb") == U"⠯⠀⠮", "UEB contracted");
    for (auto table : {"es-g1.ctb", "es.tbl"}) {
        check(translate("¿Qué? ¡Sí! 1.234,56", table) == U"⠢⠨⠟⠥⠮⠢⠀⠖⠨⠎⠌⠖⠀⠼⠁⠄⠃⠉⠙⠂⠑⠋", "Spanish NFC and numbers");
        check(translate("1.º 2.ª 10.ª", table) == U"⠼⠂⠕⠀⠼⠆⠁⠀⠼⠂⠴⠁", "Spanish ordinals");
        check(translate("25°C 9°18′27″", table) == U"⠼⠃⠑⠴⠨⠉⠀⠼⠊⠴⠼⠁⠓⠳⠼⠃⠛⠳⠳", "Spanish degree and primes");
        check(translate("≤ ≥ ≠ ≈", table) == U"⠪⠶⠀⠕⠶⠀⠘⠶⠀⠐⠶⠄", "Spanish maths signs");
        check(translate("• pan", table) == U"⠤⠀⠏⠁⠝", "Spanish bullet");
    }
    check(translate("ﬁ co\u00adop\u200b") == U"⠋⠊⠀⠉⠕⠕⠏", "Ligature and invisible formatting");
    expect_error([] { translate("Quiz 😀"); }, "GRINNING FACE");
    expect_error([] { wrap(U"⠉⠁⠞⠉⠁⠞", 5); }, "Translated word needs 6 cells");
    expect_error([&] { import_document(fixtures / "scanned.pdf", options); }, "PDF page 1 has images");
    auto source = text_document("# Cat\ncat\n\n- cat\n\t1. dog\n\nInfor-\nmation", options);
    check(std::get<Paragraph>(source[0]).kind == Paragraph::heading && std::get<Paragraph>(source[3]).rest == 8 &&
        std::get<Paragraph>(source[4]).text == "Information", "TXT headings, lists and line joining");
    options.width = 10;
    options.page_numbers = true;
    auto overflow = paginate(import_document(fixtures / "overflow.pdf", options).blocks, options);
    check(overflow.size() > 1 && overflow[0].size() == 3 && overflow[0].back() == U"⠀⠀⠀⠀⠀⠀⠀⠀⠼⠁", "Numbered Braille overflow");
}

void pdf_dots(const fs::path& output) {
    auto path = output / "dot-positions.pdf";
    preview({{U"⠁⠂⠄⠈⠐⠠⡀⢀"}}, path, 40, 25);
    PdfContext mu;
    auto doc = mu.own(mu.call([&](auto ctx) { return pdf_open_document(ctx, path.c_str()); }), pdf_drop_document);
    auto page = mu.call([&](auto ctx) { return pdf_lookup_page_obj(ctx, doc.get(), 0); });
    check(mu.call([&](auto ctx) {
        auto resources = pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources));
        return pdf_dict_get(ctx, resources, PDF_NAME(Font)) == nullptr;
    }), "Preview has no fonts");
    auto data = mu.own(mu.call([&](auto ctx) { return pdf_load_stream(ctx, pdf_dict_get(ctx, page, PDF_NAME(Contents))); }),
        fz_drop_buffer);
    unsigned char* bytes = nullptr;
    auto length = fz_buffer_storage(mu.get(), data.get(), &bytes);
    std::string commands(reinterpret_cast<char*>(bytes), length);
    std::regex dots(R"(q 1\.5 0 0 1\.5 (\d+) (-\d+) cm)");
    std::vector<std::pair<int, int>> coordinates;
    for (std::sregex_iterator it(commands.begin(), commands.end(), dots), end; it != end; ++it)
        coordinates.emplace_back(std::stoi((*it)[1]), std::stoi((*it)[2]));
    check(coordinates == std::vector<std::pair<int, int>>{
        {2, -2}, {16, -7}, {30, -12}, {49, -2}, {63, -7}, {77, -12}, {86, -17}, {105, -17}}, "Unicode dot positions");
}

void melody(const fs::path& fixtures, const fs::path& output) {
    auto original = fixtures / "melody.musicxml";
    auto expected = music_document(original);
    const auto& example = std::get<Music>(expected.back());
    // The literal BANA signs below are shared with tests/native/music.cpp.
    check(example.signature == U"⠼⠙⠲", "Four-four music signature");
    check(example.measures == Lines{U"⠼⠁⠀⠐⠹⠱⠫⠻", U"⠼⠃⠀⠐⠗⠪⠺", U"⠼⠉⠀⠨⠹⠄⠑⠫⠧", U"⠼⠙⠀⠨⠽⠣⠅"}, "Music notes and measure numbering");
    auto path = output / "melody.mxl";
    auto xml = read(original);
    std::string container = "<container><rootfiles><rootfile full-path='score.xml'/></rootfiles></container>";
    int error = 0;
    auto archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    check(archive != nullptr, "Create MXL fixture");
    for (auto [name, content] : {std::pair{"META-INF/container.xml", &container}, std::pair{"score.xml", &xml}}) {
        auto data = zip_source_buffer(archive, content->data(), content->size(), 0);
        if (!data || zip_file_add(archive, name, data, ZIP_FL_ENC_UTF_8) < 0) {
            if (data) zip_source_free(data);
            zip_discard(archive);
            throw std::runtime_error("Write MXL test fixture");
        }
    }
    int result = zip_close(archive);
    if (result) zip_discard(archive);
    check(result == 0, "Close MXL fixture");
    auto compressed = import_document(path, Options{});
    check(serialize(paginate(compressed.blocks, Options{})) == serialize(paginate(expected, Options{})), "MXL parity");
    check(review(compressed.blocks).find("C5 quarter.; D5 eighth; E5 quarter; rest quarter") != std::string::npos, "Music review");
}

void jpeg_rotation() {
    auto pix = pixCreate(24, 12, 8);
    if (!pix) throw std::runtime_error("Cannot allocate JPEG fixture");
    pixSetAll(pix);
    l_uint8* encoded = nullptr;
    std::size_t length = 0;
    auto status = pixWriteMemJpeg(&encoded, &length, pix, 90, 0);
    pixDestroy(&pix);
    std::unique_ptr<l_uint8, decltype(&lept_free)> owned(encoded, lept_free);
    check(status == 0 && length > 2, "Cannot encode source JPEG");
    std::string jpeg(reinterpret_cast<const char*>(encoded), length);
    auto unrotated = read_image(jpeg, 1000);
    check(unrotated.width == 24 && unrotated.height == 12, "Ordinary JPEG dimensions");
    // Independent EXIF APP1 segment with one little-endian orientation tag, 6.
    const unsigned char app1[]{0xff, 0xe1, 0, 34, 'E', 'x', 'i', 'f', 0, 0,
        'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 1, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0};
    jpeg.insert(2, reinterpret_cast<const char*>(app1), sizeof app1);
    auto rotated = read_image(jpeg, 1000);
    check(rotated.width == 12 && rotated.height == 24, "Camera JPEG orientation");
}

void graphics(const fs::path& examples) {
    jpeg_rotation();
    Options options;
    options.image_mode = "threshold";
    options.threshold = 128;
    Image mask{2, 4, {0, 255, 255, 0, 255, 0, 255, 0}};
    check(picture_cells(mask, 1, 1, options) == Lines{U"⠱"}, "Two by three image dot mapping");
    options.ocr = OCR::automatic;
    options.threshold.reset();
    auto chart = import_document(examples / "bar-chart.png", options);
    auto words = review(chart.blocks);
    check(words.find("Horizontal axis: Week") != std::string::npos &&
        words.find("Vertical axis: Books") != std::string::npos &&
        words.find("Ticks, left to right: 1; 2; 3; 4") != std::string::npos &&
        words.find("Ticks, bottom to top: 0; 5; 10") != std::string::npos, "Chart axis recognition");
    check(!paginate(chart.blocks, options).empty(), "Chart spatial layout");
    auto mixed = import_document(examples / "mixed-pages.pdf", options);
    check(mixed.recognized_pages == std::vector<std::variant<int, std::string>>{2},
        "Mixed PDF routes only the image-only page to OCR");
    auto readable = review(mixed.blocks);
    check(readable.starts_with("Class announcement") && readable.find("Science notes") != std::string::npos, "Mixed PDF readable text");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) return 2;
        std::string stage = argv[1];
        fs::path root = argv[2], build = argv[3];
        fs::create_directories(build);
        if (stage == "literary") literary(root / "tests/fixtures");
        else if (stage == "dots") pdf_dots(build);
        else if (stage == "music") melody(root / "tests/fixtures", build);
        else if (stage == "graphics") graphics(root / "demo/examples");
        else return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
