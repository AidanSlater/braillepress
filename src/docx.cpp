#include "xml.hpp"
#include <unicode/uchar.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace braille {
namespace {
using namespace xml;

std::string value(Node owner, std::string_view key, std::string fallback = {}) {
    return attribute(child(owner, key, word), "val", std::move(fallback), word);
}

Node with_value(Node owner, std::string_view tag, const char* field, const std::string& expected) {
    for (auto item : children(owner, tag, word))
        if (attribute(item, field, "", word) == expected)
            return item;
    return nullptr;
}

std::string clean_text(const std::string& text) {
    // Word's paragraph is a single logical block even with explicit soft breaks.
    auto chars = unicode(text);
    Cells output;
    bool gap = false;
    for (auto c : chars) {
        if (u_isUWhiteSpace(c)) {
            gap = !output.empty();
        } else {
            if (gap)
                output += U' ';
            output += c;
            gap = false;
        }
    }
    return utf8(output);
}

struct Relationship {
    std::string target, type;
    bool external;
};

using Relationships = std::map<std::string, Relationship>;
using Notes = std::map<std::pair<std::string, std::string>, Node>;

Relationships relationships(const Archive& archive, const fs::path& part) {
    auto folder = part.parent_path();
    auto path = (folder / "_rels" / (part.filename().string() + ".rels")).generic_string();
    auto data = archive.part(path, true);
    if (data.empty())
        return {};
    auto tree = parse(data, "DOCX relationships");
    Relationships items;
    for (auto entry : children(xmlDocGetRootElement(tree.get()), "Relationship")) {
        auto target = attribute(entry, "Target");
        bool external = attribute(entry, "TargetMode") == "External";
        if (!external && !target.empty()) {
            fs::path candidate(target);
            target = (candidate.is_absolute() ? candidate.relative_path() : folder / candidate).lexically_normal().generic_string();
            if (target == ".." || target.starts_with("../"))
                throw std::runtime_error("DOCX relationship points outside the archive");
        }
        items.emplace(attribute(entry, "Id"), Relationship{target, attribute(entry, "Type"), external});
    }
    return items;
}

std::string find_part(const Relationships& items, std::string_view type) {
    for (auto& [id, relation] : items)
        if (!relation.external && relation.type.ends_with(type))
            return relation.target;
    return {};
}

Tree optional_part(const Archive& archive, const Relationships& items, std::string_view type) {
    auto path = find_part(items, type);
    return path.empty() ? Tree(nullptr, xmlFreeDoc) : parse(archive.part(path), "DOCX");
}

std::string math_text(Node node) {
    auto key = name(node);
    if (!node || node->type != XML_ELEMENT_NODE)
        return {};
    if (is(node, "t", math))
        return content(node);
    if (key.ends_with("Pr"))
        return {};
    if (is(node, "d", math)) {
        auto format = child(node, "dPr", math);
        auto sign = [&](std::string_view tag, const char* fallback) {
            return attribute(child(format, tag, math), "val", fallback, math);
        };
        std::vector<std::string> items;
        for (auto item : children(node, "e", math))
            items.push_back(math_text(item));
        return sign("begChr", "(") + join(items, sign("sepChr", "|")) + sign("endChr", ")");
    }
    if (is(node, "func", math)) {
        std::vector<std::string> parts;
        for (auto tag : {"fName", "e"})
            if (auto item = child(node, tag, math))
                parts.push_back(math_text(item));
        return join(parts, " ");
    }
    if (std::set<std::string_view>{"oMathPara", "oMath", "r", "e", "box", "fName"}.contains(key)
        || is(node, "ins", word) || is(node, "moveTo", word)) {
        std::string result;
        for (auto part : children(node))
            result += math_text(part);
        return result;
    }
    if (!node->ns || !xmlStrEqual(node->ns->href, BAD_CAST math))
        return {};
    throw std::runtime_error("DOCX equations with fractions, scripts, radicals, or other layout are unsupported; type the equation as ordinary text");
}

std::string paragraph_text(Node node, const Notes& notes, std::vector<std::string>* found) {
    static const std::set<std::string_view> containers{
        "hyperlink", "ins", "moveTo", "smartTag", "customXml", "sdt", "sdtContent", "fldSimple", "dir", "bdo"};
    std::string result;
    for (auto item : children(node)) {
        if (is(item, "r", word)) {
            for (auto run : children(item)) {
                if (is(run, "t", word))
                    result += content(run);
                else if (is(run, "tab", word) || is(run, "br", word) || is(run, "cr", word))
                    result += ' ';
                else if (is(run, "noBreakHyphen", word))
                    result += '-';
            }
            if (!found)
                continue;
            for (std::string kind : {"footnote", "endnote"}) {
                for (auto reference : children(item, kind + "Reference", word)) {
                    auto match = notes.find({kind, attribute(reference, "id", "", word)});
                    if (match == notes.end())
                        throw std::runtime_error("DOCX " + kind + " reference has no matching note");
                    std::vector<std::string> paragraphs;
                    for (auto block : descendants(match->second, "p", word))
                        paragraphs.push_back(clean_text(paragraph_text(block, notes, nullptr)));
                    found->push_back(strip(join(paragraphs, " ")));
                    result += "[" + std::to_string(found->size()) + "]";
                }
            }
        } else if (containers.contains(name(item)) && is(item, name(item), word)) {
            result += paragraph_text(item, notes, found);
        } else if (is(item, "oMath", math) || is(item, "oMathPara", math)) {
            result += math_text(item);
        }
    }
    return result;
}

std::vector<Node> content_blocks(Node owner) {
    std::vector<Node> items;
    for (auto node : children(owner)) {
        if (is(node, "p", word) || is(node, "tbl", word)) {
            items.push_back(node);
        } else if (is(node, "sdt", word) || is(node, "sdtContent", word) || is(node, "customXml", word)) {
            auto nested = content_blocks(node);
            items.insert(items.end(), nested.begin(), nested.end());
        } else if (is(node, "altChunk", word)) {
            throw std::runtime_error("DOCX imported content (altChunk) is unsupported; open and save the document in Word first");
        }
    }
    return items;
}

} // namespace

std::string list_number(int number, const std::string& format) {
    if (format == "decimal" || format == "bullet")
        return std::to_string(number);
    if (format == "decimalZero")
        return (number >= 0 && number < 10 ? "0" : "") + std::to_string(number);
    if (format == "none")
        return "";
    if (format == "lowerLetter" || format == "upperLetter") {
        if (number < 1 || number > 100000)
            throw std::runtime_error("Unsupported DOCX letter-list counter");
        return std::string((number - 1) / 26 + 1, (format == "lowerLetter" ? 'a' : 'A') + (number - 1) % 26);
    }
    if (format == "lowerRoman" || format == "upperRoman") {
        if (number < 0 || number > 100000)
            throw std::runtime_error("Unsupported DOCX Roman-list counter");
        std::string result;
        const std::map<int, std::string, std::greater<int>> signs{
            {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"},
            {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}};
        for (const auto& [amount, token] : signs) {
            for (int count = 0; count < number / amount; ++count)
                result += token;
            number %= amount;
        }
        if (format == "lowerRoman")
            for (auto& ch : result)
                ch += 'a' - 'A';
        return result;
    }
    throw std::runtime_error("Unsupported DOCX numbering: " + format + "; use bullets, numbers, letters or Roman numerals");
}

namespace {

std::string inherited_value(const std::vector<Node>& owners, std::string_view key, std::string_view group = {}) {
    for (auto owner : owners) {
        auto props = child(owner, "pPr", word);
        if (!group.empty())
            props = child(props, group, word);
        auto design = value(props, key);
        if (!design.empty())
            return design;
    }
    return {};
}

struct ListItem {
    std::string marker;
    int level;
    Node definition;
};

std::optional<ListItem> list_item(const std::vector<Node>& owners, Node numbering,
                                  std::map<std::pair<std::string, int>, int>& counters) {
    auto id = inherited_value(owners, "numId", "numPr");
    if (id.empty() || id == "0")
        return {};
    if (!numbering)
        throw std::runtime_error("Unsupported DOCX list: missing numbering part");
    auto indent_level = inherited_value(owners, "ilvl", "numPr");
    int level = indent_level.empty() ? 0 : integer(indent_level, "DOCX list level");
    if (level < 0 || level > 8)
        throw std::runtime_error("Unsupported DOCX list level");
    auto number = with_value(numbering, "num", "numId", id);
    auto abstract = with_value(numbering, "abstractNum", "abstractNumId", value(number, "abstractNumId"));
    auto get_level = [&](int n) {
        auto override_node = with_value(number, "lvlOverride", "ilvl", std::to_string(n));
        auto definition = child(override_node, "lvl", word);
        if (!definition)
            definition = with_value(abstract, "lvl", "ilvl", std::to_string(n));
        return std::pair{override_node, definition};
    };
    if (!number)
        throw std::runtime_error("Unsupported DOCX list: missing numbering definition");
    auto [override_node, definition] = get_level(level);
    if (!definition)
        throw std::runtime_error("Unsupported DOCX list: missing level definition");
    auto format = value(definition, "numFmt", "decimal");
    auto start = value(override_node, "startOverride", value(definition, "start", "1"));
    std::erase_if(counters, [&](const auto& counter) { return counter.first.first == id && counter.first.second > level; });
    auto [counter, inserted] = counters.try_emplace({id, level}, integer(start, "DOCX list start"));
    if (!inserted) {
        if (counter->second == INT32_MAX)
            throw std::runtime_error("DOCX list counter overflow");
        ++counter->second;
    }
    if (format == "bullet")
        return ListItem{"•", level, definition};
    auto marker = value(definition, "lvlText", "%" + std::to_string(level + 1) + ".");
    bool legal = child(definition, "isLgl", word);
    for (int i = 0; i <= level; ++i) {
        auto def = get_level(i).second;
        auto style = legal && i < level ? "decimal" : value(def, "numFmt", "decimal");
        auto at = counters.find({id, i});
        auto label = list_number(at == counters.end() ? 1 : at->second, style);
        std::string placeholder = "%" + std::to_string(i + 1);
        for (std::size_t pos = 0; (pos = marker.find(placeholder, pos)) != std::string::npos; pos += label.size())
            marker.replace(pos, placeholder.size(), label);
    }
    if (marker.find('%') != std::string::npos)
        throw std::runtime_error("Unsupported DOCX list label");
    return ListItem{marker, level, definition};
}

void word_pictures(Node root, const Archive& archive, const Relationships& rels, Document& blocks, int max_pixels) {
    for (auto drawing : children(root)) {
        if (is(drawing, "del", word) || is(drawing, "moveFrom", word))
            continue;
        if (!is(drawing, "drawing", word) && !is(drawing, "pict", word)) {
            word_pictures(drawing, archive, rels, blocks, max_pixels);
            continue;
        }
        auto images = descendants(drawing, "blip", "http://schemas.openxmlformats.org/drawingml/2006/main");
        auto vml = descendants(drawing, "imagedata", "urn:schemas-microsoft-com:vml");
        images.insert(images.end(), vml.begin(), vml.end());
        if (images.empty())
            throw std::runtime_error("Unsupported DOCX drawing; supply an embedded raster picture instead of a chart or shape");
        for (auto blip : images) {
            auto id = attribute(blip, "embed", attribute(blip, "id", "", relations), relations);
            if (id.empty())
                throw std::runtime_error("Linked DOCX pictures are unsupported; embed the picture in the document");
            auto relation = rels.find(id);
            if (relation == rels.end() || relation->second.external || !relation->second.type.ends_with("/image"))
                throw std::runtime_error("DOCX picture has a missing or unsupported image relationship");
            auto image = read_image(archive.part(relation->second.target), max_pixels);
            auto fill = blip->parent;
            if (auto crop = child(fill, "srcRect")) {
                auto size = [&](const char* side) { return integer(attribute(crop, side, "0"), "picture crop") / 100000.0; };
                auto l = size("l"), t = size("t"), r = size("r"), b = size("b");
                if (std::min({l, t, r, b}) < 0 || l + r >= 1 || t + b >= 1)
                    throw std::runtime_error("Unsupported DOCX picture crop");
                // Python's round(): ties to even.
                auto nearest = [](double value) { return static_cast<int>(std::nearbyint(value)); };
                image = image.crop(nearest(l * image.width), nearest(t * image.height),
                    nearest((1 - r) * image.width), nearest((1 - b) * image.height));
            }
            if (auto transform = child(child(fill->parent, "spPr"), "xfrm")) {
                auto flip = [&](const char* key) {
                    auto flag = attribute(transform, key);
                    return flag == "1" || flag == "true";
                };
                image = transform_image(image, integer(attribute(transform, "rot", "0"), "picture rotation") / 60000.0,
                                        flip("flipH"), flip("flipV"), max_pixels);
            }
            blocks.emplace_back(Graphic(std::move(image)));
        }
    }
}

void word_table(Node table, const Notes& notes, std::vector<std::string>& found, Document& paragraphs) {
    std::vector<std::vector<std::string>> headers, rows;
    std::size_t columns = 0;
    for (auto row : descendants(table, "tr", word)) {
        if (!descendants(row, "tbl", word).empty())
            throw std::runtime_error("Nested DOCX tables are unsupported; use one level of table");
        std::vector<std::string> cells;
        for (auto cell : descendants(row, "tc", word)) {
            auto merge = child(child(cell, "tcPr", word), "vMerge", word);
            if (merge && attribute(merge, "val", "continue", word) == "continue") {
                cells.emplace_back();
            } else {
                std::vector<std::string> items;
                for (auto p : descendants(cell, "p", word))
                    items.push_back(clean_text(paragraph_text(p, notes, &found)));
                cells.push_back(strip(join(items, " ")));
            }
        }
        columns = std::max(columns, cells.size());
        auto repeat = child(child(row, "trPr", word), "tblHeader", word);
        (repeat && rows.empty() ? headers : rows).push_back(std::move(cells));
    }
    if (headers.empty() && rows.size() > 1) {
        headers.push_back(std::move(rows.front()));
        rows.erase(rows.begin());
    }
    std::vector<std::string> labels(columns);
    for (const auto& header : headers)
        for (std::size_t i = 0; i < header.size(); ++i)
            if (!header[i].empty())
                labels[i] += (labels[i].empty() ? "" : " ") + header[i];
    if (!headers.empty()) {
        bool blanks = std::any_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.size() > 1 && std::any_of(row.begin() + 1, row.end(), [](const auto& s) { return s.empty(); });
        });
        std::string note = "⠈⠨⠣Print format is changed. Row headings are blocked in cell 5; column headings begin in cell 1. "
            "All headings are repeated for clarity. A colon separates headings from table entries.";
        paragraphs.emplace_back(Paragraph{note + (blanks ? " A blank entry is shown as ⠐⠐⠐." : "") + "⠈⠨⠜",
            6, 4, Paragraph::note});
    }
    for (auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            auto cell = row[i];
            if (!headers.empty()) {
                if (cell.empty() && (i > 0 || labels[i].empty()))
                    cell = "⠐⠐⠐";
                if (!labels[i].empty())
                    cell = labels[i] + ": " + cell;
            }
            if (cell.empty())
                continue;
            if (!headers.empty() && i == 0)
                paragraphs.emplace_back(Paragraph{cell, 4, 4, Paragraph::row});
            else
                paragraphs.emplace_back(Paragraph{cell, 0, 2, Paragraph::entry});
        }
    }
}

} // namespace

Document word_document(const fs::path& path, const Options& options) {
    Archive archive(path);
    auto root = find_part(relationships(archive, {}), "/officeDocument");
    if (root.empty())
        throw std::runtime_error("DOCX has no main document relationship");
    auto body = parse(archive.part(root), "DOCX");
    auto rels = relationships(archive, root);
    auto styles = optional_part(archive, rels, "/styles");
    auto numbering = optional_part(archive, rels, "/numbering");
    auto footnotes = optional_part(archive, rels, "/footnotes");
    auto endnotes = optional_part(archive, rels, "/endnotes");
    auto style_root = xmlDocGetRootElement(styles.get());
    Notes notes;
    for (const auto& [kind, tree] : std::array<std::pair<const char*, xmlDoc*>, 2>{{
        {"footnote", footnotes.get()}, {"endnote", endnotes.get()}}}) {
        for (auto node : children(xmlDocGetRootElement(tree), kind, word))
            if (attribute(node, "type", "normal", word) == "normal")
                notes[{kind, attribute(node, "id", "", word)}] = node;
    }
    std::string default_style;
    for (auto style : children(style_root, "style", word))
        if (attribute(style, "type", "", word) == "paragraph" && attribute(style, "default", "", word) == "1")
            default_style = attribute(style, "styleId", "", word);
    Document paragraphs;
    std::map<std::pair<std::string, int>, int> counters;
    std::vector<std::string> found;
    auto body_node = child(xmlDocGetRootElement(body.get()), "body", word);
    if (!body_node)
        throw std::runtime_error("DOCX has no document body");
    for (auto block : content_blocks(body_node)) {
        auto start = found.size();
        if (is(block, "tbl", word)) {
            word_table(block, notes, found, paragraphs);
        } else {
            auto text = clean_text(paragraph_text(block, notes, &found));
            if (!text.empty()) {
                std::vector<Node> owners{block};
                auto style = value(child(block, "pPr", word), "pStyle", default_style);
                std::set<std::string> visited;
                while (!style.empty() && visited.insert(style).second) {
                    auto node = with_value(style_root, "style", "styleId", style);
                    if (!node)
                        break;
                    owners.push_back(node);
                    style = value(node, "basedOn");
                }
                auto outline = inherited_value(owners, "outlineLvl");
                auto track = outline.empty() ? 9 : integer(outline, "DOCX outline level");
                int level = track < 9 ? track + 1 : 1;
                auto kind = track < 9 ? Paragraph::heading : Paragraph::body;
                if (kind == Paragraph::body) {
                    for (auto style_owner = owners.begin() + 1; style_owner != owners.end(); ++style_owner) {
                        auto label = value(*style_owner, "name");
                        if (label == "Title" || label == "Subtitle" || label == "title" || label == "subtitle") {
                            kind = Paragraph::heading;
                            level = label == "Title" || label == "title" ? 1 : 2;
                            break;
                        }
                    }
                }
                auto item = list_item(owners, xmlDocGetRootElement(numbering.get()), counters);
                int first = kind == Paragraph::heading ? 0 : options.first_indent, rest = 0;
                std::optional<int> left, delta;
                auto indent = [&](Node node, bool default_zero = false) {
                    auto x = attribute(node, "left", "", word);
                    auto first_line = attribute(node, "firstLine", "", word);
                    auto hanging = attribute(node, "hanging", "", word);
                    if (!left && (!x.empty() || default_zero))
                        left = x.empty() ? 0 : integer(x, "DOCX left indent");
                    if (!delta && (!hanging.empty() || !first_line.empty() || default_zero))
                        delta = !hanging.empty() ? -integer(hanging, "DOCX hanging indent") :
                                first_line.empty() ? 0 : integer(first_line, "DOCX first-line indent");
                };
                for (auto owner : owners)
                    indent(child(child(owner, "pPr", word), "ind", word));
                if (item) {
                    if (!strip(item->marker).empty())
                        text = item->marker + " " + text;
                    kind = Paragraph::list;
                    first = item->level * options.list_indent;
                    rest = first + options.list_indent;
                    if (auto node = child(child(item->definition, "pPr", word), "ind", word))
                        indent(node, true);
                }
                if (left || delta) {
                    rest = std::max(0, static_cast<int>(std::floor(left.value_or(0) / 180.0 + 0.5)));
                    first = std::max(0, static_cast<int>(std::floor((double(left.value_or(0)) + delta.value_or(0)) / 180 + 0.5)));
                }
                if (kind == Paragraph::body && first > rest)
                    first = rest + options.first_indent;
                paragraphs.emplace_back(Paragraph{text, first, rest, kind, level});
            }
        }
        for (std::size_t i = start; i < found.size(); ++i)
            paragraphs.emplace_back(Paragraph{"[" + std::to_string(i + 1) + "] " + found[i], options.first_indent});
        if (options.images || options.ocr != OCR::off)
            word_pictures(block, archive, rels, paragraphs, options.max_pixels);
    }
    return paragraphs;
}

} // namespace braille
