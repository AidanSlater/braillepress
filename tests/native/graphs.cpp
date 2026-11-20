// Graph, orientation and picture-structure tests with literal expectations.
// The tests supply hOCR from layout(), vertical labels, and picture regions
// through PictureHooks in src/picture.hpp. Shapes are drawn with equivalents of
// Pillow's Draw.c, checked by imaging_drawn_shapes_match_pillow. Images whose
// tests draw text are stored in tests/fixtures/graphs, as drawn by Pillow
// 13.0.0 (see "Test fixtures" in README.md).

#include "braille.hpp"
#include "check.hpp"
#include "imaging.hpp"
#include "picture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>

using namespace braille;
using namespace native_test;
using imaging::Bitmap;

namespace {

constexpr std::uint8_t black = 0, white = 255, gray = 128;

// ---- ImageDraw (Pillow 13.0.0 Draw.c) ---------------------------------------

void hline(Bitmap& image, int x0, int y, int x1, std::uint8_t value) {
    if (x0 <= x1)
        imaging::fill_rectangle(image, x0, y, x1, y, value);
}

void point(Bitmap& image, int x, int y, std::uint8_t value) {
    hline(image, x, y, x, value);
}

void vline(Bitmap& image, int x, int y0, int y1, std::uint8_t value) {
    for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
        point(image, x, y, value);
}

// ImageDraw.rectangle(xy, outline=..., width=...).
void outline(Bitmap& image, Box box, std::uint8_t value, int width = 1) {
    auto [x0, y0, x1, y1] = box;
    for (int i = 0; i < width; ++i) {
        hline(image, x0, y0 + i, x1, value);
        hline(image, x0, y1 - i, x1, value);
        vline(image, x1 - i, y0 + width, y1 - width + 1, value);
        vline(image, x0 + i, y0 + width, y1 - width + 1, value);
    }
}

void rectangle(Bitmap& image, Box box, std::uint8_t value) {
    imaging::fill_rectangle(image, box.left, box.top, box.right, box.bottom, value);
}

// ImageDraw.line for the horizontal and vertical segments these tests draw.
// A wide line is ImagingDrawWideLine's polygon, here a rectangle.
void line(Bitmap& image, int x0, int y0, int x1, int y1, std::uint8_t value, int width = 1) {
    if (x0 != x1 && y0 != y1)
        throw std::logic_error("Only horizontal and vertical lines are drawn natively");
    if (width <= 1) {
        if (y0 == y1)
            hline(image, std::min(x0, x1), y0, std::max(x0, x1), value);
        else
            vline(image, x0, y0, y1, value);
        return;
    }
    auto round_up = [](double f) { return static_cast<int>(f >= 0.0 ? std::floor(f + 0.5F) : -std::floor(std::fabs(f) + 0.5F)); };
    auto round_down = [](double f) { return static_cast<int>(f >= 0.0 ? std::ceil(f - 0.5F) : -std::ceil(std::fabs(f) - 0.5F)); };
    int dx = x1 - x0, dy = y1 - y0;
    double big = std::hypot(dx, dy), small = (width - 1) / 2.0;
    double ratio_max = round_up(small) / big, ratio_min = round_down(small) / big;
    int dxmin = round_down(ratio_min * dy), dxmax = round_down(ratio_max * dy);
    int dymin = round_down(ratio_min * dx), dymax = round_down(ratio_max * dx);
    int xs[] = {x0 - dxmin, x1 - dxmin, x1 + dxmax, x0 + dxmax}, ys[] = {y0 + dymax, y1 + dymax, y1 - dymin, y0 - dymin};
    imaging::fill_rectangle(image, *std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4),
                            *std::max_element(xs, xs + 4), *std::max_element(ys, ys + 4), value);
}

// ImageDraw.ellipse(xy, fill=...): Draw.c's ellipseNew quarter-ellipse walk.
struct Quarter {
    std::int32_t a, b, cx, cy, ex, ey;
    std::int64_t a2, b2, a2b2;
    bool finished;
};

void quarter_init(Quarter& s, std::int32_t a, std::int32_t b) {
    if (a < 0 || b < 0) {
        s.finished = true;
        return;
    }
    s = {a, b, a, b % 2, a % 2, b, std::int64_t(a) * a, std::int64_t(b) * b, std::int64_t(a) * a * b * b, false};
}

std::int64_t quarter_delta(const Quarter& s, std::int64_t x, std::int64_t y) {
    return std::llabs(s.a2 * y * y + s.b2 * x * x - s.a2b2);
}

bool quarter_next(Quarter& s, std::int32_t& x, std::int32_t& y) {
    if (s.finished)
        return false;
    x = s.cx;
    y = s.cy;
    if (s.cx == s.ex && s.cy == s.ey) {
        s.finished = true;
    } else {
        std::int32_t nx = s.cx, ny = s.cy + 2;
        auto ndelta = quarter_delta(s, nx, ny);
        if (nx > 1) {
            auto newdelta = quarter_delta(s, s.cx - 2, s.cy + 2);
            if (ndelta > newdelta) {
                nx = s.cx - 2;
                ny = s.cy + 2;
                ndelta = newdelta;
            }
            newdelta = quarter_delta(s, s.cx - 2, s.cy);
            if (ndelta > newdelta) {
                nx = s.cx - 2;
                ny = s.cy;
            }
        }
        s.cx = nx;
        s.cy = ny;
    }
    return true;
}

void ellipse(Bitmap& image, Box box, std::uint8_t value) {
    int a = box.right - box.left, b = box.bottom - box.top;
    if (a < 0 || b < 0)
        return;
    Quarter outer{}, inner{};
    std::int32_t py, pl, pr, leftmost = a % 2;
    quarter_init(outer, a, b);
    if (!quarter_next(outer, pr, py))
        return;
    int width = a + b;  // filled
    if (width < 1)
        return;
    quarter_init(inner, a - 2 * (width - 1), b - 2 * (width - 1));
    pl = leftmost;
    bool finished = false;
    while (!finished) {
        std::int32_t y = py, l = pl, r = pr, cx = 0, cy = 0;
        bool more;
        while ((more = quarter_next(outer, cx, cy)) && cy <= y) {}
        if (!more) {
            finished = true;
        } else {
            pr = cx;
            py = cy;
        }
        while ((more = quarter_next(inner, cx, cy)) && cy <= y)
            l = cx;
        pl = more ? cx : leftmost;
        struct Span { std::int32_t x0, y, x1; };
        std::vector<Span> spans;
        if ((l > 0 || l < r) && y > 0)
            spans.push_back({l == 0 ? 2 : l, y, r});
        if (y > 0)
            spans.push_back({-r, y, -l});
        if (l > 0 || l < r)
            spans.push_back({l == 0 ? 2 : l, -y, r});
        spans.push_back({-r, -y, -l});
        for (auto it = spans.rbegin(); it != spans.rend(); ++it)
            hline(image, box.left + (it->x0 + a) / 2, box.top + (it->y + b) / 2, box.left + (it->x1 + a) / 2, value);
    }
}

// ---- Reference test helpers ----------------------------------------------------

struct Record {
    Box box;
    std::string text;
    int angle = 0;
};

std::string bbox(Box b) {
    return "bbox " + std::to_string(b.left) + " " + std::to_string(b.top) + " " + std::to_string(b.right) + " " +
           std::to_string(b.bottom);
}

// The hOCR layout() of the original Python tests: one paragraph, line and word per record.
Hocr layout(const std::vector<Record>& records, std::pair<int, int> origin = {0, 0}, int scale = 1, int border = 0) {
    Hocr root{"", "", "", {}};
    for (auto& [box, text, angle] : records) {
        Box b{(box.left - origin.first) * scale + border, (box.top - origin.second) * scale + border,
              (box.right - origin.first) * scale + border, (box.bottom - origin.second) * scale + border};
        auto title = bbox(b);
        Hocr word{"ocrx_word", title + "; x_wconf 96", text, {}};
        Hocr line{"ocr_line", title + "; textangle " + std::to_string(angle), "", {std::move(word)}};
        root.children.push_back(Hocr{"ocr_par", title, "", {std::move(line)}});
    }
    return root;
}

std::vector<Hocr*> words_of(Hocr& node) {
    std::vector<Hocr*> result;
    if (node.kind == "ocrx_word")
        result.push_back(&node);
    for (auto& child : node.children)
        for (auto word : words_of(child))
            result.push_back(word);
    return result;
}

void set_confidence(Hocr& word, int confidence) {
    auto at = word.title.find("x_wconf 96");
    word.title.replace(at, 10, "x_wconf " + std::to_string(confidence));
}

// mock.patch: restores the default providers when the test leaves its scope.
struct Patch {
    PictureHooks saved = picture_hooks();
    ~Patch() { picture_hooks() = saved; }
};

// side_effect=[...]: each OCR call returns the next document.
auto sequence(std::vector<Hocr> documents, std::shared_ptr<std::vector<int>> calls = {}) {
    auto queue = std::make_shared<std::deque<Hocr>>(documents.begin(), documents.end());
    return [queue, calls](const Bitmap&, const std::string&, int psm) {
        if (calls)
            calls->push_back(psm);
        if (queue->empty())
            throw std::runtime_error("StopIteration: unexpected OCR call");
        auto document = std::move(queue->front());
        queue->pop_front();
        return document;
    };
}

Image picture(const Bitmap& bitmap) {
    return to_image(bitmap);
}

Image fixture(const std::string& name) {
    return read_image(read(context().root / "tests/fixtures/graphs" / name), 100000000);
}

std::string document_text(const Document& blocks) {
    return review(blocks);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::vector<std::string> texts(const std::vector<Tick>& ticks) {
    std::vector<std::string> result;
    for (auto& tick : ticks)
        result.push_back(tick.text);
    return result;
}

// assertAlmostEqual: round(a - b, places) == 0, or |a - b| <= delta.
void almost_equal(double actual, double expected, const std::string& detail, double delta = -1) {
    bool same = delta >= 0 ? std::abs(actual - expected) <= delta : std::round((actual - expected) * 1e7) == 0;
    check(same, detail + ": " + std::to_string(actual) + " != " + std::to_string(expected));
}

std::pair<Lines, std::vector<std::string>> graph_rows(const Graphic& graph, int width, int height, const std::string& mode) {
    Options options;
    options.image_mode = mode;
    std::vector<std::string> keys;
    auto rows = graph_lines(graph, {}, width, height, options, keys);
    return {rows, keys};
}

std::string render(const Document& blocks, int cells_per_line = 40, int lines_per_page = 25, bool page_numbers = false) {
    Options options;
    options.width = cells_per_line;
    options.height = lines_per_page;
    options.page_numbers = page_numbers;
    return serialize(paginate(blocks, options));
}

Cells lstrip_blank(const Cells& row) {
    auto start = row.find_first_not_of(blank);
    return start == Cells::npos ? Cells{} : row.substr(start);
}

// GraphTests.scan keyword arguments.
struct Scan {
    bool images = true;
    std::optional<std::vector<Record>> y_ticks, x_ticks;
    std::vector<Record> original, trailing;
    std::vector<Hocr> retries;
};

// GraphTests.setUp
struct GraphTest {
    Bitmap image{400, 420, 3, white};
    Record heading{{120, 15, 260, 35}, "Example", 0};
    Record y_title{{35, 140, 48, 230}, "Count", 90};
    std::vector<Record> y_ticks{{{55, 74, 68, 86}, "10", 0}, {{58, 184, 68, 196}, "5", 0}, {{58, 274, 68, 286}, "2", 0}};
    std::vector<Record> x_ticks{{{115, 328, 125, 340}, "1", 0}, {{215, 328, 225, 340}, "2", 0}, {{315, 328, 325, 340}, "3", 0}};
    Record x_title{{160, 346, 260, 357}, "Time (s)", 0};
    Box region{40, 60, 362, 324};

    GraphTest() {
        outline(image, {80, 60, 360, 320}, black);
        for (int y : {80, 190, 280})
            line(image, 75, y, 80, y, black);
        for (int x : {120, 220, 320})
            line(image, x, 320, x, 325, black);
        ellipse(image, {225, 175, 235, 185}, black);
    }

    // Full-page OCR deliberately misses ticks; isolated axis OCR must recover them.
    Document scan(const Scan& args = {}) {
        auto records = std::vector<Record>{heading, y_title, y_ticks[0]};
        records.insert(records.end(), args.original.begin(), args.original.end());
        auto x_records = args.x_ticks.value_or(x_ticks);
        x_records.push_back(x_title);
        x_records.insert(x_records.end(), args.trailing.begin(), args.trailing.end());
        std::vector<Hocr> documents{layout(records), layout(args.y_ticks.value_or(y_ticks), {24, 48}, 3, 12),
                                    layout(x_records, {60, 320}, 3, 12)};
        documents.insert(documents.end(), args.retries.begin(), args.retries.end());
        auto last = std::vector<Record>{heading};
        last.insert(last.end(), args.trailing.begin(), args.trailing.end());
        documents.push_back(layout(last));
        Patch patch;
        picture_hooks().layout = sequence(documents);
        picture_hooks().vertical_text = [](const Image&, const std::string&) { return std::vector<VerticalLabel>{}; };
        picture_hooks().regions = [this](const Image&, const Hocr&) { return std::vector<Box>{region}; };
        Options options;
        options.images = args.images;
        return scan_picture(picture(image), options);
    }

    Graphic graph(const Scan& args = {}) {
        auto blocks = scan(args);
        check(!blocks.empty() && std::holds_alternative<Graphic>(blocks[0]), "first block is a graph");
        return std::get<Graphic>(blocks[0]);
    }
};

std::vector<Record> with_texts(const std::vector<Record>& records, std::vector<std::string> values) {
    auto result = records;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i].text = values[i];
    return result;
}

} // namespace

// ---- GraphTests ------------------------------------------------------------------

TEST_CASE("test_axis_ocr_recovers_masked_ticks_with_positions_and_units") {
    GraphTest t;
    auto graph = t.graph();
    check(graph.graph(), "isinstance Graph");
    check_equal(std::make_pair(graph.image.width, graph.image.height), std::make_pair(281, 261), "pixels.size");
    check_equal(graph.caption, std::string("Example"), "caption");
    check_equal(graph.x_title, std::string("Time (s)"), "x_title");
    check_equal(graph.y_title, std::string("Count"), "y_title");
    check_equal(texts(graph.x_labels), std::vector<std::string>{"1", "2", "3"}, "x_labels");
    check_equal(texts(graph.y_labels), std::vector<std::string>{"2", "5", "10"}, "y_labels");
    almost_equal(graph.x_labels[1].position, 0.5, "x_labels[1]");
    almost_equal(graph.y_labels[1].position, 0.5, "y_labels[1]");
    check(contains(document_text({graph}), "Ticks, left to right: 1; 2; 3"), "review lists horizontal ticks");
    check(contains(document_text({graph}), "Ticks, bottom to top: 2; 5; 10"), "review lists vertical ticks");
}

TEST_CASE("test_missing_tick_is_recovered_from_the_first_pass") {
    GraphTest t;
    auto graph = t.graph({.y_ticks = std::vector<Record>(t.y_ticks.begin() + 1, t.y_ticks.end())});
    check_equal(texts(graph.y_labels), std::vector<std::string>{"2", "5", "10"}, "y_labels");
}

TEST_CASE("test_signed_decimal_scales_keep_their_source_positions") {
    GraphTest t;
    auto graph = t.graph({.y_ticks = with_texts(t.y_ticks, {"2.5", "0", "-0.5"}),
                          .x_ticks = with_texts(t.x_ticks, {"-2", "0", "3.5"})});
    check_equal(texts(graph.x_labels), std::vector<std::string>{"-2", "0", "3.5"}, "x_labels");
    check_equal(texts(graph.y_labels), std::vector<std::string>{"-0.5", "0", "2.5"}, "y_labels");
    almost_equal(graph.x_labels[1].position, 0.5, "x_labels[1]");
    almost_equal(graph.y_labels[1].position, 0.5, "y_labels[1]");
}

TEST_CASE("test_short_unlabelled_minor_ticks_do_not_require_invented_values") {
    GraphTest t;
    line(t.image, 77, 150, 80, 150, black);
    line(t.image, 170, 320, 170, 322, black);
    auto graph = t.graph();
    check_equal(texts(graph.x_labels), std::vector<std::string>{"1", "2", "3"}, "x_labels");
    check_equal(texts(graph.y_labels), std::vector<std::string>{"2", "5", "10"}, "y_labels");
}

TEST_CASE("test_an_ocr_number_marked_rotated_is_not_an_axis_title") {
    GraphTest t;
    auto graph = t.graph({.original = {{t.y_ticks[0].box, "10", 90}}});
    check_equal(graph.y_title, std::string("Count"), "y_title");
    check_equal(texts(graph.y_labels), std::vector<std::string>{"2", "5", "10"}, "y_labels");
}

TEST_CASE("test_vertical_category_and_horizontal_categories_are_all_retained") {
    GraphTest t;
    Record vertical{{115, 328, 125, 378}, "North", 270};
    auto graph = t.graph({.x_ticks = std::vector<Record>(t.x_ticks.begin() + 1, t.x_ticks.end()), .original = {vertical}});
    check_equal(texts(graph.x_labels), std::vector<std::string>{"North", "2", "3"}, "x_labels");
    almost_equal(graph.x_labels[0].position, 40.0 / 280, "x_labels[0]");
}

TEST_CASE("test_bad_numeric_tick_is_re_read_from_pixels") {
    GraphTest t;
    std::vector<Record> ticks{t.y_ticks[0], {t.y_ticks[1].box, "<)", 0}, t.y_ticks[2]};
    auto graph = t.graph({.y_ticks = ticks, .retries = {layout({{{15, 15, 40, 70}, "5", 0}})}});
    check_equal(texts(graph.y_labels), std::vector<std::string>{"2", "5", "10"}, "y_labels");
    expect_error([&] { t.graph({.y_ticks = ticks, .retries = {layout({{{15, 15, 40, 70}, "unreadable", 0}})}}); },
                 {"numeric tick could not be read"});
}

TEST_CASE("test_scaled_thick_borders_do_not_merge_every_tick_into_one") {
    GraphTest t;
    auto image = imaging::resize(t.image, 2000, 2100, imaging::Filter::nearest);
    auto scaled = [](std::vector<Record> records) {
        for (auto& record : records)
            record.box = {record.box.left * 5, record.box.top * 5, record.box.right * 5, record.box.bottom * 5};
        return records;
    };
    auto first = layout(scaled({t.heading, t.y_title, t.y_ticks[0]}));
    // The outside border edges are x=400/1804 and y=300/1604.
    auto y_band = layout(scaled(t.y_ticks), {119, 261}, 3, 12);
    auto x_records = t.x_ticks;
    x_records.push_back(t.x_title);
    auto x_band = layout(scaled(x_records), {358, 1604}, 3, 12);
    Patch patch;
    picture_hooks().layout = sequence({y_band, x_band});
    auto result = graph_picture(picture(image), {200, 300, 1810, 1620}, first, "eng");
    check(result.has_value(), "graph found");
    auto& graph = result->graph;
    check_equal(texts(graph.x_labels), std::vector<std::string>{"1", "2", "3"}, "x_labels");
    check_equal(texts(graph.y_labels), std::vector<std::string>{"2", "5", "10"}, "y_labels");
    check_equal(graph.x_title, std::string("Time (s)"), "x_title");
    check_equal(graph.y_title, std::string("Count"), "y_title");
    almost_equal(graph.x_labels[1].position, 0.5, "x_labels[1]", 0.01);
    almost_equal(graph.y_labels[1].position, 0.5, "y_labels[1]", 0.01);
}

TEST_CASE("test_unreadable_tick_fails_instead_of_inventing_or_dropping_a_value") {
    GraphTest t;
    expect_error([&] { t.scan({.y_ticks = std::vector<Record>(t.y_ticks.begin(), t.y_ticks.begin() + 2)}); },
                 {"tick marks without readable labels"});
    expect_error([&] { t.scan({.x_ticks = std::vector<Record>(t.x_ticks.begin(), t.x_ticks.begin() + 2)}); },
                 {"tick marks without readable labels"});
}

TEST_CASE("test_internal_legend_is_kept_as_text_and_erased_from_the_plot") {
    GraphTest t;
    rectangle(t.image, {205, 104, 285, 112}, black);  // the legend's print letters
    auto graph = t.graph({.original = {{{200, 100, 290, 116}, "Series A", 0}}});
    check_equal(graph.notes, std::vector<std::string>{"Series A"}, "notes");
    check(contains(document_text({graph}), "Inside the plot: Series A"), "review lists the legend");
    auto erased = imaging::crop(gray_bitmap(graph.image), 125, 44, 205, 52);
    auto [low, high] = std::minmax_element(erased.data.begin(), erased.data.end());
    check_equal(std::make_pair(int(*low), int(*high)), std::make_pair(255, 255), "legend pixels are erased");
    auto [rows, keys] = graph_rows(graph, 40, 24, "outline");
    check_equal(keys, std::vector<std::string>{"Inside the plot: Series A"}, "keys");
}

TEST_CASE("test_text_only_keeps_axis_names_and_scales") {
    GraphTest t;
    auto blocks = t.scan({.images = false});
    check(std::all_of(blocks.begin(), blocks.end(), [](auto& b) { return std::holds_alternative<Paragraph>(b); }),
          "every block is a paragraph");
    auto text = document_text(blocks);
    check(contains(text, "Horizontal axis: Time (s)"), "horizontal axis name");
    check(contains(text, "Vertical axis: Count"), "vertical axis name");
    check(contains(text, "Ticks, bottom to top: 2; 5; 10"), "vertical ticks");
}

TEST_CASE("test_prose_below_the_axis_is_not_used_as_an_axis_title") {
    GraphTest t;
    auto blocks = t.scan({.trailing = {{{100, 390, 300, 403}, "Body paragraph", 0}}});
    check_equal(std::get<Graphic>(blocks[0]).x_title, std::string("Time (s)"), "x_title");
    check(contains(document_text(blocks), "Body paragraph"), "prose is kept");
}

TEST_CASE("test_non_graph_does_not_launch_axis_ocr") {
    Bitmap photo(400, 400, 3, white);
    ellipse(photo, {20, 20, 380, 380}, gray);
    Patch patch;
    picture_hooks().layout = [](const Bitmap&, const std::string&, int) -> Hocr {
        throw std::runtime_error("Unexpected axis OCR");
    };
    check(!graph_picture(picture(photo), {0, 0, 400, 400}, layout({}), "eng"), "graph_picture returns None");
}

TEST_CASE("test_framed_non_numeric_text_is_not_classified_as_a_graph") {
    GraphTest t;
    auto y = with_texts(t.y_ticks, {"cat", "cat", "cat"});
    auto x = with_texts(t.x_ticks, {"dog", "dog", "dog"});
    auto first = layout({t.heading, t.y_title});
    Patch patch;
    picture_hooks().layout = sequence({layout(y, {24, 48}, 3, 12), layout(x, {60, 320}, 3, 12)});
    check(!graph_picture(picture(t.image), t.region, first, "eng"), "graph_picture returns None");
}

TEST_CASE("test_literal_braille_values_are_spatially_separate_from_the_plot") {
    GraphTest t;
    auto graph = t.graph();
    for (std::string mode : {"outline", "threshold", "dither"}) {
        auto [rows, keys] = graph_rows(graph, 40, 24, mode);
        check_equal(keys, std::vector<std::string>{}, mode + " keys");
        // Independent UEB number literals: 10, 5, 2; 1, 2, 3.
        std::vector<int> y_rows;
        for (Cells value : {U"⠼⠁⠚", U"⠼⠑", U"⠼⠃"})
            for (std::size_t index = 0; index < rows.size(); ++index)
                if (lstrip_blank(rows[index]).starts_with(value))
                    y_rows.push_back(static_cast<int>(index));
        check_equal(y_rows.size(), std::size_t{3}, mode + " vertical labels");
        check(std::is_sorted(y_rows.begin(), y_rows.end()), mode + " vertical labels are ordered");
        auto values = std::find_if(rows.begin(), rows.end(), [](const Cells& row) {
            return row.find(U"⠼⠁") != Cells::npos && row.find(U"⠼⠃") != Cells::npos && row.find(U"⠼⠉") != Cells::npos;
        });
        check(values != rows.end(), mode + " horizontal values row");
        check(values->find(U"⠼⠁") < values->find(U"⠼⠃"), mode + " 1 before 2");
        check(values->find(U"⠼⠃") < values->find(U"⠼⠉"), mode + " 2 before 3");
        check(std::all_of(rows.begin(), rows.end(), [](auto& row) { return row.size() <= 40; }), mode + " width");
    }
}

TEST_CASE("test_crowded_categories_have_keys_and_each_full_label_is_retained") {
    Graphic graph(picture(Bitmap(400, 300, 3, white)));
    graph.x_labels = {{0.1, "North district"}, {0.5, "Central district"}, {0.9, "South district"}};
    graph.y_labels = {{0, "0"}, {1, "10"}};
    graph.x_title = "Region";
    graph.y_title = "Count";
    auto [rows, keys] = graph_rows(graph, 40, 24, "outline");
    check_equal(keys, std::vector<std::string>{"X labels", "A: North district", "B: Central district", "C: South district"}, "keys");
    check(std::any_of(rows.begin(), rows.end(), [](const Cells& row) {
        return row.find(U"⠠⠁") != Cells::npos && row.find(U"⠠⠃") != Cells::npos && row.find(U"⠠⠉") != Cells::npos;
    }), "keyed letters below the plot");
    auto output = render({graph}, 40, 25, true);
    auto pages = split(output, '\f');
    check_equal(pages.size(), std::size_t{2}, "pages");
    for (auto& page : pages) {
        auto lines = split(page, '\n');
        lines.pop_back();  // splitlines() drops the final newline
        check_equal(lines.size(), std::size_t{25}, "lines per page");
        for (auto& line : lines)
            check(unicode(line).size() <= 40, "line width");
        for (auto c : unicode(page))
            check(c == U'\n' || (c >= U'⠀' && c <= U'⠿'), "six-dot cells only");
    }
}

TEST_CASE("test_long_vertical_categories_are_keyed") {
    Graphic graph(picture(Bitmap(300, 200, 3, white)));
    graph.x_labels = {{0, "0"}, {1, "100"}};
    graph.y_labels = {{0, "Lower region"}, {1, "Upper region"}};
    auto [rows, keys] = graph_rows(graph, 40, 24, "outline");
    check_equal(keys, std::vector<std::string>{"Y labels", "A: Lower region", "B: Upper region"}, "keys");
}

TEST_CASE("test_collisions_and_small_pages_fail_instead_of_overwriting") {
    GraphTest t;
    auto graph = t.graph();
    expect_error([&] { render({graph}, 40, 4); }, {}, "lines_per_page=4");
    expect_error([&] { render({graph}, 8, 25); }, {}, "cells_per_line=8");
    auto crowded = graph;
    crowded.y_labels = {{0.5, "1"}, {0.501, "2"}};
    expect_error([&] { render({crowded}); }, {"Vertical graph ticks collide"});
    crowded = graph;
    crowded.x_labels = {{0.5, "1"}, {0.501, "2"}};
    expect_error([&] { render({crowded}); }, {"Horizontal graph ticks collide"});
}

TEST_CASE("test_graph_pdf_and_following_text_keep_numbering_and_grid_bounds") {
    GraphTest t;
    Document blocks{t.graph(), Paragraph{"cat"}};
    auto result = render(blocks, 40, 25, true);
    auto pages = split(result, '\f');
    check_equal(pages.size(), std::size_t{2}, "pages");
    for (std::size_t i = 0; i < 2; ++i) {
        auto lines = split(pages[i], '\n');
        lines.pop_back();
        check(unicode(lines.back()).ends_with(i == 0 ? U"⠼⠁" : U"⠼⠃"), "page number " + std::to_string(i + 1));
    }
    Options options;
    options.page_numbers = true;
    auto path = context().work / "graph.pdf";
    preview(paginate(blocks, options), path, 40, 25);
    check_equal(pdf_page_count(path), 2, "PDF pages");
}

// ---- VerticalTextTests -----------------------------------------------------------

TEST_CASE("test_both_rotation_directions_map_boxes_back_to_the_source") {
    Bitmap image(200, 300, 3, white);
    auto counterclockwise = layout({{{60, 160, 170, 180}, "Units sold", 0}});
    auto clockwise = layout({{{110, 150, 220, 170}, "North", 0}});
    auto calls = std::make_shared<std::vector<int>>();
    Patch patch;
    picture_hooks().layout = sequence({counterclockwise, clockwise}, calls);
    auto labels = vertical_text(picture(image), "eng");
    check_equal(labels.size(), std::size_t{2}, "labels");
    check(labels[0].box == Box{20, 60, 40, 170} && labels[0].text == "Units sold" && labels[0].angle == 90, "counterclockwise label");
    check(labels[1].box == Box{150, 80, 170, 190} && labels[1].text == "North" && labels[1].angle == 270, "clockwise label");
    check(*calls == std::vector<int>{11, 11}, "sparse text passes");
}

TEST_CASE("test_low_confidence_and_horizontal_source_boxes_are_rejected") {
    Bitmap image(200, 300, 3, white);
    auto first = layout({{{20, 20, 40, 180}, "ordinary horizontal text", 0}, {{60, 160, 170, 180}, "wrong direction", 0}});
    set_confidence(*words_of(first).back(), 40);
    Patch patch;
    picture_hooks().layout = sequence({first, layout({})});
    check(vertical_text(picture(image), "eng").empty(), "no vertical labels");
}

TEST_CASE("test_opposite_direction_guesses_do_not_duplicate_a_label") {
    Bitmap image(200, 300, 3, white);
    auto first = layout({{{60, 160, 170, 180}, "Units sold", 0}});
    auto second = layout({{{130, 20, 240, 40}, "upside down", 0}});
    for (auto word : words_of(second))
        set_confidence(*word, 80);
    Patch patch;
    picture_hooks().layout = sequence({first, second});
    auto labels = vertical_text(picture(image), "eng");
    check(labels.size() == 1 && labels[0].text == "Units sold", "one label");
}

TEST_CASE("test_vertical_text_outside_graphs_replaces_first_pass_guesses") {
    Bitmap image(200, 300, 3, white);
    point(image, 5, 5, black);
    Box box{20, 60, 40, 170};
    auto document = layout({{box, "wrong", 0}, {{80, 20, 180, 35}, "Horizontal", 0}});
    Patch patch;
    picture_hooks().layout = [document](const Bitmap&, const std::string&, int) { return document; };
    picture_hooks().vertical_text = [box](const Image&, const std::string&) {
        return std::vector<VerticalLabel>{{box, "Vertical label", 90, 96}};
    };
    picture_hooks().regions = [](const Image&, const Hocr&) { return std::vector<Box>{}; };
    auto blocks = scan_picture(picture(image), Options{});
    check_equal(document_text(blocks), std::string("Horizontal\n\nVertical label\n"), "document_text");
}

// ---- PictureStructureTests -------------------------------------------------------

TEST_CASE("test_solid_vertical_and_horizontal_bars_are_distinguished_from_axes") {
    Bitmap image(240, 180, 1, white);
    line(image, 0, 0, 0, 179, 0, 2);
    line(image, 0, 179, 239, 179, 0, 2);
    check(!bar_chart(picture(image)), "axes alone are not bars");
    for (auto [left, top, right] : {std::tuple{25, 80, 35}, std::tuple{100, 20, 125}, std::tuple{180, 60, 205}})
        rectangle(image, {left, top, right, 179}, 100);
    check(bar_chart(picture(image)), "vertical bars");
    check(bar_chart(picture(imaging::transpose(image, imaging::Transpose::rotate_270))), "horizontal bars");
}

TEST_CASE("test_outline_keeps_border_strokes_and_six_dot_bounds") {
    Bitmap image(400, 300, 3, white);
    line(image, 0, 0, 0, 299, black, 3);
    line(image, 0, 299, 399, 299, black, 3);
    auto rows = picture_cells(picture(image), 40, 25, Options{});
    check(std::any_of(rows.begin(), rows.end(), [](auto& row) { return (row[0] - blank) & 7; }), "left border dots");
    check(std::any_of(rows.back().begin(), rows.back().end(), [](char32_t cell) { return (cell - blank) & 36; }), "bottom border dots");
    check(std::all_of(rows.begin(), rows.end(), [](auto& row) { return row.size() <= 40; }), "width");
    check(rows.size() <= 25, "height");
    for (auto& row : rows)
        for (auto cell : row)
            check(cell >= U'⠀' && cell <= U'⠿', "six-dot cells");
}

TEST_CASE("test_blank_picture_has_no_artificial_outline") {
    for (auto [width, height] : {std::pair{1, 1}, std::pair{80, 60}})
        for (auto& row : picture_cells(picture(Bitmap(width, height, 3, white)), 40, 25, Options{}))
            check(row.find_first_not_of(blank) == Cells::npos, "blank cells only");
}

// ---- ScanStructureTests ------------------------------------------------------------

TEST_CASE("test_boxed_text_and_table_grids_stay_reading_text") {
    // boxed-text.png: the page drawn by the Python test, including its text.
    auto page = fixture("boxed-text.png");
    std::vector<Record> words{{{60, 80, 150, 100}, "Remember:", 0}, {{160, 80, 260, 100}, "units", 0},
                              {{60, 230, 150, 250}, "Cell", 0}, {{38, 80, 44, 100}, "|", 0}};  // the box's left rule
    Patch patch;
    // Stand-in engine: a word is read only while its pixels remain visible on the full page.
    picture_hooks().layout = [&](const Bitmap& image, const std::string&, int) {
        std::vector<Record> visible;
        for (auto& word : words) {
            if (image.width != page.width || image.height != page.height)
                continue;
            auto crop = imaging::crop(imaging::luminance(image), word.box.left, word.box.top, word.box.right, word.box.bottom);
            if (*std::min_element(crop.data.begin(), crop.data.end()) < 128)
                visible.push_back(word);
        }
        return layout(visible);
    };
    picture_hooks().vertical_text = [](const Image&, const std::string&) { return std::vector<VerticalLabel>{}; };
    auto blocks = scan_picture(page, Options{});
    check(std::none_of(blocks.begin(), blocks.end(), [](auto& b) { return std::holds_alternative<Graphic>(b); }), "no figures");
    check_equal(document_text(blocks), std::string("Remember:\n\nunits\n\nCell\n"), "document_text");
}

TEST_CASE("test_ruled_text_is_distinguished_from_figures") {
    // ruled-*.png: each region drawn by the Python test, including its text.
    std::vector<OcrWord> words{{{40, 40, 140, 60}, 96, "text"}, {{150, 40, 260, 60}, 96, "text"}};
    auto region = [&](const std::string& name) { return framed_text(fixture(name), {0, 0, 499, 299}, words); };
    check(region("ruled-border.png"), "a border frames text");
    check(!region("ruled-bars.png"), "bars are a figure");
    check(!region("ruled-curve.png"), "a curve is a figure");
    check(!region("ruled-ellipse.png"), "an ellipse is a figure");
    for (auto& word : words)
        word.confidence = 30;
    check(!region("ruled-border.png"), "unconfident words are not framed text");
}

TEST_CASE("test_decorative_embedded_picture_is_depicted_instead_of_failing") {
    Bitmap divider(600, 6, 3, white);
    rectangle(divider, {0, 2, 599, 3}, black);
    Patch patch;
    picture_hooks().layout = [](const Bitmap&, const std::string&, int) { return layout({}); };
    picture_hooks().vertical_text = [](const Image&, const std::string&) { return std::vector<VerticalLabel>{}; };
    auto blocks = scan_picture(picture(divider), Options{}, true);
    check(blocks.size() == 1 && std::holds_alternative<Graphic>(blocks[0]), "the picture is depicted");
    Options text_only;
    text_only.images = false;
    check(scan_picture(picture(divider), text_only, true).empty(), "nothing without images");
    expect_error([&] { scan_picture(picture(divider), Options{}); }, {"OCR found no readable text"});
}

// ---- Native imaging checks ---------------------------------------------------------

TEST_CASE("imaging_drawn_shapes_match_pillow") {
    // shapes-*.png were drawn by Pillow 13.0.0 and are kept as data.
    auto same = [](const Bitmap& drawn, const std::string& name) {
        auto expected = fixture(name);
        check(drawn.width == expected.width && drawn.height == expected.height, name + " size");
        check(imaging::luminance(drawn).data == expected.pixels, name + " pixels");
    };
    GraphTest t;
    line(t.image, 77, 150, 80, 150, black);
    line(t.image, 170, 320, 170, 322, black);
    rectangle(t.image, {205, 104, 285, 112}, black);
    same(t.image, "shapes-graph.png");
    Bitmap photo(400, 400, 3, white);
    ellipse(photo, {20, 20, 380, 380}, gray);
    same(photo, "shapes-photo.png");
    Bitmap bars(240, 180, 1, white);
    line(bars, 0, 0, 0, 179, 0, 2);
    line(bars, 0, 179, 239, 179, 0, 2);
    for (auto [left, top, right] : {std::tuple{25, 80, 35}, std::tuple{100, 20, 125}, std::tuple{180, 60, 205}})
        rectangle(bars, {left, top, right, 179}, 100);
    same(bars, "shapes-bars.png");
    Bitmap strokes(400, 300, 3, white);
    line(strokes, 0, 0, 0, 299, black, 3);
    line(strokes, 0, 299, 399, 299, black, 3);
    point(strokes, 5, 5, black);
    outline(strokes, {40, 50, 300, 130}, black, 3);
    same(strokes, "shapes-strokes.png");
}

TEST_CASE("imaging_cells_match_the_reference_for_synthetic_pictures") {
    // Expected rows from the Python reference's image_cells with Pillow 13.0.0.
    Bitmap texture(16, 12, 1);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x)
            texture.row(y)[x] = static_cast<std::uint8_t>((x * 37 + y * 23) % 256);
    Bitmap shapes(20, 14, 3, white);
    imaging::fill_rectangle(shapes, 3, 2, 15, 10, 0);
    for (int y = 2; y <= 10; ++y)
        for (int x = 3; x <= 15; ++x)
            shapes.row(y)[x * 3] = 255;  // red
    imaging::fill_rectangle(shapes, 7, 5, 11, 8, 0);
    for (int y = 5; y <= 8; ++y)
        for (int x = 7; x <= 11; ++x)
            shapes.row(y)[x * 3 + 2] = 255;  // blue
    struct Case {
        const Bitmap* image;
        int width, height;
        std::string mode;
        std::optional<int> threshold;
        Lines expected;
    };
    std::vector<Case> cases{
        {&texture, 4, 2, "outline", {}, {U"⠴⠫⠾⠫", U"⠁⠉⠁⠉"}},
        {&texture, 4, 2, "threshold", {}, {U"⠋⠜⠡⠞", U"⠈⠀⠉⠀"}},
        {&texture, 4, 2, "dither", {}, {U"⠩⠜⠡⠞", U"⠉⠈⠁⠈"}},
        {&texture, 4, 2, "outline", 100, {U"⠴⠫⠾⠫", U"⠁⠈⠁⠉"}},
        {&texture, 4, 2, "threshold", 90, {U"⠁⠔⠡⠊", U"⠈⠀⠁⠀"}},
        {&texture, 4, 2, "dither", 60, {U"⠁⠔⠁⠊", U"⠈⠀⠁⠀"}},
        {&shapes, 5, 3, "outline", {}, {U"⠿⠉⠉⠹⠇", U"⠙⠛⠛⠛⠃"}},
        {&shapes, 5, 3, "threshold", {}, {U"⠀⠶⠶⠶⠀", U"⠀⠉⠉⠉⠀"}},
        {&shapes, 5, 3, "dither", {}, {U"⠐⠶⠶⠶⠀", U"⠀⠉⠁⠁⠀"}},
        {&shapes, 5, 3, "outline", 100, {U"⠀⠉⠉⠉⠀", U"⠘⠛⠛⠛⠀"}},
        {&shapes, 5, 3, "threshold", 90, {U"⠀⠶⠶⠶⠀", U"⠀⠉⠉⠉⠀"}},
        {&shapes, 5, 3, "dither", 60, {U"⠀⠦⠦⠆⠀", U"⠀⠀⠁⠈⠀"}},
    };
    for (auto& c : cases) {
        Options options;
        options.image_mode = c.mode;
        options.threshold = c.threshold;
        auto label = (c.image == &texture ? "texture " : "shapes ") + c.mode + " " +
                     (c.threshold ? std::to_string(*c.threshold) : "auto");
        check_equal(picture_cells(picture(*c.image), c.width, c.height, options), c.expected, label);
    }
}

int main(int argc, char** argv) { return native_test::run(argc, argv); }
