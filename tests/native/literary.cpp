// Literary converter tests with literal expectations checked against ICEB UEB
// and ONCE B 2. See the test fixture documentation in README.md.

#include "braille.hpp"
#include "check.hpp"
#include "mupdf.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <unicode/normalizer2.h>
#include <unicode/unistr.h>
#include <zip.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>

using namespace braille;
using namespace native_test;

namespace {

const Cells B(1, blank);
const Cells CAT = U"⠉⠁⠞";
const Cells DOG = U"⠙⠕⠛";

fs::path samples() { return context().root / "tests" / "fixtures"; }
fs::path work(const std::string& name) { return context().work / name; }
Cells blanks(int count) { return Cells(count, blank); }

Cells joined(const std::vector<Cells>& items, const Cells& separator) {
    Cells result;
    for (std::size_t i = 0; i < items.size(); ++i)
        result += (i ? separator : Cells{}) + items[i];
    return result;
}

// Python's str.splitlines(): no trailing empty line after a final separator.
std::vector<Cells> splitlines(const Cells& text) {
    static const Cells separators = U"\n\r\v\f\x1c\x1d\x1e\x85  ";
    std::vector<Cells> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (separators.find(text[i]) == Cells::npos)
            continue;
        lines.push_back(text.substr(start, i - start));
        if (text[i] == U'\r' && i + 1 < text.size() && text[i + 1] == U'\n')
            ++i;
        start = i + 1;
    }
    if (start < text.size())
        lines.push_back(text.substr(start));
    return lines;
}

std::vector<Cells> pages_of(const std::string& braille) {
    std::vector<Cells> pages;
    for (auto& page : split(braille, '\f'))
        pages.push_back(unicode(page));
    return pages;
}

std::string normalized(const std::string& text, bool decompose) {
    UErrorCode status = U_ZERO_ERROR;
    auto normalizer = decompose ? icu::Normalizer2::getNFDInstance(status) : icu::Normalizer2::getNFCInstance(status);
    icu::UnicodeString result;
    if (U_SUCCESS(status))
        result = normalizer->normalize(icu::UnicodeString::fromUTF8(text), status);
    check(U_SUCCESS(status), "Unicode normalization");
    std::string bytes;
    return result.toUTF8String(bytes);
}

Paragraph P(std::string text, int first = 2, int rest = 0, Paragraph::Kind kind = Paragraph::body, int level = 1) {
    return Paragraph{std::move(text), first, rest, kind, level};
}

std::string kind_name(Paragraph::Kind kind) {
    static const char* names[]{"body", "list", "heading", "note", "row", "entry"};
    return names[kind];
}

std::string repr(const Paragraph& p) {
    return "Paragraph('" + p.text + "', " + std::to_string(p.first) + ", " + std::to_string(p.rest) + ", '" +
        kind_name(p.kind) + "', " + std::to_string(p.level) + ")";
}

std::vector<std::string> describe(const Document& document) {
    std::vector<std::string> result;
    for (auto& block : document) {
        if (auto p = std::get_if<Paragraph>(&block))
            result.push_back(repr(*p));
        else
            result.push_back(std::holds_alternative<Graphic>(block) ? "Graphic" : "Music");
    }
    return result;
}

std::vector<std::string> texts(const Document& document) {
    std::vector<std::string> result;
    for (auto& block : document)
        result.push_back(std::holds_alternative<Paragraph>(block) ? std::get<Paragraph>(block).text : "<non-text>");
    return result;
}

const Paragraph& paragraph_at(const Document& document, std::size_t index) {
    check(index < document.size() && std::holds_alternative<Paragraph>(document[index]), "paragraph block expected");
    return std::get<Paragraph>(document[index]);
}

// braille.text_paragraphs
Document text_paragraphs(std::string_view text, int first_indent = 2, int list_indent = 4) {
    Options options;
    options.first_indent = first_indent;
    options.list_indent = list_indent;
    return text_document(text, options);
}

// braille.read_document
Document read_document(const fs::path& path, const Options& options = {}) {
    return import_document(path, options).blocks;
}

// braille.render: paginate + serialize
std::string render(const Document& blocks, int width = 40, int height = 25, const std::string& table = "en-ueb-g1.ctb",
                   bool page_numbers = false) {
    Options options;
    options.width = width;
    options.height = height;
    options.table = table;
    options.page_numbers = page_numbers;
    return serialize(paginate(blocks, options));
}

// braille.convert, in the order of src/cli.cpp: validate, import, paginate, review text.
std::string convert(const fs::path& path, const Options& options = {}, const fs::path& text_output = {}) {
    options.validate();
    auto blocks = import_document(path, options).blocks;
    auto pages = paginate(blocks, options);
    if (!text_output.empty())
        write(text_output, review(blocks));
    return serialize(pages);
}

Options layout(int width, int height = 25) {
    Options options;
    options.width = width;
    options.height = height;
    return options;
}

void expect_error_matching(const std::function<void()>& body, const std::string& pattern, const std::string& detail) {
    try {
        body();
    } catch (const std::exception& error) {
        std::string message = error.what();
        check(std::regex_search(message, std::regex(pattern)), detail + ": error does not match /" + pattern + "/: " + message);
        return;
    }
    throw std::runtime_error(detail + ": expected an error matching /" + pattern + "/");
}

struct Run {
    int code;
    std::string stderr_text;
};

// subprocess.run([braille, *args]) with captured stderr.
Run run_cli(const std::vector<std::string>& args) {
    check(!context().cli.empty(), "the suite needs the braille CLI path");
    auto log = work("cli-stderr.txt"), out = work("cli-stdout.txt");
    std::vector<std::string> owned{context().cli.string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : owned)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        int error = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int output = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (error < 0 || output < 0 || dup2(error, 2) < 0 || dup2(output, 1) < 0)
            _exit(126);
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid, "waitpid");
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), read(log)};
}

// ZIP packages ------------------------------------------------------------

using Parts = std::vector<std::pair<std::string, std::string>>;

void write_zip(const fs::path& path, const Parts& parts) {
    int error = 0;
    auto archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    check(archive != nullptr, "create " + path.string());
    for (auto& [name, content] : parts) {
        auto data = zip_source_buffer(archive, content.data(), content.size(), 0);
        if (!data || zip_file_add(archive, name.c_str(), data, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
            if (data)
                zip_source_free(data);
            zip_discard(archive);
            throw std::runtime_error("write " + name + " to " + path.string());
        }
    }
    int result = zip_close(archive);
    if (result)
        zip_discard(archive);
    check(result == 0, "close " + path.string());
}

Parts read_zip(const fs::path& path) {
    int error = 0;
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(zip_open(path.c_str(), ZIP_RDONLY, &error), zip_discard);
    check(archive != nullptr, "open " + path.string());
    Parts parts;
    for (zip_int64_t i = 0, count = zip_get_num_entries(archive.get(), 0); i < count; ++i) {
        zip_stat_t info;
        check(zip_stat_index(archive.get(), i, 0, &info) == 0, "stat ZIP entry");
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(zip_fopen_index(archive.get(), i, 0), zip_fclose);
        std::string bytes(info.size, '\0');
        check(file && zip_fread(file.get(), bytes.data(), info.size) == static_cast<zip_int64_t>(info.size), "read ZIP entry");
        parts.emplace_back(info.name, std::move(bytes));
    }
    return parts;
}

std::string& part(Parts& parts, const std::string& name) {
    for (auto& [key, value] : parts)
        if (key == name)
            return value;
    throw std::runtime_error("missing package part " + name);
}

// DOCX packages -----------------------------------------------------------

const std::string w_ns = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const std::string w_decl = "xmlns:w=\"" + w_ns + "\"";
const std::string xml_head = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
const std::string rel_type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

// python-docx's default template: List Bullet uses numId 1 and List Number numId 5.
const std::string template_list_styles =
    R"(<w:style w:type="paragraph" w:styleId="ListBullet"><w:name w:val="List Bullet"/><w:basedOn w:val="Normal"/>)"
    R"(<w:pPr><w:numPr><w:numId w:val="1"/></w:numPr><w:contextualSpacing/></w:pPr></w:style>)"
    R"(<w:style w:type="paragraph" w:styleId="ListNumber"><w:name w:val="List Number"/><w:basedOn w:val="Normal"/>)"
    R"(<w:pPr><w:numPr><w:numId w:val="5"/></w:numPr><w:contextualSpacing/></w:pPr></w:style>)";
const std::string template_list_numbering =
    R"(<w:abstractNum w:abstractNumId="7"><w:nsid w:val="FFFFFF88"/><w:multiLevelType w:val="singleLevel"/>)"
    R"(<w:tmpl w:val="D0A62B40"/><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/>)"
    R"(<w:pStyle w:val="ListNumber"/><w:lvlText w:val="%1."/><w:lvlJc w:val="left"/><w:pPr><w:tabs>)"
    R"(<w:tab w:val="num" w:pos="360"/></w:tabs><w:ind w:left="360" w:hanging="360"/></w:pPr></w:lvl></w:abstractNum>)"
    R"(<w:abstractNum w:abstractNumId="8"><w:nsid w:val="FFFFFF89"/><w:multiLevelType w:val="singleLevel"/>)"
    R"(<w:tmpl w:val="29761A62"/><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="bullet"/>)"
    R"(<w:pStyle w:val="ListBullet"/><w:lvlText w:val=")" "" R"("/><w:lvlJc w:val="left"/><w:pPr><w:tabs>)"
    R"(<w:tab w:val="num" w:pos="360"/></w:tabs><w:ind w:left="360" w:hanging="360"/></w:pPr><w:rPr>)"
    R"(<w:rFonts w:ascii="Symbol" w:hAnsi="Symbol" w:hint="default"/></w:rPr></w:lvl></w:abstractNum>)"
    R"(<w:num w:numId="1"><w:abstractNumId w:val="8"/></w:num><w:num w:numId="5"><w:abstractNumId w:val="7"/></w:num>)";

// The parts of python-docx's Document() that the importer reads, with only
// the styles and numbering a test needs. Normal is the default paragraph style.
struct Docx {
    std::string body;                          // block content before w:sectPr
    std::string styles;                        // styles besides Normal
    std::optional<std::string> numbering = ""; // nullopt: no numbering part or relationship
    std::string footnotes;                     // complete footnotes part, if any
};

void save_docx(const fs::path& path, const Docx& docx) {
    std::string types = xml_head + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>";
    std::string rels = xml_head + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId3\" Type=\"" + rel_type + "styles\" Target=\"styles.xml\"/>";
    Parts parts;
    parts.emplace_back("_rels/.rels", xml_head + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"" + rel_type + "officeDocument\" Target=\"word/document.xml\"/></Relationships>");
    parts.emplace_back("word/document.xml", xml_head + "<w:document " + w_decl +
        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
        " xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"><w:body>" + docx.body +
        "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr></w:body></w:document>");
    parts.emplace_back("word/styles.xml", xml_head + "<w:styles " + w_decl + "><w:style w:type=\"paragraph\" w:default=\"1\" "
        "w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>" + docx.styles + "</w:styles>");
    if (docx.numbering) {
        types += "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>";
        rels += "<Relationship Id=\"rId2\" Type=\"" + rel_type + "numbering\" Target=\"numbering.xml\"/>";
        parts.emplace_back("word/numbering.xml", xml_head + "<w:numbering " + w_decl + ">" + *docx.numbering + "</w:numbering>");
    }
    if (!docx.footnotes.empty()) {
        types += "<Override PartName=\"/word/footnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml\"/>";
        rels += "<Relationship Id=\"rId9\" Type=\"" + rel_type + "footnotes\" Target=\"footnotes.xml\"/>";
        parts.emplace_back("word/footnotes.xml", xml_head + docx.footnotes);
    }
    parts.emplace_back("word/_rels/document.xml.rels", rels + "</Relationships>");
    parts.emplace(parts.begin(), "[Content_Types].xml", types + "</Types>");
    write_zip(path, parts);
}

std::string escaped(const std::string& text) {
    std::string result;
    for (char c : text)
        result += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    return result;
}

// document.add_paragraph(text, style)
std::string word_paragraph(const std::string& text, const std::string& style = {}) {
    return "<w:p>" + (style.empty() ? "" : "<w:pPr><w:pStyle w:val=\"" + style + "\"/></w:pPr>") +
        "<w:r><w:t>" + escaped(text) + "</w:t></w:r></w:p>";
}

// A package part edited like python-docx's lxml tree.
class XmlPart {
public:
    explicit XmlPart(const std::string& bytes)
        : doc_(xmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr, XML_PARSE_NONET), xmlFreeDoc) {
        check(doc_ != nullptr, "parse XML part");
    }

    std::vector<xmlNodePtr> xpath(const std::string& expression) const {
        std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)> context(xmlXPathNewContext(doc_.get()), xmlXPathFreeContext);
        xmlXPathRegisterNs(context.get(), BAD_CAST "w", BAD_CAST w_ns.c_str());
        context->node = xmlDocGetRootElement(doc_.get());
        std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)> found(
            xmlXPathEvalExpression(BAD_CAST expression.c_str(), context.get()), xmlXPathFreeObject);
        std::vector<xmlNodePtr> nodes;
        if (found && found->nodesetval)
            nodes.assign(found->nodesetval->nodeTab, found->nodesetval->nodeTab + found->nodesetval->nodeNr);
        check(!nodes.empty(), "XPath matched nothing: " + expression);
        return nodes;
    }

    // parse_xml(fragment), owned by this document until inserted.
    xmlNodePtr fragment(const std::string& xml) const {
        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> parsed(
            xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr, XML_PARSE_NONET), xmlFreeDoc);
        check(parsed != nullptr, "parse XML fragment");
        return xmlDocCopyNode(xmlDocGetRootElement(parsed.get()), doc_.get(), 1);
    }

    std::string bytes() const {
        xmlChar* data = nullptr;
        int size = 0;
        xmlDocDumpMemoryEnc(doc_.get(), &data, &size, "UTF-8");
        std::string result(reinterpret_cast<const char*>(data), size);
        xmlFree(data);
        return result;
    }

private:
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc_;
};

void set_value(xmlNodePtr node, const std::string& value) {
    xmlSetNsProp(node, node->ns, BAD_CAST "val", BAD_CAST value.c_str());
}

// PDF files -----------------------------------------------------------------

std::string pdf_stream(const std::string& entries, const std::string& data) {
    return "<< " + entries + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream";
}

// Objects are numbered from 1; object 1 is the catalog.
void write_pdf(const fs::path& path, const std::vector<std::string>& objects) {
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    auto xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (auto offset : offsets) {
        char entry[24];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", offset);
        out += entry;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
        std::to_string(xref) + "\n%%EOF\n";
    write(path, out);
}

struct Text {
    int x, y;
    std::string text;
};

const Image gray_pixel{1, 1, {0x80}};

// The page layout of the fixture PDFs (simple.pdf, overflow.pdf, scanned.pdf):
// one US Letter page, each (x, y, ASCII text) one Courier line; the image
// variant paints a 1x1 grey pixel at 100 x 100 points.
// A test may supply another picture and placement, and an invisible text layer.
void make_pdf(const fs::path& path, const std::vector<Text>& lines = {}, const Image* image = nullptr,
              bool invisible = false, const std::string& placement = "100 0 0 100 50 600") {
    std::vector<std::string> commands;
    for (auto& [x, y, text] : lines) {
        std::string safe;
        for (char c : text)
            safe += c == '\\' || c == '(' || c == ')' ? std::string{'\\', c} : std::string(1, c);
        commands.push_back(std::string("BT ") + (invisible ? "3 Tr " : "") + "/F1 12 Tf 1 0 0 1 " + std::to_string(x) +
            " " + std::to_string(y) + " Tm (" + safe + ") Tj ET");
    }
    if (image)
        commands.push_back("q " + placement + " cm /Im1 Do Q");
    std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >>" +
            std::string(image ? " /XObject << /Im1 6 0 R >>" : "") + " >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>",
        pdf_stream("", join(commands, "\n"))};
    if (image)
        objects.push_back(pdf_stream("/Type /XObject /Subtype /Image /Width " + std::to_string(image->width) +
            " /Height " + std::to_string(image->height) + " /ColorSpace /DeviceGray /BitsPerComponent 8",
            std::string(image->pixels.begin(), image->pixels.end())));
    write_pdf(path, objects);
}

// PdfWriter.append for each input, in order.
void append_pdfs(const std::vector<fs::path>& inputs, const fs::path& output) {
    PdfContext mu;
    auto merged = mu.own(mu.call([](auto ctx) { return pdf_create_document(ctx); }), pdf_drop_document);
    for (auto& input : inputs) {
        auto source = mu.own(mu.call([&](auto ctx) { return pdf_open_document(ctx, input.c_str()); }), pdf_drop_document);
        int count = mu.call([&](auto ctx) { return pdf_count_pages(ctx, source.get()); });
        for (int i = 0; i < count; ++i)
            mu.call([&](auto ctx) { pdf_graft_page(ctx, merged.get(), -1, source.get(), i); });
    }
    save_pdf(mu, merged.get(), output.string());
}

struct Operation {
    std::vector<std::string> operands;
    std::string op;
};

struct PdfPage {
    std::vector<double> mediabox;
    bool font = false;
    std::vector<Operation> operations;
};

std::vector<Operation> operations(const std::string& content) {
    std::vector<Operation> result;
    std::vector<std::string> operands;
    std::istringstream stream(content);
    for (std::string token; stream >> token;) {
        if (std::string_view("0123456789+-./").find(token.front()) != std::string_view::npos) {
            operands.push_back(token);
        } else {
            result.push_back({std::move(operands), token});
            operands.clear();
        }
    }
    return result;
}

// pypdf's PdfReader(path).pages: media box, resources and content operations.
std::vector<PdfPage> read_pdf(const fs::path& path) {
    PdfContext mu;
    auto doc = mu.own(mu.call([&](auto ctx) { return pdf_open_document(ctx, path.c_str()); }), pdf_drop_document);
    int count = mu.call([&](auto ctx) { return pdf_count_pages(ctx, doc.get()); });
    std::vector<PdfPage> pages;
    for (int index = 0; index < count; ++index) {
        PdfPage page;
        auto object = mu.call([&](auto ctx) { return pdf_lookup_page_obj(ctx, doc.get(), index); });
        auto box = mu.call([&](auto ctx) { return pdf_dict_get_inheritable(ctx, object, PDF_NAME(MediaBox)); });
        for (int i = 0; i < 4; ++i)
            page.mediabox.push_back(mu.call([&](auto ctx) { return pdf_to_real(ctx, pdf_array_get(ctx, box, i)); }));
        page.font = mu.call([&](auto ctx) {
            return pdf_dict_get(ctx, pdf_dict_get_inheritable(ctx, object, PDF_NAME(Resources)), PDF_NAME(Font)) != nullptr;
        });
        auto contents = mu.call([&](auto ctx) { return pdf_dict_get(ctx, object, PDF_NAME(Contents)); });
        std::vector<pdf_obj*> streams{contents};
        if (mu.call([&](auto ctx) { return pdf_is_array(ctx, contents); })) {
            streams.clear();
            int length = mu.call([&](auto ctx) { return pdf_array_len(ctx, contents); });
            for (int i = 0; i < length; ++i)
                streams.push_back(mu.call([&](auto ctx) { return pdf_array_get(ctx, contents, i); }));
        }
        std::string content;
        for (auto item : streams) {
            if (!item)
                continue;
            auto data = mu.own(mu.call([&](auto ctx) { return pdf_load_stream(ctx, item); }), fz_drop_buffer);
            unsigned char* bytes = nullptr;
            auto length = fz_buffer_storage(mu.get(), data.get(), &bytes);
            content += std::string(reinterpret_cast<const char*>(bytes), length) + "\n";
        }
        page.operations = operations(content);
        pages.push_back(std::move(page));
    }
    return pages;
}

using Point = std::pair<double, double>;

// The translation of every dot's cm operator, after the page's scaling cm.
std::vector<Point> centers(const PdfPage& page) {
    std::vector<Point> result;
    for (auto& [operands, op] : page.operations)
        if (op == "cm")
            result.emplace_back(std::stod(operands.at(4)), std::stod(operands.at(5)));
    if (!result.empty())
        result.erase(result.begin());
    return result;
}

int fills(const PdfPage& page) {
    return static_cast<int>(std::count_if(page.operations.begin(), page.operations.end(), [](auto& o) { return o.op == "f"; }));
}

std::string printable_points(const std::vector<Point>& points) {
    std::ostringstream out;
    for (auto& [x, y] : points)
        out << '(' << x << ", " << y << ") ";
    return out.str();
}

// write_braille_pdf takes serialized cells: split pages at FF and lines like splitlines().
Pages text_pages(const Cells& braille) {
    Pages pages;
    for (auto& page : split(utf8(braille), '\f'))
        pages.push_back(splitlines(unicode(page)));
    return pages;
}

// A clean picture of one printed word, for real OCR in place of the mocked scan_picture.
Image word_picture(const std::string& word) {
    auto path = work("word-picture.pdf");
    write_pdf(path, {"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 60] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        pdf_stream("", "BT /F1 36 Tf 1 0 0 1 12 18 Tm (" + word + ") Tj ET")});
    return render_pdf_page(path, 1, 150, 10'000'000);
}

} // namespace

TEST_CASE("test_independently_checked_ueb") {
    // Hand assembled from ICEB §§4.1, 6.1, 6.5, 7, 8.3–8.4; no converter oracle.
    const std::vector<std::pair<std::string, Cells>> cases{
        {"abc", U"⠁⠃⠉"}, {"cat", CAT}, {"dog", DOG},
        {"Hello, world!", U"⠠⠓⠑⠇⠇⠕⠂⠀⠺⠕⠗⠇⠙⠖"},
        {"ABC", U"⠠⠠⠁⠃⠉"}, {"123", U"⠼⠁⠃⠉"}, {"3b", U"⠼⠉⠰⠃"},
        {"and the", U"⠁⠝⠙⠀⠞⠓⠑"}, {"• cat", U"⠸⠲⠀⠉⠁⠞"}};
    for (auto& [text, expected] : cases)
        check_equal(translate(text), expected, "text=" + text);
}

TEST_CASE("test_configurable_table") {
    // ICEB §10.3: strong wordsigns for "and" and "the".
    check_equal(translate("and the", "en-ueb-g2.ctb"), Cells(U"⠯⠀⠮"), "en-ueb-g2.ctb");
}

TEST_CASE("test_independently_checked_spanish") {
    // ONCE B 2 (2026) §§4, 6.1, 7, 8.1; hand-written cells, no converter oracle.
    const std::vector<std::pair<std::string, Cells>> cases{
        {"á é í ó ú ñ ü", U"⠷⠀⠮⠀⠌⠀⠬⠀⠾⠀⠻⠀⠳"},
        {"Á É Í Ó Ú Ñ Ü", U"⠨⠷⠀⠨⠮⠀⠨⠌⠀⠨⠬⠀⠨⠾⠀⠨⠻⠀⠨⠳"},
        {"año", U"⠁⠻⠕"}, {"pingüino", U"⠏⠊⠝⠛⠳⠊⠝⠕"},
        {"¿Qué?", U"⠢⠨⠟⠥⠮⠢"}, {"¡Hola!", U"⠖⠨⠓⠕⠇⠁⠖"},
        {"ESPAÑA", U"⠨⠨⠑⠎⠏⠁⠻⠁"}, {"123", U"⠼⠁⠃⠉"},
        {"12,5", U"⠼⠁⠃⠂⠑"}, {"1.234,56", U"⠼⠁⠄⠃⠉⠙⠂⠑⠋"},
        {"¿Sí? ¡No!", U"⠢⠨⠎⠌⠢⠀⠖⠨⠝⠕⠖"}};
    for (std::string table : {"es-g1.ctb", "es.tbl"})
        for (auto& [text, expected] : cases) {
            check_equal(translate(text, table), expected, "table=" + table + " text=" + text);
            check_equal(translate(normalized(text, true), table), expected, "table=" + table + " NFD text=" + text);
        }
}

TEST_CASE("test_spanish_signs_and_ordinals_from_once") {
    // Hand-written cells from CBE B 2 v2 (§§5.1, 8.3, 12, 12.1, 14.1-14.2) and the CMU maths code
    // (§§4.3.1, 5.2.2, 6.1-6.2, 7.4, 11.5); see README.md#spanish.
    const std::vector<std::pair<std::string, Cells>> cases{
        {"25°C", U"⠼⠃⠑⠴⠨⠉"}, {"9°18′27″", U"⠼⠊⠴⠼⠁⠓⠳⠼⠃⠛⠳⠳"}, {"§12", U"⠬⠼⠁⠃"}, {"l·l", U"⠇⠐⠇"},
        {"1.º 2.ª 10.ª", U"⠼⠂⠕⠀⠼⠆⠁⠀⠼⠂⠴⠁"}, {"n.º", U"⠝⠄⠕"}, {"M.ª", U"⠨⠍⠄⠁"}, {"™", U"⠣⠨⠞⠨⠍⠜"},
        {"≤ ≥ ≠ ≈", U"⠪⠶⠀⠕⠶⠀⠘⠶⠀⠐⠶⠄"}, {"∞", U"⠼⠳"}, {"√8", U"⠫⠱⠼⠓"}};
    for (std::string table : {"es-g1.ctb", "es.tbl"})
        for (auto& [text, expected] : cases)
            check_equal(translate(text, table), expected, "table=" + table + " text=" + text);
}

TEST_CASE("test_spanish_text_and_docx_imports_keep_accents_and_review_text") {
    std::string text = "¿Qué? ¡Sí! 12,5";
    Cells expected = U"⠀⠀⠢⠨⠟⠥⠮⠢⠀⠖⠨⠎⠌⠖⠀⠼⠁⠃⠂⠑\n";
    auto source = work("spanish.txt");
    write(source, normalized(text, true));
    auto word = work("spanish.docx");
    save_docx(word, {word_paragraph(text)});
    auto review_path = work("review.txt");
    for (auto& path : {source, word}) {
        Options options;
        options.language = "es";
        check_equal(unicode(convert(path, options, review_path)), expected, "suffix=" + path.extension().string());
        check_equal(normalized(read(review_path), false), text + "\n", "review text, suffix=" + path.extension().string());
    }
}

TEST_CASE("test_language_preset_matches_ocr_and_allows_explicit_overrides") {
    // The Python test mocked read_document. Natively, import_document receives the
    // Options, and OCR reads the language from Options::recognition_language().
    struct Case {
        std::string label, language, table, ocr_language, expected_ocr;
        Cells expected;
    };
    const std::vector<Case> cases{
        {"{}", "en", "", "", "eng", U"⠠⠓⠕⠇⠁\n"},
        {"{language: es}", "es", "", "", "spa", U"⠨⠓⠕⠇⠁\n"},
        {"{language: es, ocr_language: spa+eng}", "es", "", "spa+eng", "spa+eng", U"⠨⠓⠕⠇⠁\n"},
        {"{language: es, table: en-ueb-g1.ctb, ocr_language: eng}", "es", "en-ueb-g1.ctb", "eng", "eng", U"⠠⠓⠕⠇⠁\n"}};
    for (auto& c : cases) {
        Options options;
        options.language = c.language;
        options.table = c.table;
        options.ocr_language = c.ocr_language;
        options.ocr = OCR::automatic;
        options.validate();
        check(options.ocr != OCR::off, "options=" + c.label + ": OCR requested");
        check_equal(options.recognition_language(), c.expected_ocr, "options=" + c.label + ": OCR language");
        check_equal(unicode(serialize(paginate({P("Hola", 0)}, options))), c.expected, "options=" + c.label);
    }
    Options unknown;
    unknown.language = "unknown";
    expect_error_matching([&] { convert(work("input.txt"), unknown); }, "Language must be", "language=unknown");
}

TEST_CASE("test_spanish_language_cli_and_table_override") {
    auto path = work("spanish.txt");
    write(path, "¿Qué? ¡Sí! 12,5");
    auto output = work("spanish.brl");
    auto result = run_cli({path, output, "--language", "es", "--first-indent", "0"});
    check_equal(result.code, 0, result.stderr_text);
    check_equal(unicode(read(output)), Cells(U"⠢⠨⠟⠥⠮⠢⠀⠖⠨⠎⠌⠖⠀⠼⠁⠃⠂⠑\n"), "Spanish preset");
    write(path, "and the");
    result = run_cli({path, output, "--language", "es", "--table", "en-ueb-g2.ctb", "--first-indent", "0"});
    check_equal(result.code, 0, result.stderr_text);
    check_equal(unicode(read(output)), Cells(U"⠯⠀⠮\n"), "explicit table overrides the preset");
}

TEST_CASE("test_equivalent_imports") {
    auto expected = read(samples() / "simple.expected.brl");
    for (std::string suffix : {"txt", "docx", "pdf"})
        check_equal(unicode(convert(samples() / ("simple." + suffix))), unicode(expected), "suffix=" + suffix);
}

TEST_CASE("test_layout_samples") {
    auto expected = read(samples() / "layout.expected.brl");
    for (std::string suffix : {"txt", "docx"})
        check_equal(unicode(convert(samples() / ("layout." + suffix), layout(12, 3))), unicode(expected), "suffix=" + suffix);
}

TEST_CASE("test_first_indent_is_only_on_first_line_across_pages") {
    auto result = render({P("cat cat cat cat cat cat", 4, 0)}, 12, 1);
    check_equal(unicode(result), blanks(4) + CAT + B + CAT + U"\n\f" + joined({CAT, CAT, CAT}, B) + U"\n\f" + CAT + U"\n",
                "first indent");
}

TEST_CASE("test_hanging_indent_across_page_break") {
    auto result = render({P("cat cat cat cat cat cat", 2, 4)}, 12, 2);
    auto pair = CAT + B + CAT;
    check_equal(unicode(result), blanks(2) + pair + U"\n" + blanks(4) + pair + U"\n\f" + blanks(4) + pair + U"\n",
                "hanging indent");
}

TEST_CASE("test_bullet_wrap_and_page_break") {
    auto result = render(text_paragraphs("- cat cat cat cat cat"), 10, 2);
    check_equal(unicode(result), U"⠸⠲" + B + CAT + B + CAT + U"\n" + blanks(4) + CAT + U"\n\f" + blanks(4) + CAT + U"\n" +
                blanks(4) + CAT + U"\n", "bullet wrap");
}

TEST_CASE("test_source_spaces_and_continuation_indent") {
    auto paragraphs = text_paragraphs("    cat cat\ncat cat\n\ncat\n    cat cat");
    check_equal(describe(paragraphs), describe({P("cat cat cat cat", 2, 0), P("cat cat cat", 2, 4)}), "paragraphs");
}

TEST_CASE("test_bana_heading_levels_and_spacing") {
    // BANA Formats §§4.2-4.6: centered (blank before/after), cell-5 and cell-7 (no blank after).
    Document blocks{P("Cat", 0, 0, Paragraph::heading, 1), P("cat"), P("Dog", 0, 0, Paragraph::heading, 2),
                    P("dog"), P("Cow", 0, 0, Paragraph::heading, 3), P("cow")};
    check_equal(splitlines(unicode(render(blocks, 20, 25))),
                std::vector<Cells>{blanks(8) + U"⠠⠉⠁⠞", U"", blanks(2) + CAT, U"", blanks(4) + U"⠠⠙⠕⠛", blanks(2) + DOG, U"",
                                   blanks(6) + U"⠠⠉⠕⠺", blanks(2) + U"⠉⠕⠺"}, "heading styles");
    std::vector<int> levels;
    for (auto& block : text_paragraphs("# A\n\n## B\n\n### C"))
        levels.push_back(std::get<Paragraph>(block).level);
    check_equal(levels, std::vector<int>{1, 2, 3}, "Markdown heading levels");
    // A heading at the foot of a page moves to the next page with its text.
    auto output = render({P("cat cat cat cat "), P("Dog", 0, 0, Paragraph::heading), P("dog")}, 10, 4);
    auto pages = pages_of(output);
    check(pages.size() > 1, "a second page");
    auto lines = splitlines(pages[1]);
    lines.resize(std::min<std::size_t>(lines.size(), 3));
    check_equal(lines, std::vector<Cells>{blanks(3) + U"⠠⠙⠕⠛", U"", blanks(2) + DOG}, "heading kept with its text");
}

TEST_CASE("test_heading_without_blank_line_and_list_nesting") {
    auto paragraphs = text_paragraphs("# Cat\ncat\n\n- cat\n\t1. dog");
    check_equal(describe(paragraphs), describe({P("Cat", 0, 0, Paragraph::heading), P("cat"), P("• cat", 0, 4, Paragraph::list),
                                                P("1. dog", 4, 8, Paragraph::list)}), "paragraphs");
}

TEST_CASE("test_docx_inherited_layout_and_numbering") {
    auto paragraphs = read_document(samples() / "layout.docx");
    std::vector<std::string> layouts;
    for (auto& block : paragraphs) {
        auto& p = std::get<Paragraph>(block);
        layouts.push_back("(" + std::to_string(p.first) + ", " + std::to_string(p.rest) + ", '" + kind_name(p.kind) + "')");
    }
    check_equal(layouts, std::vector<std::string>{"(0, 0, 'heading')", "(2, 0, 'body')", "(2, 4, 'body')", "(0, 4, 'list')",
                                                  "(4, 8, 'list')", "(0, 4, 'list')", "(0, 4, 'list')"}, "first, rest, kind");
    auto items = texts(paragraphs);
    check_equal(std::vector<std::string>(items.begin() + 3, items.begin() + 6),
                std::vector<std::string>{"1. cat cat cat cat", "1. dog dog dog dog", "2. cat"}, "list labels");
}

TEST_CASE("test_docx_direct_formatting_overrides_each_inherited_property") {
    // python-docx: Base (left 0.5", first line 0.25"), Child based on Base (left 0.25"),
    // and a direct first-line indent of -0.125" (a 180-twip hanging indent).
    Docx document;
    document.styles =
        R"(<w:style w:type="paragraph" w:customStyle="1" w:styleId="Base"><w:name w:val="Base"/><w:pPr><w:ind w:left="720" w:firstLine="360"/></w:pPr></w:style>)"
        R"(<w:style w:type="paragraph" w:customStyle="1" w:styleId="Child"><w:name w:val="Child"/><w:basedOn w:val="Base"/><w:pPr><w:ind w:left="360"/></w:pPr></w:style>)";
    document.body = R"(<w:p><w:pPr><w:pStyle w:val="Child"/><w:ind w:hanging="180"/></w:pPr><w:r><w:t>cat cat cat</w:t></w:r></w:p>)";
    auto path = work("style.docx");
    save_docx(path, document);
    check_equal(describe(read_document(path)), describe({P("cat cat cat", 1, 2)}), "inherited indents");
    check_equal(unicode(convert(path, layout(8))), B + CAT + B + CAT + U"\n" + blanks(2) + CAT + U"\n", "Braille");
}

TEST_CASE("test_builtin_word_list_styles") {
    Docx document;
    document.styles = template_list_styles;
    document.numbering = template_list_numbering;
    document.body = word_paragraph("cat", "ListNumber") + word_paragraph("dog", "ListNumber") + word_paragraph("cat", "ListBullet");
    auto path = work("lists.docx");
    save_docx(path, document);
    check_equal(describe(read_document(path)), describe({P("1. cat", 0, 2, Paragraph::list), P("2. dog", 0, 2, Paragraph::list),
                                                         P("• cat", 0, 2, Paragraph::list)}), "list paragraphs");
}

TEST_CASE("test_docx_without_optional_numbering_part") {
    Docx document;
    document.body = word_paragraph("cat");
    document.numbering.reset();
    auto path = work("plain.docx");
    save_docx(path, document);
    check_equal(unicode(convert(path)), blanks(2) + CAT + U"\n", "Braille");
}

TEST_CASE("test_docx_number_restart_and_nested_counter_reset") {
    auto parts = read_zip(samples() / "layout.docx");
    XmlPart numbering(part(parts, "word/numbering.xml"));
    auto num = numbering.xpath("./w:num[@w:numId='99']")[0];
    xmlAddChild(num, numbering.fragment("<w:lvlOverride " + w_decl + " w:ilvl=\"0\">\n"
                                        "          <w:startOverride w:val=\"7\"/></w:lvlOverride>"));
    part(parts, "word/numbering.xml") = numbering.bytes();
    XmlPart body(part(parts, "word/document.xml"));
    xmlAddPrevSibling(body.xpath("./w:body/w:sectPr")[0],
                      body.fragment("<w:p " + w_decl + "><w:pPr><w:pStyle w:val=\"NumberedChild\"/></w:pPr>"
                                    "<w:r><w:t>dog</w:t></w:r></w:p>"));
    part(parts, "word/document.xml") = body.bytes();
    auto path = work("restart.docx");
    write_zip(path, parts);
    std::vector<std::string> items;
    for (auto& block : read_document(path))
        if (auto p = std::get_if<Paragraph>(&block); p && p->kind == Paragraph::list)
            items.push_back(p->text);
    check_equal(items, std::vector<std::string>{"7. cat cat cat cat", "1. dog dog dog dog", "8. cat", "• dog dog dog dog dog", "1. dog"},
                "list labels");
}

TEST_CASE("test_single_pdf_page_expands_to_multiple_braille_pages") {
    check_equal(pdf_page_count(samples() / "overflow.pdf"), 1, "print pages");
    auto result = convert(samples() / "overflow.pdf", layout(10, 2));
    auto pair = CAT + B + CAT;
    auto page = pair + U"\n" + pair + U"\n";
    check_equal(unicode(result), blanks(2) + pair + U"\n" + pair + U"\n\f" + joined({page, page, page}, U"\f"), "Braille pages");
}

TEST_CASE("test_page_and_line_limits_and_unicode_cells") {
    for (auto [width, height] : {std::pair{12, 3}, std::pair{16, 4}, std::pair{40, 25}}) {
        auto label = "width=" + std::to_string(width) + " height=" + std::to_string(height);
        auto result = convert(samples() / "layout.docx", layout(width, height));
        for (auto& page : pages_of(result)) {
            auto lines = splitlines(page);
            check(lines.size() <= static_cast<std::size_t>(height), label + ": too many lines");
            check(!lines.empty() && !lines.front().empty() && !lines.back().empty(), label + ": blank first or last line");
            for (auto& line : lines) {
                check(line.size() <= static_cast<std::size_t>(width), label + ": line too long");
                check(std::all_of(line.begin(), line.end(), [](char32_t c) { return U'⠀' <= c && c <= U'⣿'; }),
                      label + ": non-Braille cell");
            }
        }
    }
}

TEST_CASE("test_blank_separators_do_not_create_extra_pages") {
    check_equal(unicode(render({P("cat", 0), P("dog", 0)}, 3, 1)), CAT + U"\n\f" + DOG + U"\n", "pages");
}

TEST_CASE("test_empty_inputs") {
    auto text = work("empty.txt");
    write(text, "﻿ \n\t\n");
    auto word = work("empty.docx");
    save_docx(word, {});
    auto pdf = work("empty.pdf");
    make_pdf(pdf);
    for (auto& path : {text, word, pdf})
        check_equal(convert(path), std::string(), "path=" + path.filename().string());
}

TEST_CASE("test_scanned_and_mixed_pdfs_need_ocr") {
    expect_error_matching([] { convert(samples() / "scanned.pdf"); }, "page 1.*OCR", "scanned.pdf");
    auto path = work("mixed.pdf");
    append_pdfs({samples() / "simple.pdf", samples() / "scanned.pdf"}, path);
    expect_error_matching([&] { convert(path); }, "page 2.*OCR", "mixed.pdf");
}

TEST_CASE("test_pdf_text_layout_retains_gaps_and_relative_indents") {
    auto path = work("indented.pdf");
    make_pdf(path, {{50, 740, "cat cat"}, {50, 726, "cat"}, {150, 698, "    dog dog"}, {150, 684, "dog"},
                    {150, 656, "cat cat"}, {150, 642, "    cat"}});
    check_equal(describe(read_document(path)), describe({P("cat cat cat", 2, 0), P("dog dog dog", 2, 0), P("cat cat cat", 2, 4)}),
                "paragraphs");
}

TEST_CASE("test_pdf_centered_and_right_aligned_text_is_not_large_indentation") {
    for (int footer_x : {300, 500}) {
        auto label = "footer_x=" + std::to_string(footer_x);
        auto source = work("aligned.pdf");
        make_pdf(source, {{250, 740, "Cat"}, {50, 712, "cat cat"}, {50, 698, "cat"}, {footer_x, 50, "1 / 5"}});
        // Positioning the heading/footer far right must not become
        // literal Braille-cell padding, and their text must survive.
        check_equal(describe(read_document(source)), describe({P("Cat"), P("cat cat cat"), P("1 / 5")}), label);
        auto output = work("aligned-braille.pdf");
        auto result = run_cli({source, output});
        check_equal(result.code, 0, label + ": " + result.stderr_text);
        check_equal(pdf_page_count(output), 1, label + ": Braille pages");
    }
}

TEST_CASE("test_inline_scans_without_optional_imaging_library") {
    const std::string image = "q BI /W 1 /H 1 /CS /G /BPC 8 ID \x80 EI Q";
    for (bool inside_form : {false, true}) {
        std::vector<std::string> objects{"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"};
        if (inside_form) {
            objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /XObject << /Fm1 5 0 R >> >> /Contents 4 0 R >>");
            objects.push_back(pdf_stream("", "q /Fm1 Do Q"));
            objects.push_back(pdf_stream("/Subtype /Form /BBox [0 0 1 1]", image));
        } else {
            objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R >>");
            objects.push_back(pdf_stream("", image));
        }
        auto path = work("inline.pdf");
        write_pdf(path, objects);
        expect_error_matching([&] { convert(path); }, "OCR is required", inside_form ? "inside_form=True" : "inside_form=False");
    }
}

TEST_CASE("test_encrypted_pdf_is_clearly_unsupported") {
    auto path = work("locked.pdf");
    {
        PdfContext mu;
        auto doc = mu.own(mu.call([](auto ctx) { return pdf_create_document(ctx); }), pdf_drop_document);
        add_pdf_page(mu, doc.get(), 100, 100, "");
        pdf_write_options options = pdf_default_write_options;
        options.do_encrypt = PDF_ENCRYPT_RC4_128;
        options.permissions = -1;
        std::strcpy(options.upwd_utf8, "password");
        std::strcpy(options.opwd_utf8, "password");
        mu.call([&](auto ctx) { pdf_save_document(ctx, doc.get(), path.c_str(), &options); });
    }
    expect_error_matching([&] { convert(path); }, "Encrypted PDFs are unsupported", "locked.pdf");
}

TEST_CASE("test_unsupported_extensions") {
    for (std::string name : {"legacy.doc", "scan.svg", "file.zip", "noextension"})
        expect_error_matching([&] { convert(work(name)); }, "Unsupported input", "name=" + name);
}

TEST_CASE("test_docx_tables_use_bana_listed_format") {
    // document.add_table(rows=3, cols=2) with cell.text set, as python-docx writes it.
    std::string table = R"(<w:tbl><w:tblPr><w:tblW w:type="auto" w:w="0"/><w:tblLook w:firstColumn="1" w:firstRow="1" )"
        R"(w:lastColumn="0" w:lastRow="0" w:noHBand="0" w:noVBand="1" w:val="04A0"/></w:tblPr><w:tblGrid>)"
        R"(<w:gridCol w:w="4320"/><w:gridCol w:w="4320"/></w:tblGrid>)";
    for (auto [first, second] : {std::pair{"Name", "Age"}, std::pair{"Ada", "12"}, std::pair{"Bo", ""}}) {
        table += "<w:tr>";
        for (std::string text : {first, second})
            table += R"(<w:tc><w:tcPr><w:tcW w:type="dxa" w:w="4320"/></w:tcPr><w:p>)" +
                (text.empty() ? std::string("<w:r/>") : "<w:r><w:t>" + text + "</w:t></w:r>") + "</w:p></w:tc>";
        table += "</w:tr>";
    }
    auto path = work("table.docx");
    save_docx(path, {table + "</w:tbl>"});
    auto blocks = read_document(path);
    check(!blocks.empty(), "blocks");
    check_equal(describe(Document(blocks.begin() + 1, blocks.end())),
                describe({P("Name: Ada", 4, 4, Paragraph::row), P("Age: 12", 0, 2, Paragraph::entry),
                          P("Name: Bo", 4, 4, Paragraph::row), P("Age: ⠐⠐⠐", 0, 2, Paragraph::entry)}), "rows");
    auto& note = paragraph_at(blocks, 0);
    check_equal("(" + kind_name(note.kind) + ", " + std::to_string(note.first) + ", " + std::to_string(note.rest) + ")",
                std::string("(note, 6, 4)"), "transcriber's note layout");
    check(note.text.find("A blank entry is shown as ⠐⠐⠐.") != std::string::npos, "note explains blank entries: " + note.text);
    // BANA Formats §11.16 (ICEB cells): row heading blocked in cell 5, entries in 1-3, a blank line per row.
    auto lines = splitlines(unicode(render(blocks, 40, 40)));
    check(!lines.empty() && lines[0].starts_with(blanks(6) + U"⠈⠨⠣⠠⠏⠗⠊⠝⠞"), "note in 7-5 margins");
    auto start = std::find(lines.begin(), lines.end(), blanks(4) + U"⠠⠝⠁⠍⠑⠒⠀⠠⠁⠙⠁");
    check(start != lines.end() && start != lines.begin() && lines.end() - start >= 5, "first row heading");
    check_equal(std::vector<Cells>(start - 1, start + 5),
                std::vector<Cells>{U"", blanks(4) + U"⠠⠝⠁⠍⠑⠒⠀⠠⠁⠙⠁", U"⠠⠁⠛⠑⠒⠀⠼⠁⠃", U"", blanks(4) + U"⠠⠝⠁⠍⠑⠒⠀⠠⠃⠕",
                                   U"⠠⠁⠛⠑⠒⠀⠐⠐⠐"}, "listed rows");
    // The English note is omitted for other Braille codes.
    check(!unicode(render(blocks, 40, 40, "es-g1.ctb")).starts_with(blanks(6)), "no English note in Spanish");
}

TEST_CASE("test_docx_number_formats_without_braille_labels_are_clearly_unsupported") {
    auto path = work("table.docx");
    auto parts = read_zip(samples() / "layout.docx");
    XmlPart numbering(part(parts, "word/numbering.xml"));
    set_value(numbering.xpath("./w:abstractNum[@w:abstractNumId='99']/w:lvl/w:numFmt")[0], "chineseCounting");
    part(parts, "word/numbering.xml") = numbering.bytes();
    write_zip(path, parts);
    expect_error_matching([&] { convert(path); }, "Unsupported DOCX numbering", "chineseCounting");
}

TEST_CASE("test_docx_content_controls_fields_revisions_equations_and_notes") {
    Docx document;
    document.footnotes = "<w:footnotes " + w_decl + "><w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p>"
        "</w:footnote><w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/></w:r><w:r><w:t xml:space=\"preserve\">"
        " A note.</w:t></w:r></w:p></w:footnote></w:footnotes>";
    for (const char* xml : {
             "<w:sdt><w:sdtContent><w:p><w:r><w:t>Inside a content control.</w:t></w:r></w:p></w:sdtContent></w:sdt>",
             "<w:p><w:r><w:t xml:space=\"preserve\">Kept </w:t></w:r><w:del w:id=\"1\" w:author=\"A\"><w:r>"
             "<w:delText>removed </w:delText></w:r></w:del><w:ins w:id=\"2\" w:author=\"A\"><w:r><w:t>and inserted."
             "</w:t></w:r></w:ins></w:p>",
             "<w:p><w:r><w:t xml:space=\"preserve\">Name: </w:t></w:r><w:sdt><w:sdtContent><w:r><w:t>Ada</w:t>"
             "</w:r></w:sdtContent></w:sdt><w:r><w:t xml:space=\"preserve\">, </w:t></w:r><w:fldSimple w:instr=\"DATE\">"
             "<w:r><w:t>16/11/2026</w:t></w:r></w:fldSimple></w:p>",
             "<w:p><w:r><w:t xml:space=\"preserve\">Solve </w:t></w:r><m:oMath><m:d><m:e><m:r><m:t>x+2</m:t>"
             "</m:r></m:e></m:d><m:r><m:t>=5</m:t></m:r></m:oMath><w:r><w:t xml:space=\"preserve\"> now.</w:t></w:r></w:p>",
             "<w:p><w:r><w:t>See the note.</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r></w:p>"})
        document.body += xml;
    auto path = work("hidden.docx");
    save_docx(path, document);
    auto blocks = read_document(path);
    check_equal(texts(blocks), std::vector<std::string>{"Inside a content control.", "Kept and inserted.", "Name: Ada, 16/11/2026",
                                                        "Solve (x+2)=5 now.", "See the note.[1]", "[1] A note."}, "texts");
    auto& last = paragraph_at(blocks, blocks.size() - 1);
    check_equal("(" + std::to_string(last.first) + ", " + std::to_string(last.rest) + ")", std::string("(2, 0)"),
                "note (first, rest)");
    document.body += "<w:p><m:oMath><m:f><m:num><m:r><m:t>1</m:t></m:r></m:num>"
                     "<m:den><m:r><m:t>2</m:t></m:r></m:den></m:f></m:oMath></w:p>";
    save_docx(path, document);
    expect_error_matching([&] { read_document(path); }, "equations with fractions", "fraction");
    save_docx(path, {"<w:sdt><w:sdtContent><w:tbl><w:tr><w:tc><w:p><w:r><w:t>Cell</w:t></w:r></w:p></w:tc></w:tr>"
                     "</w:tbl></w:sdtContent></w:sdt>"});
    check_equal(describe(read_document(path)), describe({P("Cell", 0, 2, Paragraph::entry)}), "table in a content control");
}

TEST_CASE("test_line_end_hyphens_rejoin_divided_words") {
    auto text = "The infor-\nmation on Franco-\nPrussian and COVID-\n19 uses a dash -\nhere.";
    check_equal(texts(text_paragraphs(text)),
                std::vector<std::string>{"The information on Franco-Prussian and COVID-19 uses a dash - here."}, "joined text");
}

TEST_CASE("test_symbols_without_braille_are_replaced_or_named") {
    // ICEB §4.1: f ⠋, i ⠊, c ⠉, o ⠕, p ⠏. ONCE B 2: hyphen ⠤, capital ⠨, n ⠝, a ⠁, l ⠇, e ⠑, h ⠓.
    check_equal(translate("ﬁ"), Cells(U"⠋⠊"), "ligature");
    check_equal(translate("co­op​"), Cells(U"⠉⠕⠕⠏"), "format characters");
    check_equal(translate("• pan", "es-g1.ctb"), Cells(U"⠤⠀⠏⠁⠝"), "Spanish bullet");
    check_equal(translate("Nº 5", "es-g1.ctb"), Cells(U"⠨⠝⠕⠀⠼⠑"), "compatibility form");
    for (auto [text, table, name] : {std::tuple{"Hi 😀", "en-ueb-g1.ctb", "GRINNING FACE"},
                                     std::tuple{"x¹", "en-ueb-g1.ctb", "SUPERSCRIPT ONE"},
                                     std::tuple{"a → b", "es-g1.ctb", "RIGHTWARDS ARROW"}})
        expect_error_matching([&] { translate(text, table); }, std::string("no symbol for .*") + name, std::string("text=") + text);
    auto path = work("lista.txt");
    write(path, "- pan\n- leche\n");
    Options spanish;
    spanish.language = "es";
    check_equal(unicode(convert(path, spanish)), Cells(U"⠤⠀⠏⠁⠝\n⠤⠀⠇⠑⠉⠓⠑\n"), "Spanish list");
}

TEST_CASE("test_docx_letter_and_roman_numbering") {
    auto parts = read_zip(samples() / "layout.docx");
    XmlPart numbering(part(parts, "word/numbering.xml"));
    auto levels = numbering.xpath("./w:abstractNum[@w:abstractNumId='99']/w:lvl");
    check(levels.size() >= 2, "two list levels");
    auto field = [&](xmlNodePtr level, const char* name) {
        for (auto node = level->children; node; node = node->next)
            if (node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name))
                return node;
        throw std::runtime_error(std::string("missing ") + name);
    };
    set_value(field(levels[0], "numFmt"), "upperRoman");
    set_value(field(levels[1], "numFmt"), "lowerLetter");
    set_value(field(levels[1], "lvlText"), "%1.%2)");
    part(parts, "word/numbering.xml") = numbering.bytes();
    auto path = work("letters.docx");
    write_zip(path, parts);
    std::vector<std::string> items;
    for (auto& block : read_document(path))
        if (auto p = std::get_if<Paragraph>(&block); p && p->kind == Paragraph::list && items.size() < 3)
            items.push_back(p->text);
    check_equal(items, std::vector<std::string>{"I. cat cat cat cat", "I.a) dog dog dog dog", "II. cat"}, "labels");
    for (auto [number, form, label] : {std::tuple{1, "lowerLetter", "a"}, std::tuple{26, "lowerLetter", "z"},
                                       std::tuple{28, "upperLetter", "BB"}, std::tuple{4, "upperRoman", "IV"},
                                       std::tuple{9, "lowerRoman", "ix"}, std::tuple{14, "upperRoman", "XIV"},
                                       std::tuple{3, "decimalZero", "03"}, std::tuple{7, "decimal", "7"}})
        check_equal(list_number(number, form), std::string(label), std::to_string(number) + " as " + form);
}

TEST_CASE("test_text_lists_with_letters_roman_numerals_and_parentheses") {
    auto text = "a) one\nb. two\n(c) three\nii. four\nA) five\nIV. six\n\ne.g. not a list\n\nA. Lincoln wrote";
    std::vector<std::string> found;
    for (auto& block : text_paragraphs(text)) {
        auto& p = std::get<Paragraph>(block);
        found.push_back("('" + p.text + "', '" + kind_name(p.kind) + "')");
    }
    check_equal(found, std::vector<std::string>{"('a) one', 'list')", "('b. two', 'list')", "('(c) three', 'list')",
                                                "('ii. four', 'list')", "('A) five', 'list')", "('IV. six', 'list')",
                                                "('e.g. not a list', 'body')", "('A. Lincoln wrote', 'body')"}, "kinds");
}

TEST_CASE("test_pdf_print_page_numbers_and_running_heads_are_removed") {
    std::vector<fs::path> pages;
    const char* words[]{"cat", "dog", "cow"};
    for (int index = 0; index < 3; ++index) {
        pages.push_back(work("page" + std::to_string(index) + ".pdf"));
        make_pdf(pages.back(), {{250, 760, "Annual Report"}, {50, 700, words[index]},
                                {300, 40, "- " + std::to_string(index + 5) + " -"}});
    }
    auto source = work("report.pdf");
    append_pdfs(pages, source);
    check_equal(describe(read_document(source)), describe({P("Annual Report"), P("cat"), P("dog"), P("cow")}), "report");
    pages.clear();
    const char* numbers[]{"42", "7"};
    for (int index = 0; index < 2; ++index) {
        pages.push_back(work("answer" + std::to_string(index) + ".pdf"));
        make_pdf(pages.back(), {{50, 700, "cat"}, {50, 600, numbers[index]}});
    }
    append_pdfs(pages, source);
    check_equal(describe(read_document(source)), describe({P("cat"), P("42"), P("cat"), P("7")}), "answers");
}

TEST_CASE("test_pdf_two_columns_are_read_left_then_right") {
    std::vector<std::string> left, right;
    for (int i = 0; i < 5; ++i) {
        left.push_back("left words " + std::to_string(i) + " of the first column");
        right.push_back("right words " + std::to_string(i) + " of the second column");
    }
    left.emplace_back();
    right.emplace_back();
    for (int i = 5; i < 9; ++i) {
        left.push_back("left words " + std::to_string(i) + " in another part");
        right.push_back("right words " + std::to_string(i) + " closing this page");
    }
    std::vector<Text> lines{{50, 720, "A Title Across Both Columns Of This Page Here"}};
    for (int index = 0; index < static_cast<int>(left.size()); ++index)
        for (auto& [x, words] : {std::pair{50, left[index]}, std::pair{320, right[index]}})
            if (!words.empty())
                lines.push_back({x, 680 - 14 * index, words});
    auto source = work("columns.pdf");
    make_pdf(source, lines);
    auto section = [](const std::vector<std::string>& column, int from, int to) {
        return join(std::vector<std::string>(column.begin() + from, column.begin() + to), " ");
    };
    check_equal(texts(read_document(source)),
                std::vector<std::string>{"A Title Across Both Columns Of This Page Here", section(left, 0, 5), section(left, 6, 10),
                                         section(right, 0, 5), section(right, 6, 10)}, "column order");
    // Short label/value pairs, as in a form, keep their rows together.
    lines.clear();
    for (int i = 0; i < 10; ++i)
        for (auto& [x, words] : {std::pair{50, "Label " + std::to_string(i) + ":"}, std::pair{320, "value " + std::to_string(i)}})
            lines.push_back({x, 700 - 14 * i, words});
    make_pdf(source, lines);
    auto form = read_document(source);
    check(!form.empty() && paragraph_at(form, 0).text.starts_with("Label 0: value 0 Label 1: value 1"),
          "form rows stay together: " + (form.empty() ? std::string() : paragraph_at(form, 0).text));
}

TEST_CASE("test_pdf_scan_with_a_few_text_layer_words_is_recognized") {
    // The Python test mocked scan_picture and the page renderer. Natively, Tesseract reads a
    // real picture of the word "recognized"; the scanner's three words are an invisible
    // text layer, so only the picture is visible when the page is rendered.
    auto picture = word_picture("recognized");
    auto source = work("scan.pdf");
    make_pdf(source, {{50, 740, "Scanned with App"}}, &picture, true, "240 0 0 60 50 600");
    Options service;  // page_renderer: the service renders pages for OCR
    service.ocr = OCR::automatic;
    service.ocr_embedded = false;
    auto imported = import_document(source, service);
    check_equal(describe(imported.blocks), describe({P("recognized")}), "rendered page recognized");
    check(imported.recognized_pages == std::vector<std::variant<int, std::string>>{1}, "OCR pages are [1]");
    // Without a page renderer (the CLI), the words stay and the picture is recognized.
    Options cli;
    cli.ocr = OCR::automatic;
    check_equal(describe(read_document(source, cli)), describe({P("Scanned with App"), P("recognized")}), "CLI");
    // Twelve text-layer words are a text page: OCR would add "recognized".
    auto full = work("full.pdf");
    std::vector<std::string> twelve(12, "word");
    make_pdf(full, {{50, 740, join(twelve, " ")}}, &picture, false, "240 0 0 60 50 600");
    service.images = false;
    check_equal(texts(read_document(full, service)), std::vector<std::string>{join(twelve, " ")}, "no OCR");
}

TEST_CASE("test_configuration_and_overlong_words") {
    auto options = [](auto change) {
        Options value;
        change(value);
        return value;
    };
    const std::vector<std::pair<std::string, Options>> cases{
        {"cells_per_line=0", options([](Options& o) { o.width = 0; })},
        {"lines_per_page=0", options([](Options& o) { o.height = 0; })},
        {"first_indent=-1", options([](Options& o) { o.first_indent = -1; })},
        {"list_indent=-1", options([](Options& o) { o.list_indent = -1; })},
        {"cells_per_line=2", options([](Options& o) { o.width = 2; })},
        {"first_indent=40", options([](Options& o) { o.first_indent = 40; })}};
    for (auto& [label, value] : cases)
        expect_error([&] { convert(samples() / "simple.txt", value); }, {}, "options=" + label);
    expect_error_matching([] { render({P("abcdef", 0)}, 5); }, "Translated word needs 6 cells", "overlong word");
}

TEST_CASE("test_indentation_and_table_options_through_cli") {
    auto path = work("input.txt");
    write(path, "and the and the\n\n- cat cat cat cat");
    auto output = work("output.brl");
    auto result = run_cli({path, output, "--table", "en-ueb-g2.ctb", "--first-indent", "0", "--list-indent", "2",
                           "--cells-per-line", "10", "--lines-per-page", "2"});
    check_equal(result.code, 0, result.stderr_text);
    check_equal(unicode(read(output)), Cells(U"⠯⠀⠮⠀⠯⠀⠮\n⠸⠲⠀⠉⠁⠞⠀⠉⠁⠞\n\f⠀⠀⠉⠁⠞⠀⠉⠁⠞\n"), "Braille");
}

TEST_CASE("test_cli_errors_preserve_output_and_have_no_traceback") {
    auto output = work("output.brl");
    write(output, "existing output");
    auto malformed = work("bad.docx");
    write(malformed, "not a Word document");
    auto bad_pdf = work("bad.pdf");
    write(bad_pdf, "not a PDF");
    auto invalid_utf8 = work("bad.txt");
    write(invalid_utf8, "\xff");
    const std::vector<std::vector<std::string>> cases{
        {samples() / "scanned.pdf"}, {malformed}, {bad_pdf}, {invalid_utf8}, {work("missing.txt")}, {work("old.doc")},
        {samples() / "simple.txt", "--table", "missing-table.ctb"}, {samples() / "simple.txt", "--cells-per-line", "2"}};
    for (auto& args : cases) {
        auto label = "source=" + fs::path(args.front()).filename().string() + " options=" +
            join(std::vector<std::string>(args.begin() + 1, args.end()), " ");
        std::vector<std::string> command{args.front(), output};
        command.insert(command.end(), args.begin() + 1, args.end());
        auto result = run_cli(command);
        check_equal(result.code, 2, label + ": " + result.stderr_text);
        check(result.stderr_text.find("error:") != std::string::npos, label + ": no error message");
        check(result.stderr_text.find("Traceback") == std::string::npos, label + ": traceback");
        check_equal(read(output), std::string("existing output"), label + ": output changed");
    }
}

TEST_CASE("test_cli_refuses_to_overwrite_its_input") {
    auto path = work("source.txt");
    write(path, "cat");
    auto result = run_cli({path, path});
    check_equal(result.code, 2, result.stderr_text);
    check(result.stderr_text.find("different files") != std::string::npos, result.stderr_text);
    check_equal(read(path), std::string("cat"), "input unchanged");
}

TEST_CASE("test_pdf_draws_correct_dot_positions_without_fonts") {
    auto output = work("dots.pdf");
    // Unicode single-dot cells 1–8; these literals are independent of Liblouis.
    preview(text_pages(U"⠁⠂⠄⠈⠐⠠⡀⢀\n"), output, 8, 1);
    auto pages = read_pdf(output);
    check(!pages.empty(), "a page");
    auto& page = pages[0];
    std::vector<Point> expected{{2, -2}, {16, -7}, {30, -12}, {49, -2}, {63, -7}, {77, -12}, {86, -17}, {105, -17}};
    check(centers(page) == expected, "dot centers: " + printable_points(centers(page)));
    check(page.mediabox == std::vector<double>{0, 0, 612, 792}, "US Letter media box");
    check(!page.font, "no font resource");
    check_equal(fills(page), 8, "filled dots");
}

TEST_CASE("test_cli_writes_visible_pdf_from_each_input_format") {
    for (std::string suffix : {"txt", "docx", "pdf"}) {
        auto output = work(suffix + ".pdf");
        auto result = run_cli({samples() / ("simple." + suffix), output});
        check_equal(result.code, 0, "suffix=" + suffix + ": " + result.stderr_text);
        auto pages = read_pdf(output);
        check_equal(static_cast<int>(pages.size()), 1, "suffix=" + suffix + ": pages");
        // Cat = 8, dog = 10, numeric 123 = 9; page number 1 adds 5 dots.
        check_equal(fills(pages[0]), 32, "suffix=" + suffix + ": dots");
    }
}

TEST_CASE("test_pdf_preserves_hanging_indents_across_pages") {
    auto output = work("layout.pdf");
    auto result = run_cli({samples() / "layout.docx", output, "--cells-per-line", "12", "--lines-per-page", "3"});
    check_equal(result.code, 0, result.stderr_text);
    auto pages = read_pdf(output);
    check_equal(static_cast<int>(pages.size()), 9, "pages");
    std::vector<std::vector<Point>> dots;
    for (auto& page : pages)
        dots.push_back(centers(page));
    auto first = [&](int page) { return dots[page].empty() ? Point{-1, -1} : dots[page].front(); };
    check(std::find(dots[2].begin(), dots[2].end(), Point{30, -30}) != dots[2].end(), "two-cell first-line indent");
    check(first(3) == Point{58, -2}, "four-cell hanging continuation: " + printable_points({first(3)}));
    check(first(6) == Point{114, -2}, "eight-cell nested continuation: " + printable_points({first(6)}));
    check(first(8) == Point{58, -2}, "four-cell indent after page break: " + printable_points({first(8)}));
}

TEST_CASE("test_prose_indents_normalize_without_blank_paragraph_lines") {
    auto paragraphs = text_paragraphs("      cat cat cat\ncat\n\n      dog");
    check_equal(describe(paragraphs), describe({P("cat cat cat cat"), P("dog")}), "paragraphs");
    check_equal(unicode(render(paragraphs, 12)), blanks(2) + CAT + B + CAT + U"\n" + CAT + B + CAT + U"\n" + blanks(2) + DOG + U"\n",
                "Braille");
    check_equal(describe(text_paragraphs("      cat", 4)), describe({P("cat", 4)}), "configured first indent");
}

TEST_CASE("test_blocked_paragraphs_remain_separated") {
    check_equal(unicode(render({P("cat", 0), P("dog", 0)}, 10)), CAT + U"\n\n" + DOG + U"\n", "blank line between blocks");
}

TEST_CASE("test_numbered_pages_reserve_last_line_and_keep_continuation_margin") {
    Document paragraphs{P("cat cat cat cat cat cat cat cat")};
    auto pair = CAT + B + CAT;
    auto result = render(paragraphs, 10, 3, "en-ueb-g1.ctb", true);
    check_equal(unicode(result), blanks(2) + pair + U"\n" + pair + U"\n" + blanks(8) + U"⠼⠁\n\f" + pair + U"\n" + pair + U"\n" +
                blanks(8) + U"⠼⠃\n", "numbered pages");
    for (auto& page : pages_of(result)) {
        auto lines = splitlines(page);
        check_equal(static_cast<int>(lines.size()), 3, "lines per page");
        check_equal(static_cast<int>(lines.back().size()), 10, "page number line width");
    }
    for (auto [width, height] : {std::pair{4, 3}, std::pair{10, 1}})
        expect_error([&] { render({P("cat", 0)}, width, height, "en-ueb-g1.ctb", true); }, {},
                     "width=" + std::to_string(width) + " height=" + std::to_string(height));
}

TEST_CASE("test_pdf_page_count_footers_are_removed_only_at_page_end") {
    auto first = work("first.pdf"), second = work("second.pdf");
    make_pdf(first, {{50, 740, "      cat cat"}, {50, 726, "cat"}, {50, 698, "1 / 2"}, {50, 670, "dog"}, {400, 50, "1 / 2"}});
    make_pdf(second, {{50, 740, "      dog"}, {400, 50, "Page 2 of 2"}});
    auto source = work("pages.pdf");
    append_pdfs({first, second}, source);
    check_equal(describe(read_document(source)), describe({P("cat cat cat"), P("1 / 2"), P("dog"), P("dog")}), "paragraphs");
}

TEST_CASE("test_empty_pdf_output_is_one_blank_page") {
    auto source = work("empty.txt");
    write(source, "");
    auto output = work("empty.pdf");
    auto result = run_cli({source, output});
    check_equal(result.code, 0, result.stderr_text);
    auto pages = read_pdf(output);
    check_equal(static_cast<int>(pages.size()), 1, "pages");
    check_equal(fills(pages[0]), 0, "dots");
}

TEST_CASE("test_pdf_large_grid_fits_on_page") {
    auto output = work("large.pdf");
    preview(text_pages(Cells(99, U'\n') + blanks(99) + U"⣿\n"), output, 100, 100);
    auto pages = read_pdf(output);
    check(!pages.empty(), "a page");
    std::vector<std::vector<double>> matrices;
    for (auto& [operands, op] : pages[0].operations)
        if (op == "cm") {
            std::vector<double> values;
            for (auto& operand : operands)
                values.push_back(std::stod(operand));
            matrices.push_back(values);
        }
    check(matrices.size() > 1, "dots drawn");
    double scale = matrices[0][0];
    for (std::size_t i = 1; i < matrices.size(); ++i) {
        double x = matrices[i][4], y = matrices[i][5];
        check(36 <= 36 + scale * x && 36 + scale * x <= 576, "x inside margins");
        check(36 <= 756 + scale * y && 756 + scale * y <= 756, "y inside margins");
    }
}

int main(int argc, char** argv) { return native_test::run(argc, argv); }
