// Browser, IPP queue and raster checks of braille-service: the PrinterTests and
// RasterTests of the original Python suite, one TEST_CASE each with the same
// name and literal expectations. The service runs on free loopback ports with a
// temporary state directory. cpp-httplib plays the browser, and libcups sends
// the IPP requests and writes the PWG and Apple raster inputs. Direct
// Get-Printer-Attributes checks replace the ipptool and driverless runs.

#include "braille.hpp"
#include "check.hpp"
#include "job_settings.hpp"
#include "mupdf.hpp"
#include "raster.hpp"

#include <cups/cups.h>
#include <cups/pwg.h>
#include <cups/raster.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <thread>

using namespace braille;
using namespace native_test;
using json = nlohmann::json;
using namespace std::chrono_literals;

extern char** environ;

namespace {

const std::map<std::string, std::string> mimes{
    {".txt", "text/plain"}, {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".pdf", "application/pdf"}, {".pwg", "image/pwg-raster"}, {".urf", "image/urf"},
    {".brl", "application/vnd.openbraille"}};

fs::path fixture(const std::string& name) { return context().root / "tests/fixtures" / name; }

json result(const fs::path& job) { return json::parse(read(job / "result.json")); }

// Removed with its contents at the end of its scope, like tempfile.TemporaryDirectory.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string pattern = std::string(base && *base ? base : "/tmp") + "/braille-printer-XXXXXX";
        check(mkdtemp(pattern.data()) != nullptr, "Cannot create a temporary directory");
        path_ = pattern;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

int free_port() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof address;
    bool found = fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
                 getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    if (fd >= 0) close(fd);
    check(found, "Cannot reserve a loopback port");
    return ntohs(address.sin_port);
}

// An HTTP error status, as urllib raises it.
struct HttpError : std::runtime_error {
    int code;
    HttpError(int status, const std::string& message) : std::runtime_error(message), code(status) {}
};

struct Page {
    std::string path;  // after redirects, like the response URL
    std::string body;
    int status;
};

// urllib's opener with a cookie jar: keeps cookies, follows redirects, and
// raises HttpError for 4xx and 5xx responses.
class Browser {
public:
    explicit Browser(int port) : port_(port) {}

    Page get(const std::string& path, int timeout = 30) { return open(path, nullptr, {}, timeout); }
    Page post(const std::string& path, const std::string& body, const std::string& type) {
        return open(path, &body, type, 30);
    }

private:
    Page open(std::string path, const std::string* body, const std::string& type, int timeout) {
        for (int redirect = 0; redirect < 10; ++redirect) {
            httplib::Client client("127.0.0.1", port_);
            client.set_connection_timeout(timeout);
            client.set_read_timeout(timeout);
            client.set_write_timeout(timeout);
            httplib::Headers headers;
            std::string cookie;
            for (const auto& [name, value] : cookies_) cookie += (cookie.empty() ? "" : "; ") + name + '=' + value;
            if (!cookie.empty()) headers.emplace("Cookie", cookie);
            auto method = std::string(body ? "POST " : "GET ") + path;
            auto response = body ? client.Post(path, headers, *body, type) : client.Get(path, headers);
            if (!response) throw std::runtime_error(method + ": " + httplib::to_string(response.error()));
            auto [first, last] = response->headers.equal_range("Set-Cookie");
            for (auto header = first; header != last; ++header) {
                auto pair = header->second.substr(0, header->second.find(';'));
                if (auto equals = pair.find('='); equals != std::string::npos)
                    cookies_[pair.substr(0, equals)] = pair.substr(equals + 1);
            }
            if (response->status >= 400)
                throw HttpError(response->status, method + ": HTTP " + std::to_string(response->status));
            if (response->status >= 300 && response->has_header("Location")) {
                path = response->get_header_value("Location");
                if (auto scheme = path.find("://"); scheme != std::string::npos)
                    path = path.substr(std::min(path.size(), path.find('/', scheme + 3)));
                body = nullptr;  // 303 See Other: fetch the new page
                continue;
            }
            return {path, response->body, response->status};
        }
        throw std::runtime_error("Too many redirects");
    }

    int port_;
    std::map<std::string, std::string> cookies_;
};

std::string form_token(const std::string& page) {
    std::smatch match;
    check(std::regex_search(page, match, std::regex("name=token value=([a-f0-9]+)")), "No form token in " + page);
    return match[1];
}

// braille-service with its state in a temporary directory, shared by the
// queue tests and stopped when the suite ends.
class Service {
public:
    Service() : port_(free_port()) {
        do ipp_port_ = free_port();
        while (ipp_port_ == port_);
        start();
    }
    ~Service() { stop(false); }

    const fs::path& directory() const { return directory_.path(); }
    int ipp_port() const { return ipp_port_; }
    Browser& browser() { return *browser_; }

    void start() {
        auto log = directory() / "test.log";
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        check(fd >= 0, "Cannot open " + log.string());
        std::vector<std::string> args{context().cli.string(), "--state", directory().string(),
                                      "--port", std::to_string(port_), "--ipp-port", std::to_string(ipp_port_)};
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t files;
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_adddup2(&files, fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&files, fd, STDERR_FILENO);
        int failed = posix_spawn(&pid_, argv.front(), &files, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&files);
        close(fd);
        if (failed) {
            pid_ = -1;
            throw std::runtime_error("Cannot start " + args.front());
        }
        browser_ = std::make_unique<Browser>(port_);
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
                pid_ = -1;
                throw std::runtime_error(read(log));
            }
            try {
                browser_->get("/", 1);
                return;
            } catch (const std::exception&) {
                std::this_thread::sleep_for(100ms);
            }
        }
        stop(false);
        throw std::runtime_error("Test service did not start");
    }

    // SIGTERM, then up to 15 seconds for a clean exit.
    void stop(bool strict = true) {
        if (pid_ < 0) return;
        kill(pid_, SIGTERM);
        for (int attempt = 0; attempt < 150; ++attempt) {
            if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(100ms);
        }
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
        if (strict) throw std::runtime_error("The service did not stop within 15 seconds");
    }

private:
    TemporaryDirectory directory_;
    int port_, ipp_port_ = 0;
    pid_t pid_ = -1;
    std::unique_ptr<Browser> browser_;
};

std::unique_ptr<Service> running;
std::optional<std::string> start_failure;

Service& service() {
    if (start_failure) throw std::runtime_error(*start_failure);
    if (!running) {
        try {
            running = std::make_unique<Service>();
        } catch (const std::exception& error) {
            start_failure = error.what();
            throw;
        }
    }
    return *running;
}

fs::path prepared(int number) {
    auto directory = service().directory() / "jobs" / std::to_string(number);
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (fs::exists(directory / "failed")) throw std::runtime_error(read(directory / "failed"));
        if (fs::exists(directory / "prepared")) return directory;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("Conversion did not finish");
}

// The browser's multipart form submission; the job number ends the response URL.
int upload(const std::string& boundary, const std::string& body) {
    auto page = service().browser().post("/submit", body, "multipart/form-data; boundary=" + boundary);
    return integer(page.path.substr(page.path.rfind('/') + 1), "job number");
}

// ---- IPP through libcups ---------------------------------------------------

using Response = std::unique_ptr<ipp_t, decltype(&ippDelete)>;

// The operation attributes that every request starts with.
ipp_t* request(ipp_op_t operation) {
    static int id = 0;
    auto uri = "ipp://127.0.0.1:" + std::to_string(service().ipp_port()) + "/ipp/print";
    ipp_t* message = ippNew();
    ippSetOperation(message, operation);
    ippSetRequestId(message, ++id);
    ippAddString(message, IPP_TAG_OPERATION, IPP_TAG_CHARSET, "attributes-charset", nullptr, "utf-8");
    ippAddString(message, IPP_TAG_OPERATION, IPP_TAG_LANGUAGE, "attributes-natural-language", nullptr, "en");
    ippAddString(message, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, uri.c_str());
    ippAddString(message, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr, cupsUser());
    return message;
}

// Sends and frees the request. The status must be successful-ok when exact,
// and otherwise any successful status.
Response send(ipp_t* message, const fs::path& document = {}, bool exact = false) {
    std::string operation = ippOpString(ippGetOperation(message));
    std::unique_ptr<http_t, decltype(&httpClose)> http(
        httpConnect2("127.0.0.1", service().ipp_port(), nullptr, AF_INET, HTTP_ENCRYPTION_IF_REQUESTED, 1, 30000, nullptr),
        httpClose);
    if (!http) {
        ippDelete(message);
        throw std::runtime_error("Cannot reach the IPP queue");
    }
    Response response(document.empty() ? cupsDoRequest(http.get(), message, "/ipp/print")
                                       : cupsDoFileRequest(http.get(), message, "/ipp/print", document.c_str()),
                      ippDelete);
    auto status = response ? ippGetStatusCode(response.get()) : IPP_STATUS_ERROR_INTERNAL;
    check(response && (exact ? status == IPP_STATUS_OK : status < IPP_STATUS_ERROR_BAD_REQUEST),
          operation + ": " + ippErrorString(status) + " (" + cupsLastErrorString() + ")");
    return response;
}

int job_id(const Response& response) {
    auto attribute = ippFindAttribute(response.get(), "job-id", IPP_TAG_INTEGER);
    check(attribute != nullptr, "The response has no job-id");
    return ippGetInteger(attribute, 0);
}

// Print-Job held indefinitely, with the JSON settings as braille-settings.
int submit(const fs::path& source, const json& settings = json::object(), int copies = 0, int page = 0) {
    auto format = mimes.at(extension(source));
    ipp_t* job = request(IPP_OP_PRINT_JOB);
    ippAddString(job, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name", nullptr, source.filename().c_str());
    ippAddString(job, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", nullptr, format.c_str());
    ippAddString(job, IPP_TAG_JOB, IPP_TAG_TEXT, "braille-settings", nullptr, settings.dump().c_str());
    ippAddString(job, IPP_TAG_JOB, IPP_TAG_KEYWORD, "job-hold-until", nullptr, "indefinite");
    if (copies) ippAddInteger(job, IPP_TAG_JOB, IPP_TAG_INTEGER, "copies", copies);
    if (page) ippAddRange(job, IPP_TAG_JOB, "page-ranges", page, page);
    return job_id(send(job, source));
}

struct Job {
    int id = 0;
    std::string name;
    int state = 0;
};

std::vector<Job> jobs() {
    ipp_t* query = request(IPP_OP_GET_JOBS);
    ippAddString(query, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", nullptr, "all");
    const char* attributes[] = {"job-id", "job-name", "job-state"};
    ippAddStrings(query, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 3, nullptr, attributes);
    auto response = send(query);
    std::vector<Job> list;
    Job job;
    auto finish = [&] {
        if (job.id) list.push_back(job);
        job = {};
    };
    for (auto attribute = ippFirstAttribute(response.get()); attribute; attribute = ippNextAttribute(response.get())) {
        const char* name = ippGetName(attribute);
        if (!name || ippGetGroupTag(attribute) != IPP_TAG_JOB) {
            finish();
            continue;
        }
        std::string_view key = name;
        if (key == "job-id") job.id = ippGetInteger(attribute, 0);
        else if (key == "job-state") job.state = ippGetInteger(attribute, 0);
        else if (key == "job-name" && ippGetString(attribute, 0, nullptr)) job.name = ippGetString(attribute, 0, nullptr);
    }
    finish();
    return list;
}

Job job(int number) {
    for (const auto& entry : jobs())
        if (entry.id == number) return entry;
    throw std::runtime_error("Job " + std::to_string(number) + " is not in the queue");
}

void cancel(int number) {
    ipp_t* message = request(IPP_OP_CANCEL_JOB);
    ippAddInteger(message, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", number);
    send(message);
}

struct Media {
    int width = -1, height = -1, bottom = -1, left = -1, right = -1, top = -1;
    bool operator==(const Media&) const = default;
    std::string text() const {
        return std::to_string(width) + "x" + std::to_string(height) + " margins " + std::to_string(bottom) + "/" +
               std::to_string(left) + "/" + std::to_string(right) + "/" + std::to_string(top);
    }
};

Media media(ipp_t* collection) {
    auto integer = [](ipp_t* group, const char* name) {
        auto attribute = ippFindAttribute(group, name, IPP_TAG_INTEGER);
        return attribute ? ippGetInteger(attribute, 0) : -1;
    };
    Media value;
    if (auto size = ippFindAttribute(collection, "media-size", IPP_TAG_BEGIN_COLLECTION)) {
        value.width = integer(ippGetCollection(size, 0), "x-dimension");
        value.height = integer(ippGetCollection(size, 0), "y-dimension");
    }
    value.bottom = integer(collection, "media-bottom-margin");
    value.left = integer(collection, "media-left-margin");
    value.right = integer(collection, "media-right-margin");
    value.top = integer(collection, "media-top-margin");
    return value;
}

// Linux clients generate a PPD from these attributes (OpenPrinting's driverless).
// Its default PageSize/PageRegion name comes from the default size and
// whether its margins are zero, ImageableArea from the margins, and
// PaperDimension from the size. Each default is one of the generated choices
// when the default size and margins match a media-col-database entry and the
// default media name is supported.
void check_ppd_defaults() {
    ipp_t* query = request(IPP_OP_GET_PRINTER_ATTRIBUTES);
    const char* attributes[] = {"media-col-database", "media-col-default", "media-default", "media-supported"};
    ippAddStrings(query, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 4, nullptr, attributes);
    auto response = send(query);
    auto collections = [&](const char* name) {
        std::vector<Media> list;
        if (auto attribute = ippFindAttribute(response.get(), name, IPP_TAG_BEGIN_COLLECTION))
            for (int i = 0; i < ippGetCount(attribute); ++i) list.push_back(media(ippGetCollection(attribute, i)));
        return list;
    };
    auto keywords = [&](const char* name) {
        std::vector<std::string> list;
        if (auto attribute = ippFindAttribute(response.get(), name, IPP_TAG_ZERO))
            for (int i = 0; i < ippGetCount(attribute); ++i)
                if (auto text = ippGetString(attribute, i, nullptr)) list.emplace_back(text);
        return list;
    };
    auto database = collections("media-col-database");
    auto defaults = collections("media-col-default");
    check(defaults.size() == 1, "The printer reports no media-col-default");
    const auto& default_media = defaults.front();
    check(default_media.bottom >= 0 && default_media.left >= 0 && default_media.right >= 0 && default_media.top >= 0,
          "Incomplete media-col-default margins: " + default_media.text());
    std::string choices;
    for (const auto& entry : database) choices += "\n  " + entry.text();
    check(std::find(database.begin(), database.end(), default_media) != database.end(),
          "Default media " + default_media.text() + " is not among the media-col-database choices:" + choices);
    auto name = keywords("media-default");
    auto supported = keywords("media-supported");
    check(name.size() == 1, "The printer reports no media-default");
    check(std::find(supported.begin(), supported.end(), name.front()) != supported.end(),
          "media-default " + name.front() + " is not in media-supported: " + printable(supported));
    const auto* named_size = pwgMediaForPWG(name.front().c_str());
    check(named_size && named_size->width == default_media.width && named_size->length == default_media.height,
          "media-default " + name.front() + " does not match the dimensions in media-col-default");
}

// ---- Fixtures --------------------------------------------------------------

// libcups' encoder as an independent source: two 8 × 4 pages, 1-bit or 8-bit.
void write_raster(const fs::path& path, bool apple, int bits) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    check(fd >= 0, "Cannot create " + path.string());
    cups_page_header2_t header{};
    header.cupsWidth = 8;
    header.cupsHeight = 4;
    header.cupsBitsPerColor = header.cupsBitsPerPixel = bits;
    header.cupsColorOrder = CUPS_ORDER_CHUNKED;
    header.cupsNumColors = 1;
    header.cupsColorSpace = bits == 1 ? CUPS_CSPACE_K : CUPS_CSPACE_SW;
    header.cupsBytesPerLine = bits == 1 ? 1 : 8;
    header.HWResolution[0] = header.HWResolution[1] = 300;
    header.PageSize[0] = header.cupsPageSize[0] = 612;
    header.PageSize[1] = header.cupsPageSize[1] = 792;
    std::strcpy(header.cupsPageSizeName, "na_letter_8.5x11in");
    unsigned char gray[8] = {0, 255, 0, 255, 255, 0, 255, 0}, packed = 0xa5;
    auto raster = cupsRasterOpen(fd, apple ? CUPS_RASTER_WRITE_APPLE : CUPS_RASTER_WRITE_PWG);
    bool written = raster != nullptr;
    for (int page = 0; written && page < 2; ++page) {
        written = cupsRasterWriteHeader2(raster, &header);
        for (int y = 0; written && y < 4; ++y)
            written = cupsRasterWritePixels(raster, bits == 1 ? &packed : gray, header.cupsBytesPerLine) ==
                      header.cupsBytesPerLine;
    }
    if (raster) cupsRasterClose(raster);
    close(fd);
    check(written, "Cannot encode " + path.string());
}

// The service's raster decoding with the limits of the given settings.
std::vector<Image> raster_pages(const fs::path& path, const json& settings) {
    auto options = job_options(job_settings(settings), "");
    std::vector<Image> pages;
    read_raster(path, options.max_pages, options.max_pixels, [&](int, Image image) { pages.push_back(std::move(image)); });
    return pages;
}

// The written gray row, four times: bytes([0, 255, 0, 255, 255, 0, 255, 0]) * 4.
const std::vector<std::uint8_t> stripes = [] {
    constexpr std::uint8_t row[8] = {0, 255, 0, 255, 255, 0, 255, 0};
    std::vector<std::uint8_t> pixels(32);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = row[i % 8];
    return pixels;
}();

// Every page of each source, in order, as one PDF.
void join_pdfs(const std::vector<fs::path>& sources, const fs::path& target) {
    PdfContext mu;
    auto document = mu.own(mu.call([](fz_context* ctx) { return pdf_create_document(ctx); }), pdf_drop_document);
    for (const auto& source : sources) {
        auto input = mu.own(mu.call([&](fz_context* ctx) { return pdf_open_document(ctx, source.c_str()); }),
                            pdf_drop_document);
        int count = mu.call([&](fz_context* ctx) { return pdf_count_pages(ctx, input.get()); });
        for (int page = 0; page < count; ++page)
            mu.call([&](fz_context* ctx) { pdf_graft_page(ctx, document.get(), -1, input.get(), page); });
    }
    save_pdf(mu, document.get(), target.string());
}

} // namespace

// ---- PrinterTests ----------------------------------------------------------

TEST_CASE("test_equivalent_original_documents_remain_held") {
    std::vector<std::string> outputs;
    for (std::string suffix : {"txt", "docx", "pdf"}) {
        int number = submit(fixture("simple." + suffix));
        auto directory = prepared(number);
        outputs.push_back(read(directory / "converted.brl"));
        auto metadata = result(directory);
        check(!metadata["used_ocr"].get<bool>(), suffix + ": used_ocr");
        check_equal(read(directory / "imported.txt"), std::string("Cat\n\ndog\n\n123\n"), suffix + ": review text");
        check_equal(job(number).state, 4, suffix + ": job state");  // IPP pending-held, never completed/embossed
    }
    check_equal(outputs[0], outputs[1], "TXT and DOCX Braille");
    check_equal(outputs[0], outputs[2], "TXT and PDF Braille");
}

TEST_CASE("test_upload_uses_same_queue_and_downloads_results") {
    auto& browser = service().browser();
    auto token = form_token(browser.get("/").body);
    std::string boundary = "BrailleTestBoundary";
    std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"token\"\r\n\r\n" + token + "\r\n"
                       "--" + boundary + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"simple.txt\"\r\n"
                       "Content-Type: text/plain\r\n\r\nCat\n\n dog\r\n--" + boundary + "--\r\n";
    int number = upload(boundary, body);
    prepared(number);
    auto download = browser.get("/jobs/" + std::to_string(number) + "/converted.brl");
    check_equal(download.status, 200, "Download status");
    check(download.body.find("⠠⠉⠁⠞") != std::string::npos, "Downloaded Braille: " + download.body);
}

TEST_CASE("test_spanish_upload_selects_table_and_remains_held") {
    auto& browser = service().browser();
    auto home = browser.get("/").body;
    check(home.find("<option value=es-g1.ctb>Spanish uncontracted (Español)</option>") != std::string::npos,
          "The upload page lacks the Spanish table");
    auto token = form_token(home);
    std::string boundary = "SpanishUploadBoundary";
    std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"token\"\r\n\r\n" + token + "\r\n"
                       "--" + boundary + "\r\nContent-Disposition: form-data; name=\"table\"\r\n\r\nes-g1.ctb\r\n"
                       "--" + boundary + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"spanish.txt\"\r\n"
                       "Content-Type: text/plain; charset=utf-8\r\n\r\n¿Qué? 12,5\r\n--" + boundary + "--\r\n";
    int number = upload(boundary, body);
    auto directory = prepared(number);
    auto cells = read(directory / "converted.brl");
    check(cells.starts_with("⠀⠀⠢⠨⠟⠥⠮⠢⠀⠼⠁⠃⠂⠑\n"), "Spanish cells: " + cells);
    check_equal(read(directory / "imported.txt"), std::string("¿Qué? 12,5\n"), "Review text");
    check_equal(result(directory)["settings"]["table"].get<std::string>(), std::string("es-g1.ctb"), "Recorded table");
    check_equal(job(number).state, 4, "Job state");
}

TEST_CASE("test_csrf_and_path_traversal_are_rejected") {
    auto& browser = service().browser();
    int code = 0;
    try {
        browser.post("/jobs/1/cancel", "token=wrong", "application/x-www-form-urlencoded");
    } catch (const HttpError& error) {
        code = error.code;
    }
    check_equal(code, 403, "Cancellation with a wrong form token");
    bool rejected = false;
    try {
        browser.get("/jobs/../queue.state");
    } catch (const HttpError&) {
        rejected = true;
    }
    check(rejected, "A path outside the job directories must be rejected");
}

TEST_CASE("test_force_ocr_requires_review_and_approval_does_not_print") {
    int number = submit(fixture("simple.pdf"), {{"ocr", "force"}});
    auto directory = prepared(number);
    auto metadata = result(directory);
    check(metadata["ocr_pages"] == json::array({1}), "ocr_pages: " + metadata["ocr_pages"].dump());
    check(fs::exists(directory / "review-required"), "Forced OCR requires review");
    check(!fs::exists(directory / "approved"), "The job must not start approved");
    auto& browser = service().browser();
    auto url = "/jobs/" + std::to_string(number);
    auto token = form_token(browser.get(url).body);
    browser.post(url + "/approve", "token=" + token, "application/x-www-form-urlencoded");
    check(fs::exists(directory / "approved"), "Approval is recorded");
    check_equal(job(number).state, 4, "Job state after approval");
}

TEST_CASE("test_cancelled_job_is_not_reported_as_completed") {
    int number = submit(fixture("simple.txt"));
    prepared(number);
    cancel(number);
    check_equal(job(number).state, 7, "Job state after cancellation");
}

TEST_CASE("test_source_page_selection_precedes_braille_pagination") {
    auto source = service().directory() / "two-pages.pdf";
    join_pdfs({fixture("simple.pdf"), fixture("overflow.pdf")}, source);
    int number = submit(source, {{"cells", 10}, {"lines", 4}}, 2, 2);
    auto directory = prepared(number);
    auto metadata = result(directory);
    check_equal(metadata["copies"].get<int>(), 2, "Copies");
    check(metadata["source_page_ranges"] == json::parse("[[2, 2]]"),
          "source_page_ranges: " + metadata["source_page_ranges"].dump());
    auto imported = read(directory / "imported.txt");
    check(imported.find("123") == std::string::npos, "Page 1 text in review: " + imported);
    check(imported.find("dog") == std::string::npos, "Page 1 text in review: " + imported);
    check(metadata["braille_pages"].get<int>() > 1, "braille_pages: " + metadata["braille_pages"].dump());
}

TEST_CASE("test_restart_preserves_job_ids_and_results") {
    auto& printer = service();
    int number = submit(fixture("simple.txt"));
    auto directory = prepared(number);
    auto before = read(directory / "converted.brl");
    printer.stop();
    // Simulate the original zero-margin state, whose default paper name
    // disagreed with the borderless choices generated by Linux clients.
    auto state = printer.directory() / "queue.state";
    write(state, std::regex_replace(read(state), std::regex(R"( (?:bottom|left|right|top)=\d+)"), ""));
    printer.start();
    bool listed = false;
    for (const auto& entry : jobs()) listed = listed || entry.id == number;
    check(listed, "Job " + std::to_string(number) + " is listed after the restart");
    check_equal(read(directory / "converted.brl"), before, "Braille after the restart");
    check_ppd_defaults();
}

TEST_CASE("test_generated_linux_paper_defaults_match_available_choices") {
    check_ppd_defaults();
}

TEST_CASE("test_pretranslated_braille_is_not_translated_again") {
    auto source = fixture("simple.expected.brl");
    int number = submit(source);
    auto directory = prepared(number);
    check_equal(read(directory / "converted.brl"), read(source), "Pretranslated Braille");
}

TEST_CASE("test_standard_ipp_raster_submission_is_spooled_and_held") {
    TemporaryDirectory temporary;
    for (auto [suffix, format] : {std::pair{"pwg", "image/pwg-raster"}, std::pair{"urf", "image/urf"}}) {
        auto source = temporary.path() / ("input." + std::string(suffix));
        write_raster(source, std::string_view(suffix) == "urf", 8);
        for (bool two_stage : {false, true}) {
            auto title = "Protocol " + std::string(suffix) + (two_stage ? " True" : " False");
            auto describe_job = [&](ipp_t* message) {
                ippAddString(message, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name", nullptr, title.c_str());
                ippAddString(message, IPP_TAG_JOB, IPP_TAG_TEXT, "braille-settings", nullptr, R"({"images_only":true})");
            };
            if (two_stage) {
                ipp_t* create = request(IPP_OP_CREATE_JOB);
                describe_job(create);
                int id = job_id(send(create, {}, true));
                ipp_t* document = request(IPP_OP_SEND_DOCUMENT);
                ippAddInteger(document, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", id);
                ippAddBoolean(document, IPP_TAG_OPERATION, "last-document", 1);
                ippAddString(document, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", nullptr, format);
                send(document, source, true);
            } else {
                ipp_t* print = request(IPP_OP_PRINT_JOB);
                ippAddString(print, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", nullptr, format);
                describe_job(print);
                send(print, source, true);
            }
            std::optional<Job> submitted;
            for (const auto& entry : jobs())
                if (!submitted && entry.name == title) submitted = entry;
            check(submitted.has_value(), "No queued job named " + title);
            check_equal(submitted->state, 4, title + ": job state");
            auto spooled = prepared(submitted->id);
            check_equal(read(spooled / ("input." + std::string(suffix))), read(source), title + ": spooled raster");
            check_equal(result(spooled)["braille_pages"].get<int>(), 2, title + ": braille_pages");
        }
    }
}

// ---- RasterTests -----------------------------------------------------------

TEST_CASE("test_libcups_decodes_one_bit_and_grayscale_in_page_order") {
    TemporaryDirectory temporary;
    for (int bits : {1, 8}) {
        auto path = temporary.path() / (std::to_string(bits) + ".pwg");
        write_raster(path, false, bits);
        auto images = raster_pages(path, json::object());
        check_equal(images.size(), std::size_t(2), std::to_string(bits) + "-bit page count");
        for (const auto& image : images)
            check(image.pixels == stripes, std::to_string(bits) + "-bit pixels");
    }
}

TEST_CASE("test_truncated_raster_and_page_limits_fail") {
    TemporaryDirectory temporary;
    auto path = temporary.path() / "bad.pwg";
    write_raster(path, false, 8);
    expect_error([&] { raster_pages(path, {{"max_pages", 1}}); }, {}, "Page limit");
    auto bytes = read(path);
    write(path, std::string_view(bytes).substr(0, bytes.size() - 8));
    expect_error([&] { raster_pages(path, json::object()); }, {}, "Truncated raster");
}

TEST_CASE("test_libcups_decodes_apple_grayscale_raster") {
    TemporaryDirectory temporary;
    auto path = temporary.path() / "apple.urf";
    write_raster(path, true, 8);
    auto images = raster_pages(path, json::object());
    check_equal(images.size(), std::size_t(2), "Page count");
    check(images[0].pixels == stripes, "First page pixels");
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    int status = native_test::run(argc, argv);
    running.reset();
    return status;
}
