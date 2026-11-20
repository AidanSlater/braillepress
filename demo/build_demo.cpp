#include "braille.hpp"
#include "mupdf.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>

#include "cases.hpp"

extern char** environ;

namespace {
using namespace braille;
constexpr int page_width = 960, page_height = 600;

struct Rect {
    double x, y, w, h;
};

struct SourcePage {
    std::string image;
    Rect whole, content;
};

struct Bookmark {
    std::string title;
    int page;
};

class Canvas {
public:
    Canvas() { out_.imbue(std::locale::classic()); }

    void rect(Rect r, std::string_view color) {
        out_ << color << " rg " << r.x << ' ' << r.y << ' ' << r.w << ' ' << r.h << " re f\n";
    }

    void text(int x, int y, std::string_view value, double size, bool bold = false, std::string_view color = ink) {
        out_ << "BT /" << (bold ? "Bold" : "Regular") << ' ' << size << " Tf " << color << " rg "
             << x << ' ' << y << " Td (" << literal(value) << ") Tj ET\n";
    }

    int paragraph(int x, int y, const std::string& value, int width, double size, int leading, std::string_view color = ink) {
        std::istringstream words(value);
        // Base-14 Helvetica stays legible on projection. Use a little extra
        // width for capitals and keep explicit source filenames intact.
        std::string row;
        double measured = 0;
        auto measure = [size](std::string_view word) {
            double length = 0;
            for (auto ch : unicode(word))
                length += ch == U' ' || ch == U'i' || ch == U'l' || ch == U'.' ? .26 :
                    ch == U'M' || ch == U'W' || ch == U'm' || ch == U'w' ? .88 :
                    ch >= U'A' && ch <= U'Z' ? .67 : .55;
            return length * size;
        };
        for (std::string word; words >> word;) {
            auto pixels = measure(word);
            if (!row.empty() && measured + pixels + size * .27 > width) {
                text(x, y, row, size, false, color);
                y -= leading;
                row.clear();
                measured = 0;
            }
            if (!row.empty()) {
                row += ' ';
                measured += size * .27;
            }
            row += word;
            measured += pixels;
        }
        if (!row.empty()) {
            text(x, y, row, size, false, color);
            y -= leading;
        }
        return y;
    }

    void place(const SourcePage& source, Rect target, bool crop) {
        auto area = crop ? source.content : source.whole;
        double scale = std::min(target.w / area.w, target.h / area.h);
        double x = target.x + (target.w - area.w * scale) * .5;
        double y = target.y + (target.h - area.h * scale) * .5;
        out_ << "q " << x << ' ' << y << ' ' << area.w * scale << ' ' << area.h * scale << " re W n\n"
             << scale << " 0 0 " << scale << ' ' << x - area.x * scale
             << ' ' << y - area.y * scale << " cm 0 g 0 G /" << source.image << " Do Q\n";
    }

    std::string commands() const { return out_.str(); }
    static constexpr std::string_view teal = "0 .43 .43", ink = ".08 .14 .20", muted = ".30 .37 .43";
    static constexpr std::string_view pale = ".93 .96 .96";

private:
    static std::string literal(std::string_view text) {
        std::string result;
        for (auto c : unicode(text)) {
            if (c == U'(' || c == U')' || c == U'\\') result += '\\';
            if (c >= 32 && c <= 255) result += static_cast<char>(c);
            else if (c == U'–' || c == U'—') result += '-';
            else if (c > 255) {
                std::ostringstream label;
                label << "U+" << std::uppercase << std::hex << static_cast<unsigned>(c);
                result += label.str();
            } else result += ' ';
        }
        return result;
    }
    std::ostringstream out_;
};

class Book {
public:
    Book() : doc_(mu_.call([](auto ctx) { return pdf_create_document(ctx); })),
             fonts_(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 2); })),
             images_(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 64); })) {
        for (auto [tag, base] : {std::pair{"Regular", "Helvetica"}, std::pair{"Bold", "Helvetica-Bold"}}) {
            auto font = mu_.own(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 4); }), pdf_drop_obj);
            mu_.call([&](auto ctx) {
                pdf_dict_put(ctx, font.get(), PDF_NAME(Type), PDF_NAME(Font));
                pdf_dict_put(ctx, font.get(), PDF_NAME(Subtype), PDF_NAME(Type1));
                pdf_dict_put_name(ctx, font.get(), PDF_NAME(BaseFont), base);
                pdf_dict_put(ctx, font.get(), PDF_NAME(Encoding), PDF_NAME(WinAnsiEncoding));
                pdf_dict_puts(ctx, fonts_, tag, font.get());
            });
        }
    }

    ~Book() {
        pdf_drop_obj(mu_.get(), images_);
        pdf_drop_obj(mu_.get(), fonts_);
        pdf_drop_document(mu_.get(), doc_);
    }

    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    std::vector<SourcePage> pdf(const fs::path& path, bool crop, int footer = 0) {
        auto str = path.string();
        auto source = mu_.own(mu_.call([&](auto ctx) { return pdf_open_document(ctx, str.c_str()); }), pdf_drop_document);
        auto graft = mu_.own(mu_.call([&](auto ctx) { return pdf_new_graft_map(ctx, doc_); }), pdf_drop_graft_map);
        int count = mu_.call([&](auto ctx) { return pdf_count_pages(ctx, source.get()); });
        std::vector<SourcePage> pages;
        for (int n = 0; n < count; ++n) {
            auto page = mu_.call([&](auto ctx) { return pdf_lookup_page_obj(ctx, source.get(), n); });
            auto box = mu_.call([&](auto ctx) {
                return pdf_to_rect(ctx, pdf_dict_get_inheritable(ctx, page, PDF_NAME(MediaBox)));
            });
            auto form = mu_.own(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 4); }), pdf_drop_obj);
            auto resources = mu_.own(mu_.call([&](auto ctx) {
                return pdf_graft_mapped_object(ctx, graft.get(), pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources)));
            }), pdf_drop_obj);
            auto contents = mu_.call([&](auto ctx) { return pdf_dict_get(ctx, page, PDF_NAME(Contents)); });
            auto buffer = mu_.own(mu_.call([](auto ctx) { return fz_new_buffer(ctx, 4096); }), fz_drop_buffer);
            auto append = [&](pdf_obj* object) {
                if (!object) return;
                auto stream = mu_.own(mu_.call([&](auto ctx) { return pdf_load_stream(ctx, object); }), fz_drop_buffer);
                mu_.call([&](auto ctx) { fz_append_buffer(ctx, buffer.get(), stream.get()); fz_append_byte(ctx, buffer.get(), '\n'); });
            };
            if (mu_.call([&](auto ctx) { return pdf_is_array(ctx, contents); })) {
                int size = mu_.call([&](auto ctx) { return pdf_array_len(ctx, contents); });
                for (int i = 0; i < size; ++i)
                    append(mu_.call([&](auto ctx) { return pdf_array_get(ctx, contents, i); }));
            } else {
                append(contents);
            }
            auto key = new_form(form.get(), buffer.get(), resources.get(), box);
            Rect whole{box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0};
            pages.push_back({key, whole, crop ? ink_bounds(path, n + 1, whole, footer) : whole});
        }
        return pages;
    }

    SourcePage picture(const fs::path& path) {
        auto bytes = read(path);
        auto buffer = mu_.own(mu_.call([&](auto ctx) {
            return fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        }), fz_drop_buffer);
        auto image = mu_.own(mu_.call([&](auto ctx) { return fz_new_image_from_buffer(ctx, buffer.get()); }), fz_drop_image);
        double scale = 660.0 / std::max(image->w, image->h);
        float w = image->w * scale, h = image->h * scale;
        auto ref = mu_.own(mu_.call([&](auto ctx) { return pdf_add_image(ctx, doc_, image.get()); }), pdf_drop_obj);
        auto resource = mu_.own(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 1); }), pdf_drop_obj);
        mu_.call([&](auto ctx) {
            auto pictures = pdf_dict_put_dict(ctx, resource.get(), PDF_NAME(XObject), 1);
            pdf_dict_puts(ctx, pictures, "Picture", ref.get());
        });
        auto commands = "q " + std::to_string(w) + " 0 0 " + std::to_string(h) + " 0 0 cm /Picture Do Q\n";
        auto data = mu_.own(mu_.call([&](auto ctx) {
            return fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(commands.data()), commands.size());
        }), fz_drop_buffer);
        auto form = mu_.own(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 4); }), pdf_drop_obj);
        auto key = new_form(form.get(), data.get(), resource.get(), {0, 0, w, h});
        return {key, {0, 0, w, h}, {0, 0, w, h}};
    }

    SourcePage error_page(const std::string& message) {
        Canvas error;
        error.text(36, 200, "The converter rejected this source", 20, true);
        error.paragraph(36, 155, message, 520, 15, 21);
        auto commands = error.commands();
        auto data = mu_.own(mu_.call([&](auto ctx) {
            return fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(commands.data()), commands.size());
        }), fz_drop_buffer);
        auto form = mu_.own(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 4); }), pdf_drop_obj);
        auto resource = new_resources(false);
        auto key = new_form(form.get(), data.get(), resource.get(), {0, 0, 600, 245});
        return {key, {0, 0, 600, 245}, {0, 0, 600, 245}};
    }

    void page(const Canvas& canvas) {
        auto resource = new_resources(true);
        add_pdf_page(mu_, doc_, page_width, page_height, canvas.commands(), resource.get());
    }

    void finish(const fs::path& destination, const std::vector<Bookmark>& links, int pages) {
        auto outline = mu_.own(mu_.call([&](auto ctx) { return pdf_add_new_dict(ctx, doc_, 4); }), pdf_drop_obj);
        mu_.call([&](auto ctx) {
            auto catalog = pdf_dict_get(ctx, pdf_trailer(ctx, doc_), PDF_NAME(Root));
            pdf_dict_put(ctx, catalog, PDF_NAME(Outlines), outline.get());
            auto info = pdf_dict_put_dict(ctx, pdf_trailer(ctx, doc_), PDF_NAME(Info), 2);
            pdf_dict_put_text_string(ctx, info, PDF_NAME(Title), "Open Braille Embosser - Software Demo");
            pdf_dict_put_text_string(ctx, info, PDF_NAME(Author), "Open Braille Embosser");
        });
        auto previous = mu_.own(static_cast<pdf_obj*>(nullptr), pdf_drop_obj);
        for (auto& link : links) {
            if (link.page < 1 || link.page > pages)
                throw std::runtime_error("Bookmark destination is outside the presentation");
            auto item = mu_.own(mu_.call([&](auto ctx) { return pdf_add_new_dict(ctx, doc_, 6); }), pdf_drop_obj);
            mu_.call([&](auto ctx) {
                pdf_dict_put_text_string(ctx, item.get(), PDF_NAME(Title), link.title.c_str());
                pdf_dict_put(ctx, item.get(), PDF_NAME(Parent), outline.get());
                auto destination = pdf_dict_put_array(ctx, item.get(), PDF_NAME(Dest), 2);
                pdf_array_push(ctx, destination, pdf_lookup_page_obj(ctx, doc_, link.page - 1));
                pdf_array_push(ctx, destination, PDF_NAME(Fit));
                if (!previous) pdf_dict_put(ctx, outline.get(), PDF_NAME(First), item.get());
                else {
                    pdf_dict_put(ctx, previous.get(), PDF_NAME(Next), item.get());
                    pdf_dict_put(ctx, item.get(), PDF_NAME(Prev), previous.get());
                }
            });
            previous.reset(item.release());
        }
        mu_.call([&](auto ctx) {
            pdf_dict_put(ctx, outline.get(), PDF_NAME(Last), previous.get());
            pdf_dict_put_int(ctx, outline.get(), PDF_NAME(Count), links.size());
        });
        auto temporary = destination;
        temporary += ".building";
        save_pdf(mu_, doc_, temporary.string());
        auto verify = mu_.own(mu_.call([&](auto ctx) { return pdf_open_document(ctx, temporary.c_str()); }), pdf_drop_document);
        if (mu_.call([&](auto ctx) { return pdf_count_pages(ctx, verify.get()); }) != pages)
            throw std::runtime_error("Assembled page count does not match the navigation");
        fs::rename(temporary, destination);
    }

private:
    std::string new_form(pdf_obj* form, fz_buffer* contents, pdf_obj* resources, fz_rect box) {
        auto key = "View" + std::to_string(++views_);
        mu_.call([&](auto ctx) {
            pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
            pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
            pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), box);
            if (resources) pdf_dict_put(ctx, form, PDF_NAME(Resources), resources);
            auto ref = pdf_add_stream(ctx, doc_, contents, form, 0);
            pdf_dict_puts(ctx, images_, key.c_str(), ref);
            pdf_drop_obj(ctx, ref);
        });
        return key;
    }

    auto new_resources(bool views) -> decltype(std::declval<PdfContext>().own(static_cast<pdf_obj*>(nullptr), pdf_drop_obj)) {
        auto res = mu_.own(mu_.call([&](auto ctx) { return pdf_new_dict(ctx, doc_, 2); }), pdf_drop_obj);
        mu_.call([&](auto ctx) {
            pdf_dict_put(ctx, res.get(), PDF_NAME(Font), fonts_);
            if (views) pdf_dict_put(ctx, res.get(), PDF_NAME(XObject), images_);
        });
        return res;
    }

    static Rect ink_bounds(const fs::path& file, int page, Rect all, int footer) {
        auto raster = render_pdf_page(file, page, 110, 4'000'000);
        int xmin = raster.width, ymin = raster.height, xmax = 0, ymax = 0;
        double sx = all.w / raster.width, sy = all.h / raster.height;
        int bottom = raster.height - std::min(raster.height, int(footer / sy));
        for (int y = 0; y < bottom; ++y)
            for (int x = 0; x < raster.width; ++x)
                if (raster.at(x, y) < 200) {
                    xmin = std::min(xmin, x);
                    xmax = std::max(xmax, x);
                    ymin = std::min(ymin, y);
                    ymax = std::max(ymax, y);
                }
        if (xmax <= xmin || ymax <= ymin) return all;
        int left = std::max(0, xmin - 9), top = std::max(0, ymin - 9);
        int right = std::min(raster.width, xmax + 10), low = std::min(bottom, ymax + 10);
        return {all.x + left * sx, all.y + all.h - low * sy, (right - left) * sx, (low - top) * sy};
    }

    PdfContext mu_;
    pdf_document* doc_;
    pdf_obj *fonts_, *images_;
    int views_ = 0;
};

Canvas frame(const std::string& title, const std::string& section, int page) {
    Canvas canvas;
    canvas.rect({0, 0, page_width, page_height}, "1 1 1");
    canvas.rect({0, 588, page_width, 12}, Canvas::teal);
    canvas.text(34, 558, "OPEN BRAILLE EMBOSSER", 10, true, Canvas::teal);
    canvas.text(678, 558, section, 10, true, Canvas::muted);
    canvas.text(34, 516, title, 27, true);
    canvas.rect({34, 47, 892, 1}, ".79 .85 .85");
    canvas.text(34, 27, "Source documents and converter previews", 10, false, Canvas::muted);
    canvas.text(899, 27, std::to_string(page), 12, true, Canvas::teal);
    return canvas;
}

void comparison(Book& book, const Case& example, const std::vector<SourcePage>& pages, int& number,
                const std::string& variant = "") {
    bool result = !variant.empty();
    for (std::size_t i = 0; i < pages.size(); ++i) {
        auto canvas = frame(example.title, std::string(example.live ? "ONE-MINUTE PRESENTATION" : "APPENDIX") +
            (result ? "  /  BRAILLE RESULT" : "  /  SOURCE"), number++);
        auto descriptor = (result ? variant : "Source") + " page " + std::to_string(i + 1) + " of " + std::to_string(pages.size());
        canvas.text(295, 487, descriptor, 10, true, Canvas::muted);
        canvas.text(34, 456, "WHAT TO NOTICE", 10, true, Canvas::teal);
        int y = canvas.paragraph(34, 431, example.look, 214, 14, 20);
        y = canvas.paragraph(34, y - 12, example.scope, 214, 11.5, 17, Canvas::muted);
        if (!result && !example.say.empty())
            canvas.paragraph(34, y - 15, "\"" + example.say + "\"", 214, 12, 17);
        auto area = Rect{289, 67, 637, 411};
        canvas.rect(area, Canvas::pale);
        canvas.rect({295, 73, 625, 399}, "1 1 1");
        canvas.place(pages[i], {301, 77, 613, 389}, true);
        if (result || example.id == "newsletter") {
            int top = std::min(233, y - 20);
            canvas.text(34, top, result ? "FULL OUTPUT PAGE" : "FULL SOURCE PAGE", 10, true, Canvas::teal);
            canvas.place(pages[i], {39, 81, 205, double(top - 89)}, false);
        }
        canvas.text(34, 61, example.input, 10, false, Canvas::muted);
        book.page(canvas);
    }
}

struct Product {
    std::string variant;
    std::vector<SourcePage> pages;
};
struct Example {
    const Case* definition;
    std::vector<SourcePage> source;
    std::vector<Product> products;
    int start = 0;
};

int command(std::vector<std::string> argv, const fs::path& log) {
    std::vector<char*> raw;
    for (auto& argument : argv) raw.push_back(argument.data());
    raw.push_back(nullptr);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t child;
    int error = posix_spawn(&child, argv.front().c_str(), &files, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    if (error) throw std::runtime_error("Cannot start the native converter");
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string two_digits(int number) {
    return (number < 10 ? "0" : "") + std::to_string(number);
}

void build(const fs::path& repository, const fs::path& converter, const fs::path& work, const fs::path& output) {
    fs::create_directories(work / "results");
    std::ofstream manifest(work / "manifest.txt");
    if (!manifest) throw std::runtime_error("Cannot write the demo manifest");
    Book book;
    std::vector<Example> examples;
    int next = 2, runs = 0;
    for (const auto& c : cases) {
        auto input = repository / "demo/source" / c.input;
        if (!fs::exists(input)) input = repository / "demo/examples" / c.input;
        auto suffix = extension(input);
        Example example{&c, {}, {}, next};
        if (!c.live && examples.size() == 4) ++example.start;
        next = example.start;
        if (image_extension(suffix)) example.source.push_back(book.picture(input));
        else example.source = book.pdf(suffix == ".pdf" ? input : repository / "demo/previews" / (c.input + ".pdf"),
            suffix == ".txt" || suffix == ".docx" || c.id == "newsletter", c.id == "newsletter" ? 80 : 0);
        next += example.source.size();
        int variant_number = 0;
        for (auto& variant : c.variants) {
            auto stem = work / "results" / (c.id + "-" + std::to_string(++variant_number));
            auto product = stem.string() + (c.unicode ? ".brl" : ".pdf");
            auto review = stem.string() + ".txt";
            auto log = stem.string() + ".stderr";
            std::vector<std::string> args{converter.string(), input.string(), product, "--text-output", review};
            args.insert(args.end(), variant.args.begin(), variant.args.end());
            std::cout << '[' << examples.size() + 1 << '/' << cases.size() << "] " << c.title << " / " << variant.name << '\n' << std::flush;
            for (const auto& arg : args) manifest << std::quoted(arg) << ' ';
            int status = command(args, log);
            ++runs;
            manifest << "(exit " << status << ")\n" << std::flush;
            std::vector<SourcePage> pages;
            if (c.error) {
                auto error = strip(read(log));
                if (status != 2 || error.find("GRINNING FACE") == std::string::npos || fs::exists(product))
                    throw std::runtime_error("Expected a named unsupported-symbol error in " + log);
                pages.push_back(book.error_page(error));
            } else {
                if (status != 0) throw std::runtime_error(c.title + ": " + strip(read(log)));
                auto imported = read(review);
                for (auto& text : c.required)
                    if (imported.find(text) == std::string::npos)
                        throw std::runtime_error(c.title + ": missing required review text " + text);
                if (c.unicode) {
                    Options options;
                    auto pdf_view = stem.string() + ".pdf";
                    preview(parse_cells(read(product), options, true), pdf_view, options.width, options.height);
                    product = pdf_view;
                }
                pages = book.pdf(product, true, c.unicode ? 0 : c.id == "resolution" ? 155 : 114);
            }
            next += pages.size();
            example.products.push_back({variant.name, std::move(pages)});
        }
        examples.push_back(std::move(example));
    }
    int appendix = examples[4].start - 1;
    if (appendix != 10) throw std::runtime_error("The one-minute presentation must fit pages 1 through 9");
    auto cover = frame("From print to Braille", "60 SECONDS", 1);
    cover.paragraph(34, 456, "Four source and result pairs for the live presentation. The detailed appendix follows.", 620, 21, 28);
    std::array<std::string, 4> times{"00-15 s", "15-28 s", "28-40 s", "40-53 s"};
    for (int i = 0; i < 4; ++i) {
        int y = 340 - 57 * i;
        int start = examples[i].start;
        cover.text(34, y, times[i], 12, true, Canvas::teal);
        cover.text(135, y, cases[i].title, 18, true);
        cover.text(520, y, "Source " + two_digits(start) + " / Result " + two_digits(start + 1), 13, false, Canvas::muted);
    }
    cover.rect({740, 146, 185, 309}, Canvas::pale);
    cover.text(759, 420, "53-60 seconds", 13, true, Canvas::teal);
    cover.paragraph(759, 384, "The printer accepts original files and external scans. Users can review the recognized text before embossing.", 145, 15, 22);
    cover.text(34, 86, "Appendix starts on page 10. Use the PDF bookmarks for individual examples.", 13, true, Canvas::teal);
    book.page(cover);
    int number = 2;
    std::vector<Bookmark> links{{"One-minute presentation", 1}};
    for (const auto& example : examples) {
        auto& c = *example.definition;
        if (number == appendix) {
            auto index = frame("Detailed examples", "APPENDIX  /  CONTENTS", number);
            links.push_back({"Appendix contents", number++});
            for (std::size_t i = 4; i < examples.size(); ++i) {
                int x = 34 + ((i - 4) / 9) * 450;
                int y = 456 - ((i - 4) % 9) * 39;
                index.text(x, y, two_digits(examples[i].start), 14, true, Canvas::teal);
                index.text(x + 38, y, examples[i].definition->title, 13);
            }
            index.paragraph(34, 81, "This build keeps the raw converter products and readable reviews for each example.", 850, 11, 14, Canvas::muted);
            book.page(index);
        }
        links.push_back({c.title + " - source", number});
        comparison(book, c, example.source, number);
        for (auto& product : example.products) {
            links.push_back({c.title + " - " + product.variant, number});
            comparison(book, c, product.pages, number, product.variant);
        }
    }
    auto workflow = frame("From file to a held job", "APPENDIX  /  PRINTER WORKFLOW", number);
    links.push_back({"Printer workflow", number});
    const std::array<std::pair<std::string, std::string>, 4> cards{{
        {"Import", "Original TXT, DOCX, PDF, pictures, and MusicXML."},
        {"Convert", "Literary Braille, OCR, dot graphics, and music Braille."},
        {"Review", "Readable OCR text, Unicode cells, and a vector PDF preview."},
        {"Hold", "Approved or native-text jobs remain held for the printer hardware."}}};
    for (std::size_t i = 0; i < cards.size(); ++i) {
        int x = 34 + i * 225;
        workflow.rect({double(x), 289, 208, 167}, Canvas::pale);
        workflow.text(x + 16, 420, cards[i].first, 22, true);
        workflow.paragraph(x + 16, 384, cards[i].second, 175, 13, 19);
    }
    workflow.paragraph(34, 230, "Send the original document through the browser or desktop sender. OCR jobs remain held until someone checks the result.", 848, 15, 23);
    workflow.paragraph(34, 147, "These previews show the six-dot layout. Reader testing and calibrated embossing are future bench work.", 848, 13, 20, Canvas::muted);
    book.page(workflow);
    if (number != next) throw std::runtime_error("Presentation page count changed during assembly");
    fs::create_directories(output.parent_path());
    book.finish(output, links, number);
    std::cout << runs << " converter runs verified. " << number << " pages. Presentation on pages 1-9.\n"
              << "PDF " << output.string() << "\nRun logs and results " << work.string() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("usage: braille-demo SOURCE_DIRECTORY CONVERTER WORK_DIRECTORY OUTPUT.pdf");
        auto output = braille::fs::absolute(argv[4]);
        if (braille::extension(output) != ".pdf") throw std::runtime_error("The destination must end in .pdf");
        auto source = braille::fs::weakly_canonical(argv[1]);
        auto relative = braille::fs::weakly_canonical(output).lexically_relative(source);
        if (!relative.empty() && *relative.begin() != "output" && *relative.begin() != "..")
            throw std::runtime_error("Keep demo results outside the source tree or under its output directory");
        build(source, argv[2], argv[3], output);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
