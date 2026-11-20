#include "omr.hpp"
#include "omr_model.hpp"

#include <leptonica/allheaders.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace braille {
namespace {

constexpr int max_pixels = 20'000'000;
constexpr int divisions = 128;
constexpr int whole = 4 * divisions;
constexpr int ink = 178;

struct StaffLine {
    int first, last, left, right;
    int y() const { return (first + last) / 2; }
    int width() const { return right - left; }
};

struct Staff {
    int top, bottom, left, right;
    double space;
};

std::vector<Staff> find_staves(const Image& page) {
    std::vector<StaffLine> lines;
    const int max_gap = std::max(1, page.width / 1000);
    const int min_width = std::max(40, page.width / 4);
    for (int y = 0; y < page.height; ++y) {
        int left = 0, last_ink = -1, best_left = 0, best_right = 0;
        for (int x = 0; x < page.width; ++x) {
            if (page.at(x, y) >= ink)
                continue;
            if (x - last_ink > max_gap + 1 || last_ink < 0)
                left = x;
            last_ink = x;
            if (x + 1 - left > best_right - best_left) {
                best_left = left;
                best_right = x + 1;
            }
        }
        if (best_right - best_left < min_width)
            continue;
        if (lines.empty() || lines.back().last + 1 != y) {
            lines.push_back({y, y, best_left, best_right});
        } else {
            auto& line = lines.back();
            line.last = y;
            if (best_right - best_left > line.width()) {
                line.left = best_left;
                line.right = best_right;
            }
        }
    }

    std::vector<Staff> result;
    for (std::size_t i = 0; i + 4 < lines.size();) {
        const auto* group = lines.data() + i;
        double space = (group[4].y() - group[0].y()) / 4.;
        const double tolerance = std::max(1., space * .16);
        int left = group[0].left, right = group[0].right;
        int inner_left = left, inner_right = right;
        bool evenly_spaced = space >= 3;
        for (int line = 0; line < 5; ++line) {
            evenly_spaced &= std::abs(group[line].y() - group[0].y() - space * line) <= tolerance
                && group[line].last - group[line].first < space * .55;
            left = std::min(left, group[line].left);
            right = std::max(right, group[line].right);
            inner_left = std::max(inner_left, group[line].left);
            inner_right = std::min(inner_right, group[line].right);
        }
        if (evenly_spaced && inner_right - inner_left >= (right - left) * .8) {
            result.push_back({group[0].y(), group[4].y(), left, right, space});
            i += 5;
        } else {
            ++i;
        }
    }
    return result;
}

double skew(const Image& page) {
    const double scale = std::min(1., 1600. / std::max(page.width, page.height));
    const auto sample = resize_image(page, std::max(1, int(page.width * scale)), std::max(1, int(page.height * scale)));
    auto destroy = [](PIX* pix) { pixDestroy(&pix); };
    std::unique_ptr<PIX, decltype(destroy)> binary(pixCreate(sample.width, sample.height, 1), destroy);
    if (!binary)
        throw std::runtime_error("Cannot allocate staff deskew image");
    for (int y = 0; y < sample.height; ++y) {
        auto row = pixGetData(binary.get()) + y * pixGetWpl(binary.get());
        for (int x = 0; x < sample.width; ++x)
            if (sample.at(x, y) < ink)
                SET_DATA_BIT(row, x);
    }
    float angle = 0, confidence = 0;
    return pixFindSkew(binary.get(), &angle, &confidence) == 0 && confidence >= 2 ? angle : 0;
}

std::vector<Image> staff_images(Image page) {
    auto staves = find_staves(page);
    if (staves.empty()) {
        auto angle = skew(page);
        if (std::abs(angle) >= .1 && std::abs(angle) <= 6) {
            page = transform_image(page, angle, false, false, max_pixels);
            staves = find_staves(page);
        }
    }
    if (staves.empty() || staves.size() > 40)
        throw std::runtime_error("Expected one or more clear five-line staves on the page");

    std::vector<Image> result;
    for (std::size_t i = 0; i < staves.size(); ++i) {
        const auto& staff = staves[i];
        if (i && staff.top - staves[i - 1].bottom < 4 * std::min(staff.space, staves[i - 1].space))
            throw std::runtime_error("Closely spaced or paired staves are unsupported in scanned music");
        int left = std::max(0, staff.left - int(.6 * staff.space));
        int right = std::min(page.width, staff.right + int(.6 * staff.space));
        int top = std::max(0, staff.top - int(2.5 * staff.space));
        int bottom = std::min(page.height, staff.bottom + int(2.25 * staff.space) + 1);
        if (page.height - bottom < staff.space)
            bottom = page.height;
        if (i)
            top = std::max(top, (staves[i - 1].bottom + staff.top) / 2);
        if (i + 1 < staves.size())
            bottom = std::min(bottom, (staff.bottom + staves[i + 1].top) / 2);
        result.push_back(page.crop(left, top, right, bottom));
    }
    return result;
}

struct Note {
    std::string pitch;
    std::string_view kind, articulation;
    int duration = 0, dots = 0, alter = 0;
    bool accidental = false, full_rest = false;
    bool slur_start = false, slur_stop = false, tie_start = false, tie_stop = false;
};

struct Measure {
    std::vector<Note> notes;
    std::string_view bar;
    std::string clef;
    int duration = 0, fifths = 0, unit = 0, beats = 0;
    bool time_mark = false, repeat_start = false, new_system = false, partial = false;
};

struct Signature {
    std::string clef;
    int fifths = 0, unit = 0, beats = 0;
};

Note read_note(const ScoreSymbol& symbol, Measure& measure, std::map<std::string, int>& accidentals) {
    if (symbol.position != 1)
        throw std::runtime_error("Scanned music must contain one voice on a single staff");
    std::string_view duration = symbol.rhythm;
    bool is_rest = duration.starts_with("rest_");
    duration.remove_prefix(5);
    Note note;
    while (duration.ends_with('.')) {
        ++note.dots;
        duration.remove_suffix(1);
    }
    if (duration.ends_with('G') || duration.ends_with('m'))
        throw std::runtime_error("Grace notes and multi-measure rests are unsupported in scanned music");
    int reciprocal = integer(duration, "music duration");
    constexpr std::array<std::string_view, 8> types{"whole", "half", "quarter", "eighth", "16th", "32nd", "64th", "128th"};
    if (is_rest && reciprocal == 0 && note.dots == 0) {
        note.full_rest = true;
        note.kind = "whole";
    } else {
        if (reciprocal < 1 || reciprocal > 128 || (reciprocal & (reciprocal - 1)))
            throw std::runtime_error("Tuplets or this note value are unsupported in scanned music");
        int index = 0;
        while ((1 << index) != reciprocal) ++index;
        note.kind = types[index];
        note.duration = whole / reciprocal * ((2 << note.dots) - 1) / (1 << note.dots);
    }
    note.articulation = symbol.articulation;
    for (const auto& mark : split(note.articulation, '_'))
        if (!mark.empty() && mark != "." && mark != "fermata" && mark != "accent" && mark != "staccato"
            && mark != "staccatissimo" && mark != "tenuto")
            throw std::runtime_error("Unsupported scanned articulation " + mark);
    note.slur_stop = symbol.slur == 2 || symbol.slur == 4;
    note.slur_start = symbol.slur == 2 || symbol.slur == 3;
    if (is_rest) {
        if (!symbol.pitch.empty() || symbol.lift > 1 || note.slur_start || note.slur_stop ||
            (note.articulation != "_" && note.articulation != "." && note.articulation != "fermata"))
            throw std::runtime_error("Invalid rest in recognized staff");
        return note;
    }
    note.pitch = symbol.pitch;
    if (note.pitch.empty())
        throw std::runtime_error("Missing pitch in recognized staff");
    auto order = measure.fifths < 0 ? std::string_view("BEADGCF") : std::string_view("FCGDAEB");
    int implied = order.substr(0, std::abs(measure.fifths)).find(note.pitch[0]) == order.npos ? 0 : (measure.fifths < 0 ? -1 : 1);
    auto it = accidentals.try_emplace(note.pitch, implied).first;
    constexpr std::array<int, 7> alters{0, 0, 1, 2, 0, -1, -2};
    note.alter = symbol.lift > 1 ? alters[symbol.lift] : it->second;
    note.accidental = note.alter != it->second;
    it->second = note.alter;
    return note;
}

void read_measures(const std::vector<ScoreSymbol>& symbols, std::vector<Measure>& measures, Signature& signature) {
    Measure measure;
    measure.clef = signature.clef;
    measure.fifths = signature.fifths;
    measure.unit = signature.unit;
    measure.new_system = !measures.empty();
    std::map<std::string, int> accidentals;
    auto close = [&] {
        if (measure.notes.empty())
            throw std::runtime_error("Empty scanned measure");
        measures.push_back(std::move(measure));
        measure = {};
        measure.clef = signature.clef;
        measure.fifths = signature.fifths;
        measure.unit = signature.unit;
        accidentals.clear();
    };
    auto first = measures.size();
    for (const auto& symbol : symbols) {
        const std::string& rhythm = symbol.rhythm;
        if (rhythm.starts_with("note_") || rhythm.starts_with("rest_")) {
            if (measure.clef.empty())
                throw std::runtime_error("No clef was recognized before the first note");
            auto note = read_note(symbol, measure, accidentals);
            measure.duration += note.duration;
            measure.notes.push_back(std::move(note));
        } else if (rhythm.starts_with("clef_") || rhythm.starts_with("keySignature_") || rhythm.starts_with("timeSignature/")) {
            if (!measure.notes.empty())
                throw std::runtime_error("Mid-measure signature change in recognized staff");
            if (rhythm.starts_with("clef_")) {
                if (symbol.position != 1)
                    throw std::runtime_error("Scanned music must contain a single staff per system");
                measure.clef = signature.clef = rhythm.substr(5);
            } else if (rhythm.starts_with("keySignature_")) {
                measure.fifths = signature.fifths = integer(rhythm.substr(13), "key signature");
            } else {
                measure.unit = signature.unit = integer(rhythm.substr(14), "beat type");
                if ((measure.unit & (measure.unit - 1)) != 0)
                    throw std::runtime_error("Unsupported scanned time signature denominator");
                measure.time_mark = true;
            }
        } else if (rhythm == "repeatStart") {
            if (!measure.notes.empty()) close();
            measure.repeat_start = true;
        } else if (rhythm == "repeatEnd" || rhythm == "repeatEndStart") {
            measure.bar = "repeat";
            close();
            measure.repeat_start = rhythm == "repeatEndStart";
        } else if (rhythm == "barline" || rhythm == "doublebarline" || rhythm == "bolddoublebarline") {
            if (rhythm == "doublebarline") measure.bar = "light-light";
            else if (rhythm == "bolddoublebarline") measure.bar = "light-heavy";
            close();
        } else {
            throw std::runtime_error("Unsupported scanned music symbol " + rhythm);
        }
    }
    if (!measure.notes.empty()) close();
    if (measures.size() == first)
        throw std::runtime_error("No measures were recognized on the staff");
}

void measure_meter(std::vector<Measure>& measures, int inherited_beats) {
    for (std::size_t start = 0; start < measures.size();) {
        const int unit = measures[start].unit;
        if (!unit)
            throw std::runtime_error("No time signature was recognized before the first measure");
        std::size_t end = start + 1;
        while (end < measures.size() && !measures[end].time_mark) ++end;

        // This model identifies the beat type. Recover the numerator from the written durations.
        int beats = inherited_beats;
        if (measures[start].time_mark) {
            int longest = 0;
            for (auto i = start; i < end; ++i)
                longest = std::max(longest, measures[i].duration);
            if (longest * unit % whole != 0)
                throw std::runtime_error("Cannot recover the time signature from the recognized measures");
            beats = longest * unit / whole;
        }
        if (beats < 1 || beats > 24)
            throw std::runtime_error("Cannot recover the time signature from the recognized measures");

        int capacity = beats * whole / unit;
        for (auto i = start; i < end; ++i) {
            auto& measure = measures[i];
            measure.beats = beats;
            for (auto& note : measure.notes)
                if (note.full_rest) {
                    if (measure.notes.size() != 1)
                        throw std::runtime_error("A whole-measure rest must be alone in its measure");
                    measure.duration = note.duration = capacity;
                }
            measure.partial = measure.duration < capacity;
            if (measure.duration > capacity || (measure.partial && i != 0 && i + 1 != measures.size()))
                throw std::runtime_error("Recognized durations do not fill measure " + std::to_string(i + 1));
        }
        start = end;
    }
}

void infer_ties(std::vector<Measure>& measures) {
    Note* previous = nullptr;
    for (auto& measure : measures)
        for (auto& note : measure.notes) {
            if (previous && previous->slur_start && note.slur_stop && !note.pitch.empty()
                && previous->pitch == note.pitch && previous->alter == note.alter) {
                previous->slur_start = note.slur_stop = false;
                previous->tie_start = note.tie_stop = true;
            }
            previous = &note;
        }
}

void note_xml(std::ostringstream& xml, const Note& note) {
    xml << "    <note>";
    if (note.pitch.empty()) {
        xml << (note.full_rest ? "<rest measure=\"yes\"/>" : "<rest/>");
    } else {
        xml << "<pitch><step>" << note.pitch[0] << "</step>";
        if (note.alter || note.accidental) xml << "<alter>" << note.alter << "</alter>";
        xml << "<octave>" << note.pitch[1] << "</octave></pitch>";
    }
    xml << "<duration>" << note.duration << "</duration>";
    if (note.tie_stop) xml << "<tie type=\"stop\"/>";
    if (note.tie_start) xml << "<tie type=\"start\"/>";
    xml << "<type>" << note.kind << "</type>";
    for (int i = 0; i < note.dots; ++i) xml << "<dot/>";
    if (note.accidental) {
        constexpr std::array<std::string_view, 5> names{"flat-flat", "flat", "natural", "sharp", "double-sharp"};
        xml << "<accidental>" << names[note.alter + 2] << "</accidental>";
    }
    std::string marks;
    bool fermata = false;
    for (const auto& mark : split(note.articulation, '_')) {
        if (mark == "fermata") fermata = true;
        else if (!mark.empty() && mark != ".") marks += '<' + mark + "/>";
    }
    if (!marks.empty() || fermata || note.tie_stop || note.tie_start || note.slur_stop || note.slur_start) {
        xml << "<notations>";
        if (note.tie_stop) xml << "<tied type=\"stop\"/>";
        if (note.tie_start) xml << "<tied type=\"start\"/>";
        if (note.slur_stop) xml << "<slur type=\"stop\" number=\"1\"/>";
        if (note.slur_start) xml << "<slur type=\"start\" number=\"1\"/>";
        if (!marks.empty()) xml << "<articulations>" << marks << "</articulations>";
        if (fermata) xml << "<fermata/>";
        xml << "</notations>";
    }
    xml << "</note>\n";
}

std::string score_xml(const std::vector<Measure>& measures) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<score-partwise version=\"4.0\">\n"
           " <part-list><score-part id=\"P1\"><part-name>Music</part-name></score-part></part-list>\n <part id=\"P1\">\n";
    const Measure* previous = nullptr;
    int number = 0;
    for (const auto& measure : measures) {
        xml << "  <measure number=\"" << ++number << '"';
        if (measure.partial) xml << " implicit=\"yes\"";
        xml << ">\n";
        if (measure.new_system) xml << "    <print new-system=\"yes\"/>\n";
        bool key = !previous || measure.fifths != previous->fifths;
        bool time = !previous || measure.beats != previous->beats || measure.unit != previous->unit;
        bool clef = !previous || measure.clef != previous->clef;
        if (key || time || clef) {
            xml << "    <attributes>";
            if (!previous) xml << "<divisions>" << divisions << "</divisions>";
            if (key) xml << "<key><fifths>" << measure.fifths << "</fifths></key>";
            if (time) xml << "<time><beats>" << measure.beats << "</beats><beat-type>" << measure.unit << "</beat-type></time>";
            if (clef) xml << "<clef><sign>" << measure.clef[0] << "</sign><line>" << measure.clef[1] << "</line></clef>";
            xml << "</attributes>\n";
        }
        if (measure.repeat_start) xml << "    <barline location=\"left\"><repeat direction=\"forward\"/></barline>\n";
        for (const auto& note : measure.notes) note_xml(xml, note);
        if (measure.bar == "repeat") xml << "    <barline location=\"right\"><repeat direction=\"backward\"/></barline>\n";
        else if (!measure.bar.empty()) xml << "    <barline location=\"right\"><bar-style>" << measure.bar << "</bar-style></barline>\n";
        xml << "  </measure>\n";
        previous = &measure;
    }
    return xml.str() + " </part>\n</score-partwise>\n";
}

fs::path review_path(const fs::path& directory, int page) {
    auto number = std::to_string(page);
    return directory / ("page-" + std::string(3 - number.size(), '0') + number + ".musicxml");
}

void save_review(const fs::path& path, std::string_view text) {
#ifdef _WIN32
    auto* file = _wfopen(path.c_str(), L"wbx");
#else
    auto* file = std::fopen(path.c_str(), "wbx");
#endif
    if (!file)
        throw std::runtime_error("Cannot create a new MusicXML review at " + path.string());
    auto bytes = std::fwrite(text.data(), 1, text.size(), file);
    auto closed = std::fclose(file);
    if (bytes != text.size() || closed != 0) {
        std::error_code ignored;
        fs::remove(path, ignored);
        throw std::runtime_error("Cannot save MusicXML review at " + path.string());
    }
}

} // namespace

Document scanned_score(const fs::path& input, const fs::path& music_review,
                       const fs::path& output, const fs::path& text_output) {
    auto suffix = extension(input);
    bool pdf = suffix == ".pdf";
    if (!pdf && !image_extension(suffix))
        throw std::runtime_error("Music recognition needs a sheet-music PDF/image or MusicXML score");
    const auto model_dir = std::getenv("BRAILLE_OMR_MODELS");
    if (!model_dir || !*model_dir)
        throw std::runtime_error("Set BRAILLE_OMR_MODELS to the folder with the homr 0.7.0 CPU ONNX models. See README.md.");
    int count = pdf ? pdf_page_count(input) : 1;
    if (count < 1 || count > 100)
        throw std::runtime_error("Sheet music must contain 1 to 100 source pages");
    if (!music_review.empty()) {
        fs::create_directories(music_review);
        for (int page = 1; page <= count; ++page) {
            auto path = review_path(music_review, page);
            auto review = fs::weakly_canonical(path);
            if (fs::symlink_status(path).type() != fs::file_type::not_found || review == fs::weakly_canonical(output)
                || (!text_output.empty() && review == fs::weakly_canonical(text_output)))
                throw std::runtime_error("MusicXML review path already exists or is in use: " + path.string());
        }
    }
    StaffRecognizer recognizer(model_dir);
    Document result;
    Signature signature;
    for (int number = 1; number <= count; ++number) {
        try {
            const auto staves = staff_images(pdf ? render_pdf_page(input, number, 300, max_pixels) : read_image(read(input), max_pixels));
            std::vector<Measure> measures;
            for (std::size_t i = 0; i < staves.size(); ++i) {
                try {
                    read_measures(recognizer.read_staff(staves[i]), measures, signature);
                } catch (const std::exception& error) {
                    throw std::runtime_error("staff " + std::to_string(i + 1) + ": " + error.what());
                }
            }
            measure_meter(measures, signature.beats);
            signature.beats = measures.back().beats;
            infer_ties(measures);
            auto bytes = score_xml(measures);
            if (!music_review.empty()) save_review(review_path(music_review, number), bytes);
            auto page = music_document_xml(bytes);
            if (count > 1)
                result.emplace_back(Paragraph{"Source page " + std::to_string(number), 0, 0, Paragraph::heading, 2});
            for (auto& block : page) result.push_back(std::move(block));
        } catch (const std::exception& error) {
            throw std::runtime_error("Music recognition failed on source page " + std::to_string(number) + ": " + error.what());
        }
    }
    return result;
}

} // namespace braille
