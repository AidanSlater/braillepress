#include "braille.hpp"

#include <liblouis.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#include <unicode/unistr.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace braille {
namespace {

const std::regex heading(R"(^ *(#{1,6})\s+(.+)$)");
const std::regex list_item(R"(^( *)([-*+]|•|●|▪|◦||[0-9]+[.)]|[a-z][.)]|[ivx]+[.)]|[A-Z]\)|[IVX]+\.|\([0-9a-z]+\))\s+(.+)$)");
const std::regex page_number(R"((?:(?:page|p\.|pág\.|página)\s*)?(?:-|–|—|\()?\s*\d+\s*(?:-|–|—|\))?)", std::regex::icase);
const Cells bullets = U"-*+•●▪◦";
const Cells scripts = U"⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾ⁱⁿ₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎";
constexpr int louis_mode = dotsIO | ucBrl;

Cells normalize(const Cells& input, bool compatibility = false) {
    UErrorCode status = U_ZERO_ERROR;
    auto normalizer = compatibility ? icu::Normalizer2::getNFKCInstance(status)
                                    : icu::Normalizer2::getNFCInstance(status);
    icu::UnicodeString source, target;
    for (char32_t character : input)
        source.append(static_cast<UChar32>(character));
    if (U_SUCCESS(status))
        normalizer->normalize(source, target, status);
    if (U_FAILURE(status))
        throw std::runtime_error("Cannot normalize Unicode text");
    Cells result;
    for (int i = 0; i < target.length();) {
        auto ch = target.char32At(i);
        result.push_back(ch);
        i += U16_LENGTH(ch);
    }
    return result;
}

std::string table_extension() {
    if (const char* path = std::getenv("BRAILLE_SPANISH_TABLE"))
        return path;
#ifdef __linux__
    std::error_code error;
    auto executable = fs::read_symlink("/proc/self/exe", error);
    if (!error) {
        auto relative_table = executable.parent_path().parent_path() / BRAILLE_RELATIVE_TABLE;
        if (fs::is_regular_file(relative_table))
            return relative_table.string();
    }
#endif
    if (fs::is_regular_file(BRAILLE_SOURCE_TABLE))
        return BRAILLE_SOURCE_TABLE;
    return BRAILLE_INSTALLED_TABLE;
}

Cells louis_translate(const Cells& source, const std::string& tables, int mode) {
    if (source.empty())
        return {};
    std::vector<widechar> input;
    for (auto ch : source) {
        if (ch > static_cast<char32_t>(std::numeric_limits<widechar>::max()))
            throw std::runtime_error("Character is outside the installed Liblouis Unicode range");
        input.push_back(static_cast<widechar>(ch));
    }
    if (input.size() > (std::numeric_limits<int>::max() - 32) / 20)
        throw std::runtime_error("Paragraph exceeds the Liblouis input limit");
    std::vector<widechar> output(input.size() * 20 + 32);
    int consumed = static_cast<int>(input.size()), size = static_cast<int>(output.size());
    if (!lou_translateString(tables.c_str(), input.data(), &consumed, output.data(), &size,
                             nullptr, nullptr, mode) || consumed != static_cast<int>(input.size()))
        throw std::runtime_error("Liblouis could not translate using table " + repr(tables.substr(0, tables.find(',')))
                                 + "; check the table name/path");
    return Cells(output.begin(), output.begin() + size);
}

bool defined(char32_t ch, const std::string& tables) {
    static std::map<std::pair<std::string, char32_t>, bool> cached;
    if (ch > static_cast<char32_t>(std::numeric_limits<widechar>::max()))
        return false;
    auto key = std::make_pair(tables, ch);
    if (auto hit = cached.find(key); hit != cached.end())
        return hit->second;
    Cells one(1, ch);
    bool result = louis_translate(one, tables, louis_mode) == louis_translate(one, tables, louis_mode | noUndefined);
    if (cached.size() >= 4096)
        cached.clear();
    cached.emplace(std::move(key), result);
    return result;
}

std::string ordinals(const std::string& text) {
    static const std::regex ordinal(R"(([0-9]+)\.?(º|ª))");
    const Cells digits = U"⠴⠂⠆⠒⠲⠢⠖⠶⠦⠔";
    std::string result;
    std::size_t copied = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), ordinal), end; it != end; ++it) {
        const auto& match = *it;
        auto pos = static_cast<std::size_t>(match.position());
        if (pos && std::string_view("0123456789.,").find(text[pos - 1]) != std::string_view::npos)
            continue;
        result += text.substr(copied, pos - copied);
        result += "⠼";
        for (char digit : match[1].str())
            result += utf8(Cells(1, digits[digit - '0']));
        result += match[2] == "º" ? "⠕" : "⠁";
        copied = pos + match.length();
    }
    return result + text.substr(copied);
}

std::string reflow(const std::vector<std::string>& group) {
    Cells result;
    for (const auto& piece : group) {
        auto line = unicode(strip(piece));
        if (line.empty())
            continue;
        if (result.size() > 1 && result.back() == U'-' && u_isalpha(result[result.size() - 2])
            && u_isalnum(line.front())) {
            if (u_islower(line.front()))
                result.pop_back();
        } else if (!result.empty()) {
            result += U' ';
        }
        bool gap = false;
        for (auto ch : line) {
            if (u_isUWhiteSpace(ch)) {
                gap = true;
            } else {
                if (gap)
                    result += U' ';
                result += ch;
                gap = false;
            }
        }
    }
    return utf8(result);
}

int margin(const std::string& line) {
    auto text = unicode(line);
    return static_cast<int>(std::find_if_not(text.begin(), text.end(), u_isUWhiteSpace) - text.begin());
}

void read_group(std::vector<std::string>& group, Document& output, const Options& options) {
    if (group.empty())
        return;
    std::smatch match;
    int indent = margin(group.front());
    Paragraph block;
    block.first = indent ? indent : options.first_indent;
    if (std::regex_match(group.front(), match, heading)) {
        block = {match[2], 0, 0, Paragraph::heading, static_cast<int>(match[1].length())};
        group[0] = block.text;
    } else if (!std::regex_match(strip(group[0]), page_number) && std::regex_match(group[0], match, list_item)) {
        auto marker = match[2].str();
        if (bullets.find(unicode(marker)) != Cells::npos)
            marker = "•";
        group[0] = marker + " " + match[3].str();
        block = {"", indent, indent + options.list_indent, Paragraph::list};
    }
    if (group.size() > 1 && block.kind != Paragraph::heading)
        block.rest = margin(group[1]);
    if (block.kind == Paragraph::body && block.first > block.rest)
        block.first = block.rest + options.first_indent;
    block.text = reflow(group);
    output.emplace_back(std::move(block));
    group.clear();
}

Cells centered(const Cells& line, int width) {
    return Cells(std::max(0, (width - static_cast<int>(line.size())) / 2), blank) + line;
}

} // namespace

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open " + path.string());
    std::ostringstream data;
    data << stream.rdbuf();
    if (stream.bad())
        throw std::runtime_error("Cannot read " + path.string());
    return data.str();
}

void write(const fs::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
        throw std::runtime_error("Cannot write " + path.string());
}

Cells unicode(std::string_view text) {
    if (text.size() > INT32_MAX)
        throw std::runtime_error("Text exceeds the Unicode input limit");
    Cells result;
    auto data = reinterpret_cast<const std::uint8_t*>(text.data());
    for (int offset = 0, length = static_cast<int>(text.size()); offset < length;) {
        UChar32 ch;
        U8_NEXT(data, offset, length, ch);
        if (ch < 0)
            throw std::runtime_error("Invalid UTF-8 text");
        result.push_back(ch);
    }
    return result;
}

std::string utf8(std::u32string_view text) {
    std::string result;
    for (auto ch : text) {
        std::uint8_t bytes[4];
        int count = 0;
        UBool invalid = false;
        U8_APPEND(bytes, count, 4, ch, invalid);
        if (invalid)
            throw std::runtime_error("Invalid Unicode character");
        result.append(reinterpret_cast<const char*>(bytes), count);
    }
    return result;
}

std::string strip(std::string_view text) {
    auto chars = unicode(text);
    auto start = std::find_if_not(chars.begin(), chars.end(), u_isUWhiteSpace);
    auto end = std::find_if_not(chars.rbegin(), chars.rend(), u_isUWhiteSpace).base();
    return start < end ? utf8(Cells(start, end)) : std::string{};
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> result;
    for (std::size_t start = 0;;) {
        auto next = text.find(separator, start);
        result.emplace_back(text.substr(start, next == std::string_view::npos ? next : next - start));
        if (next == std::string_view::npos)
            return result;
        start = next + 1;
    }
}

std::string repr(std::string_view text) {
    auto cells = unicode(text);
    char32_t quote = cells.find(U'\'') != Cells::npos && cells.find(U'"') == Cells::npos ? U'"' : U'\'';
    std::string out(1, static_cast<char>(quote));
    for (char32_t ch : cells) {
        if (ch == quote || ch == U'\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch == U'\t') {
            out += "\\t";
        } else if (ch == U'\n') {
            out += "\\n";
        } else if (ch == U'\r') {
            out += "\\r";
        } else {
            // Python escapes the Other and Separator categories, except the ASCII space.
            auto type = u_charType(ch);
            bool hidden = ch != U' ' && (type == U_CONTROL_CHAR || type == U_FORMAT_CHAR || type == U_SURROGATE ||
                type == U_PRIVATE_USE_CHAR || type == U_UNASSIGNED || type == U_SPACE_SEPARATOR ||
                type == U_LINE_SEPARATOR || type == U_PARAGRAPH_SEPARATOR);
            if (!hidden) {
                out += utf8(Cells(1, ch));
                continue;
            }
            std::ostringstream escape;
            escape << (ch < 0x100 ? "\\x" : ch < 0x10000 ? "\\u" : "\\U") << std::hex << std::setfill('0')
                   << std::setw(ch < 0x100 ? 2 : ch < 0x10000 ? 4 : 8) << static_cast<std::uint32_t>(ch);
            out += escape.str();
        }
    }
    return out + static_cast<char>(quote);
}

std::string join(const std::vector<std::string>& strings, std::string_view separator) {
    std::string result;
    for (std::size_t i = 0; i < strings.size(); ++i) {
        if (i)
            result += separator;
        result += strings[i];
    }
    return result;
}

std::string extension(const fs::path& path) {
    auto value = path.extension().string();
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return value;
}

int integer(std::string_view text, std::string_view label) {
    int value = 0;
    if (!text.empty() && text.front() == '+')
        text.remove_prefix(1);
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid " + std::string(label) + ": " + std::string(text));
    return value;
}

std::string Options::translation_table() const {
    return table.empty() ? (language == "es" ? "es-g1.ctb" : "en-ueb-g1.ctb") : table;
}

std::string Options::recognition_language() const {
    return ocr_language.empty() ? (language == "es" ? "spa" : "eng") : ocr_language;
}

bool Options::selects(int page) const {
    return ranges.empty() || std::any_of(ranges.begin(), ranges.end(), [page](auto range) {
        return range.first <= page && page <= range.second;
    });
}

void Options::validate() const {
    if (language != "en" && language != "es")
        throw std::runtime_error("Language must be en (English) or es (Spanish)");
    if (width < 1 || height < 1)
        throw std::runtime_error("Cells per line and lines per page must be positive");
    if (page_numbers && height < 2)
        throw std::runtime_error("Numbered pages need at least two lines per page");
    if (first_indent < 0 || list_indent < 0)
        throw std::runtime_error("Indentation must be nonnegative");
    if (images_only && !images)
        throw std::runtime_error("--images-only and --skip-images cannot be combined");
    if (image_mode != "outline" && image_mode != "threshold" && image_mode != "dither")
        throw std::runtime_error("Image mode must be outline, threshold, or dither");
    if (threshold && (*threshold < 0 || *threshold > 255))
        throw std::runtime_error("Image threshold must be between 0 and 255");
    if (image_width < 0 || image_height < 0)
        throw std::runtime_error("Image width and height must be positive");
}

Cells translate(std::string text, const std::string& table) {
    // The Liblouis table cache is process-global.
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    auto tables = table;
    if (table == "es-g1.ctb" || table == "es.tbl") {
        tables += "," + table_extension();
        text = ordinals(text);
    }
    auto original = normalize(unicode(text));
    Cells source, missing;
    for (auto ch : original) {
        if (ch >= 0xfb00 && ch <= 0xfb06) {
            source += normalize(Cells(1, ch), true);
            continue;
        }
        if (u_charType(ch) == U_FORMAT_CHAR)
            continue;
        if (u_isUWhiteSpace(ch) || defined(ch, tables)) {
            source += ch;
            continue;
        }
        auto plain = scripts.find(ch) == Cells::npos ? normalize(Cells(1, ch), true) : Cells{};
        if (plain == Cells(1, ch) || !std::all_of(plain.begin(), plain.end(), [&](auto cp) { return defined(cp, tables); }))
            plain.clear();
        if (plain.empty()) {
            char32_t fallback = 0;
            if (bullets.find(ch) != Cells::npos || ch == U'−')
                fallback = U'-';
            if (ch == U'′')
                fallback = U'\'';
            if (ch == U'″')
                fallback = U'"';
            if (fallback && fallback != ch && defined(fallback, tables))
                plain = Cells(1, fallback);
        }
        if (plain.empty() && missing.find(ch) == Cells::npos)
            missing += ch;
        source += plain;
    }
    if (!missing.empty()) {
        std::vector<std::string> names;
        for (auto ch : missing.substr(0, 5)) {
            char name[128]{};
            UErrorCode status = U_ZERO_ERROR;
            u_charName(ch, U_UNICODE_CHAR_NAME, name, sizeof name, &status);
            std::ostringstream label;
            if (*name && U_SUCCESS(status))
                label << name;
            else
                label << "U+" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << static_cast<std::uint32_t>(ch);
            names.push_back("'" + utf8(Cells(1, ch)) + "' (" + label.str() + ")");
        }
        Cells expanded;
        for (auto ch : original)
            expanded += ch >= 0xfb00 && ch <= 0xfb06 ? normalize(Cells(1, ch), true) : Cells(1, ch);
        auto place = expanded.find(missing.front());
        auto start = place > 30 ? place - 30 : 0;
        throw std::runtime_error("Braille table " + repr(table) + " has no symbol for " + join(names, ", ")
            + (missing.size() > 5 ? " and " + std::to_string(missing.size() - 5) + " more" : "")
            + " in " + repr(utf8(expanded.substr(start, place - start + 30))) + "; replace it with words or plain text");
    }
    auto result = louis_translate(source, tables, louis_mode);
    if (!std::all_of(result.begin(), result.end(), [](auto cp) { return blank <= cp && cp <= U'⣿'; }))
        throw std::runtime_error("Liblouis returned non-Braille output");
    return result;
}

Lines wrap(const Cells& cells, int width, int first, int rest) {
    if (std::min(first, rest) < 0 || std::max(first, rest) >= width)
        throw std::runtime_error("Indentation must be nonnegative and smaller than cells per line");
    Lines lines;
    Cells line(first, blank);
    bool has_word = false;
    for (std::size_t start = cells.find_first_not_of(blank); start != Cells::npos;) {
        auto next = cells.find(blank, start);
        auto word = cells.substr(start, next == Cells::npos ? next : next - start);
        if (has_word && line.size() + 1 + word.size() > static_cast<std::size_t>(width)) {
            lines.push_back(std::move(line));
            line.assign(rest, blank);
            has_word = false;
        }
        if (line.size() + word.size() > static_cast<std::size_t>(width))
            throw std::runtime_error("Translated word needs " + std::to_string(word.size()) +
                " cells; increase cells per line or reduce indentation");
        if (has_word)
            line += blank;
        line += word;
        has_word = true;
        start = next == Cells::npos ? next : cells.find_first_not_of(blank, next);
    }
    if (has_word)
        lines.push_back(std::move(line));
    return lines;
}

Document text_document(std::string_view input, const Options& options) {
    auto characters = unicode(input);
    if (!characters.empty() && characters.front() == U'\ufeff')
        characters.erase(0, 1);
    std::string text;
    int column = 0;
    for (std::size_t i = 0; i < characters.size(); ++i) {
        auto ch = characters[i];
        if (ch == U'\t') {
            text.append(4 - column % 4, ' ');
            column += 4 - column % 4;
        } else if (ch == U'\r' || ch == U'\n' || ch == U'\f' || ch == U'\v' || ch == U'\u0085'
                   || ch == U'\u2028' || ch == U'\u2029' || (U'\x1c' <= ch && ch <= U'\x1e')) {
            if (ch == U'\r' && i + 1 < characters.size() && characters[i + 1] == U'\n')
                ++i;
            text += ch == U'\f' ? "\n\n" : "\n";
            column = 0;
        } else {
            text += utf8(Cells(1, ch));
            ++column;
        }
    }
    Document result;
    std::vector<std::string> group;
    for (auto& line : split(text, '\n')) {
        auto clean = strip(line);
        bool is_heading = std::regex_match(line, heading);
        if (clean.empty() || is_heading || (!std::regex_match(clean, page_number) && std::regex_match(line, list_item)))
            read_group(group, result, options);
        if (!clean.empty())
            group.push_back(std::move(line));
        if (is_heading)
            read_group(group, result, options);
    }
    read_group(group, result, options);
    return result;
}

bool Graphic::graph() const {
    return !x_labels.empty() || !y_labels.empty() || !x_title.empty() || !y_title.empty();
}

Pages paginate(const Document& document, const Options& options) {
    options.validate();
    const auto table = options.translation_table();
    const int width = options.width, height = options.height - options.page_numbers;
    Pages pages(1);
    auto append = [&](const Lines& lines) {
        for (const auto& line : lines) {
            if (static_cast<int>(pages.back().size()) == height)
                pages.emplace_back();
            pages.back().push_back(line);
        }
    };
    auto fresh = [&] {
        if (!pages.back().empty())
            pages.emplace_back();
    };
    std::map<int, int> styles;
    for (auto& block : document)
        if (auto p = std::get_if<Paragraph>(&block); p && p->kind == Paragraph::heading)
            styles[p->level] = -1;
    if (styles.size() == 2)
        styles.rbegin()->second = 4;
    if (styles.size() >= 3) {
        styles.rbegin()->second = 6;
        std::next(styles.rbegin())->second = 4;
    }

    int previous_kind = -1, previous_style = 0, images = 0;
    bool previous_graphic = false;
    for (auto& block : document) {
        if (auto music = std::get_if<Music>(&block)) {
            Cells signs = music->metronome;
            if (!signs.empty() && !music->signature.empty())
                signs += blank;
            signs += music->signature;
            Lines lines;
            if (!music->tempo.empty()) {
                auto tempo = translate(music->tempo, table);
                if (static_cast<int>(tempo.size() + 1 + signs.size() + 6) > width)
                    lines.push_back(tempo);
                else
                    signs = tempo + blank + signs;
            }
            lines.push_back(signs);
            for (auto& line : lines)
                line = centered(line, width);
            lines.insert(lines.end(), music->measures.begin(), music->measures.end());
            if (std::any_of(lines.begin(), lines.end(), [width](auto& line) { return line.size() > static_cast<std::size_t>(width); }))
                throw std::runtime_error("A music measure/signature exceeds cells per line; increase --cells-per-line (measures are not split)");
            if (!pages.back().empty() && static_cast<int>(pages.back().size()) < height)
                pages.back().emplace_back();
            append(lines);
            previous_kind = -1;
            previous_style = 0;
            previous_graphic = false;
            continue;
        }
        if (auto graphic = std::get_if<Graphic>(&block)) {
            auto label = "Image " + std::to_string(++images);
            auto caption = wrap(translate(label + (graphic->caption.empty() ? "" : ". " + graphic->caption), table), width);
            if (graphic->graph()) {
                std::vector<std::string> key;
                auto rows = graph_lines(*graphic, caption, width, height, options, key);
                if (!key.empty()) {
                    auto heading_lines = wrap(translate(label + " label key", table), width);
                    heading_lines.emplace_back();
                    int available = height - static_cast<int>(heading_lines.size());
                    if (available < 1)
                        throw std::runtime_error("Graph label keys need more lines per page");
                    Lines lines;
                    for (auto& item : key) {
                        auto wrapped = wrap(translate(item, table), width);
                        lines.insert(lines.end(), wrapped.begin(), wrapped.end());
                    }
                    for (auto start = lines.begin(); start != lines.end();) {
                        fresh();
                        append(heading_lines);
                        auto end = start + std::min<std::ptrdiff_t>(available, lines.end() - start);
                        append(Lines(start, end));
                        start = end;
                    }
                }
                fresh();
                append(rows);
            } else {
                int available = height - static_cast<int>(caption.size()) - 1;
                if (available < 1)
                    throw std::runtime_error("Image pages need room for a caption, a blank line, and the graphic; increase lines per page");
                auto rows = picture_cells(graphic->image,
                    options.image_width ? std::min(width, options.image_width) : width,
                    options.image_height ? std::min(available, options.image_height) : available, options);
                fresh();
                append(caption);
                pages.back().emplace_back();
                for (auto& row : rows)
                    pages.back().push_back(centered(row, width));
            }
            previous_kind = -1;
            previous_style = 0;
            previous_graphic = true;
            continue;
        }
        const auto& paragraph = std::get<Paragraph>(block);
        auto kind = paragraph.kind;
        if (kind == Paragraph::note && !table.starts_with("en-"))
            continue;
        auto cells = translate(paragraph.text, table);
        int style = kind == Paragraph::heading ? styles.at(paragraph.level) : kind == Paragraph::row ? 4 : 0;
        Lines lines;
        if (style) {
            try {
                lines = style < 0 ? wrap(cells, width - 6) : wrap(cells, width, style, style);
                if (style < 0)
                    for (auto& line : lines)
                        line = centered(line, width);
            } catch (const std::runtime_error&) {
                lines = wrap(cells, width, paragraph.first, paragraph.rest);
            }
        } else {
            lines = wrap(cells, width, paragraph.first, paragraph.rest);
        }
        if (lines.empty())
            continue;
        if (previous_graphic)
            fresh();
        bool separate;
        if (style < 0 || kind == Paragraph::row || kind == Paragraph::note)
            separate = true;
        else if (style > 0)
            separate = previous_style != 4;
        else if (kind == Paragraph::entry)
            separate = previous_kind != Paragraph::row && previous_kind != Paragraph::entry;
        else
            separate = previous_style < 0 || previous_kind == Paragraph::row || previous_kind == Paragraph::entry ||
                previous_kind == Paragraph::note || (kind == Paragraph::body && paragraph.first == paragraph.rest);
        int needed = style ? static_cast<int>(lines.size()) + (style < 0 ? 2 : 1) : 1;
        if (!pages.back().empty()) {
            if (static_cast<int>(pages.back().size()) + separate + std::min(needed, height) > height)
                fresh();
            else if (separate)
                pages.back().emplace_back();
        }
        append(lines);
        previous_kind = kind;
        previous_style = style;
        previous_graphic = false;
    }
    if (pages.back().empty())
        pages.pop_back();
    if (options.page_numbers) {
        for (std::size_t i = 0; i < pages.size(); ++i) {
            auto label = translate(std::to_string(i + 1), table);
            if (static_cast<int>(label.size()) + 3 > width)
                throw std::runtime_error("Page numbers need at least three preceding blank cells; increase cells per line");
            pages[i].resize(height);
            pages[i].push_back(Cells(width - label.size(), blank) + label);
        }
    }
    return pages;
}

std::string review(const Document& document) {
    std::vector<std::string> items;
    int number = 0;
    for (auto& block : document) {
        if (auto graphic = std::get_if<Graphic>(&block)) {
            items.push_back("[Image " + std::to_string(++number) + "]" +
                (graphic->caption.empty() ? "" : " " + graphic->caption));
            auto details = describe_graph(*graphic);
            items.insert(items.end(), details.begin(), details.end());
        } else if (auto p = std::get_if<Paragraph>(&block)) {
            items.push_back(p->text);
        } else {
            items.push_back(std::get<Music>(block).text);
        }
    }
    return items.empty() ? "" : join(items, "\n\n") + "\n";
}

std::string serialize(const Pages& pages) {
    std::string result;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (i)
            result += '\f';
        for (const auto& line : pages[i])
            result += utf8(line) + "\n";
    }
    return result;
}

Pages parse_cells(std::string_view bytes, const Options& options, bool six_dots) {
    Pages result;
    if (bytes.empty())
        return result;
    auto pages = split(bytes, '\f');
    if (pages.size() > static_cast<std::size_t>(options.max_pages))
        throw std::runtime_error("Pre-translated job exceeds the configured page limit");
    int number = 0;
    for (const auto& page : pages) {
        Lines lines;
        auto rows = split(page, '\n');
        if (!rows.empty() && rows.back().empty())
            rows.pop_back();
        if (rows.size() > static_cast<std::size_t>(options.height))
            throw std::runtime_error("Pre-translated job exceeds the configured page grid");
        for (const auto& row : rows) {
            auto line = unicode(row);
            if (line.size() > static_cast<std::size_t>(options.width))
                throw std::runtime_error("Pre-translated job exceeds the configured page grid");
            for (auto cell : line)
                if (cell < blank || cell > (six_dots ? U'⠿' : U'⣿'))
                    throw std::runtime_error("Pre-translated jobs must contain six-dot Unicode Braille, LF, and form feeds only");
            lines.push_back(std::move(line));
        }
        if (options.selects(++number))
            result.push_back(std::move(lines));
    }
    return result;
}

} // namespace braille
