#include "job_settings.hpp"

#include <algorithm>
#include <initializer_list>
#include <regex>
#include <stdexcept>

namespace braille {

using json = nlohmann::json;

json job_settings(const json& values) {
    json result{{"cells", 40}, {"lines", 25}, {"table", "en-ueb-g1.ctb"}, {"ocr", "auto"},
        {"image_mode", "outline"}, {"images_only", false}, {"max_pages", 100}, {"max_pixels", 20000000}};
    if (!values.is_object())
        throw std::runtime_error("Unknown conversion settings");
    for (auto& [key, value] : values.items()) {
        if (!result.contains(key) && key != "mode" && key != "legend")
            throw std::runtime_error("Unknown conversion setting " + key);
        result[key] = value;
    }
    auto limit = [&](const char* key, int low, int high) {
        auto& v = result[key];
        if (!v.is_number_integer() || v < low || v > high)
            throw std::runtime_error(std::string(key) + " must be between " + std::to_string(low) + " and " + std::to_string(high));
    };
    limit("cells", 8, 120);
    limit("lines", 4, 100);
    limit("max_pages", 1, 100);
    limit("max_pixels", 1, 20000000);
    auto one_of = [&](const char* key, std::initializer_list<const char*> choices) {
        if (std::none_of(choices.begin(), choices.end(), [&](auto x) { return result.at(key) == x; }))
            throw std::runtime_error(std::string("Unsupported ") + key);
    };
    one_of("table", {"en-ueb-g1.ctb", "en-ueb-g2.ctb", "es-g1.ctb"});
    one_of("ocr", {"off", "auto", "force"});
    one_of("image_mode", {"outline", "threshold", "dither"});
    if (!result["images_only"].is_boolean())
        throw std::runtime_error("images_only must be true or false");
    if (result.contains("mode")) one_of("mode", {"auto", "text", "picture", "map"});
    if (result.contains("legend")) {
        auto legend = result["legend"].get<std::string>();
        if (result.value("mode", "auto") != "map" || legend.size() > 1200)
            throw std::runtime_error("Map legend must be at most 1200 UTF-8 bytes");
        (void)unicode(legend);
    }
    return result;
}

Options job_options(const json& config, const std::string& ranges) {
    Options opts;
    opts.width = config["cells"];
    opts.height = config["lines"];
    opts.max_pages = config["max_pages"];
    opts.max_pixels = config["max_pixels"];
    opts.table = config["table"];
    opts.language = opts.table == "es-g1.ctb" ? "es" : "en";
    opts.image_mode = config["image_mode"];
    opts.ocr = config["ocr"] == "force" ? OCR::force : config["ocr"] == "auto" ? OCR::automatic : OCR::off;
    opts.ocr_embedded = false;
    opts.images_only = config["images_only"];
    auto mode = config.value("mode", "auto");
    if (mode == "text") {
        opts.images = false;
        opts.images_only = false;
        opts.ocr = OCR::automatic;
    }
    if (mode == "map" || mode == "picture") {
        opts.images_only = true;
        opts.render_picture_pages = true;
    }
    if (mode == "map")
        opts.image_mode = "threshold";
    if (opts.images_only)
        opts.ocr = OCR::off;
    opts.page_numbers = true;
    if (!ranges.empty()) {
        const std::regex valid(R"([1-9][0-9]*(?:-[1-9][0-9]*)?)");
        for (const auto& range : split(ranges, ',')) {
            if (!std::regex_match(range, valid))
                throw std::runtime_error("Invalid source page selection");
            auto bound = split(range, '-');
            int first = integer(bound.front(), "page range"), last = integer(bound.back(), "page range");
            if (last < first)
                throw std::runtime_error("Invalid source page selection");
            opts.ranges.emplace_back(first, last);
        }
    }
    opts.validate();
    return opts;
}

} // namespace braille
