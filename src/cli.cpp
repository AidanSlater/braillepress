#include "braille.hpp"
#ifdef BRAILLE_MUSIC_SCAN
#include "omr.hpp"
#endif

#include <iostream>
#include <stdexcept>

using namespace braille;

int main(int argc, char** argv) {
    try {
        Options options;
        fs::path text_output, music_review;
        bool music = false, positional = false;
        std::vector<fs::path> files;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--" && !positional) {
                positional = true;
                continue;
            }
            if (positional || !arg.starts_with('-')) {
                files.emplace_back(arg);
                continue;
            }
            auto equal = arg.find('=');
            std::string key = arg.substr(0, equal);
            auto value = [&]() -> std::string {
                if (equal != std::string::npos)
                    return arg.substr(equal + 1);
                if (++i == argc)
                    throw std::runtime_error("Missing value for " + key);
                return argv[i];
            };
            auto number = [&] { return integer(value(), key); };
            if (key == "--help" || key == "-h") {
                std::cout <<
                    "usage: braille INPUT OUTPUT [options]\n"
                    "Convert TXT, DOCX, PDF, raster pictures, and MusicXML/MXL to Unicode Braille or dot PDF.\n\n"
                    "  --language en|es        literary Braille and OCR preset\n"
                    "  --table TABLE          Liblouis override, e.g. en-ueb-g2.ctb\n"
                    "  --cells-per-line N     default 40\n"
                    "  --lines-per-page N     default 25\n"
                    "  --first-indent N       default 2\n"
                    "  --list-indent N        default 4\n"
                    "  --ocr                  recognize scans and retain figures\n"
                    "  --ocr-language LANG    Tesseract override\n"
                    "  --text-output PATH     save readable review text\n"
                    "  --skip-images          omit images\n"
                    "  --images-only          depict images or scanned pages\n"
                    "  --image-mode outline|threshold|dither\n"
                    "  --image-threshold N    0 through 255\n"
                    "  --image-width N        maximum image columns\n"
                    "  --image-height N       maximum image rows\n"
                    "  --music                recognize scanned scores in a music-enabled build\n"
                    "  --music-review PATH    save MusicXML for each recognized source page\n";
                return 0;
            }
            if (key == "--language") options.language = value();
            else if (key == "--table") options.table = value();
            else if (key == "--ocr-language") options.ocr_language = value();
            else if (key == "--cells-per-line") options.width = number();
            else if (key == "--lines-per-page") options.height = number();
            else if (key == "--first-indent") options.first_indent = number();
            else if (key == "--list-indent") options.list_indent = number();
            else if (key == "--image-mode") options.image_mode = value();
            else if (key == "--image-threshold") options.threshold = number();
            else if (key == "--image-width" || key == "--image-height") {
                int size = number();
                if (size < 1)
                    throw std::runtime_error("Image width and height must be positive");
                (key == "--image-width" ? options.image_width : options.image_height) = size;
            } else if (key == "--text-output") text_output = value();
            else if (key == "--music-review") music_review = value();
            else if (equal != std::string::npos) throw std::runtime_error("Unexpected option value: " + arg);
            else if (key == "--ocr") options.ocr = OCR::automatic;
            else if (key == "--skip-images") options.images = false;
            else if (key == "--images-only") options.images_only = true;
            else if (key == "--music") music = true;
            else throw std::runtime_error("Unknown option " + key);
        }
        if (files.size() != 2)
            throw std::runtime_error("Expected an input path and an output path; see --help");
        const auto& input = files.front();
        const auto& output = files.back();
        auto same_file = [](const fs::path& a, const fs::path& b) {
            return fs::weakly_canonical(a) == fs::weakly_canonical(b)
                || (fs::exists(a) && fs::exists(b) && fs::equivalent(a, b));
        };
        if (same_file(input, output))
            throw std::runtime_error("Input and output must be different files");
        if (!text_output.empty() && (same_file(input, text_output) || same_file(output, text_output)))
            throw std::runtime_error("Review text, input, and Braille output must be different files");
        options.page_numbers = extension(output) == ".pdf";
        options.validate();
        bool xml_score = extension(input) == ".musicxml" || extension(input) == ".mxl";
        if (!music_review.empty() && (!music || xml_score))
            throw std::runtime_error("--music-review needs --music with a PDF/image input");
        Document document;
        if (music && !xml_score) {
            if (options.images_only)
                throw std::runtime_error("--music cannot be combined with --images-only");
            if (extension(input) != ".pdf" && !image_extension(extension(input)))
                throw std::runtime_error("Music recognition needs a sheet-music PDF/image or MusicXML score");
#ifdef BRAILLE_MUSIC_SCAN
            document = scanned_score(input, music_review, output, text_output);
#else
            throw std::runtime_error("Scanned scores require a build with BRAILLE_MUSIC_SCAN=ON, or MusicXML/MXL");
#endif
        } else {
            document = import_document(input, options).blocks;
        }
        auto pages = paginate(document, options);
        if (!text_output.empty())
            write(text_output, review(document));
        if (options.page_numbers)
            preview(pages, output, options.width, options.height);
        else
            write(output, serialize(pages));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
