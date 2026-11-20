// Music tests with literal cells checked against BANA 2015 Tables 1-7 and 10.

#include "braille.hpp"
#include "check.hpp"
#include "mupdf.hpp"

#include <leptonica/allheaders.h>
#include <sys/wait.h>
#include <zip.h>

#include <algorithm>
#include <cstdlib>
#include <optional>

using namespace braille;
using namespace native_test;

namespace {

const Cells B = U"⠀";

fs::path fixture() { return context().root / "tests/fixtures/melody.musicxml"; }

// Python's str.replace: every occurrence.
std::string replace(std::string text, std::string_view from, std::string_view to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

std::vector<std::string> split_text(const std::string& text, std::string_view separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t end; (end = text.find(separator, start)) != std::string::npos; start = end + separator.size())
        parts.push_back(text.substr(start, end - start));
    parts.push_back(text.substr(start));
    return parts;
}

std::vector<Cells> split32(const Cells& text, char32_t separator) {
    std::vector<Cells> parts;
    std::size_t start = 0;
    for (std::size_t end; (end = text.find(separator, start)) != Cells::npos; start = end + 1)
        parts.push_back(text.substr(start, end - start));
    parts.push_back(text.substr(start));
    return parts;
}

// str.splitlines(): no empty piece after the final newline.
std::vector<Cells> splitlines(const Cells& text) {
    auto lines = split32(text, U'\n');
    if (lines.back().empty()) lines.pop_back();
    return lines;
}

// The Python note() and score() helpers, with keyword arguments as designated initializers.
// A missing step is a rest.
struct NoteArgs {
    std::optional<std::string> step = "C";
    int octave = 4;
    std::string kind = "quarter", duration = "4", extra, alter = "0";
};

std::string note(const NoteArgs& a = {}) {
    auto pitch = a.step ? "<pitch><step>" + *a.step + "</step><alter>" + a.alter + "</alter><octave>" +
        std::to_string(a.octave) + "</octave></pitch>" : std::string("<rest/>");
    return "<note>" + pitch + "<duration>" + a.duration + "</duration><type>" + a.kind + "</type>" + a.extra + "</note>";
}

struct ScoreArgs {
    int beats = 4, beat_type = 4, fifths = 0;
    std::string symbol = "normal", extra_measures;
};

std::string score(const std::string& notes, const ScoreArgs& a = {}) {
    return "<?xml version=\"1.0\"?>\n"
        "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\"><part-name>Music</part-name></score-part></part-list>\n"
        "<part id=\"P1\"><measure number=\"1\"><attributes><divisions>4</divisions>\n"
        "<key><fifths>" + std::to_string(a.fifths) + "</fifths></key><time symbol=\"" + a.symbol + "\"><beats>" +
        std::to_string(a.beats) + "</beats><beat-type>" + std::to_string(a.beat_type) + "</beat-type></time>\n"
        "<clef><sign>G</sign><line>2</line></clef></attributes>" + notes + "</measure>" + a.extra_measures +
        "</part></score-partwise>";
}

// setUp's temporary directory with the source() and music() helpers.
struct Scratch {
    fs::path directory;

    explicit Scratch(const std::string& name) : directory(context().work / name) {
        fs::remove_all(directory);
        fs::create_directories(directory);
    }

    fs::path source(const std::string& xml) const {
        auto path = directory / "score.musicxml";
        write(path, xml);
        return path;
    }

    Music music(const std::string& xml) const {
        for (auto& block : import_document(source(xml), Options{}).blocks)
            if (auto found = std::get_if<Music>(&block)) return *found;
        throw std::out_of_range("no music block");  // Python's [0] raises IndexError, not ValueError
    }
};

Cells render(const Document& blocks, int width = 40, int height = 25, bool page_numbers = false,
             const std::string& table = {}) {
    Options options;
    options.width = width;
    options.height = height;
    options.page_numbers = page_numbers;
    options.table = table;
    return unicode(serialize(paginate(blocks, options)));
}

Cells convert(const fs::path& path) { return render(import_document(path, Options{}).blocks); }

// assertRaises(ValueError) and assertRaisesRegex: the converter reports these as std::runtime_error.
void raises(const std::function<void()>& body, std::initializer_list<std::string_view> fragments,
            const std::string& detail) {
    try {
        body();
    } catch (const std::runtime_error& error) {
        std::string message = error.what();
        for (auto fragment : fragments)
            check(message.find(fragment) != std::string::npos,
                  detail + ": error lacks \"" + std::string(fragment) + "\": " + message);
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(detail + ": expected a conversion error, got: " + error.what());
    }
    throw std::runtime_error(detail + ": expected a conversion error");
}

struct Run {
    int status;
    std::string error;
};

std::string quoted(const std::string& value) {
    std::string result = "'";
    for (char c : value) result += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return result + "'";
}

// subprocess.run with captured output, against the native CLI.
Run cli(const fs::path& directory, const std::vector<std::string>& args) {
    check(!context().cli.empty(), "pass the braille CLI path as the third argument");
    auto log = directory / "stderr.txt";
    std::string command = quoted(context().cli.string());
    for (const auto& arg : args) command += ' ' + quoted(arg);
    int status = std::system((command + " >/dev/null 2>" + quoted(log.string())).c_str());
    auto error = read(log);
    fs::remove(log);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, error};
}

// The CLI reports ValueError as "error: ..." with exit status 2.
void fails(const Run& run, std::string_view fragment, const std::string& detail) {
    check_equal(run.status, 2, detail + ": exit status (" + run.error + ")");
    check(run.error.find(fragment) != std::string::npos,
          detail + ": error lacks \"" + std::string(fragment) + "\": " + run.error);
}

#ifdef BRAILLE_MUSIC_SCAN
bool models() {
    auto folder = std::getenv("BRAILLE_OMR_MODELS");
    return folder && *folder;
}
#endif

// What --music must report before it can recognize anything. The default CLI has no recognizer;
// a BRAILLE_MUSIC_SCAN build needs BRAILLE_OMR_MODELS (see tests/native/music_scan.cmake).
std::string recognizer(std::string_view with_models) {
#ifdef BRAILLE_MUSIC_SCAN
    return models() ? std::string(with_models) : "BRAILLE_OMR_MODELS";
#else
    (void)with_models;
    return "BRAILLE_MUSIC_SCAN=ON";
#endif
}

// Image.new('RGB', size, 'white').save(path)
void white_picture(const fs::path& path, int width, int height) {
    PIX* pix = pixCreate(width, height, 32);
    check(pix != nullptr, "allocate picture");
    pixSetAll(pix);
    int status = pixWrite(path.c_str(), pix, IFF_PNG);
    pixDestroy(&pix);
    check(status == 0, "write " + path.string());
}

void write_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries, bool deflate) {
    int error = 0;
    auto archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    check(archive != nullptr, "create " + path.string());
    for (const auto& [name, data] : entries) {
        auto source = zip_source_buffer(archive, data.data(), data.size(), 0);
        zip_int64_t index = source ? zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) : -1;
        if (index < 0 || zip_set_file_compression(archive, index, deflate ? ZIP_CM_DEFLATE : ZIP_CM_STORE, 0) < 0) {
            if (source && index < 0) zip_source_free(source);
            zip_discard(archive);
            throw std::runtime_error("write " + name + " to " + path.string());
        }
    }
    if (zip_close(archive)) {
        zip_discard(archive);
        throw std::runtime_error("close " + path.string());
    }
}

void repeat_page(const fs::path& from, const fs::path& to, int copies) {
    PdfContext mu;
    auto source = mu.own(mu.call([&](auto ctx) { return pdf_open_document(ctx, from.c_str()); }), pdf_drop_document);
    auto target = mu.own(mu.call([](auto ctx) { return pdf_create_document(ctx); }), pdf_drop_document);
    for (int i = 0; i < copies; ++i)
        mu.call([&](auto ctx) { pdf_graft_page(ctx, target.get(), -1, source.get(), 0); });
    save_pdf(mu, target.get(), to.string());
}

} // namespace

TEST_CASE("test_literal_melody") {
    auto blocks = import_document(fixture(), Options{}).blocks;
    const auto& music = std::get<Music>(blocks.back());
    check_equal(music.signature, Cells(U"⠼⠙⠲"), "signature");
    check_equal(music.measures, Lines{U"⠼⠁⠀⠐⠹⠱⠫⠻", U"⠼⠃⠀⠐⠗⠪⠺", U"⠼⠉⠀⠨⠹⠄⠑⠫⠧", U"⠼⠙⠀⠨⠽⠣⠅"}, "measures");
}

TEST_CASE("test_note_and_rest_values_from_tables") {
    Scratch t("note_and_rest_values_from_tables");
    // Independent table rows: CDEFGAB, then the rest; divisions are 4/quarter.
    struct Row {
        std::string kind, duration;
        int beats, beat_type;
        Cells pitches, rest, value_prefix;
    };
    const Row cases[]{
        {"whole", "16", 4, 4, U"⠽⠵⠯⠿⠷⠮⠾", U"⠍", U""},
        {"half", "8", 2, 4, U"⠝⠕⠏⠟⠗⠎⠞", U"⠥", U""},
        {"quarter", "4", 1, 4, U"⠹⠱⠫⠻⠳⠪⠺", U"⠧", U""},
        {"eighth", "2", 1, 8, U"⠙⠑⠋⠛⠓⠊⠚", U"⠭", U""},
        {"16th", "1", 1, 16, U"⠽⠵⠯⠿⠷⠮⠾", U"⠍", U"⠠⠣⠂"},
        {"32nd", "0.5", 1, 32, U"⠝⠕⠏⠟⠗⠎⠞", U"⠥", U"⠠⠣⠂"},
        {"64th", "0.25", 1, 64, U"⠹⠱⠫⠻⠳⠪⠺", U"⠧", U"⠠⠣⠂"},
        {"128th", "0.125", 1, 128, U"⠙⠑⠋⠛⠓⠊⠚", U"⠭", U"⠠⠣⠂"},
    };
    for (const auto& row : cases) {
        for (std::size_t i = 0; i < 7; ++i) {
            std::string step(1, "CDEFGAB"[i]);
            auto result = t.music(score(note({.step = step, .kind = row.kind, .duration = row.duration}),
                                        {.beats = row.beats, .beat_type = row.beat_type}));
            check_equal(result.measures, Lines{U"⠼⠁" + B + row.value_prefix + U"⠐" + row.pitches.substr(i, 1) + U"⠣⠅"},
                        "kind=" + row.kind + " step=" + step);
        }
        auto result = t.music(score(note({.step = std::nullopt, .kind = row.kind, .duration = row.duration}),
                                    {.beats = row.beats, .beat_type = row.beat_type}));
        check_equal(result.measures, Lines{U"⠼⠁" + B + row.value_prefix + row.rest + U"⠣⠅"}, "kind=" + row.kind + " rest");
    }
}

TEST_CASE("test_octaves_and_melodic_intervals") {
    Scratch t("octaves_and_melodic_intervals");
    const Cells signs[]{U"⠈⠈", U"⠈", U"⠘", U"⠸", U"⠐", U"⠨", U"⠰", U"⠠", U"⠠⠠"};
    for (int octave = 0; octave < 9; ++octave)
        check_equal(t.music(score(note({.octave = octave}), {.beats = 1})).measures,
                    Lines{U"⠼⠁" + B + signs[octave] + U"⠹⠣⠅"}, "octave=" + std::to_string(octave));
    std::string melody;
    const std::vector<std::pair<std::string, int>> pitches{{"C", 4}, {"F", 4}, {"C", 5}, {"G", 4}, {"B", 4}, {"D", 5}};
    for (const auto& [step, octave] : pitches) melody += note({.step = step, .octave = octave});
    check_equal(t.music(score(melody, {.beats = 6})).measures, Lines{U"⠼⠁⠀⠐⠹⠻⠨⠹⠐⠳⠺⠱⠣⠅"}, "melodic intervals");
}

TEST_CASE("test_dots_and_whole_measure_rests") {
    Scratch t("dots_and_whole_measure_rests");
    check_equal(t.music(score(note({.duration = "7", .extra = "<dot/><dot/>"}), {.beats = 7, .beat_type = 16})).measures,
                Lines{U"⠼⠁⠀⠐⠹⠄⠄⠣⠅"}, "double-dotted quarter");
    check_equal(t.music(score("<note><rest measure=\"yes\"/><duration>12</duration></note>", {.beats = 3})).measures,
                Lines{U"⠼⠁⠀⠍⠣⠅"}, "whole-measure rest");
}

TEST_CASE("test_key_signatures_and_accidental_scope") {
    Scratch t("key_signatures_and_accidental_scope");
    auto melody = note({.step = "F", .alter = "1"}) + note({.step = "F", .extra = "<accidental>natural</accidental>"}) +
        note({.step = "F"}) + note({.step = "F", .extra = "<accidental>sharp</accidental>", .alter = "1"});
    auto following = "<measure number=\"2\">" + note({.step = "F", .kind = "whole", .duration = "16", .alter = "1"}) +
        "</measure>";
    auto result = t.music(score(melody, {.fifths = 1, .extra_measures = following}));
    check_equal(result.signature, Cells(U"⠩⠼⠙⠲"), "one sharp");
    check_equal(result.measures, Lines{U"⠼⠁⠀⠐⠻⠡⠻⠻⠩⠻", U"⠼⠃⠀⠐⠿⠣⠅"}, "accidentals last one measure");
    check_equal(t.music(score(note({.kind = "whole", .duration = "16"}), {.fifths = 4})).signature,
                Cells(U"⠼⠙⠩⠼⠙⠲"), "four sharps");
    check_equal(t.music(score(note({.kind = "whole", .duration = "16"}), {.fifths = -2})).signature,
                Cells(U"⠣⠣⠼⠙⠲"), "two flats");
    auto inferred = t.music(score(note({.step = "F", .alter = "1"}), {.beats = 1}));
    check_equal(inferred.measures, Lines{U"⠼⠁⠀⠐⠩⠐⠻⠣⠅"}, "inferred accidental");
}

TEST_CASE("test_meter_and_value_changes") {
    Scratch t("meter_and_value_changes");
    check_equal(t.music(score(note({.kind = "whole", .duration = "16"}), {.symbol = "common"})).signature,
                Cells(U"⠨⠉"), "common time");
    check_equal(t.music(score(note({.kind = "whole", .duration = "16"}), {.beats = 2, .beat_type = 2, .symbol = "cut"}))
                    .signature, Cells(U"⠸⠉"), "cut time");
    check_equal(t.music(score(note({.step = std::nullopt, .kind = "whole", .duration = "24", .extra = "<dot/>"}),
                              {.beats = 12, .beat_type = 8})).signature, Cells(U"⠼⠁⠃⠦"), "twelve-eight");
    auto melody = note({.kind = "16th", .duration = "1"}) + note({.step = "D", .kind = "eighth", .duration = "2"}) +
        note({.step = "E", .kind = "16th", .duration = "1"});
    check_equal(t.music(score(melody, {.beats = 1})).measures, Lines{U"⠼⠁⠀⠠⠣⠂⠐⠽⠘⠣⠂⠑⠠⠣⠂⠯⠣⠅"}, "value changes");
}

TEST_CASE("test_ties_across_measures_and_bad_ties") {
    Scratch t("ties_across_measures_and_bad_ties");
    auto start = note({.kind = "whole", .duration = "16",
                       .extra = "<tie type=\"start\"/><notations><tied type=\"start\"/></notations>"});
    auto end = "<measure number=\"2\">" + note({.kind = "whole", .duration = "16", .extra = "<tie type=\"stop\"/>"}) +
        "</measure>";
    check_equal(t.music(score(start, {.extra_measures = end})).measures, Lines{U"⠼⠁⠀⠐⠽⠈⠉", U"⠼⠃⠀⠐⠽⠣⠅"},
                "tie across the bar line");
    raises([&] { t.music(score(start)); }, {"unfinished tie"}, "tie at the end");
    raises([&] { t.music(score(start, {.extra_measures = replace(end, "<step>C</step>", "<step>D</step>")})); },
           {"same pitch"}, "tie to another pitch");
}

TEST_CASE("test_tied_accidental_is_not_repeated_after_the_bar_line") {
    Scratch t("tied_accidental_is_not_repeated_after_the_bar_line");
    // BANA signs: F ⠛ with half/whole value dots, sharp ⠩, fourth octave ⠐, tie ⠈⠉, final bar ⠣⠅.
    // Print shows no sign on the tied F sharp; the following F is natural by the key signature.
    auto start = note({.step = "F", .kind = "whole", .duration = "16",
                       .extra = "<accidental>sharp</accidental><tie type=\"start\"/>", .alter = "1"});
    auto bar = "<measure number=\"2\">" +
        note({.step = "F", .kind = "half", .duration = "8", .extra = "<tie type=\"stop\"/>", .alter = "1"}) +
        note({.step = "F", .kind = "half", .duration = "8"}) + "</measure>";
    check_equal(t.music(score(start, {.extra_measures = bar})).measures, Lines{U"⠼⠁⠀⠩⠐⠿⠈⠉", U"⠼⠃⠀⠐⠟⠟⠣⠅"},
                "tied accidental");
}

TEST_CASE("test_repeats_and_endings") {
    Scratch t("repeats_and_endings");
    // §17.1: start repeat ⠣⠶, end repeat ⠣⠆; endings ⠼⠂ / ⠼⠆ take dot 3 before a sign with dots 1-3.
    std::string start =
        "<barline location=\"left\"><bar-style>heavy-light</bar-style><repeat direction=\"forward\"/></barline>";
    auto first = "<measure number=\"2\"><barline location=\"left\"><ending number=\"1\" type=\"start\"/></barline>" +
        note({.step = "F", .kind = "whole", .duration = "16", .extra = "<accidental>sharp</accidental>", .alter = "1"}) +
        "<barline location=\"right\"><bar-style>light-heavy</bar-style><ending number=\"1\" type=\"stop\"/>"
        "<repeat direction=\"backward\"/></barline></measure>";
    auto second = "<measure number=\"3\"><barline location=\"left\"><ending number=\"2\" type=\"start\"/></barline>" +
        note({.step = "E", .kind = "whole", .duration = "16"}) + "</measure>";
    check_equal(t.music(score(start + note({.kind = "whole", .duration = "16"}), {.extra_measures = first + second}))
                    .measures, Lines{U"⠼⠁⠀⠣⠶⠐⠽", U"⠼⠃⠀⠼⠂⠄⠩⠐⠿⠣⠆", U"⠼⠉⠀⠼⠆⠐⠯⠣⠅"}, "repeats and endings");
}

TEST_CASE("test_triplets_dynamics_hairpins_articulations_and_fermata") {
    Scratch t("triplets_dynamics_hairpins_articulations_and_fermata");
    const std::string triplet =
        "<time-modification><actual-notes>3</actual-notes><normal-notes>2</normal-notes></time-modification>";
    auto melody =
        note({.step = "C", .kind = "eighth", .duration = "2", .extra = triplet + "<notations><tuplet type=\"start\"/></notations>"}) +
        note({.step = "D", .kind = "eighth", .duration = "2", .extra = triplet}) +
        note({.step = "E", .kind = "eighth", .duration = "2", .extra = triplet + "<notations><tuplet type=\"stop\"/></notations>"}) +
        note({.step = "F", .duration = "6"});
    auto xml = replace(score(melody, {.beats = 2}), "<divisions>4</divisions>", "<divisions>6</divisions>");
    check_equal(t.music(xml).measures, Lines{U"⠼⠁⠀⠆⠐⠙⠑⠋⠻⠣⠅"}, "triplet");  // §8.4: ⠆ before the group
    auto direction = [](const std::string& content) {
        return "<direction><direction-type>" + content + "</direction-type></direction>";
    };
    melody = direction("<dynamics><mf/></dynamics>") +
        note({.extra = "<notations><articulations><staccato/></articulations></notations>"}) +
        direction("<wedge type=\"crescendo\"/>") +
        note({.step = "D", .extra = "<notations><articulations><accent/></articulations></notations>"}) +
        note({.step = "E"}) + direction("<wedge type=\"stop\"/>") +
        note({.step = "F", .extra = "<notations><fermata/></notations>"});
    // §22.3: ⠜⠍⠋ takes dot 3 before staccato ⠦; the note after a word sign has an octave mark;
    // the hairpin end ⠜⠒ follows the last note; fermata ⠣⠇ follows its note (§22.2).
    check_equal(t.music(score(melody)).measures, Lines{U"⠼⠁⠀⠜⠍⠋⠄⠦⠐⠹⠜⠉⠨⠦⠐⠱⠫⠜⠒⠻⠣⠇⠣⠅"},
                "dynamics, hairpin, articulations and fermata");
}

TEST_CASE("test_short_and_long_slurs") {
    Scratch t("short_and_long_slurs");
    const std::string start = "<notations><slur type=\"start\"/></notations>",
                      stop = "<notations><slur type=\"stop\"/></notations>";
    auto short_slur = note({.extra = start}) + note({.step = "D"}) + note({.step = "E", .extra = stop}) + note({.step = "F"});
    check_equal(t.music(score(short_slur)).measures, Lines{U"⠼⠁⠀⠐⠹⠉⠱⠉⠫⠻⠣⠅"}, "short slur");  // §13.2
    const std::string steps = "CDEFGAB";
    std::string eighths;
    for (std::size_t i = 0; i < steps.size(); ++i)
        eighths += note({.step = std::string(1, steps[i]), .kind = "eighth", .duration = "2",
                         .extra = i == 0 ? start : i == 4 ? stop : ""});
    eighths += note({.step = "C", .octave = 5, .kind = "eighth", .duration = "2"});
    check_equal(t.music(score(eighths)).measures, Lines{U"⠼⠁⠀⠰⠃⠐⠙⠑⠋⠛⠓⠘⠆⠊⠚⠙⠣⠅"}, "long slur");  // §13.3
}

TEST_CASE("test_tempo_heading_metronome_words_and_playback_markup") {
    Scratch t("tempo_heading_metronome_words_and_playback_markup");
    std::string opening = "<direction><direction-type><words>Allegro</words></direction-type>"
        "<direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>100</per-minute>"
        "</metronome></direction-type><sound tempo=\"100\"/></direction>";
    auto later = "<measure number=\"2\"><direction><direction-type><words>rit.</words></direction-type></direction>" +
        note({.kind = "whole", .duration = "16", .extra = "<instrument id=\"P1-I1\"/>"}) + "</measure>"
        "<measure number=\"3\"><direction><direction-type><words>a tempo</words></direction-type></direction>" +
        note({.kind = "whole", .duration = "16"}) + "</measure>";
    auto music = t.music(score(opening + note({.kind = "whole", .duration = "16"}), {.extra_measures = later}));
    check_equal(music.tempo, std::string("Allegro"), "tempo");  // §1.7
    check_equal(music.metronome, Cells(U"⠹⠶⠼⠁⠚⠚"), "metronome");
    check_equal(music.measures, Lines{U"⠼⠁⠀⠐⠽", U"⠼⠃⠀⠜⠗⠊⠞⠄⠐⠽", U"⠼⠉⠀⠜⠁⠀⠞⠑⠍⠏⠕⠜⠀⠐⠽⠣⠅"}, "measures");
    // The tempo text (literary UEB), metronome mark and signatures form one centered line.
    check_equal(splitlines(render({music}, 40, 25)).at(0), Cells(10, blank) + U"⠠⠁⠇⠇⠑⠛⠗⠕⠀⠹⠶⠼⠁⠚⠚⠀⠼⠙⠲",
                "centered heading line");
}

TEST_CASE("test_mxl_container_and_namespaced_xml") {
    Scratch t("mxl_container_and_namespaced_xml");
    auto expected = convert(fixture());
    auto path = t.directory / "score.mxl";
    auto bytes = read(fixture());
    write_zip(path, {{"META-INF/container.xml",
                      "<container><rootfiles><rootfile full-path=\"scores/main.xml\"/></rootfiles></container>"},
                     {"scores/main.xml", bytes}, {"unrelated.xml", "<not-a-score/>"}}, true);
    check_equal(convert(path), expected, "MXL container");
    auto namespaced = replace(bytes, "<score-partwise version=\"4.0\">",
                              "<score-partwise xmlns=\"http://www.musicxml.org/ns/musicxml\" version=\"4.0\">");
    check_equal(convert(t.source(namespaced)), expected, "namespaced MusicXML");
    write_zip(path, {{"unrelated.xml", "<not-a-score/>"}}, false);
    raises([&] { convert(path); }, {"container.xml"}, "MXL without a container");
}

TEST_CASE("test_music_cells_bypass_literary_translation_and_paginate") {
    auto blocks = import_document(fixture(), Options{}).blocks;
    Document music{std::get<Music>(blocks.back())};
    // Python patches braille.translate to fail. Here the literary table cannot be loaded, so
    // translating any music would fail the same way. Liblouis logs that it cannot resolve it.
    const std::string unusable = "music-must-bypass-literary-translation.ctb";
    raises([&] { translate("2", unusable); }, {}, "the stand-in table rejects literary translation");
    auto result = split32(render(music, 16, 2, false, unusable), U'\f');
    check_equal(result.at(0), Cells(6, blank) + U"⠼⠙⠲\n⠼⠁⠀⠐⠹⠱⠫⠻\n", "first page");
    check_equal(result.at(1), Cells(U"⠼⠃⠀⠐⠗⠪⠺\n⠼⠉⠀⠨⠹⠄⠑⠫⠧\n"), "second page");
    auto numbered = render(music, 16, 3, true);
    auto rows = splitlines(split32(numbered, U'\f').at(1));
    check_equal(rows.at(rows.size() - 1), Cells(14, blank) + U"⠼⠃", "page number");
    raises([&] { render(music, 5); }, {"measures are not split"}, "narrow page");
    check(std::all_of(numbered.begin(), numbered.end(),
                      [](char32_t c) { return c == U'\n' || c == U'\f' || (U'⠀' <= c && c <= U'⠿'); }),
          "only six-dot cells, newlines and form feeds");
}

TEST_CASE("test_unsupported_semantics_and_inconsistent_rhythm_fail") {
    Scratch t("unsupported_semantics_and_inconsistent_rhythm_fail");
    auto base = score(note({.kind = "whole", .duration = "16"}));
    const std::vector<std::pair<std::string, std::string>> cases{
        {"chord", replace(base, "<note>", "<note><chord/>")},
        {"backup", replace(base, "<note>", "<backup><duration>4</duration></backup><note>")},
        {"empty time modification", replace(base, "<type>whole</type>", "<type>whole</type><time-modification/>")},
        {"lyric", replace(base, "<type>whole</type>", "<type>whole</type><lyric><text>la</text></lyric>")},
        {"unfinished slur",
         replace(base, "<type>whole</type>", "<type>whole</type><notations><slur type=\"start\"/></notations>")},
        {"rehearsal mark",
         replace(base, "<note>", "<direction><direction-type><rehearsal>A</rehearsal></direction-type></direction><note>")},
        {"fp dynamic",
         replace(base, "<note>", "<direction><direction-type><dynamics><fp/></dynamics></direction-type></direction><note>")},
        {"middle bar line", replace(base, "</measure>", "<barline location=\"middle\"/></measure>")},
        {"short duration", replace(base, "<duration>16</duration>", "<duration>15</duration>")},
        {"long duration", replace(base, "<duration>16</duration>", "<duration>32</duration>")},
        {"octave 9", replace(base, "<octave>4</octave>", "<octave>9</octave>")},
        {"quarter-tone alter", replace(base, "<alter>0</alter>", "<alter>0.5</alter>")},
        {"two staves", replace(base, "<clef>", "<staves>2</staves><clef>")},
    };
    for (const auto& item : cases)
        raises([&] { t.music(item.second); }, {}, item.first);
}

TEST_CASE("test_music_review_contains_notes_and_cli_writes_pdf") {
    Scratch t("music_review_contains_notes_and_cli_writes_pdf");
    auto output = t.directory / "music.pdf", review = t.directory / "notes.txt";
    auto run = cli(t.directory, {fixture().string(), output.string(), "--text-output", review.string()});
    check_equal(run.status, 0, run.error);
    check_equal(pdf_page_count(output), 1, "PDF pages");
    auto text = read(review);
    check(text.find("C5 quarter.; D5 eighth; E5 quarter; rest quarter") != std::string::npos, "review: " + text);
    check(text.find("Measure 4 (source 4): C5 whole") != std::string::npos, "review: " + text);
    write(output, "preserve this output");
    auto invalid = t.source(score(note({.kind = "whole", .duration = "15"})));
    run = cli(t.directory, {invalid.string(), output.string()});
    check_equal(run.status, 2, "invalid score: " + run.error);
    check(run.error.find("Traceback") == std::string::npos, "invalid score: " + run.error);
    check_equal(read(output), std::string("preserve this output"), "output after a failed conversion");
}

// The three recognition tests patch the Python homr subprocess. The native recognizer runs ONNX
// in process, so these cases keep what each build can observe without a stand-in engine.
TEST_CASE("test_image_recognition_keeps_review_and_ordinary_images_stay_graphics") {
    Scratch t("image_recognition_keeps_review_and_ordinary_images_stay_graphics");
    auto path = t.directory / "sheet.png";
    white_picture(path, 200, 100);
    auto blocks = import_document(path, Options{}).blocks;
    check(!blocks.empty() && std::holds_alternative<Graphic>(blocks[0]), "an ordinary image stays a graphic");
    // A review from an earlier run is never overwritten (FileExistsError in Python).
    auto folder = t.directory / "recognized";
    fs::create_directories(folder);
    auto saved = folder / "page-001.musicxml";
    auto corrected = read(fixture());
    write(saved, corrected);
    auto output = t.directory / "out.brl";
    fails(cli(t.directory, {path.string(), output.string(), "--music", "--music-review", folder.string()}),
          recognizer("review path already exists"), "existing review");
    check_equal(read(saved), corrected, "existing review after --music");
    check(!fs::exists(output), "no Braille without recognition");
}

TEST_CASE("test_recognition_failure_and_timeout_do_not_fall_back_to_text_ocr") {
    Scratch t("recognition_failure_and_timeout_do_not_fall_back_to_text_ocr");
    auto path = t.directory / "sheet.png";
    white_picture(path, 200, 100);
    auto output = t.directory / "out.brl";
    // A page without staves fails; even with --ocr, --music never falls back to text recognition.
    for (const auto& extra : std::vector<std::vector<std::string>>{{}, {"--ocr"}}) {
        std::vector<std::string> args{path.string(), output.string(), "--music"};
        args.insert(args.end(), extra.begin(), extra.end());
        fails(cli(t.directory, args), recognizer("Music recognition failed"), "--music " + join(extra, " "));
        check(!fs::exists(output), "no fallback output with --music " + join(extra, " "));
    }
    // The native decoder's two-minute staff limit runs inside ONNX inference and has no stand-in here.
}

TEST_CASE("test_multi_page_pdf_keeps_both_score_sections_and_review_files") {
    Scratch t("multi_page_pdf_keeps_both_score_sections_and_review_files");
    // Native recognition needs real staves, so both pages repeat the printed Frère Jacques fixture.
    auto source = t.directory / "sheet.pdf";
    repeat_page(context().root / "tests/fixtures/frere-jacques.pdf", source, 2);
    check_equal(pdf_page_count(source), 2, "source pages");
    auto folder = t.directory / "review", output = t.directory / "out.brl", notes = t.directory / "notes.txt";
    auto run = cli(t.directory, {source.string(), output.string(), "--music", "--music-review", folder.string(),
                                 "--text-output", notes.string()});
    std::vector<std::string> saved;
    if (fs::exists(folder))
        for (const auto& entry : fs::directory_iterator(folder)) saved.push_back(entry.path().filename().string());
    std::sort(saved.begin(), saved.end());
#ifdef BRAILLE_MUSIC_SCAN
    if (models()) {
        check_equal(run.status, 0, run.error);
        std::vector<std::string> sections;
        int scores = 0;
        for (const auto& item : split_text(read(notes), "\n\n")) {
            if (item.starts_with("Source page")) sections.push_back(item);
            scores += item.starts_with("Key signature:");
        }
        check_equal(sections, std::vector<std::string>{"Source page 1", "Source page 2"}, "source page sections");
        check_equal(scores, 2, "music sections");
        check_equal(saved, std::vector<std::string>{"page-001.musicxml", "page-002.musicxml"}, "review files");
        return;
    }
#endif
    fails(run, recognizer(""), "two-page PDF");
    check(saved.empty(), "no review files without recognition");
    check(!fs::exists(output), "no Braille without recognition");
}

TEST_CASE("test_parts_are_kept_separate_and_pickup_number_is_preserved") {
    Scratch t("parts_are_kept_separate_and_pickup_number_is_preserved");
    auto xml = replace(score(note(), {.beats = 1}), "number=\"1\"", "number=\"0\" implicit=\"yes\"");
    auto begin = xml.find("<part id="), stop = xml.find("</score-partwise>");
    check(begin != std::string::npos && stop != std::string::npos, "score layout");
    auto part = xml.substr(begin, stop - begin);
    xml = replace(xml, "</score-partwise>", replace(part, "id=\"P1\"", "id=\"P2\"") + "</score-partwise>");
    std::vector<Lines> music;
    for (const auto& block : import_document(t.source(xml), Options{}).blocks)
        if (auto found = std::get_if<Music>(&block)) music.push_back(found->measures);
    check_equal(music.size(), std::size_t(2), "music blocks");
    check_equal(music, std::vector<Lines>(2, Lines{U"⠼⠚⠀⠐⠹⠣⠅"}), "measures");
}

TEST_CASE("test_signature_changes_multiple_voices_and_malformed_xml_fail") {
    Scratch t("signature_changes_multiple_voices_and_malformed_xml_fail");
    auto first = note({.kind = "whole", .duration = "16"});
    auto second = "<measure number=\"2\"><attributes><key><fifths>1</fifths></key></attributes>" + first + "</measure>";
    // §§6.5, 7.1: a change is written where printed, with a space each side; naturals cancel first.
    check_equal(t.music(score(first, {.extra_measures = second})).measures, Lines{U"⠼⠁⠀⠐⠽", U"⠼⠃⠀⠩⠀⠐⠽⠣⠅"},
                "key change");
    auto cancel = replace(second, "<fifths>1</fifths>", "<fifths>0</fifths>");
    check_equal(t.music(score(first, {.fifths = 1, .extra_measures = cancel})).measures.at(1),
                Cells(U"⠼⠃⠀⠡⠀⠐⠽⠣⠅"), "cancelled key");
    auto meter = "<measure number=\"2\"><attributes><time><beats>3</beats><beat-type>4</beat-type></time></attributes>" +
        note({.kind = "half", .duration = "12", .extra = "<dot/>"}) + "</measure>";
    check_equal(t.music(score(first, {.extra_measures = meter})).measures.at(1), Cells(U"⠼⠃⠀⠼⠉⠲⠀⠐⠝⠄⠣⠅"),
                "time change");
    raises([&] {
        t.music(score(note({.kind = "half", .duration = "8"}) + "<attributes><key><fifths>1</fifths></key></attributes>" +
                      note({.kind = "half", .duration = "8"})));
    }, {"mid-measure attribute changes"}, "mid-measure key change");
    raises([&] { t.music(score(note({.extra = "<voice>1</voice>"}) + note({.extra = "<voice>2</voice>"}), {.beats = 2})); },
           {"multiple voices"}, "two voices");
    const std::vector<std::pair<std::string, std::string>> malformed{
        {"unclosed score", "<score-partwise>"},
        {"empty divisions", replace(score(first), "<divisions>4</divisions>", "<divisions/>")},
        {"zero denominator", replace(score(first), "<duration>16</duration>", "<duration>1/0</duration>")},
    };
    for (const auto& item : malformed)
        raises([&] { t.music(item.second); }, {}, item.first);
}

TEST_CASE("test_music_option_conflicts_are_clear") {
    Scratch t("music_option_conflicts_are_clear");
    auto output = (t.directory / "out.brl").string();
    fails(cli(t.directory, {fixture().string(), output, "--images-only"}), "images-only", "MusicXML with --images-only");
    fails(cli(t.directory, {fixture().string(), output, "--music", "--music-review", (t.directory / "review").string()}),
          "PDF/image input", "MusicXML with --music-review");
    fails(cli(t.directory, {(t.directory / "text.txt").string(), output, "--music"}), "sheet-music PDF/image",
          "text with --music");
}

// Native-only: review text quotes direction words as the Python reference's repr()
// did (it printed text "Don't rush" and text 'Slowly').
TEST_CASE("native_review_quotes_words_like_the_reference") {
    Scratch scratch("review-quotes");
    auto words = [](const std::string& text) {
        return "<direction><direction-type><words>" + text + "</words></direction-type></direction>";
    };
    auto whole = note({.kind = "whole", .duration = "16"});
    auto text = review(import_document(scratch.source(score(words("Don&apos;t  rush") + whole)), Options{}).blocks);
    check(text.find("text \"Don't rush\"; C4 whole") != std::string::npos, "Apostrophe uses double quotes: " + text);
    text = review(import_document(scratch.source(score(words("Slowly") + whole)), Options{}).blocks);
    check(text.find("text 'Slowly'; C4 whole") != std::string::npos, "Plain words use single quotes: " + text);
}

// Native-only: slur errors keep the Python reference's wording (its music_slurs()).
TEST_CASE("native_slur_errors_match_the_reference") {
    Scratch scratch("slur-errors");
    auto slur = [](const std::string& types) {
        std::string marks;
        for (auto type : split(types, ' ')) marks += "<slur type=\"" + type + "\"/>";
        return note({.extra = "<notations>" + marks + "</notations>"});
    };
    auto rest = note({.step = std::nullopt});
    expect_error([&] { scratch.music(score(slur("start stop") + rest + rest + rest)); },
                 {"slurs that meet or overlap on one note are unsupported"}, "start and stop on one note");
    expect_error([&] { scratch.music(score(slur("start") + slur("start") + slur("stop") + rest)); },
                 {"overlapping slurs are unsupported"}, "second start inside a slur");
}

int main(int argc, char** argv) { return native_test::run(argc, argv); }
