#include "braille.hpp"
#include "mupdf.hpp"
#include "picture.hpp"

#include <unicode/uchar.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

namespace braille {
namespace {

struct PrintPage {
    int number;
    Document text, pictures;
};

auto open(const PdfContext& mu, const fs::path& path) {
    auto name = path.string();
    auto doc = mu.own(mu.call([&](auto ctx) { return pdf_open_document(ctx, name.c_str()); }), pdf_drop_document);
    if (doc->crypt)
        throw std::runtime_error("Encrypted PDFs are unsupported; supply a decrypted text PDF");
    return doc;
}

auto load(const PdfContext& mu, pdf_document* doc, int number) {
    return mu.own(mu.call([&](auto ctx) { return static_cast<fz_page*>(&pdf_load_page(ctx, doc, number - 1)->super); }), fz_drop_page);
}

Image grayscale(const PdfContext& mu, fz_pixmap* pix, int limit) {
    auto ctx = mu.get();
    int width = fz_pixmap_width(ctx, pix), height = fz_pixmap_height(ctx, pix);
    auto stride = fz_pixmap_stride(ctx, pix);
    auto source = fz_pixmap_samples(ctx, pix);
    if (width < 1 || height < 1 || std::int64_t(width) * height > limit)
        throw std::runtime_error("Rendered page exceeds the pixel limit");
    Image image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height)};
    for (int y = 0; y < height; ++y)
        std::copy_n(source + y * stride, width, image.pixels.begin() + y * width);
    return image;
}

Image render_page(const PdfContext& mu, fz_page* page, int dpi, int limit) {
    auto rect = mu.call([&](auto ctx) { return fz_bound_page(ctx, page); });
    auto matrix = fz_scale(dpi / 72.f, dpi / 72.f);
    auto pixels = fz_round_rect(fz_transform_rect(rect, matrix));
    if (std::int64_t(pixels.x1 - pixels.x0) * (pixels.y1 - pixels.y0) > limit)
        throw std::runtime_error("Rendered page exceeds the pixel limit");
    auto pix = mu.own(mu.call([&](auto ctx) {
        return fz_new_pixmap_from_page(ctx, page, matrix, fz_device_gray(ctx), 0);
    }), fz_drop_pixmap);
    return grayscale(mu, pix.get(), limit);
}

// The Python reference decoded the raw samples (pypdf) and converted them with Pillow:
// gray and RGB stayed as stored, and CMYK used Pillow's cmyk2rgb.
Image embedded_picture(const PdfContext& mu, fz_image* image, int max_pixels) {
    if (image->w <= 0 || image->h <= 0 || std::int64_t(image->w) * image->h > max_pixels)
        throw std::runtime_error("PDF picture exceeds the pixel limit");
    auto pix = mu.own(mu.call([&](auto ctx) { return fz_get_unscaled_pixmap_from_image(ctx, image); }), fz_drop_pixmap);
    auto ctx = mu.get();
    auto colorspace = fz_pixmap_colorspace(ctx, pix.get());
    auto kind = colorspace ? fz_colorspace_type(ctx, colorspace) : FZ_COLORSPACE_NONE;
    bool supported = kind == FZ_COLORSPACE_GRAY || kind == FZ_COLORSPACE_RGB || kind == FZ_COLORSPACE_BGR || kind == FZ_COLORSPACE_CMYK;
    auto converted = mu.own(supported ? nullptr : mu.call([&](auto c) {
        return fz_convert_pixmap(c, pix.get(), fz_device_rgb(c), nullptr, nullptr, fz_default_color_params, 0);
    }), fz_drop_pixmap);
    fz_pixmap* samples = supported ? pix.get() : converted.get();
    if (!supported)
        kind = FZ_COLORSPACE_RGB;
    int width = fz_pixmap_width(ctx, samples), height = fz_pixmap_height(ctx, samples);
    int n = fz_pixmap_components(ctx, samples);
    auto stride = fz_pixmap_stride(ctx, samples);
    auto source = fz_pixmap_samples(ctx, samples);
    if (width < 1 || height < 1 || std::int64_t(width) * height > max_pixels)
        throw std::runtime_error("PDF picture exceeds the pixel limit");
    imaging::Bitmap bitmap(width, height, kind == FZ_COLORSPACE_GRAY ? 1 : 3);
    for (int y = 0; y < height; ++y) {
        auto in = source + y * stride;
        auto out = bitmap.row(y);
        for (int x = 0; x < width; ++x, in += n) {
            if (kind == FZ_COLORSPACE_GRAY) {
                out[x] = in[0];
            } else if (kind == FZ_COLORSPACE_CMYK) {
                int nk = 255 - in[3];
                for (int i = 0; i < 3; ++i) {
                    int t = in[i] * nk + 128;
                    out[x * 3 + i] = static_cast<std::uint8_t>(std::clamp(nk - (((t >> 8) + t) >> 8), 0, 255));
                }
            } else {
                bool bgr = kind == FZ_COLORSPACE_BGR;
                out[x * 3] = in[bgr ? 2 : 0];
                out[x * 3 + 1] = in[1];
                out[x * 3 + 2] = in[bgr ? 0 : 2];
            }
        }
    }
    return to_image(bitmap);
}

int words(const Cells& text) {
    int count = 0;
    bool previous_space = true;
    for (auto ch : text) {
        bool space = u_isUWhiteSpace(ch);
        count += !space && previous_space;
        previous_space = space;
    }
    return count;
}

bool empty(const Cells& text) { return words(text) == 0; }

std::vector<Cells> columns(const std::vector<Cells>& lines) {
    int width = 0, populated = 0;
    for (auto& line : lines) {
        width = std::max(width, static_cast<int>(line.size()));
        populated += !line.empty();
    }
    int column = 0;
    std::vector<int> best;
    auto begins = [](const Cells& line, int start) { return std::min<std::size_t>(start, line.size()); };
    for (int index = std::max(1, width / 4); index < width * 3 / 4; ++index) {
        int crossing = 0;
        std::vector<int> both;
        for (auto& line : lines) {
            if (!empty(line.substr(begins(line, index - 1), 3))) {
                ++crossing;
                continue;
            }
            auto left = words(line.substr(0, index)), right = words(line.substr(begins(line, index + 2)));
            if (left && right)
                both.push_back(std::min(left, right));
        }
        if (crossing <= populated * .1 && both.size() > best.size()) {
            column = index;
            best = std::move(both);
        }
    }
    if (best.size() < std::max(8., populated * .6))
        return lines;
    std::sort(best.begin(), best.end());
    if (best[best.size() / 2] < 4)
        return lines;

    std::vector<Cells> result, left, right;
    auto flush = [&] {
        result.insert(result.end(), left.begin(), left.end());
        result.emplace_back();
        result.insert(result.end(), right.begin(), right.end());
        result.emplace_back();
        left.clear();
        right.clear();
    };
    for (auto& line : lines) {
        if (!empty(line.substr(begins(line, column - 1), 3))) {
            flush();
            result.push_back(line);
            result.emplace_back();
        } else {
            left.push_back(line.substr(0, column));
            right.push_back(line.substr(begins(line, column)));
        }
    }
    flush();
    return result;
}

std::string layout_text(fz_stext_page* page) {
    struct Run { float x, right, y, size; Cells text; };
    std::vector<Run> runs;
    double total_width = 0, total_chars = 0;
    for (auto block = page->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT)
            continue;
        for (auto line = block->u.t.first_line; line; line = line->next) {
            if (!line->first_char || line->wmode || std::abs(line->dir.y) > .1)
                continue;
            // MuPDF may insert spaces or merge text runs on one baseline.
            // Break large geometric gaps into separate fixed-layout runs.
            Run current{line->first_char->origin.x, 0, line->first_char->origin.y, line->first_char->size, {}};
            auto finish = [&] {
                if (current.text.empty())
                    return;
                total_width += std::max(0.f, current.right - current.x);
                total_chars += current.text.size();
                runs.push_back(std::move(current));
            };
            for (auto c = line->first_char; c; c = c->next) {
                if (!current.text.empty() && c->origin.x - current.right > c->size * 1.5) {
                    finish();
                    current = {c->origin.x, 0, c->origin.y, c->size, {}};
                }
                current.right = std::max(c->quad.lr.x, c->quad.ur.x);
                current.text += c->c >= 14 && c->c < 32 ? U' ' : static_cast<char32_t>(c->c);
            }
            finish();
        }
    }
    if (runs.empty())
        return {};
    std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
        return std::tie(a.y, a.x) < std::tie(b.y, b.x);
    });
    double char_width = std::max(.25, total_width / (total_chars * 1.25));
    std::vector<Cells> lines;
    float y = runs.front().y, right = 0;
    lines.emplace_back();
    for (auto& run : runs) {
        if (!std::isfinite(run.x) || !std::isfinite(run.y) || !std::isfinite(run.size))
            continue;
        double gap = run.y - y;
        if (gap > std::max(1., run.size * .35)) {
            if (gap >= 2 * std::max(.25f, run.size))
                lines.emplace_back();
            lines.emplace_back();
            y = run.y;
            right = 0;
        }
        auto& row = lines.back();
        int offset = static_cast<int>(std::clamp(run.x / char_width, 0., 4096.));
        if (offset > static_cast<int>(row.size()) && (row.empty() || std::ceil(right) < std::floor(run.x)))
            row.resize(offset, U' ');
        else if (!row.empty() && !run.text.empty() && run.x > right + 1 && row.back() != U' ' && run.text.front() != U' ')
            row += U' ';
        row += run.text;
        right = run.right;
    }
    auto ordered = columns(lines);
    std::vector<std::string> blocks, group;
    auto flush = [&] {
        if (group.empty())
            return;
        std::size_t left = 4096;
        for (auto& line : group) left = std::min(left, line.find_first_not_of(' '));
        for (auto& line : group) line.erase(0, left);
        blocks.push_back(join(group, "\n"));
        group.clear();
    };
    for (auto& line : ordered) {
        if (empty(line))
            flush();
        else
            group.push_back(utf8(line));
    }
    flush();
    return join(blocks, "\n\n");
}

void remove_furniture(std::vector<PrintPage>& pages, int total) {
    static const std::regex simple(R"((?:(?:page|p\.|pág\.|página)\s*)?(?:-|–|—|\()?\s*([0-9]+)\s*(?:-|–|—|\))?)", std::regex::icase);
    static const std::regex counted(R"((?:(?:page|página)\s+)?([0-9]+)\s*(?:/|of|de)\s*([0-9]+))", std::regex::icase);
    std::vector<std::tuple<std::size_t, std::size_t, int>> candidates;
    std::map<int, int> offsets;
    for (std::size_t index = 0; index < pages.size(); ++index) {
        auto& [number, blocks, pictures] = pages[index];
        if (blocks.empty()) continue;
        for (auto position : std::set<std::size_t>{0, blocks.size() - 1}) {
            auto& text = std::get<Paragraph>(blocks[position]).text;
            std::smatch m;
            std::optional<int> offset;
            try {
                if (std::regex_match(text, m, counted) && std::stoi(m[1]) == number && std::stoi(m[2]) == total)
                    offset = 0;
                else if (std::regex_match(text, m, simple))
                    offset = std::stoi(m[1]) - number;
            } catch (const std::out_of_range&) { /* Ordinary long numbers are text. */ }
            if (offset) {
                ++offsets[*offset];
                candidates.emplace_back(index, position, *offset);
            }
        }
    }
    if (!offsets.empty()) {
        auto [offset, count] = *std::max_element(offsets.begin(), offsets.end(), [](auto& a, auto& b) {
            return a.second < b.second;
        });
        if (count >= 2 || (pages.size() == 1 && offset == 0))
            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                auto [index, position, found] = *it;
                if (found == offset) pages[index].text.erase(pages[index].text.begin() + position);
            }
    }
    int populated = std::count_if(pages.begin(), pages.end(), [](auto& page) { return !page.text.empty(); });
    for (bool last : {false, true}) {
        std::map<std::string, int> repetitions;
        std::set<std::string> seen;
        for (auto& page : pages)
            if (!page.text.empty())
                ++repetitions[std::get<Paragraph>(last ? page.text.back() : page.text.front()).text];
        for (auto& page : pages) {
            if (page.text.empty()) continue;
            auto item = last ? page.text.end() - 1 : page.text.begin();
            const auto& text = std::get<Paragraph>(*item).text;
            int count = repetitions[text];
            if (count >= 3 && count * 2 >= populated && words(unicode(text)) <= 12 && !seen.insert(text).second)
                page.text.erase(item);
        }
    }
}

Imported pdf_document_import(const fs::path& path, const Options& options) {
    PdfContext mu;
    auto doc = open(mu, path);
    int total = mu.call([&](auto ctx) { return pdf_count_pages(ctx, doc.get()); });
    if (total > options.max_pages)
        throw std::runtime_error("Document exceeds the source page limit");
    std::vector<PrintPage> pages;
    Imported result;
    for (int number = 1; number <= total; ++number) {
        if (!options.selects(number))
            continue;
        auto page = load(mu, doc.get(), number);
        fz_stext_options st{};
        st.flags = FZ_STEXT_PRESERVE_IMAGES | FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_MEDIABOX_CLIP;
        auto content = mu.own(mu.call([&](auto ctx) { return fz_new_stext_page_from_page(ctx, page.get(), &st); }), fz_drop_stext_page);
        auto text = layout_text(content.get());
        auto word_count = words(unicode(text));
        std::vector<fz_image*> pictures;
        for (auto b = content->first_block; b; b = b->next)
            if (b->type == FZ_STEXT_BLOCK_IMAGE)
                pictures.push_back(b->u.i.image);
        PrintPage item{number, {}, {}};
        auto picture_error = [number](const std::exception& error) {
            return std::runtime_error("Cannot process picture on PDF page " + std::to_string(number) + ": " + error.what());
        };
        if (options.ocr != OCR::off && (options.ocr == OCR::force || word_count == 0 || (word_count < 12 && !pictures.empty()))) {
            bool embedded = options.ocr_embedded && options.ocr != OCR::force && pictures.size() == 1;
            try {
                Image input = embedded
                    ? embedded_picture(mu, pictures.front(), options.max_pixels)
                    : render_page(mu, page.get(), options.ocr_dpi, options.max_pixels);
                item.pictures = scan_picture(input, options, options.ocr_embedded && !pictures.empty() && word_count > 0);
            } catch (const std::exception& error) {
                // As in the Python reference, imported pictures name their page; the service's rendered pages do not.
                if (!options.ocr_embedded)
                    throw;
                throw picture_error(error);
            }
            // Only a rendered page recognizes the text layer again; otherwise keep its words.
            if (embedded)
                item.text = text_document(text, options);
            result.recognized_pages.emplace_back(number);
        } else if (options.render_picture_pages) {
            item.pictures.emplace_back(Graphic(render_page(mu, page.get(), options.ocr_dpi, options.max_pixels)));
        } else {
            if (!word_count && !pictures.empty() && !options.images_only)
                throw std::runtime_error("PDF page " + std::to_string(number) + " has images but no extractable text; OCR is required for text (use --ocr), or use --images-only to depict the page as a picture");
            item.text = text_document(text, options);
            if (options.images)
                for (auto picture : pictures) {
                    try {
                        item.pictures.emplace_back(Graphic(embedded_picture(mu, picture, options.max_pixels)));
                    } catch (const std::exception& error) {
                        throw picture_error(error);
                    }
                }
        }
        pages.push_back(std::move(item));
    }
    remove_furniture(pages, total);
    for (auto& page : pages) {
        for (auto& block : page.text) result.blocks.push_back(std::move(block));
        for (auto& block : page.pictures) result.blocks.push_back(std::move(block));
    }
    return result;
}

} // namespace

int pdf_page_count(const fs::path& path) {
    PdfContext mu;
    auto doc = open(mu, path);
    return mu.call([&](auto ctx) { return pdf_count_pages(ctx, doc.get()); });
}

Image render_pdf_page(const fs::path& path, int number, int dpi, int limit) {
    PdfContext mu;
    auto doc = open(mu, path);
    auto count = mu.call([&](auto ctx) { return pdf_count_pages(ctx, doc.get()); });
    if (number < 1 || number > count)
        throw std::runtime_error("Page outside document");
    auto page = load(mu, doc.get(), number);
    return render_page(mu, page.get(), dpi, limit);
}

Imported import_document(const fs::path& path, const Options& options) {
    auto suffix = extension(path);
    Imported result;
    if (suffix == ".txt") {
        result.blocks = text_document(read(path), options);
    } else if (suffix == ".pdf") {
        result = pdf_document_import(path, options);
    } else if (suffix == ".docx") {
        auto blocks = word_document(path, options);
        int index = 0;
        for (auto& block : blocks) {
            ++index;
            if (auto picture = std::get_if<Graphic>(&block); picture && options.ocr != OCR::off) {
                result.recognized_pages.emplace_back("Picture block " + std::to_string(index));
                for (auto& recognized : scan_picture(picture->image, options, true))
                    result.blocks.push_back(std::move(recognized));
            } else {
                result.blocks.push_back(std::move(block));
            }
        }
    } else if (suffix == ".musicxml" || suffix == ".mxl") {
        if (options.images_only)
            throw std::runtime_error("MusicXML cannot be used with --images-only");
        result.blocks = music_document(path);
    } else if (image_extension(suffix)) {
        auto image = read_image(read(path), options.max_pixels);
        if (options.ocr != OCR::off) {
            result.blocks = scan_picture(image, options);
            result.recognized_pages.emplace_back("Image 1");
        } else if (options.images) {
            result.blocks.emplace_back(Graphic(std::move(image)));
        } else {
            throw std::runtime_error("A picture input cannot be used with --skip-images");
        }
    } else {
        throw std::runtime_error("Unsupported input " + (suffix.empty() ? "(no extension)" : suffix) +
            "; use .txt, .docx, .pdf, .musicxml, .mxl, or a raster picture");
    }
    if (options.images_only) {
        std::erase_if(result.blocks, [](const auto& b) { return !std::holds_alternative<Graphic>(b); });
        if (result.blocks.empty())
            throw std::runtime_error("No raster pictures found in this input");
    }
    return result;
}

void preview(const Pages& pages, const fs::path& path, int width, int height) {
    PdfContext mu;
    auto document = mu.own(mu.call([](auto ctx) { return pdf_create_document(ctx); }), pdf_drop_document);
    double scale = std::min({1., 540. / (width * 14.), 720. / (height * 28.)});
    constexpr std::array<std::pair<int, int>, 8> dots{{{0,0}, {0,1}, {0,2}, {1,0}, {1,1}, {1,2}, {0,3}, {1,3}}};
    constexpr auto circle = "1 0 m 1 .552285 .552285 1 0 1 c -.552285 1 -1 .552285 -1 0 c "
                            "-1 -.552285 -.552285 -1 0 -1 c .552285 -1 1 -.552285 1 0 c h f";
    auto add = [&](const Lines& lines) {
        std::ostringstream commands;
        commands.imbue(std::locale::classic());
        commands << "q " << std::fixed << std::setprecision(8) << scale << " 0 0 " << scale << " 36 756 cm 0 g\n";
        for (std::size_t row = 0; row < lines.size(); ++row)
            for (std::size_t col = 0; col < lines[row].size(); ++col) {
                int mask = lines[row][col] - blank;
                for (std::size_t bit = 0; bit < dots.size(); ++bit)
                    if (mask & (1 << bit)) {
                        auto [dx, dy] = dots[bit];
                        commands << "q 1.5 0 0 1.5 " << 2 + 14 * col + dx * 5 << ' '
                                 << -2 - static_cast<long long>(28 * row) - dy * 5 << " cm " << circle << " Q\n";
                    }
            }
        commands << "Q\n";
        add_pdf_page(mu, document.get(), 612, 792, commands.str());
    };
    if (pages.empty())
        add({});
    else
        for (auto& page : pages) add(page);
    save_pdf(mu, document.get(), path.string());
}

} // namespace braille
