// Service settings and conversion-worker tests with literal expectations.
// The worker runs as a braille-service subprocess. The OCR checks use Tesseract
// on rendered text.

#include "braille.hpp"
#include "check.hpp"
#include "job_settings.hpp"

#include <cups/raster.h>
#include <fcntl.h>
#include <leptonica/allheaders.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <memory>
#include <tuple>

using namespace braille;
using namespace native_test;
using json = nlohmann::json;

extern char** environ;

namespace {

int spawn(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ))
        throw std::runtime_error("Cannot start " + args[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        throw std::runtime_error("Cannot wait for " + args[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

fs::path fresh(const std::string& name) {
    auto path = context().work / name;
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

// Runs the service's conversion worker for job 1, as the queue does.
int prepare(const fs::path& state, const fs::path& source, const std::string& mime, const json& settings) {
    return spawn({context().cli.string(), "worker", state.string(), "1", source.string(), mime, settings.dump(), "64000000"});
}

// One page of large Helvetica text, rendered by MuPDF at 300 dpi.
Image rendered_text(const std::string& text) {
    auto pdf = context().work / "text.pdf";
    std::string content = "BT /F1 48 Tf 30 40 Td (" + text + ") Tj ET";
    write(pdf, "%PDF-1.4\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
               "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
               "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(40 + 30 * text.size()) +
               " 120] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
               "4 0 obj << /Length " + std::to_string(content.size()) + " >> stream\n" + content + "\nendstream endobj\n"
               "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
               "trailer << /Root 1 0 R >>\n%%EOF\n");
    return render_pdf_page(pdf, 1, 300, 20000000);
}

void write_png(const Image& image, const fs::path& path) {
    std::unique_ptr<PIX, void (*)(PIX*)> pix(pixCreate(image.width, image.height, 8), [](PIX* p) { pixDestroy(&p); });
    check(pix != nullptr, "Cannot allocate PNG fixture");
    pixSetResolution(pix.get(), 300, 300);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            pixSetPixel(pix.get(), x, y, image.at(x, y));
    check(pixWrite(path.c_str(), pix.get(), IFF_PNG) == 0, "Cannot write PNG fixture");
}

// Independent PWG or Apple raster encoding through libcups.
void write_raster(const Image& image, const fs::path& path, bool apple) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    check(fd >= 0, "Cannot create raster fixture");
    cups_page_header2_t header{};
    header.cupsWidth = image.width;
    header.cupsHeight = image.height;
    header.cupsBitsPerColor = header.cupsBitsPerPixel = 8;
    header.cupsColorOrder = CUPS_ORDER_CHUNKED;
    header.cupsNumColors = 1;
    header.cupsColorSpace = CUPS_CSPACE_SW;
    header.cupsBytesPerLine = image.width;
    header.HWResolution[0] = header.HWResolution[1] = 300;
    header.PageSize[0] = header.cupsPageSize[0] = image.width * 72 / 300;
    header.PageSize[1] = header.cupsPageSize[1] = image.height * 72 / 300;
    auto raster = cupsRasterOpen(fd, apple ? CUPS_RASTER_WRITE_APPLE : CUPS_RASTER_WRITE_PWG);
    bool written = raster && cupsRasterWriteHeader2(raster, &header);
    for (int y = 0; written && y < image.height; ++y)
        written = cupsRasterWritePixels(raster, const_cast<unsigned char*>(&image.pixels[std::size_t(y) * image.width]),
                                        image.width) == unsigned(image.width);
    if (raster) cupsRasterClose(raster);
    close(fd);
    check(written, "Cannot encode raster fixture");
}

// A single-page PDF whose only content is a grayscale image, as a scanner writes it.
void write_scanned_pdf(const Image& image, const fs::path& path) {
    std::string pixels(image.pixels.begin(), image.pixels.end());
    double width = image.width * 72.0 / 300, height = image.height * 72.0 / 300;
    std::string content = "q " + std::to_string(width) + " 0 0 " + std::to_string(height) + " 0 0 cm /Im1 Do Q";
    write(path, "%PDF-1.4\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
                "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
                "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(width) + " " + std::to_string(height) +
                "] /Contents 4 0 R /Resources << /XObject << /Im1 5 0 R >> >> >> endobj\n"
                "4 0 obj << /Length " + std::to_string(content.size()) + " >> stream\n" + content + "\nendstream endobj\n"
                "5 0 obj << /Type /XObject /Subtype /Image /Width " + std::to_string(image.width) + " /Height " +
                std::to_string(image.height) + " /ColorSpace /DeviceGray /BitsPerComponent 8 /Length " +
                std::to_string(pixels.size()) + " >> stream\n" + pixels + "\nendstream endobj\n"
                "trailer << /Root 1 0 R >>\n%%EOF\n");
}

} // namespace

TEST_CASE("test_unknown_and_out_of_range_settings_fail") {
    for (const auto& values : {json{{"table", "/etc/passwd"}}, json{{"cells", true}}, json{{"cells", 0}}, json{{"lines", 1000}},
                               json{{"ocr", "maybe"}}, json{{"image_mode", "bad"}}, json{{"images_only", "true"}},
                               json{{"extra", 1}}})
        expect_error([&] { job_settings(values); }, {}, values.dump());
}

TEST_CASE("test_spanish_text_preparation_retains_cells_and_settings") {
    auto state = fresh("spanish-text");
    auto source = state / "spanish.txt";
    write(source, "¿Qué? 12,5");
    check_equal(prepare(state, source, "text/plain", {{"table", "es-g1.ctb"}}), 0, "Worker status");
    auto job = state / "jobs/1";
    check(read(job / "converted.brl").starts_with("⠀⠀⠢⠨⠟⠥⠮⠢⠀⠼⠁⠃⠂⠑\n"), "Spanish cells: " + read(job / "converted.brl"));
    check_equal(read(job / "imported.txt"), std::string("¿Qué? 12,5\n"), "Review text");
    auto metadata = json::parse(read(job / "result.json"));
    check_equal(metadata["settings"]["table"].get<std::string>(), std::string("es-g1.ctb"), "Recorded table");
    check(!metadata["used_ocr"].get<bool>(), "Native text must not use OCR");
}

TEST_CASE("test_spanish_bulleted_list_uses_six_dot_hyphen_bullets") {
    auto state = fresh("spanish-list");
    auto source = state / "lista.txt";
    write(source, "- pan\n- leche\n");
    check_equal(prepare(state, source, "text/plain", {{"table", "es-g1.ctb"}}), 0, "Worker status");
    auto cells = read(state / "jobs/1/converted.brl");
    check(cells.starts_with("⠤⠀⠏⠁⠝\n⠤⠀⠇⠑⠉⠓⠑\n"), "Spanish bullets: " + cells);
}

TEST_CASE("test_ocr_language_follows_table_for_images_and_print_rasters") {
    auto image = rendered_text("Hola");
    for (auto [table, language, cells] : {std::tuple{"es-g1.ctb", "spa", "⠨⠓⠕⠇⠁"}, std::tuple{"en-ueb-g1.ctb", "eng", "⠠⠓⠕⠇⠁"}}) {
        check_equal(job_options(job_settings({{"table", table}}), "").recognition_language(), std::string(language),
                    std::string("OCR language for ") + table);
        for (auto [suffix, mime] : {std::pair{".png", "image/png"}, std::pair{".pwg", "image/pwg-raster"},
                                    std::pair{".urf", "image/urf"}}) {
            auto label = std::string(table) + suffix;
            auto state = fresh("ocr-" + label);
            auto source = state / (std::string("source") + suffix);
            if (std::string(suffix) == ".png") write_png(image, source);
            else write_raster(image, source, std::string(suffix) == ".urf");
            check_equal(prepare(state, source, mime, {{"table", table}}), 20, label + " worker status (20 = OCR used)");
            auto job = state / "jobs/1";
            auto converted = read(job / "converted.brl");
            check(converted.starts_with("⠀⠀" + std::string(cells) + "\n"), label + " cells: " + converted);
            check(json::parse(read(job / "result.json"))["used_ocr"].get<bool>(), label + " used_ocr");
            check(fs::exists(job / "review-required"), label + " requires review");
        }
    }
}

TEST_CASE("test_native_pdf_preserves_text_without_rendering") {
    auto options = job_options(job_settings(json::object()), "");
    auto imported = import_document(context().root / "tests/fixtures/simple.pdf", options);
    check_equal(review(imported.blocks), std::string("Cat\n\ndog\n\n123\n"), "Native PDF text");
    check(imported.recognized_pages.empty(), "Native PDF text must not be recognized");
}

TEST_CASE("test_pdf_rendering_hook_reports_scanned_page") {
    // The Python test replaced OCR with a stub returning "cat"; this renders
    // a real image-only page and checks that only that page goes through OCR.
    auto path = fresh("scanned-page") / "scan.pdf";
    write_scanned_pdf(rendered_text("The cat sat"), path);
    auto imported = import_document(path, job_options(job_settings(json::object()), ""));
    check(imported.recognized_pages == std::vector<std::variant<int, std::string>>{1}, "Recognized page list");
    check(review(imported.blocks).find("cat sat") != std::string::npos, "Recognized text: " + review(imported.blocks));
}

TEST_CASE("test_worker_failure_is_readable_and_never_prepared") {
    auto state = fresh("worker-failure");
    auto source = state / "bad.txt";
    write(source, "\xff");
    check_equal(prepare(state, source, "text/plain", json::object()), 1, "Worker status");
    auto job = state / "jobs/1";
    check(fs::exists(job / "failed") && !read(job / "failed").empty(), "Readable failure");
    check(!fs::exists(job / "prepared"), "A failed job must never be prepared");
}

int main(int argc, char** argv) { return native_test::run(argc, argv); }
