// Pictures, OCR and graphs. Each function keeps the name and behavior of the
// Python reference function it was ported from; the pixel operations are the
// Pillow ports in imaging.cpp.

#include "picture.hpp"
#include "xml.hpp"

#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <unicode/uchar.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <tuple>

namespace braille {
namespace {

using imaging::Bitmap;
using imaging::Filter;
using imaging::Lut;
using imaging::Transpose;

// Python's round(): nearest integer, ties to even.
int rounded(double value) {
    return static_cast<int>(std::nearbyint(value));
}

Box unite(Box a, Box b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

bool reading_order(Box a, Box b) {
    return std::tie(a.top, a.left) < std::tie(b.top, b.left);
}

// box_overlap: fraction of the smaller box covered by their intersection.
double box_overlap(Box first, Box second) {
    long long area = static_cast<long long>(std::max(0, std::min(first.right, second.right) - std::max(first.left, second.left))) *
                     std::max(0, std::min(first.bottom, second.bottom) - std::max(first.top, second.top));
    long long smaller = std::min(static_cast<long long>(first.right - first.left) * (first.bottom - first.top),
                                 static_cast<long long>(second.right - second.left) * (second.bottom - second.top));
    return static_cast<double>(area) / std::max(1LL, smaller);
}

// Python str predicates.
bool is_alpha(char32_t c) { return u_isalpha(c); }
bool is_alnum(char32_t c) { return u_isalpha(c) || u_getIntPropertyValue(c, UCHAR_NUMERIC_TYPE) != U_NT_NONE; }
bool is_decimal(char32_t c) { return u_charType(c) == U_DECIMAL_DIGIT_NUMBER; }
bool is_space(char32_t c) { return u_isUWhiteSpace(c) || (c >= 0x1c && c <= 0x1f); }
bool is_word(char32_t c) { return is_alnum(c) || c == U'_'; }

template <typename Predicate>
int count(const std::string& text, Predicate predicate) {
    auto chars = unicode(text);
    return static_cast<int>(std::count_if(chars.begin(), chars.end(), predicate));
}

// len(text.split())
std::size_t word_count(const std::string& text) {
    std::size_t words = 0;
    bool space = true;
    for (auto c : unicode(text)) {
        bool now = is_space(c);
        words += !now && space;
        space = now;
    }
    return words;
}

// re.fullmatch(r"[+\-−]?(?:\d+(?:[.,]\d+)?|[.,]\d+)%?", text)
bool numeric(const std::string& text) {
    auto chars = unicode(text);
    std::size_t i = 0, n = chars.size();
    auto digits = [&] {
        std::size_t start = i;
        while (i < n && is_decimal(chars[i]))
            ++i;
        return i > start;
    };
    auto fraction = [&] {
        if (i + 1 < n && (chars[i] == U'.' || chars[i] == U',') && is_decimal(chars[i + 1])) {
            ++i;
            return digits();
        }
        return false;
    };
    if (i < n && (chars[i] == U'+' || chars[i] == U'-' || chars[i] == U'−'))
        ++i;
    if (digits())
        fraction();
    else if (!fraction())
        return false;
    if (i < n && chars[i] == U'%')
        ++i;
    return i == n;
}

// re.match(r"(?:fig(?:ure)?\.?|photo(?:graph)?|plate|image)\s*\d+\b", text, re.I)
bool labeled(const std::string& text) {
    auto chars = unicode(text);
    for (std::u32string_view prefix : {U"figure.", U"figure", U"fig.", U"fig", U"photograph", U"photo", U"plate", U"image"}) {
        if (chars.size() < prefix.size())
            continue;
        bool same = true;
        for (std::size_t i = 0; i < prefix.size() && same; ++i)
            same = static_cast<char32_t>(u_tolower(chars[i])) == prefix[i];
        if (!same)
            continue;
        std::size_t i = prefix.size();
        while (i < chars.size() && is_space(chars[i]))
            ++i;
        std::size_t start = i;
        while (i < chars.size() && is_decimal(chars[i]))
            ++i;
        if (i > start && (i == chars.size() || !is_word(chars[i])))
            return true;
    }
    return false;
}

// str.strip(characters)
std::string strip_characters(const std::string& text, std::u32string_view characters) {
    auto chars = unicode(text);
    auto keep = [&](char32_t c) { return characters.find(c) == std::u32string_view::npos; };
    auto start = std::find_if(chars.begin(), chars.end(), keep);
    auto end = std::find_if(chars.rbegin(), chars.rend(), keep).base();
    return start < end ? utf8(Cells(start, end)) : std::string{};
}

std::string without_spaces(std::string text) {
    std::erase(text, ' ');
    return text;
}

template <typename Node, typename Visit>
void visit(Node& node, Visit&& f) {
    f(node);
    for (auto& child : node.children)
        visit(child, f);
}

std::optional<int> word_confidence(const Hocr& node) {
    static const std::regex pattern(R"(\bx_wconf (\d+))");
    std::smatch m;
    if (!std::regex_search(node.title, m, pattern))
        return {};
    return std::stoi(m[1]);
}

Hocr element(xmlNode* node) {
    Hocr result{xml::attribute(node, "class"), xml::attribute(node, "title"), {}, {}};
    if (result.kind == "ocrx_word")
        result.text = xml::content(node);
    for (auto child : xml::children(node))
        result.children.push_back(element(child));
    return result;
}

struct Group {
    Box box;
    std::string text;
    bool rotated;
};

// ocr_groups: keep spatially separate tick labels apart, even within one OCR line.
std::vector<Group> ocr_groups(const Hocr& document) {
    static const std::regex turned(R"(textangle (?:90|270)\b)");
    std::vector<Group> groups;
    visit(document, [&](const Hocr& line) {
        std::vector<const Hocr*> words;
        for (auto& child : line.children)
            if (child.kind == "ocrx_word")
                words.push_back(&child);
        if (words.empty())
            return;
        if (std::regex_search(line.title, turned)) {
            std::vector<std::string> texts;
            for (auto word : words)
                texts.push_back(strip(word->text));
            groups.push_back({layout_box(line), join(texts, " "), true});
            return;
        }
        std::vector<std::string> current;
        std::optional<Box> box;
        for (auto word : words) {
            auto bounds = layout_box(*word);
            if (box && bounds.left - box->right > 1.5 * std::max(bounds.bottom - bounds.top, box->bottom - box->top)) {
                groups.push_back({*box, join(current, " "), false});
                current.clear();
                box.reset();
            }
            auto text = strip(word->text);
            if (!text.empty()) {
                current.push_back(std::move(text));
                box = box ? unite(*box, bounds) : bounds;
            }
        }
        if (box)
            groups.push_back({*box, join(current, " "), false});
    });
    return groups;
}

std::vector<VerticalLabel> vertical_labels(const Bitmap& image, const std::string& language) {
    Bitmap working = image;
    imaging::thumbnail(working, 2400, 2400);
    double sx = static_cast<double>(image.width) / working.width, sy = static_cast<double>(image.height) / working.height;
    std::vector<VerticalLabel> found;
    for (int angle : {90, 270}) {
        auto rotated = imaging::transpose(working, angle == 90 ? Transpose::rotate_90 : Transpose::rotate_270);
        auto document = tesseract_layout(rotated, language, 11);
        std::vector<const Hocr*> words;
        visit(document, [&](const Hocr& node) {
            if (node.kind == "ocrx_word")
                words.push_back(&node);
        });
        for (auto& [bounds, text, _] : ocr_groups(document)) {
            auto [a, b, c, d] = bounds;
            Box box = angle == 90 ? Box{working.width - d, a, working.width - b, c}
                                  : Box{b, working.height - c, d, working.height - a};
            // Ordinary text turned on its side, axes and photo texture are not
            // evidence of a vertical label. Require a tall source text region.
            int letters = count(text, is_alnum);
            if (box.bottom - box.top < 1.2 * (box.right - box.left) || letters < 2)
                continue;
            std::vector<int> confidence;
            for (auto word : words) {
                auto [x, y, right, bottom] = layout_box(*word);
                auto score = word_confidence(*word);
                if (score && a <= x && x < right && right <= c && b <= y && y < bottom && bottom <= d)
                    confidence.push_back(*score);
            }
            if (confidence.empty())
                continue;
            double mean = static_cast<double>(std::accumulate(confidence.begin(), confidence.end(), 0LL)) / confidence.size();
            if (*std::min_element(confidence.begin(), confidence.end()) < 35 || mean < (letters < 4 ? 85 : 75))
                continue;
            box = {rounded(box.left * sx), rounded(box.top * sy), rounded(box.right * sx), rounded(box.bottom * sy)};
            found.push_back({box, text, angle, mean});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const VerticalLabel& x, const VerticalLabel& y) {
        return std::tie(x.score, x.box, x.text, x.angle) > std::tie(y.score, y.box, y.text, y.angle);
    });
    std::vector<VerticalLabel> result;
    for (auto& label : found)
        if (std::none_of(result.begin(), result.end(), [&](auto& old) { return box_overlap(label.box, old.box) > .5; }))
            result.push_back(label);
    std::stable_sort(result.begin(), result.end(), [](auto& x, auto& y) { return reading_order(x.box, y.box); });
    return result;
}

// add_vertical_text: replace overlapping first-pass guesses, without duplicating the words.
void add_vertical_text(Hocr& document, const std::vector<VerticalLabel>& labels) {
    visit(document, [&](Hocr& parent) {
        std::erase_if(parent.children, [&](const Hocr& word) {
            return word.kind == "ocrx_word" && std::any_of(labels.begin(), labels.end(), [&](auto& label) {
                return box_overlap(layout_box(word), label.box) > .5;
            });
        });
    });
    for (auto& label : labels) {
        auto b = label.box;
        auto title = "bbox " + std::to_string(b.left) + " " + std::to_string(b.top) + " " +
                     std::to_string(b.right) + " " + std::to_string(b.bottom);
        Hocr word{"ocrx_word", title + "; x_wconf " + std::to_string(rounded(label.score)), label.text, {}};
        Hocr line{"ocr_line", title + "; textangle " + std::to_string(label.angle), {}, {std::move(word)}};
        document.children.push_back(Hocr{"ocr_par", title, {}, {std::move(line)}});
    }
}

Lut threshold_lut(int cutoff, std::uint8_t below, std::uint8_t above) {
    Lut lut;
    for (int value = 0; value < 256; ++value)
        lut[value] = value < cutoff ? below : above;
    return lut;
}

std::vector<Box> regions_of(const Bitmap& gray, const Hocr& document) {
    auto mask = imaging::point(gray, threshold_lut(250, 255, 0));
    std::vector<Box> regions;
    visit(document, [&](const Hocr& node) {
        if (node.kind == "ocr_photo") {
            regions.push_back(layout_box(node));
        } else if (node.kind == "ocrx_word") {
            auto confidence = word_confidence(node);
            if (confidence && *confidence >= 50) {
                auto b = layout_box(node);
                imaging::fill_rectangle(mask, b.left - 2, b.top - 2, b.right + 2, b.bottom + 2, 0);
            }
        }
    });
    double scale = std::min({1.0, 600.0 / gray.width, 750.0 / gray.height});
    mask = imaging::resize(mask, std::max(1, rounded(gray.width * scale)), std::max(1, rounded(gray.height * scale)), Filter::box);
    Lut any = threshold_lut(1, 0, 255);
    mask = imaging::rank_filter(imaging::point(mask, any), 3, true);
    int width = mask.width, height = mask.height;
    auto& pixels = mask.data;
    std::vector<int> pending;
    for (int start = 0; start < width * height; ++start) {
        if (!pixels[start])
            continue;
        pending.push_back(start);
        pixels[start] = 0;
        int left = start % width, right = left, top = start / width, bottom = top, count = 0;
        while (!pending.empty()) {
            int position = pending.back();
            pending.pop_back();
            int y = position / width, x = position % width;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
            ++count;
            for (int neighbor : {x ? position - 1 : -1, x + 1 < width ? position + 1 : -1,
                                 y ? position - width : -1, y + 1 < height ? position + width : -1})
                if (neighbor >= 0 && pixels[neighbor]) {
                    pixels[neighbor] = 0;
                    pending.push_back(neighbor);
                }
        }
        if (right - left >= 20 && bottom - top >= 20 && count >= 50) {
            long long w = gray.width, h = gray.height;
            regions.push_back({std::max(0, static_cast<int>(static_cast<double>(left * w) / width)),
                               std::max(0, static_cast<int>(static_cast<double>(top * h) / height)),
                               static_cast<int>(std::min<double>(w, std::ceil(static_cast<double>((right + 1) * w) / width))),
                               static_cast<int>(std::min<double>(h, std::ceil(static_cast<double>((bottom + 1) * h) / height)))});
        }
    }
    // Merge touching/overlapping boxes so chart axes, bars, and flowchart arrows stay together.
    std::vector<Box> merged;
    while (!regions.empty()) {
        auto box = regions.back();
        regions.pop_back();
        auto touching = std::find_if(regions.begin(), regions.end(), [&](Box other) {
            return box.left <= other.right + 4 && other.left <= box.right + 4 &&
                   box.top <= other.bottom + 4 && other.top <= box.bottom + 4;
        });
        if (touching == regions.end())
            merged.push_back(box);
        else
            *touching = unite(*touching, box);
    }
    std::stable_sort(merged.begin(), merged.end(), reading_order);
    return merged;
}

// Maximal runs of 255 at least minimum long in each row: (row, start, end).
struct Run {
    int row, start, end;
    auto operator<=>(const Run&) const = default;
};

std::vector<Run> long_runs(const Bitmap& picture, int minimum) {
    std::vector<Run> runs;
    for (int row = 0; row < picture.height; ++row) {
        const std::uint8_t* line = picture.row(row);
        for (int x = 0; x < picture.width;) {
            if (line[x] != 255) {
                ++x;
                continue;
            }
            int start = x;
            while (x < picture.width && line[x] == 255)
                ++x;
            if (x - start >= minimum)
                runs.push_back({row, start, x});
        }
    }
    return runs;
}

bool framed(const Bitmap& gray, Box box, const std::vector<OcrWord>& words) {
    auto [left, top, right, bottom] = box;
    std::vector<std::pair<Box, int>> inside;
    for (auto& word : words) {
        double x = (word.box.left + word.box.right) / 2.0, y = (word.box.top + word.box.bottom) / 2.0;
        if (left <= x && x <= right && top <= y && y <= bottom)
            inside.emplace_back(word.box, word.confidence);
    }
    if (std::none_of(inside.begin(), inside.end(), [](auto& item) { return item.second >= 50; }))
        return false;
    auto crop = imaging::crop(gray, left, top, right, bottom);
    for (auto& [b, confidence] : inside)
        imaging::fill_rectangle(crop, b.left - left - 2, b.top - top - 2, b.right - left + 2, b.bottom - top + 2, 255);
    double scale = std::min(1.0, 1200.0 / std::max(crop.width, crop.height));
    crop = imaging::resize(crop, std::max(1, rounded(crop.width * scale)), std::max(1, rounded(crop.height * scale)), Filter::bicubic);
    auto ink = imaging::point(crop, threshold_lut(128, 255, 0));
    auto rules = [](const Bitmap& picture) {
        Bitmap runs(picture.width, picture.height, 1, 0);
        for (auto run : long_runs(picture, std::max(8, rounded(picture.width * .25))))
            std::fill(runs.row(run.row) + run.start, runs.row(run.row) + run.end, 255);
        // Filled shapes such as bars also contain long runs; keep only thin rules.
        return imaging::subtract(runs, imaging::rank_filter(imaging::rank_filter(runs, 7, false), 7, true));
    };
    auto straight = imaging::lighter(rules(ink), imaging::transpose(rules(imaging::transpose(ink, Transpose::transpose)),
                                                                    Transpose::transpose));
    long long lines = imaging::histogram(straight)[255];
    long long rest = imaging::histogram(imaging::subtract(ink, straight))[255];
    // Curves, photos, symbols and data marks leave other ink and remain figures.
    return lines >= ink.width + ink.height && rest <= std::min(.25 * lines, .02 * ink.width * ink.height);
}

struct Label {
    Box box;
    std::string text;
};

double center_x(Box b) { return (b.left + b.right) / 2.0; }
double center_y(Box b) { return (b.top + b.bottom) / 2.0; }

std::optional<FoundGraph> find_graph(const Bitmap& image, const Bitmap& gray, Box region, const Hocr& document,
                                     const std::string& language) {
    auto crop = imaging::crop(gray, region.left, region.top, region.right, region.bottom);
    // Look for intersecting long runs, rather than treating every picture as a graph.
    double scale = std::min(1.0, 1200.0 / std::max(crop.width, crop.height));
    crop = imaging::resize(crop, std::max(1, rounded(crop.width * scale)), std::max(1, rounded(crop.height * scale)), Filter::bicubic);
    auto mask = imaging::point(crop, threshold_lut(128, 255, 0));
    int width = mask.width, height = mask.height;
    auto horizontal = long_runs(mask, std::max(20, rounded(mask.width * 0.55)));
    auto vertical = long_runs(imaging::transpose(mask, Transpose::transpose), std::max(20, rounded(mask.height * 0.55)));
    if (horizontal.empty() || vertical.empty())
        return {};
    auto [base, start, end] = *std::max_element(horizontal.begin(), horizontal.end());
    std::vector<Run> candidates;
    for (auto run : vertical)
        if (std::abs(run.row - start) <= std::max(3.0, width * 0.03) && std::abs(run.end - 1 - base) <= std::max(3.0, height * 0.03))
            candidates.push_back(run);
    if (candidates.empty())
        return {};
    auto corner = *std::min_element(candidates.begin(), candidates.end());
    int left = rounded(corner.row / scale) + region.left, top = rounded(corner.start / scale) + region.top;
    int right = rounded((end - 1) / scale) + region.left, bottom = rounded(base / scale) + region.top;
    // Snap back to the outside edges at full resolution. A rounded coordinate
    // inside a thick border would make that entire border look like one tick.
    int radius = std::max(2, static_cast<int>(std::ceil(2 / scale)));
    auto edge = [&](int value, bool is_vertical, bool smallest) {
        std::vector<int> found;
        int limit = is_vertical ? image.width : image.height;
        int length = is_vertical ? bottom - top + 1 : right - left + 1;
        for (int position = std::max(0, value - radius); position < std::min(limit, value + radius + 1); ++position) {
            auto line = is_vertical ? imaging::crop(gray, position, top, position + 1, bottom + 1)
                                    : imaging::crop(gray, left, position, right + 1, position + 1);
            auto counts = imaging::histogram(line);
            long long dark = 0;
            for (int v = 0; v < 128; ++v)
                dark += counts[v];
            if (dark >= length * 0.7)
                found.push_back(position);
        }
        if (found.empty())
            return value;
        return smallest ? *std::min_element(found.begin(), found.end()) : *std::max_element(found.begin(), found.end());
    };
    std::tie(left, right) = std::make_tuple(edge(left, true, true), edge(right, true, false));
    std::tie(top, bottom) = std::make_tuple(edge(top, false, true), edge(bottom, false, false));
    int plot_width = right - left, plot_height = bottom - top;
    auto original = ocr_groups(document);
    std::vector<Label> vertical_titles, vertical_ticks;
    for (auto& [box, text, rotated] : original)
        if (rotated && box.bottom - box.top > 1.2 * (box.right - box.left) && count(text, is_alpha) &&
            left - plot_width * 0.4 <= box.left && box.left < box.right && box.right < left &&
            top <= center_y(box) && center_y(box) <= bottom)
            vertical_titles.push_back({box, text});
    for (auto& [box, text, rotated] : original)
        if (rotated && left <= center_x(box) && center_x(box) <= right && bottom < box.top && box.top < bottom + plot_height * .35)
            vertical_ticks.push_back({box, text});
    int x_padding = std::max(20, rounded(plot_width * 0.03)), y_padding = std::max(12, rounded(plot_height * 0.03));
    std::array<Box, 2> bands{
        Box{std::max(0, left - rounded(plot_width * 0.2)), std::max(0, top - y_padding), left, std::min(image.height, bottom + y_padding)},
        Box{std::max(0, left - x_padding), bottom, std::min(image.width, right + x_padding),
            std::min(image.height, bottom + std::max(40, rounded(plot_height * 0.45)))}};
    std::array<std::vector<Label>, 2> labels;
    std::array<std::vector<double>, 2> tick_marks;
    std::vector<Box> erased;
    for (auto& label : vertical_titles)
        erased.push_back(label.box);
    for (auto& label : vertical_ticks)
        erased.push_back(label.box);
    for (int axis = 0; axis < 2; ++axis) {
        Box box = bands[axis];
        auto picture = imaging::crop(image, box.left, box.top, box.right, box.bottom);
        if (picture.width < 8 || picture.height < 8)
            return {};
        for (auto [a, b, c, d] : erased)
            imaging::fill_rectangle(picture, a - box.left - 2, b - box.top - 2, c - box.left + 2, d - box.top + 2, 255);
        // Remove only strokes connected to the axis, so ticks cannot become OCR minus signs.
        std::vector<std::vector<std::pair<int, int>>> strokes;
        int reach = std::max(4, rounded((axis == 0 ? plot_width : plot_height) * 0.04));
        for (int position = axis == 0 ? top : left; position <= (axis == 0 ? bottom : right); ++position) {
            int distance = 1;
            while (distance < reach) {
                int x = axis == 0 ? left - distance : position, y = axis == 0 ? position : bottom + distance;
                if (!(0 <= x && x < image.width && 0 <= y && y < image.height) || gray.at(x, y) >= 200)
                    break;
                ++distance;
            }
            if (distance > 2) {
                if (!strokes.empty() && position - strokes.back().back().first <= 2)
                    strokes.back().emplace_back(position, distance - 1);
                else
                    strokes.push_back({{position, distance - 1}});
            }
            Box stroke = axis == 0 ? Box{left - distance, position - 1, left, position + 1}
                                   : Box{position - 1, bottom, position + 1, bottom + distance};
            imaging::fill_rectangle(picture, stroke.left - box.left, stroke.top - box.top, stroke.right - box.left,
                                    stroke.bottom - box.top, 255);
        }
        int longest = 0;
        for (auto& stroke : strokes)
            for (auto [position, length] : stroke)
                longest = std::max(longest, length);
        for (auto& stroke : strokes) {
            int most = 0;
            double sum = 0;
            for (auto [position, length] : stroke) {
                most = std::max(most, length);
                sum += position;
            }
            if (most >= longest * 0.7)
                tick_marks[axis].push_back(sum / stroke.size());
        }
        auto enlarged = imaging::expand(imaging::resize(picture, picture.width * 3, picture.height * 3, Filter::bicubic), 12, 255);
        for (auto& [bounds, found_text, rotated] : ocr_groups(tesseract_layout(enlarged, language, 6))) {
            Box place{rounded((bounds.left - 12) / 3.0) + box.left, rounded((bounds.top - 12) / 3.0) + box.top,
                      rounded((bounds.right - 12) / 3.0) + box.left, rounded((bounds.bottom - 12) / 3.0) + box.top};
            if (rotated || strip(found_text).empty())
                continue;
            // Prefer the original spelling when re-OCR only inserts spaces in a word.
            std::string text = found_text;
            for (auto& old : original)
                if (!old.rotated && without_spaces(old.text) == without_spaces(found_text) &&
                    std::abs(old.box.left - place.left) < 12 && std::abs(old.box.top - place.top) < 12) {
                    text = old.text;
                    break;
                }
            labels[axis].push_back({place, text});
        }
    }
    std::vector<Label> y_labels, x_labels;
    for (auto& label : labels[0])
        if (top - 8 <= center_y(label.box) && center_y(label.box) <= bottom + 8)
            y_labels.push_back(label);
    for (auto& label : labels[1])
        if (left - 8 <= center_x(label.box) && center_x(label.box) <= right + 8)
            x_labels.push_back(label);
    if (y_labels.empty() || (x_labels.empty() && vertical_ticks.empty()))
        return {};
    int tick_bottom = bottom;
    if (!x_labels.empty()) {
        tick_bottom = x_labels.front().box.bottom;
        for (auto& label : x_labels)
            tick_bottom = std::min(tick_bottom, label.box.bottom);
    }
    std::vector<Label> x_ticks;
    if (!vertical_ticks.empty()) {
        int first = vertical_ticks.front().box.top, thickness = vertical_ticks.front().box.right - vertical_ticks.front().box.left;
        for (auto& label : vertical_ticks) {
            first = std::min(first, label.box.top);
            thickness = std::max(thickness, label.box.right - label.box.left);
        }
        x_ticks = vertical_ticks;
        for (auto& label : x_labels)
            if (label.box.top <= first + thickness)
                x_ticks.push_back(label);
    } else {
        for (auto& label : x_labels)
            if (label.box.top <= tick_bottom)
                x_ticks.push_back(label);
    }
    tick_bottom = x_ticks.front().box.bottom;
    int text_height = std::min(x_ticks.front().box.right - x_ticks.front().box.left, x_ticks.front().box.bottom - x_ticks.front().box.top);
    for (auto& label : x_ticks) {
        tick_bottom = std::max(tick_bottom, label.box.bottom);
        text_height = std::max(text_height, std::min(label.box.right - label.box.left, label.box.bottom - label.box.top));
    }
    double title_gap = std::max(2.5 * text_height, plot_height * (vertical_ticks.empty() ? .15 : .25));
    std::vector<Label> x_titles;
    for (auto& label : x_labels)
        if (tick_bottom < label.box.top && label.box.top <= tick_bottom + title_gap)
            x_titles.push_back(label);
    auto is_number = [](const Label& label) { return numeric(label.text); };
    if (std::none_of(y_labels.begin(), y_labels.end(), is_number) && std::none_of(x_ticks.begin(), x_ticks.end(), is_number))
        return {};
    if (std::max(y_labels.size(), tick_marks[0].size()) < 2 || std::max(x_ticks.size(), tick_marks[1].size()) < 2)
        return {};
    if (x_ticks.size() < 2)
        throw std::runtime_error("Graph horizontal tick labels could not be read; supply a clearer scan");
    for (int axis = 0; axis < 2; ++axis) {
        auto& ticks = axis == 0 ? y_labels : x_ticks;
        auto coordinate = [axis](Box b) { return axis == 0 ? center_y(b) : center_x(b); };
        double tolerance = std::max(4.0, (axis == 0 ? plot_height : plot_width) * 0.025);
        // Retain a first-pass word if the isolated OCR pass misses its tick.
        for (double mark : tick_marks[axis]) {
            if (std::any_of(ticks.begin(), ticks.end(), [&](auto& label) { return std::abs(coordinate(label.box) - mark) <= tolerance; }))
                continue;
            std::vector<Label> extra;
            for (auto& [box, text, rotated] : original)
                if (!rotated && std::abs(coordinate(box) - mark) <= tolerance &&
                    (axis == 0 ? bands[0].left <= box.left && box.left < box.right && box.right <= bands[0].right
                               : bottom < box.top && box.top <= tick_bottom))
                    extra.push_back({box, text});
            if (extra.size() != 1)
                throw std::runtime_error("Graph has tick marks without readable labels; supply a clearer scan or check the source");
            ticks.push_back(extra.front());
        }
        auto numbers = std::count_if(ticks.begin(), ticks.end(), is_number);
        if (numbers >= 2 && numbers >= ticks.size() / 2.0)
            for (auto& [box, text] : ticks) {
                if (numeric(text) || count(text, is_alpha) >= 2)
                    continue;
                // Read a suspicious numeric tick as one word, without adjacent
                // axis strokes. Never fill it from the surrounding sequence.
                auto picture = imaging::crop(image, box.left, box.top, box.right, box.bottom);
                double factor = std::max(1.0, std::min(4.0, 80.0 / picture.height));
                picture = imaging::expand(imaging::resize(picture, std::max(1, rounded(picture.width * factor)),
                                                          std::max(1, rounded(picture.height * factor)), Filter::bicubic), 15, 255);
                auto readings = ocr_groups(tesseract_layout(picture, language, 8));
                if (readings.size() != 1 || !numeric(readings.front().text))
                    throw std::runtime_error("Graph numeric tick could not be read; supply a clearer scan or check the source");
                text = readings.front().text;
            }
    }
    std::vector<Label> inside;
    for (auto& [box, text, rotated] : original)
        if (count(text, is_alnum) && left + 4 < box.left && box.left < box.right && box.right < right - 4 &&
            top + 4 < box.top && box.top < box.bottom && box.bottom < bottom - 4)
            inside.push_back({box, text});
    std::stable_sort(inside.begin(), inside.end(), [](auto& a, auto& b) { return reading_order(a.box, b.box); });
    auto plot = imaging::crop(image, left, top, right + 1, bottom + 1);
    // Legends and data labels are kept as text; their print letters would only be dot noise.
    for (auto& [b, text] : inside)
        imaging::fill_rectangle(plot, b.left - left - 2, b.top - top - 2, b.right - left + 2, b.bottom - top + 2, 255);
    for (auto* group : {&y_labels, &x_ticks, &x_titles})
        for (auto& label : *group)
            erased.push_back(label.box);
    Graphic graph(to_image(plot));
    for (auto& label : inside)
        graph.notes.push_back(label.text);
    for (auto& [box, text] : x_ticks)
        graph.x_labels.push_back({std::clamp((center_x(box) - left) / plot_width, 0.0, 1.0), text});
    for (auto& [box, text] : y_labels)
        graph.y_labels.push_back({std::clamp(1 - (center_y(box) - top) / plot_height, 0.0, 1.0), text});
    auto tick_order = [](const Tick& a, const Tick& b) { return std::tie(a.position, a.text) < std::tie(b.position, b.text); };
    std::stable_sort(graph.x_labels.begin(), graph.x_labels.end(), tick_order);
    std::stable_sort(graph.y_labels.begin(), graph.y_labels.end(), tick_order);
    std::stable_sort(x_titles.begin(), x_titles.end(), [](auto& a, auto& b) { return reading_order(a.box, b.box); });
    std::stable_sort(vertical_titles.begin(), vertical_titles.end(), [](auto& a, auto& b) {
        return std::tie(a.box, a.text) < std::tie(b.box, b.text);
    });
    std::vector<std::string> words;
    for (auto& label : x_titles)
        words.push_back(label.text);
    graph.x_title = join(words, " ");
    words.clear();
    for (auto& label : vertical_titles)
        words.push_back(label.text);
    graph.y_title = join(words, " ");
    return FoundGraph{{left, top, right + 1, bottom + 1}, std::move(graph), std::move(erased)};
}

int otsu(const Bitmap& image) {
    auto histogram = imaging::histogram(image);
    long long total = 0, weighted = 0;
    for (int value = 0; value < 256; ++value) {
        total += histogram[value];
        weighted += value * histogram[value];
    }
    long long background = 0, subtotal = 0;
    double best = -1;
    int threshold = 128;
    for (int value = 0; value < 255; ++value) {
        background += histogram[value];
        subtotal += value * histogram[value];
        long long foreground = total - background;
        if (!background || !foreground)
            continue;
        double difference = static_cast<double>(subtotal) / background - static_cast<double>(weighted - subtotal) / foreground;
        double score = static_cast<double>(background * foreground) * difference * difference;
        if (score > best) {
            best = score;
            threshold = value + 1;
        }
    }
    return threshold;
}

// picture_outline: contours above dot resolution, retaining connected weaker edges.
Bitmap picture_outline(const Bitmap& source, int width, int height, std::optional<int> threshold) {
    auto image = imaging::expand(imaging::resize(source, width * 4, height * 4, Filter::lanczos), 2, 255);
    image = imaging::find_edges(imaging::gaussian_blur(image, .7f));
    // Padding avoids FIND_EDGES' unchanged outer pixels without deleting real
    // strokes on the source border, such as a graph's left and bottom axes.
    image = imaging::autocontrast(imaging::crop(image, 2, 2, image.width - 2, image.height - 2));
    int high = threshold ? 256 - *threshold : std::max(16, otsu(image));
    int low = threshold ? high : std::max(6, rounded(high * .35));
    const auto& edges = image.data;
    std::vector<std::uint8_t> weak(edges.size()), kept(edges.size());
    std::vector<int> pending;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        weak[i] = edges[i] >= low;
        if (edges[i] >= high)
            pending.push_back(static_cast<int>(i));
    }
    for (int index : pending) {
        weak[index] = 0;
        kept[index] = 255;
    }
    // Hysteresis retains a faint part of a contour when it joins a strong edge.
    while (!pending.empty()) {
        int index = pending.back();
        pending.pop_back();
        int y = index / image.width, x = index % image.width;
        for (int row = std::max(0, y - 1); row < std::min(image.height, y + 2); ++row)
            for (int column = std::max(0, x - 1); column < std::min(image.width, x + 2); ++column) {
                int neighbor = row * image.width + column;
                if (weak[neighbor]) {
                    weak[neighbor] = 0;
                    kept[neighbor] = 255;
                    pending.push_back(neighbor);
                }
            }
    }
    image.data = std::move(kept);
    // Preserve a small contour crossing a dot's footprint during downsampling.
    image = imaging::resize(imaging::rank_filter(image, 3, true), width, height, Filter::box);
    Lut lut;
    for (int value = 0; value < 256; ++value)
        lut[value] = value >= 48 ? 0 : 255;
    return imaging::point(image, lut);
}

bool bars(const Bitmap& image) {
    auto gray = image;
    imaging::thumbnail(gray, 600, 600);
    auto mask = imaging::point(gray, threshold_lut(otsu(gray), 255, 0));
    for (const auto& picture : {mask, imaging::transpose(mask, Transpose::rotate_90)}) {
        int width = picture.width, height = picture.height;
        int inset_x = std::max(2, rounded(width * .015)), inset_y = std::max(2, rounded(height * .015));
        std::vector<int> lengths;
        for (int x = inset_x; x < width - inset_x; ++x) {
            int y = height - 1 - inset_y, bottom = y;
            while (y >= 0 && picture.at(x, y))
                --y;
            lengths.push_back(bottom - y);
        }
        int found = 0;
        std::optional<std::size_t> start;
        for (std::size_t x = 0; x <= lengths.size(); ++x) {
            int length = x < lengths.size() ? lengths[x] : 0;
            if (length >= height * .06 && !start) {
                start = x;
            } else if (length < height * .06 && start) {
                auto [low, high] = std::minmax_element(lengths.begin() + *start, lengths.begin() + x);
                double size = static_cast<double>(x - *start);
                if (!(std::max(2.0, width * .007) <= size && size <= width * .35) || *high - *low > std::max(3.0, height * .015)) {
                    found = 0;
                    break;
                }
                ++found;
                start.reset();
            }
        }
        if (found >= 2)
            return true;
    }
    return false;
}

Image crop_image(const Image& image, Box box) {
    return to_image(imaging::crop(image.color.empty() ? gray_bitmap(image) : color_bitmap(image),
                                  box.left, box.top, box.right, box.bottom));
}

} // namespace

PictureHooks& picture_hooks() {
    static PictureHooks hooks;
    return hooks;
}

Image to_image(const Bitmap& bitmap) {
    Image image{bitmap.width, bitmap.height, {}, {}};
    if (bitmap.bands == 1) {
        image.pixels = bitmap.data;
        return image;
    }
    image.pixels = imaging::luminance(bitmap).data;
    for (std::size_t i = 0; i < bitmap.data.size(); i += 3)
        if (bitmap.data[i] != bitmap.data[i + 1] || bitmap.data[i] != bitmap.data[i + 2]) {
            image.color = bitmap.data;
            break;
        }
    return image;
}

Bitmap gray_bitmap(const Image& image) {
    Bitmap bitmap;
    bitmap.width = image.width;
    bitmap.height = image.height;
    bitmap.data = image.pixels;
    return bitmap;
}

Bitmap color_bitmap(const Image& image) {
    if (image.color.empty())
        return imaging::rgb(gray_bitmap(image));
    Bitmap bitmap;
    bitmap.width = image.width;
    bitmap.height = image.height;
    bitmap.bands = 3;
    bitmap.data = image.color;
    return bitmap;
}

Box layout_box(const Hocr& node) {
    static const std::regex bbox(R"(\bbbox (\d+) (\d+) (\d+) (\d+))");
    std::smatch m;
    if (!std::regex_search(node.title, m, bbox))
        throw std::runtime_error("OCR layout has a region without valid coordinates");
    return {std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]), std::stoi(m[4])};
}

Hocr parse_hocr(std::string_view text) {
    auto tree = xml::parse(text, "OCR layout");
    return element(xmlDocGetRootElement(tree.get()));
}

// tesseract_layout: words and their boxes from local Tesseract hOCR. The
// Python reference sent the RGB picture to the tesseract command as PPM.
Hocr tesseract_layout(const Bitmap& image, const std::string& language, int psm) {
    auto rgb = imaging::rgb(image);
    if (auto& hooks = picture_hooks(); hooks.layout)
        return hooks.layout(rgb, language, psm);
    tesseract::TessBaseAPI api;
    // The Python reference captured the engine's diagnostics ("Estimating
    // resolution as ...") with its subprocess output instead of printing them.
    api.SetVariable("debug_file", "/dev/null");
    if (api.Init(nullptr, language.c_str()) != 0)
        throw std::runtime_error("Tesseract OCR failed to load language " + language + "; install its trained data");
    api.SetPageSegMode(static_cast<tesseract::PageSegMode>(psm));
    api.SetImage(rgb.data.data(), rgb.width, rgb.height, 3, rgb.width * 3);
    tesseract::ETEXT_DESC monitor;
    monitor.set_deadline_msecs(60000);
    if (api.Recognize(&monitor) != 0 || monitor.deadline_exceeded())
        throw std::runtime_error("Tesseract OCR failed or timed out");
    std::unique_ptr<char[]> hocr(api.GetHOCRText(0));
    if (!hocr)
        throw std::runtime_error("Tesseract OCR did not return layout data");
    return parse_hocr(hocr.get());
}

std::vector<VerticalLabel> vertical_text(const Image& image, const std::string& language) {
    if (auto& hooks = picture_hooks(); hooks.vertical_text)
        return hooks.vertical_text(image, language);
    return vertical_labels(color_bitmap(image), language);
}

std::vector<Box> picture_regions(const Image& image, const Hocr& document) {
    if (auto& hooks = picture_hooks(); hooks.regions)
        return hooks.regions(image, document);
    return regions_of(gray_bitmap(image), document);
}

bool framed_text(const Image& image, Box box, const std::vector<OcrWord>& words) {
    return framed(gray_bitmap(image), box, words);
}

std::optional<FoundGraph> graph_picture(const Image& image, Box region, const Hocr& document, const std::string& language) {
    return find_graph(color_bitmap(image), gray_bitmap(image), region, document, language);
}

bool bar_chart(const Image& image) {
    return bars(gray_bitmap(image));
}

int otsu_threshold(const Bitmap& image) {
    return otsu(image);
}

Image Image::crop(int left, int top, int right, int bottom) const {
    if (right <= left || bottom <= top)
        throw std::runtime_error("Picture crop leaves no pixels");
    Image result{right - left, bottom - top, std::vector<std::uint8_t>(static_cast<std::size_t>(right - left) * (bottom - top), 255), {}};
    if (!color.empty())
        result.color.assign(result.pixels.size() * 3, 255);
    for (int y = std::max(0, top); y < std::min(height, bottom); ++y) {
        int x = std::max(0, left), n = std::min(width, right) - x;
        if (n <= 0)
            continue;
        std::copy_n(pixels.begin() + y * width + x, n, result.pixels.begin() + (y - top) * result.width + x - left);
        if (!color.empty())
            std::copy_n(color.begin() + (y * width + x) * 3, n * 3,
                        result.color.begin() + ((y - top) * result.width + x - left) * 3);
    }
    return result;
}

bool image_extension(std::string_view suffix) {
    static const std::set<std::string_view> allowed{".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".jp2"};
    return allowed.contains(suffix);
}

Image read_image(std::string_view bytes, int max_pixels) {
    return to_image(imaging::decode_picture(bytes, max_pixels));
}

// Used by music recognition, which has no reference pixel pipeline.
Image resize_image(const Image& image, int width, int height) {
    if (width == image.width && height == image.height)
        return Image{image.width, image.height, image.pixels, {}};
    // Lanczos with a wider source footprint for reductions.
    auto horizontal = [](const Image& old, int w, bool transpose) {
        int old_width = transpose ? old.height : old.width;
        int rows = transpose ? old.width : old.height;
        Image scaled{transpose ? rows : w, transpose ? w : rows, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * rows, 255), {}};
        double step = double(old_width) / w, spread = std::max(1., step);
        auto lanczos = [](double d) {
            if (std::abs(d) < 1e-10) return 1.;
            if (std::abs(d) >= 3) return 0.;
            double x = std::numbers::pi * d;
            return std::sin(x) / x * std::sin(x / 3) / (x / 3);
        };
        for (int x = 0; x < w; ++x) {
            double center = (x + .5) * step - .5;
            int lo = std::max(0, static_cast<int>(std::ceil(center - spread * 3)));
            int hi = std::min(old_width - 1, static_cast<int>(std::floor(center + spread * 3)));
            std::vector<double> coefficients;
            double sum = 0;
            for (int i = lo; i <= hi; ++i) {
                auto value = lanczos((i - center) / spread);
                coefficients.push_back(value);
                sum += value;
            }
            for (int y = 0; y < rows; ++y) {
                double pixel = 0;
                for (int i = lo; i <= hi; ++i)
                    pixel += coefficients[i - lo] * (transpose ? old.at(y, i) : old.at(i, y));
                scaled.pixels[transpose ? x * rows + y : y * w + x] = std::clamp(rounded(pixel / sum), 0, 255);
            }
        }
        return scaled;
    };
    auto across = width == image.width ? Image{image.width, image.height, image.pixels, {}} : horizontal(image, width, false);
    return height == across.height ? std::move(across) : horizontal(across, height, true);
}

Image transform_image(const Image& image, double angle, bool flip_h, bool flip_v, int max_pixels) {
    double radians = std::remainder(angle, 360.) * std::numbers::pi / 180;
    double c = std::cos(radians), s = std::sin(radians);
    int w = std::ceil(std::abs(c) * image.width + std::abs(s) * image.height - 1e-8);
    int h = std::ceil(std::abs(s) * image.width + std::abs(c) * image.height - 1e-8);
    if (std::int64_t(w) * h > max_pixels)
        throw std::runtime_error("Rotated picture exceeds the pixel limit");
    Image result{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h, 255), {}};
    if (!image.color.empty())
        result.color.assign(result.pixels.size() * 3, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double a = x - (w - 1) * .5, b = y - (h - 1) * .5;
            int col = rounded((c * a + s * b) * (flip_h ? -1 : 1) + (image.width - 1) * .5);
            int row = rounded((-s * a + c * b) * (flip_v ? -1 : 1) + (image.height - 1) * .5);
            if (col >= 0 && col < image.width && row >= 0 && row < image.height) {
                result.pixels[y * w + x] = image.at(col, row);
                if (!image.color.empty())
                    std::copy_n(image.color.begin() + (row * image.width + col) * 3, 3, result.color.begin() + (y * w + x) * 3);
            }
        }
    return result;
}

// image_cells: fit a picture to a six-dot cell grid; return spatial rows, never words.
Lines picture_cells(const Image& source, int width, int height, const Options& options) {
    auto image = imaging::autocontrast(gray_bitmap(source));
    // Match the preview's 14 x 28 point cells, sampled at 2 x 3 dots per cell.
    double scale = std::min(static_cast<double>(width * 2) / image.width, height * 3 / (image.height * 0.75));
    int w = std::max(1, rounded(image.width * scale)), h = std::max(1, rounded(image.height * scale * 0.75));
    if (std::int64_t(w) * h > 4'000'000)
        throw std::runtime_error("Picture dot grid exceeds the limit");
    if (options.image_mode == "outline")
        image = picture_outline(image, w, h, options.threshold);
    else
        image = imaging::resize(image, w, h, Filter::lanczos);
    if (options.image_mode == "dither") {
        if (options.threshold) {
            // Change midtones without adding texture to pure white backgrounds.
            Lut levels;
            for (int value = 0; value < 256; ++value)
                levels[value] = value == 0 || value == 255 ? value : std::clamp(value + 128 - *options.threshold, 0, 255);
            image = imaging::point(image, levels);
        }
        image = imaging::floyd_steinberg(image);
    } else if (options.image_mode != "outline") {
        image = imaging::point(image, threshold_lut(options.threshold.value_or(otsu(image)), 0, 255));
    }
    Lines rows;
    for (int top = 0; top < h; top += 3) {
        Cells row;
        for (int left = 0; left < w; left += 2) {
            unsigned mask = 0;
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 3; ++dy)
                    if (left + dx < w && top + dy < h && !image.at(left + dx, top + dy))
                        mask |= 1u << (dx * 3 + dy);
            row += blank + mask;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::string> describe_graph(const Graphic& graphic) {
    if (!graphic.graph())
        return {};
    auto labels = [](const std::vector<Tick>& ticks) {
        std::vector<std::string> names;
        for (auto& tick : ticks) names.push_back(tick.text);
        return join(names, "; ");
    };
    std::vector<std::string> result{
        "Horizontal axis" + (graphic.x_title.empty() ? "" : ": " + graphic.x_title),
        "Ticks, left to right: " + labels(graphic.x_labels),
        "Vertical axis" + (graphic.y_title.empty() ? "" : ": " + graphic.y_title),
        "Ticks, bottom to top: " + labels(graphic.y_labels)};
    if (!graphic.notes.empty())
        result.push_back("Inside the plot: " + join(graphic.notes, "; "));
    return result;
}

Lines graph_lines(const Graphic& graphic, const Lines& caption, int width, int height,
                  const Options& options, std::vector<std::string>& keys) {
    auto table = options.translation_table();
    auto heading = caption;
    auto y_title = wrap(translate("Y: " + (graphic.y_title.empty() ? "vertical axis" : graphic.y_title), table), width);
    auto footer = wrap(translate("X: " + (graphic.x_title.empty() ? "horizontal axis" : graphic.x_title), table), width);
    heading.emplace_back();
    heading.insert(heading.end(), y_title.begin(), y_title.end());
    heading.emplace_back();
    int available = height - static_cast<int>(heading.size() + footer.size()) - 3;
    if (available < 2)
        throw std::runtime_error("Graph labels need more space; increase --lines-per-page");
    using Labels = std::vector<std::pair<double, Cells>>;
    auto labels = [&](const std::vector<Tick>& ticks, std::string axis = {}) {
        Labels result;
        if (!axis.empty()) {
            if (ticks.size() > 26)
                throw std::runtime_error("Graph label keys support at most 26 ticks per axis");
            keys.push_back(axis + " labels");
        }
        for (auto& tick : ticks) {
            auto name = axis.empty() ? tick.text : std::string(1, 'A' + result.size());
            if (!axis.empty()) keys.push_back(name + ": " + tick.text);
            result.emplace_back(tick.position, translate(name, table));
        }
        return result;
    };
    auto longest = [](const Labels& names) {
        int result = 0;
        for (auto& [p, text] : names) result = std::max(result, static_cast<int>(text.size()));
        return result;
    };
    auto y = labels(graphic.y_labels), x = labels(graphic.x_labels);
    if (longest(y) > std::max(4, width / 4))
        y = labels(graphic.y_labels, "Y");
    int margin = longest(y) + 2, right_margin = std::clamp(longest(x) / 2, 1, 4);
    if (width - margin - right_margin < 4)
        throw std::runtime_error("Graph labels need more space; increase --cells-per-line");
    // Bar interiors carry information. Keep their source fill rather than
    // reducing a solid bar to two fragile outline strokes in the default mode.
    Options plot_options = options;
    if (options.image_mode == "outline" && bar_chart(graphic.image))
        plot_options.image_mode = "threshold";
    auto plot = picture_cells(graphic.image,
        std::min(width - margin - right_margin, options.image_width ? options.image_width : width),
        std::min(available, options.image_height ? options.image_height : available), plot_options);
    if (!graphic.notes.empty()) keys.push_back("Inside the plot: " + join(graphic.notes, "; "));
    int rows = plot.size(), columns = plot.front().size();
    auto position_x = [&](double position) { return margin + rounded(position * (columns - 1)); };
    auto fits = [&](const Labels& names) {
        int after = 0;
        for (auto& [p, text] : names) {
            int start = position_x(p) - static_cast<int>(text.size()) / 2;
            int end = start + static_cast<int>(text.size());
            if (start < after || end > width)
                return false;
            after = end + 1;
        }
        return true;
    };
    if (!fits(x)) {
        x = labels(graphic.x_labels, "X");
        if (!fits(x))
            throw std::runtime_error("Horizontal graph ticks collide; increase --cells-per-line and --lines-per-page");
    }
    for (auto& row : plot) {
        row.insert(0, margin, blank);
        row.resize(width, blank);
        row[margin] |= 7;
    }
    for (int col = margin; col < margin + columns; ++col)
        plot.back()[col] |= 36;
    std::set<int> occupied;
    for (auto& [position, text] : y) {
        int dot = rounded((1 - position) * (rows * 3 - 1)), row = dot / 3, dy = dot % 3;
        if (!occupied.insert(row).second)
            throw std::runtime_error("Vertical graph ticks collide; increase --lines-per-page");
        plot[row].replace(margin - 2 - text.size(), text.size(), text);
        plot[row][margin - 1] = blank + (1 << dy) + (1 << (dy + 3));
    }
    Cells guides(width, blank), values(width, blank);
    for (auto& [position, text] : x) {
        int at = position_x(position);
        guides[at] = U'⠇';
        values.replace(at - text.size() / 2, text.size(), text);
    }
    plot.push_back(std::move(guides));
    plot.push_back(std::move(values));
    for (auto& line : plot) {
        auto end = line.find_last_not_of(blank);
        line.resize(end == Cells::npos ? 0 : end + 1);
        heading.push_back(std::move(line));
    }
    heading.emplace_back();
    heading.insert(heading.end(), footer.begin(), footer.end());
    return heading;
}

// scan_picture: separate a clean scanned page into reading text, captions, and figures.
// An embedded picture (picture=true) without text or a distinct figure is depicted whole.
Document scan_picture(const Image& image, const Options& options, bool picture) {
    if (std::all_of(image.pixels.begin(), image.pixels.end(), [](int value) { return value == 255; }))
        return {};
    auto language = options.recognition_language();
    auto rgb = color_bitmap(image);
    auto gray = gray_bitmap(image);
    auto document = tesseract_layout(rgb, language);
    auto vertical = vertical_text(image, language);
    add_vertical_text(document, vertical);
    auto regions = picture_regions(image, document);
    std::vector<OcrWord> words;
    visit(document, [&](const Hocr& node) {
        if (node.kind == "ocrx_word")
            words.push_back({layout_box(node), word_confidence(node).value_or(0), strip(node.text)});
    });
    struct Figure {
        Box box;
        Graphic graphic;
        bool graph;
    };
    std::vector<Figure> figures;
    std::vector<Box> masks, framed_boxes;
    for (auto box : regions) {
        if (auto graph = find_graph(rgb, gray, box, document, language)) {
            figures.push_back({graph->plot, std::move(graph->graph), true});
            masks.push_back(graph->plot);
            masks.insert(masks.end(), graph->erased.begin(), graph->erased.end());
        } else if (framed(gray, box, words)) {
            // Boxed text and table grids stay readable text; their rules are omitted.
            framed_boxes.push_back(box);
        } else {
            figures.push_back({box, Graphic(crop_image(image, box)), false});
            masks.push_back(box);
        }
    }
    // Recognize text again with figures masked, preventing photo texture and
    // plot lines from becoming spurious words or changing paragraph boundaries.
    if (!regions.empty()) {
        auto text_image = rgb;
        for (auto* boxes : {&masks, &framed_boxes})
            for (auto [left, top, right, bottom] : *boxes)
                imaging::fill_rectangle(text_image, left - 2, top - 2, right + 2, bottom + 2, 255);
        for (auto [left, top, right, bottom] : framed_boxes)
            for (auto& [box, confidence, word] : words) {
                // Restore words, not rules that OCR misread as "|" or "_".
                double x = center_x(box), y = center_y(box);
                if (left <= x && x <= right && top <= y && y <= bottom && !strip_characters(word, U"|¦_—–-~=[]!").empty()) {
                    Box piece{std::max(left, box.left - 2), std::max(top, box.top - 2),
                              std::min(right, box.right + 2), std::min(bottom, box.bottom + 2)};
                    imaging::paste(text_image, imaging::crop(rgb, piece.left, piece.top, piece.right, piece.bottom), piece.left, piece.top);
                }
            }
        document = tesseract_layout(text_image, language);
        std::vector<VerticalLabel> outside;
        for (auto& label : vertical)
            if (std::none_of(masks.begin(), masks.end(), [&](Box box) { return box_overlap(label.box, box) > .5; }))
                outside.push_back(label);
        add_vertical_text(document, outside);
    }
    struct Record {
        Box box;
        std::vector<std::string> lines;
    };
    std::vector<Record> records;
    visit(document, [&](const Hocr& node) {
        if (node.kind != "ocr_par")
            return;
        std::vector<std::string> lines;
        for (auto& line : node.children) {
            std::vector<std::string> texts;
            visit(line, [&](const Hocr& word) {
                if (word.kind == "ocrx_word")
                    texts.push_back(strip(word.text));
            });
            if (!texts.empty())
                lines.push_back(join(texts, " "));
        }
        if (!lines.empty())
            records.push_back({layout_box(node), std::move(lines)});
    });
    std::stable_sort(records.begin(), records.end(), [](auto& a, auto& b) { return reading_order(a.box, b.box); });
    std::vector<Figure> text_graphs;
    if (!options.images) {
        for (auto& figure : figures)
            if (figure.graph)
                text_graphs.push_back(std::move(figure));
        figures.clear();
    }
    bool short_figure_page = figures.size() == 1;
    std::size_t total_words = 0;
    for (auto& record : records) {
        short_figure_page &= record.lines.size() <= 3;
        total_words += word_count(join(record.lines, " "));
    }
    short_figure_page &= total_words <= 24;
    std::set<std::size_t> used;
    for (auto& figure : figures) {
        std::vector<std::string> captions;
        for (std::size_t number = 0; number < records.size(); ++number) {
            auto& [text_box, lines] = records[number];
            auto text = join(lines, " ");
            if (used.contains(number) || lines.size() > 3 || word_count(text) > 24)
                continue;
            int gap = std::max(figure.box.top - text_box.bottom, text_box.top - figure.box.bottom);
            bool nearby = 0 <= gap && gap <= std::max(20, 2 * (text_box.bottom - text_box.top));
            bool overlaps = text_box.left < figure.box.right && figure.box.left < text_box.right;
            if (short_figure_page || (labeled(text) && nearby && overlaps)) {
                captions.push_back(std::move(text));
                used.insert(number);
            }
        }
        figure.graphic.caption = join(captions, ". ");
    }
    std::vector<std::pair<Box, std::variant<Paragraph, Graphic, Music>>> ordered;
    for (auto& figure : figures)
        ordered.emplace_back(figure.box, std::move(figure.graphic));
    for (auto& figure : text_graphs)
        for (auto& text : describe_graph(figure.graphic))
            ordered.emplace_back(figure.box, Paragraph{std::move(text), 0, 0});
    for (std::size_t number = 0; number < records.size(); ++number)
        if (!used.contains(number))
            for (auto& block : text_document(join(records[number].lines, "\n"), options))
                ordered.emplace_back(records[number].box, std::move(block));
    if (ordered.empty() && !regions.empty() && !options.images)
        return {};
    if (ordered.empty() && picture)
        return options.images ? Document{Graphic(image)} : Document{};
    if (ordered.empty())
        throw std::runtime_error("OCR found no readable text or picture regions; check the scan, or use --images-only "
                                 "(the upload page's pictures-only option) to depict the original pixels");
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return reading_order(a.first, b.first); });
    Document result;
    for (auto& [box, block] : ordered)
        result.push_back(std::move(block));
    return result;
}

} // namespace braille
