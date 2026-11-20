#include "xml.hpp"

#include <unicode/uchar.h>

#include <algorithm>
#include <array>
#include <compare>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

namespace braille {
namespace {
using namespace xml;

class Fraction {
public:
    Fraction(std::int64_t a = 0, std::int64_t b = 1) : n(a), d(b) {
        if (!d || n < -limit || n > limit || d < -limit || d > limit)
            throw std::runtime_error("Invalid MusicXML numeric value or excessive precision");
        if (d < 0) { n = -n; d = -d; }
        auto factor = std::gcd(n, d);
        n /= factor;
        d /= factor;
    }

    bool operator==(const Fraction&) const = default;
    auto operator<=>(const Fraction& value) const { return n * value.d <=> value.n * d; }
    Fraction operator+(const Fraction& value) const { return {n * value.d + value.n * d, d * value.d}; }
    Fraction operator-(const Fraction& value) const { return *this + Fraction(-value.n, value.d); }
    Fraction operator*(const Fraction& value) const { return {n * value.n, d * value.d}; }
    Fraction operator/(const Fraction& value) const { return {n * value.d, d * value.n}; }

    int pitch_alteration() const {
        if (d != 1 || n < -2 || n > 2)
            throw std::runtime_error("Unsupported pitch or microtonal accidental");
        return static_cast<int>(n);
    }

private:
    static constexpr std::int64_t limit = 2'000'000'000;
    std::int64_t n, d;
};

Fraction parse_fraction(const std::string& text_value) {
    auto slash = text_value.find('/');
    auto decimal = text_value.find('.');
    if (slash != std::string::npos)
        return {integer(text_value.substr(0, slash), "MusicXML number"),
                integer(text_value.substr(slash + 1), "MusicXML denominator")};
    if (decimal == std::string::npos)
        return integer(text_value, "MusicXML number");
    auto digits = text_value;
    digits.erase(decimal, 1);
    std::int64_t denominator = 1;
    for (std::size_t i = decimal + 1; i < text_value.size(); ++i) {
        denominator *= 10;
        if (denominator > 1'000'000'000)
            throw std::runtime_error("Invalid MusicXML numeric value or excessive precision");
    }
    return {integer(digits, "MusicXML number"), denominator};
}

// Any number the integer arithmetic cannot represent is reported like the Python reference's Fraction().
Fraction fraction(const std::string& text_value) {
    try {
        return parse_fraction(text_value);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid MusicXML numeric value " + repr(text_value));
    }
}

void allowed(Node node, std::initializer_list<std::string_view> tags, const std::string& where) {
    std::set<std::string> names;
    for (auto item : children(node))
        if (std::find(tags.begin(), tags.end(), name(item)) == tags.end())
            names.emplace(name(item));
    if (!names.empty())
        throw std::runtime_error(where + ": unsupported music notation: " + join({names.begin(), names.end()}, ", "));
}

const Cells upper_digits = U"⠚⠁⠃⠉⠙⠑⠋⠛⠓⠊", lower_digits = U"⠴⠂⠆⠒⠲⠢⠖⠶⠦⠔";
const std::array<Cells, 9> octaves{U"⠈⠈", U"⠈", U"⠘", U"⠸", U"⠐", U"⠨", U"⠰", U"⠠", U"⠠⠠"};
const std::array<Cells, 5> signs{U"⠣⠣", U"⠣", U"⠡", U"⠩", U"⠩⠩"};
const std::map<std::string, int> accidentals{{"flat-flat", -2}, {"double-flat", -2}, {"flat", -1}, {"natural", 0},
    {"sharp", 1}, {"double-sharp", 2}, {"sharp-sharp", 2}};
const std::map<std::string, Cells> dynamics{{"pp", U"⠏⠏"}, {"p", U"⠏"}, {"mp", U"⠍⠏"}, {"mf", U"⠍⠋"},
    {"f", U"⠋"}, {"ff", U"⠋⠋"}, {"sf", U"⠎⠋"}, {"sfz", U"⠎⠋⠵"}};
const std::array<std::pair<std::string_view, std::u32string_view>, 5> articulations{{
    {"staccato", U"⠦"}, {"staccatissimo", U"⠠⠦"}, {"accent", U"⠨⠦"}, {"tenuto", U"⠸⠦"}, {"strong-accent", U"⠰⠦"}}};
constexpr std::string_view steps = "CDEFGAB";
const Cells notes = U"⠙⠑⠋⠛⠓⠊⠚";

struct Duration {
    Fraction beat;
    int mask;
    char32_t rest;
    bool small = false;
};

const std::map<std::string, Duration> durations{
    {"whole", {4, 36, U'⠍'}}, {"half", {2, 4, U'⠥'}}, {"quarter", {1, 32, U'⠧'}}, {"eighth", {{1, 2}, 0, U'⠭'}},
    {"16th", {{1, 4}, 36, U'⠍', true}}, {"32nd", {{1, 8}, 4, U'⠥', true}},
    {"64th", {{1, 16}, 32, U'⠧', true}}, {"128th", {{1, 32}, 0, U'⠭', true}}};

bool digits(std::string_view number) {
    return !number.empty() && std::all_of(number.begin(), number.end(), [](char ch) { return '0' <= ch && ch <= '9'; });
}

Cells number(const std::string& value, bool lower = false) {
    if (!digits(value))
        throw std::runtime_error("MusicXML measure labels must be nonnegative integers");
    Cells result = lower ? U"" : U"⠼";
    auto& table = lower ? lower_digits : upper_digits;
    for (auto ch : value)
        result += table[ch - '0'];
    return result;
}

Cells key_sign(int count, char32_t sign) {
    return count < 4 ? Cells(count, sign) : number(std::to_string(count)) + sign;
}

Cells time_sign(int beats, int unit, const std::string& symbol) {
    if (symbol == "common") return U"⠨⠉";
    if (symbol == "cut") return U"⠸⠉";
    return number(std::to_string(beats)) + number(std::to_string(unit), true);
}

Cells dot_three(Cells sign, const Cells& following, bool ending = false) {
    if (!following.empty() && (ending || (!following.starts_with(U'⠜') && !sign.ends_with(U'⠄')))
        && ((following.front() - blank) & 7))
        sign += U'⠄';
    return sign;
}

std::string collapse(const std::string& value) {
    std::istringstream in(value);
    std::vector<std::string> words;
    for (std::string part; in >> part;)
        words.push_back(std::move(part));
    return join(words, " ");
}

Cells expression(std::string words, const std::string& where) {
    const Cells letters = U"⠁⠃⠉⠙⠑⠋⠛⠓⠊⠚⠅⠇⠍⠝⠕⠏⠟⠗⠎⠞⠥⠧⠺⠭⠽⠵";
    Cells result = U"⠜";
    for (char c : words) {
        if ('A' <= c && c <= 'Z') c += 'a' - 'A';
        if (c == ' ') result += blank;
        else if (c == '.') result += U'⠄';
        else if (c >= 'a' && c <= 'z') result += letters[c - 'a'];
        else {
            Cells lower;  // the Python reference reported the lowercased text
            for (auto ch : unicode(words))
                lower += static_cast<char32_t>(u_tolower(static_cast<UChar32>(ch)));
            throw std::runtime_error(where + ": music text must use unaccented letters, spaces and periods: " + repr(utf8(lower)));
        }
    }
    if (words.find(' ') != std::string::npos) result += U'⠜';
    return result;
}

using Slurs = std::map<Node, std::pair<int, int>>;

Slurs slurs(Node part, const std::string& label) {
    Slurs result;
    std::vector<Node> current;
    for (auto note : descendants(part, "note")) {
        int starts = 0, stops = 0, more = 0;
        for (auto group : children(note, "notations"))
            for (auto mark : children(group, "slur")) {
                auto type = attribute(mark, "type");
                if (type == "start") ++starts;
                else if (type == "stop") ++stops;
                else if (type == "continue") ++more;
                else throw std::runtime_error(label + ": unsupported slur");
            }
        if (child(note, "rest")) {
            if (starts || stops || more) throw std::runtime_error(label + ": slurs on rests are unsupported");
            continue;
        }
        if (starts > 1 || (starts && stops))
            throw std::runtime_error(label + ": slurs that meet or overlap on one note are unsupported");
        if (starts && !current.empty())
            throw std::runtime_error(label + ": overlapping slurs are unsupported");
        if (starts || !current.empty()) current.push_back(note);
        if (stops) {
            if (current.empty()) throw std::runtime_error(label + ": a slur ends without a start");
            for (std::size_t i = 0; i < current.size(); ++i)
                result.emplace(current[i], std::pair{i, current.size()});
            current.clear();
        }
    }
    if (!current.empty())
        throw std::runtime_error(label + ": unfinished slur");
    return result;
}

struct Measure {
    std::string label;
    Lines units, opening, cells;
    Cells end;
    std::vector<std::string> words;
    Fraction elapsed;
    std::optional<std::pair<int, int>> previous;
    std::map<std::pair<int, int>, int> accidental;
};

class Melody {
public:
    Melody(Node part, std::string label) : label_(std::move(label)), slurs_(slurs(part, label_)) {
        allowed(part, {"measure"}, label_);
        int index = 0;
        for (auto source : children(part, "measure")) {
            Measure measure;
            measure.label = attribute(source, "number", std::to_string(++index));
            where_ = label_ + ", measure " + measure.label;
            allowed(source, {"attributes", "note", "print", "barline", "direction", "sound"}, where_);
            for (auto node : children(source)) {
                if (is(node, "attributes")) attributes(node, measure);
                else if (is(node, "barline")) barline(node, measure);
                else if (is(node, "direction")) direction(node, measure);
                else if (is(node, "note")) add_note(node, measure);
            }
            if (measure.cells.empty()) fail("empty measures are unsupported; include rests");
            Fraction capacity(4LL * beats_, unit_);
            if (measure.elapsed > capacity || (measure.elapsed != capacity && attribute(source, "implicit") != "yes"))
                fail("durations do not fill the time signature; mark pickup measures implicit=yes");
            if (signature_.empty()) {
                signature_ = key_sign(std::abs(fifths_), fifths_ > 0 ? U'⠩' : U'⠣') + time_sign(beats_, unit_, symbol_);
                review_.push_back("Key signature: " + std::to_string(fifths_) + " fifths. Time: " +
                                  std::to_string(beats_) + "/" + std::to_string(unit_) + ".");
            }
            if (!digits(measure.label)) fail("measure labels must be nonnegative integers");
            review_.push_back("Measure " + std::to_string(index) + " (source " + measure.label + "): " + join(measure.words, "; "));
            built_.push_back(std::move(measure));
        }
        where_ = label_;
        if (built_.empty()) fail("no measures");
        if (tie_) fail("unfinished tie at the end of the score");
        if (!said_.empty() || tuplet_ || !wedge_.empty())
            fail("a dynamic, expression, hairpin or tuplet has no following note");
    }

    Music result() const {
        Lines rows;
        for (const auto& measure : built_) {
            auto nonzero = measure.label.find_first_not_of('0');
            Cells row = number(nonzero == std::string::npos ? "0" : measure.label.substr(nonzero)) + blank;
            for (auto& unit : measure.units) row += unit + blank;
            for (std::size_t i = 0; i < measure.opening.size(); ++i) {
                const auto& sign = measure.opening[i];
                const auto& next = i + 1 < measure.opening.size() ? measure.opening[i + 1] : measure.cells.front();
                row += sign.starts_with(U'⠼') ? dot_three(sign, next, true) : sign;
            }
            for (auto& note : measure.cells) row += note;
            row += (&measure == &built_.back() && measure.end != U"⠣⠆") ? U"⠣⠅" : measure.end;
            rows.push_back(std::move(row));
        }
        auto tempo = join(tempo_, " ");
        auto readable = review_;
        if (!tempo.empty() || !metronome_.empty())
            readable.insert(readable.begin(), "Tempo: " + tempo + (tempo.empty() || metronome_.empty() ? "" : " ") + utf8(metronome_));
        return Music{signature_, std::move(rows), join(readable, "\n"), tempo, metronome_};
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(where_ + ": " + message);
    }

    int count(Node node, std::string_view tag, const char* fallback = "") const {
        return integer(text(node, tag, fallback), "MusicXML " + std::string(tag));
    }

    void attributes(Node node, Measure& measure) {
        allowed(node, {"divisions", "key", "time", "clef", "staves"}, where_);
        if (!measure.cells.empty()) fail("mid-measure attribute changes are unsupported");
        if (child(node, "divisions")) {
            divisions_ = fraction(text(node, "divisions"));
            if (divisions_ <= 0) fail("divisions must be positive");
        }
        if (text(node, "staves", "1") != "1")
            fail("use a single-staff melody; grand staves are unsupported");
        for (auto clef : children(node, "clef")) {
            allowed(clef, {"sign", "line", "clef-octave-change"}, where_);
            auto sign = text(clef, "sign");
            if ((sign != "G" && sign != "F" && sign != "C") || text(clef, "clef-octave-change", "0") != "0")
                fail("percussion or octave-transposing clefs are unsupported");
        }
        Cells change;
        for (auto key : children(node, "key")) {
            allowed(key, {"cancel", "fifths", "mode"}, where_);
            int new_fifths = count(key, "fifths", "0");
            if (new_fifths < -7 || new_fifths > 7) fail("invalid key signature");
            if (!signature_.empty() && new_fifths != fifths_) {
                int cancel = count(key, "cancel", "0");
                if (cancel < -7 || cancel > 7) fail("invalid key cancellation");
                int naturals = cancel ? std::abs(cancel) : new_fifths == 0 ? std::abs(fifths_) : 0;
                change += key_sign(naturals, U'⠡') + key_sign(std::abs(new_fifths), new_fifths > 0 ? U'⠩' : U'⠣');
                measure.words.push_back("key change to " + std::to_string(new_fifths) + " fifths");
            }
            fifths_ = new_fifths;
        }
        for (auto time : children(node, "time")) {
            allowed(time, {"beats", "beat-type"}, where_);
            if (children(time, "beats").size() != 1 || children(time, "beat-type").size() != 1)
                fail("combined time signatures are unsupported");
            int beats = count(time, "beats"), unit = count(time, "beat-type");
            if (beats < 1 || unit < 1 || unit > 128 || (unit & (unit - 1))) fail("invalid time signature");
            auto symbol = attribute(time, "symbol", "normal");
            if ((symbol != "common" && symbol != "cut" && symbol != "normal")
                || (symbol == "common" && (beats != 4 || unit != 4)) || (symbol == "cut" && (beats != 2 || unit != 2)))
                fail("unsupported time-signature symbol");
            if (!signature_.empty() && std::tie(beats, unit, symbol) != std::tie(beats_, unit_, symbol_)) {
                change += time_sign(beats, unit, symbol);
                measure.words.push_back("time change to " + std::to_string(beats) + "/" + std::to_string(unit));
            }
            beats_ = beats;
            unit_ = unit;
            symbol_ = std::move(symbol);
        }
        if (!change.empty()) measure.units.push_back(std::move(change));
    }

    void barline(Node node, Measure& measure) {
        allowed(node, {"bar-style", "repeat", "ending"}, where_);
        auto location = attribute(node, "location", "right"), style = text(node, "bar-style", "regular");
        auto repeat = child(node, "repeat"), ending = child(node, "ending");
        if (!std::set<std::string>{"regular", "light-light", "light-heavy", "heavy-heavy", "heavy-light"}.contains(style))
            fail("unsupported bar line " + style);
        if (location == "left") {
            if (repeat && attribute(repeat, "direction") != "forward")
                fail("a backward repeat must be at the end of a measure");
            if (!repeat && style != "regular") fail("non-right double bars are unsupported");
            if (ending) {
                static const std::regex digits_pattern(R"([0-9]+)");
                auto label = attribute(ending, "number");
                Lines endings;
                for (std::sregex_iterator it(label.begin(), label.end(), digits_pattern), end; it != end; ++it)
                    endings.push_back(number(it->str(), true));
                if (attribute(ending, "type") != "start" || endings.empty() || endings.size() > 2) fail("unsupported ending");
                measure.opening.push_back(U"⠼" + endings[0] + (endings.size() == 2 ? U"⠤" + endings[1] : U""));
                measure.words.push_back("ending " + label);
            }
            if (repeat) {
                measure.opening.emplace_back(U"⠣⠶");
                measure.words.emplace_back("start repeat");
            }
        } else if (location == "right") {
            auto type = attribute(ending, "type");
            if (ending && type != "stop" && type != "discontinue") fail("unsupported ending");
            if (repeat) {
                if (attribute(repeat, "direction") != "backward")
                    fail("a forward repeat must be at the start of a measure");
                measure.end = U"⠣⠆";
                measure.words.emplace_back("end repeat");
            } else if (style != "regular") {
                measure.end = style == "light-light" ? U"⠣⠅⠄" : U"⠣⠅";
            }
        } else {
            fail("bar lines inside a measure are unsupported");
        }
    }

    void direction(Node node, Measure& measure) {
        allowed(node, {"direction-type", "offset", "sound", "staff", "voice"}, where_);
        if (text(node, "staff", "1") != "1") fail("directions for other staves are unsupported");
        for (auto group : children(node, "direction-type")) {
            allowed(group, {"dynamics", "wedge", "words", "metronome"}, where_);
            for (auto item : children(group)) {
                if (is(item, "dynamics")) {
                    auto marks = children(item);
                    auto mark = marks.size() == 1 ? std::string(name(marks[0])) : "text";
                    if (!dynamics.contains(mark)) fail("unsupported dynamic " + mark);
                    said_.push_back(U"⠜" + dynamics.at(mark));
                    measure.words.push_back(mark);
                } else if (is(item, "wedge")) {
                    auto kind = attribute(item, "type");
                    if ((kind == "crescendo" || kind == "diminuendo") && wedge_.empty()) {
                        wedge_ = kind;
                        said_.push_back(kind == "crescendo" ? U"⠜⠉" : U"⠜⠙");
                        measure.words.push_back(kind);
                    } else if (kind == "stop" && !wedge_.empty()) {
                        if (last_note_) {
                            auto [index, offset] = *last_note_;
                            auto& previous = index == built_.size() ? measure : built_.at(index);
                            previous.cells.at(offset) += wedge_ == "crescendo" ? U"⠜⠒" : U"⠜⠲";
                        }
                        measure.words.push_back("end " + wedge_);
                        wedge_.clear();
                    } else fail("unsupported or overlapping hairpin");
                } else if (is(item, "words")) {
                    auto label = collapse(content(item));
                    if (label.empty()) continue;
                    if (built_.empty() && measure.cells.empty())
                        tempo_.push_back(label);
                    else if (label.find(' ') == std::string::npos)
                        said_.insert(said_.begin(), expression(label, where_));
                    else if (measure.cells.empty())
                        measure.units.push_back(expression(label, where_));
                    else
                        said_.insert(said_.begin(), U"⠐" + Cells(1, blank) + expression(label, where_) + blank);
                    measure.words.push_back("text " + repr(label));
                } else {
                    allowed(item, {"beat-unit", "beat-unit-dot", "per-minute"}, where_);
                    auto unit = text(item, "beat-unit"), rate = text(item, "per-minute");
                    if (!durations.contains(unit) || !digits(rate)) fail("unsupported metronome mark");
                    auto mark = Cells(1, U'⠙' | durations.at(unit).mask) + Cells(children(item, "beat-unit-dot").size(), U'⠄')
                        + U"⠶" + number(std::to_string(integer(rate, "metronome rate")));
                    if (built_.empty() && measure.cells.empty()) metronome_ = mark;
                    else if (measure.cells.empty()) measure.units.push_back(U"⠠⠄" + mark + U"⠠⠄");
                    else fail("metronome marks inside a measure are unsupported");
                    measure.words.push_back(unit + " = " + rate);
                }
            }
        }
    }

    struct Tuplet { int actual, normal; Fraction remaining; };

    Cells timing(Node node, Measure& measure, const std::string& kind, int dots, bool full_rest) {
        if (!unit_ || divisions_ == 0) fail("supply divisions and a time signature before notes");
        if (dots > 3 || (full_rest && dots)) fail("note duration disagrees with its type/dots/tuplet");
        Fraction duration = fraction(text(node, "duration", "0")) / divisions_, ratio = 1;
        int actual = 0, normal = 0;
        auto modification = child(node, "time-modification");
        if (modification) {
            allowed(modification, {"actual-notes", "normal-notes", "normal-type"}, where_);
            actual = count(modification, "actual-notes", "0");
            normal = count(modification, "normal-notes", "0");
            if (actual < 2 || actual > 99 || normal < 1 || normal > 99 || full_rest) fail("unsupported tuplet");
            ratio = Fraction(normal, actual);
        }
        const auto& value = durations.at(kind);
        auto expected = full_rest ? Fraction(4LL * beats_, unit_) : value.beat * Fraction((2 << dots) - 1, 1 << dots) * ratio;
        if (duration <= 0 || duration != expected) fail("note duration disagrees with its type/dots/tuplet");
        measure.elapsed = measure.elapsed + duration;
        bool starts = false, stops = false;
        for (auto group : children(node, "notations"))
            for (auto mark : children(group, "tuplet")) {
                auto type = attribute(mark, "type");
                starts |= type == "start";
                stops |= type == "stop";
            }
        Cells signs;
        if (modification) {
            if (!tuplet_ || starts) {
                if (tuplet_) fail("nested or unfinished tuplets are unsupported");
                signs = actual == 3 ? U"⠆" : U"⠸" + number(std::to_string(actual), true) + U"⠄";
                auto unit = durations.find(text(modification, "normal-type", kind));
                tuplet_ = Tuplet{actual, normal, (unit == durations.end() ? value : unit->second).beat * normal};
                measure.words.push_back("group of " + std::to_string(actual));
            } else if (tuplet_->actual != actual || tuplet_->normal != normal) fail("mixed tuplets are unsupported");
            tuplet_->remaining = tuplet_->remaining - duration;
            if (tuplet_->remaining <= 0 || stops) tuplet_.reset();
        } else if (tuplet_) fail("a tuplet ends before its group is complete");
        return signs;
    }

    void add_note(Node node, Measure& measure) {
        allowed(node, {"pitch", "rest", "duration", "type", "dot", "voice", "staff", "accidental", "notations", "tie",
                      "stem", "beam", "time-modification", "instrument", "play", "listen"}, where_);
        if (attribute(node, "print-object", "yes") != "yes" || text(node, "staff", "1") != "1")
            fail("hidden notes or multiple staves are unsupported");
        auto voice = text(node, "voice", "1");
        if (!voice_.empty() && voice != voice_) fail("multiple voices are unsupported; supply one melody");
        voice_ = std::move(voice);
        auto pitch = child(node, "pitch"), rest = child(node, "rest"), accidental = child(node, "accidental");
        if (bool(pitch) == bool(rest)) fail("each note must have one pitch or rest");
        bool full_rest = rest && attribute(rest, "measure") == "yes";
        auto kind = full_rest ? "whole" : text(node, "type");
        if (!durations.contains(kind)) fail("unsupported or missing note type " + repr(kind));
        auto value = durations.at(kind);
        int dots = children(node, "dot").size();
        auto group = timing(node, measure, kind, dots, full_rest);
        Cells value_sign = value.small == small_ ? U"" : value.small ? U"⠠⠣⠂" : U"⠘⠣⠂";
        small_ = value.small;
        auto ties = children(node, "tie");
        std::vector<std::string> marks;
        Cells fermata;
        for (auto group : children(node, "notations")) {
            allowed(group, {"tied", "tuplet", "articulations", "fermata", "slur"}, where_);
            auto more = children(group, "tied");
            ties.insert(ties.end(), more.begin(), more.end());
            for (auto a : children(group, "articulations")) {
                allowed(a, {"staccato", "staccatissimo", "accent", "tenuto", "strong-accent"}, where_);
                for (auto mark : children(a)) marks.emplace_back(name(mark));
            }
            for (auto mark : children(group, "fermata")) {
                auto form = strip(content(mark));
                if (!form.empty() && form != "normal") fail("unsupported fermata shape");
                fermata = U"⠣⠇";
            }
        }
        bool tie_start = false, tie_stop = false;
        for (auto tie : ties) {
            auto type = attribute(tie, "type");
            if (type == "start") tie_start = true;
            else if (type == "stop") tie_stop = true;
            else fail("unsupported tie");
        }
        Cells tail = Cells(dots, U'⠄') + fermata;
        std::string description;
        if (rest) {
            if (tie_ || tie_start || tie_stop || accidental || !marks.empty()) fail("invalid rest or interrupted tie");
            allowed(rest, {"display-step", "display-octave"}, where_);
            measure.cells.push_back(value_sign + group + value.rest + tail);
            description = full_rest ? "whole-measure rest" : "rest " + kind;
        } else {
            allowed(pitch, {"step", "alter", "octave"}, where_);
            auto step = text(pitch, "step");
            auto index = steps.find(step);
            int octave = count(pitch, "octave", "-1");
            int alter = fraction(text(pitch, "alter", "0")).pitch_alteration();
            if (step.size() != 1 || index == steps.npos || octave < 0 || octave > 8) fail("unsupported pitch or microtonal accidental");
            auto current = std::tuple{static_cast<int>(index), octave, alter};
            if ((tie_stop && tie_ != current) || (!tie_stop && tie_)) fail("tie does not continue the same pitch");
            if (tie_start) tie_ = current; else tie_.reset();
            auto order = fifths_ > 0 ? std::string_view("FCGDAEB") : std::string_view("BEADGCF");
            int implied = order.substr(0, std::abs(fifths_)).find(step) != std::string_view::npos ? (fifths_ > 0 ? 1 : -1) : 0;
            auto [at, inserted] = measure.accidental.try_emplace({index, octave}, implied);
            Cells prefix;
            if (accidental) {
                auto form = strip(content(accidental));
                if (!accidentals.contains(form) || accidentals.at(form) != alter || attribute(accidental, "cautionary") == "yes"
                    || attribute(accidental, "parentheses") == "yes" || attribute(accidental, "bracket") == "yes")
                    fail("unsupported or inconsistent accidental");
                prefix = signs[alter + 2];
                at->second = alter;
            } else if (!tie_stop && at->second != alter) {
                prefix = U"⠐" + signs[alter + 2];
                at->second = alter;
            }
            int position = octave * 7 + static_cast<int>(index);
            if (!said_.empty() || !measure.previous || std::abs(position - measure.previous->first) > 4 ||
                (std::abs(position - measure.previous->first) >= 3 && octave != measure.previous->second))
                prefix += octaves[octave];
            measure.previous = {position, octave};
            Cells body = group;
            auto span = slurs_.find(node);
            if (span != slurs_.end() && span->second.first == 0 && span->second.second > 4) body += U"⠰⠃";
            for (auto [tag, sign] : articulations)
                if (std::find(marks.begin(), marks.end(), tag) != marks.end()) body += sign;
            body += prefix + static_cast<char32_t>(notes[index] | value.mask) + tail;
            if (span != slurs_.end()) {
                auto [offset, length] = span->second;
                if (length <= 4 && offset < length - 1) body += U"⠉";
                if (length > 4 && offset == length - 1) body += U"⠘⠆";
            }
            if (tie_start) body += U"⠈⠉";
            Cells before;
            for (std::size_t i = 0; i < said_.size(); ++i)
                before += dot_three(said_[i], i + 1 < said_.size() ? said_[i + 1] : body);
            said_.clear();
            measure.cells.push_back(value_sign + before + body);
            last_note_ = {built_.size(), measure.cells.size() - 1};
            const std::array<std::string, 5> alteration{"bb", "b", "", "#", "##"};
            description = step + alteration[alter + 2] + std::to_string(octave) + " " + kind;
            for (auto& mark : marks) description += " " + mark;
            if (!fermata.empty()) description += " fermata";
        }
        measure.words.push_back(description + std::string(dots, '.') + (tie_start ? " tied" : ""));
    }

    std::string label_, where_;
    Slurs slurs_;
    Fraction divisions_;
    int beats_ = 0, unit_ = 0, fifths_ = 0;
    std::string symbol_, voice_, wedge_;
    bool small_ = false;
    std::optional<std::tuple<int, int, int>> tie_;
    std::optional<Tuplet> tuplet_;
    std::optional<std::pair<std::size_t, std::size_t>> last_note_;
    std::vector<std::string> tempo_, review_;
    Cells signature_, metronome_;
    Lines said_;
    std::vector<Measure> built_;
};

} // namespace

Document music_document(const fs::path& path) {
    std::string bytes;
    if (extension(path) == ".mxl") {
        Archive archive(path);
        auto container = parse(archive.part("META-INF/container.xml"), "MXL container");
        auto entry = child(child(xmlDocGetRootElement(container.get()), "rootfiles"), "rootfile");
        auto score = attribute(entry, "full-path");
        if (score.empty()) throw std::runtime_error("MXL container does not identify a MusicXML score");
        bytes = archive.part(score);
    } else {
        if (fs::file_size(path) > 16 * 1024 * 1024)
            throw std::runtime_error("MusicXML exceeds the 16 MiB score limit");
        bytes = read(path);
    }
    return music_document_xml(bytes);
}

Document music_document_xml(std::string_view bytes) {
    if (bytes.size() > 16 * 1024 * 1024)
        throw std::runtime_error("MusicXML exceeds the 16 MiB expanded score limit");
    auto xml = parse(bytes, "MusicXML");
    auto root = xmlDocGetRootElement(xml.get());
    if (!is(root, "score-partwise")) throw std::runtime_error("Music input must be a partwise MusicXML score");
    auto parts = children(root, "part");
    if (parts.empty()) throw std::runtime_error("MusicXML contains no musical parts");
    Document result;
    auto title = text(child(root, "work"), "work-title");
    if (title.empty()) title = text(root, "movement-title");
    if (!title.empty()) result.emplace_back(Paragraph{title, 0, 0, Paragraph::heading});
    std::map<std::string, std::string> names;
    for (auto item : children(child(root, "part-list"), "score-part"))
        names[attribute(item, "id")] = text(item, "part-name");
    for (auto part : parts) {
        auto id = attribute(part, "id");
        auto name = names[id].empty() ? (id.empty() ? "Music" : id) : names[id];
        result.emplace_back(Paragraph{name, 0, 0, Paragraph::heading, 2});
        result.emplace_back(Melody(part, name).result());
    }
    return result;
}

} // namespace braille
