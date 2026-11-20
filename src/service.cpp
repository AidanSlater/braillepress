#include "braille.hpp"
#include "cells.hpp"
#include "firmware_rpc.hpp"
#include "job_settings.hpp"
#include "raster.hpp"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>

// AddressSanitizer reserves far more address space than the worker's memory cap allows.
#if defined(__SANITIZE_ADDRESS__)
#define BRAILLE_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BRAILLE_ADDRESS_SANITIZER 1
#endif
#endif

extern char** environ;

namespace {
using namespace braille;
using json = nlohmann::json;
using namespace std::chrono_literals;

const std::map<std::string, std::string> formats{
    {".txt", "text/plain"}, {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".pdf", "application/pdf"}, {".pwg", "image/pwg-raster"}, {".urf", "image/urf"}, {".png", "image/png"},
    {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".bmp", "image/bmp"}, {".gif", "image/gif"},
    {".tif", "image/tiff"}, {".tiff", "image/tiff"}, {".jp2", "image/jp2"}, {".brl", "application/vnd.openbraille"},
    {".musicxml", "application/vnd.recordare.musicxml+xml"}, {".mxl", "application/vnd.recordare.musicxml"}};
constexpr int upload_limit = 64 * 1024 * 1024;
constexpr char discovery_type[] = "_openbraille._tcp";
volatile sig_atomic_t stopping = 0;
void request_stop(int) { stopping = 1; }

struct File {
    int fd;
    ~File() { if (fd >= 0) close(fd); }
};

std::string hex(const unsigned char* bytes, std::size_t count) {
    std::string value;
    constexpr auto digits = "0123456789abcdef";
    for (std::size_t i = 0; i < count; ++i) {
        value += digits[bytes[i] >> 4];
        value += digits[bytes[i] & 15];
    }
    return value;
}

std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1)
        throw std::runtime_error("Cannot generate form token");
    return hex(bytes.data(), bytes.size());
}

std::string digest(const std::string& content, const std::string& settings, const std::string& filename) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned count;
    if (!hash || !EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr)
        || !EVP_DigestUpdate(hash.get(), settings.data(), settings.size() + 1)
        || !EVP_DigestUpdate(hash.get(), filename.data(), filename.size() + 1)
        || !EVP_DigestUpdate(hash.get(), content.data(), content.size())
        || !EVP_DigestFinal_ex(hash.get(), bytes, &count))
        throw std::runtime_error("Cannot identify retried submission");
    return hex(bytes, count);
}

void atomic_write(const fs::path& path, const std::string& bytes) {
    auto staging = path;
    staging += ".new";
    write(staging, bytes);
    File file{open(staging.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.fd < 0 || fsync(file.fd))
        throw std::runtime_error("Cannot persist " + path.filename().string());
    fs::rename(staging, path);
    File directory{open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.fd < 0 || fsync(directory.fd))
        throw std::runtime_error("Cannot sync job directory");
}

class Temporary {
public:
    Temporary(const fs::path& root, const std::string& prefix) : path_((root / (prefix + "-XXXXXX")).string()) {
        File file{mkstemp(path_.data())};
        if (file.fd < 0)
            throw std::runtime_error("Cannot create a temporary queue file");
    }
    ~Temporary() { unlink(path_.c_str()); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

class Child {
public:
    Child(std::vector<std::string> command, int output, int errors) {
        std::vector<char*> argv;
        for (auto& option : command) argv.push_back(option.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t files;
        posix_spawn_file_actions_init(&files);
        if (output >= 0) posix_spawn_file_actions_adddup2(&files, output, STDOUT_FILENO);
        if (errors >= 0) posix_spawn_file_actions_adddup2(&files, errors, STDERR_FILENO);
        int result = posix_spawn(&pid_, argv.front(), &files, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&files);
        if (result)
            throw std::runtime_error("Cannot start " + command.front() + ": " + std::strerror(result));
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            if (!wait(5s)) {
                kill(pid_, SIGKILL);
                while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            }
        }
    }
    std::optional<int> wait(std::chrono::milliseconds timeout) {
        if (pid_ <= 0)
            return status_;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            int state;
            auto result = waitpid(pid_, &state, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                status_ = WIFEXITED(state) ? WEXITSTATUS(state) : 1;
                return status_;
            }
            if (result < 0 && errno != EINTR) {
                pid_ = -1;
                status_ = 1;
                return status_;
            }
            if (timeout != 0ms) std::this_thread::sleep_for(25ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return std::nullopt;
    }
private:
    pid_t pid_ = -1;
    int status_ = 1;
};

std::string suffix_for(std::string_view mime) {
    for (auto& [suffix, type] : formats)
        if (type == mime)
            return suffix;
    throw std::runtime_error("Unsupported document format");
}

bool prepare(const fs::path& source, const std::string& mime, const fs::path& directory,
             const json& config, int copies, const std::string& ranges) {
    if (copies < 1 || copies > 999)
        throw std::runtime_error("Copies must be between 1 and 999");
    auto options = job_options(config, ranges);
    auto suffix = suffix_for(mime);
    auto path = directory / ("input" + suffix);
    if (!fs::exists(path))
        fs::create_hard_link(source, path);
    bool raster = suffix == ".pwg" || suffix == ".urf";
    if (!options.ranges.empty() && suffix != ".pdf" && suffix != ".brl" && !raster)
        throw std::runtime_error("Source page ranges require PDF, raster pages, or pre-translated Braille");
    Imported document;
    Pages pages;
    if (suffix == ".brl") {
        pages = parse_cells(read(path), options, true);
    } else if (raster) {
        read_raster(path, options.max_pages, options.max_pixels, [&](int number, Image image) {
            if (!options.selects(number))
                return;
            if (options.images_only) {
                document.blocks.emplace_back(Graphic(std::move(image)));
            } else if (options.ocr == OCR::off) {
                throw std::runtime_error("Raster print jobs require OCR or the images-only option");
            } else {
                for (auto& block : scan_picture(image, options))
                    document.blocks.push_back(std::move(block));
                document.recognized_pages.emplace_back(number);
            }
        });
    } else {
        document = import_document(path, options);
    }
    auto legend = config.value("legend", "");
    if (!legend.empty()) {
        Document introduction = text_document(legend, options);
        introduction.insert(introduction.begin(), Paragraph{"Map legend", 0, 0, Paragraph::heading});
        introduction.insert(introduction.end(), std::make_move_iterator(document.blocks.begin()), std::make_move_iterator(document.blocks.end()));
        document.blocks = std::move(introduction);
    }
    if (suffix != ".brl")
        pages = paginate(document.blocks, options);
    for (auto& page : pages)
        for (auto& line : page)
            if (std::any_of(line.begin(), line.end(), [](auto ch) { return ch < blank || ch > U'⠿'; }))
                throw std::runtime_error("The job contains dots outside the six-dot printer alphabet");
    bool used_ocr = !document.recognized_pages.empty();
    json recognized = json::array();
    for (auto& page : document.recognized_pages)
        std::visit([&](const auto& value) { recognized.push_back(value); }, page);
    atomic_write(directory / "converted.brl", serialize(pages));
    preview(pages, directory / "preview.pdf.new", options.width, options.height);
    fs::rename(directory / "preview.pdf.new", directory / "preview.pdf");
    atomic_write(directory / "imported.txt", review(document.blocks));
    atomic_write(directory / "result.json", json{{"used_ocr", used_ocr}, {"ocr_pages", recognized}, {"copies", copies},
        {"source_page_ranges", options.ranges}, {"braille_pages", pages.size()}, {"settings", config}}.dump());
    if (used_ocr)
        atomic_write(directory / "review-required", "");
    atomic_write(directory / "prepared", "");
    return used_ocr;
}

int worker(int argc, char** argv) {
    if (argc < 8 || argc > 10) return 2;
    pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) return 1;
    int number = integer(argv[3], "job number");
    if (number < 1) return 2;
    auto directory = fs::path(argv[2]) / "jobs" / std::to_string(number);
    fs::create_directories(directory);
    try {
#ifndef BRAILLE_ADDRESS_SANITIZER
        struct rlimit memory{1536ULL * 1024 * 1024, 1536ULL * 1024 * 1024};
        if (setrlimit(RLIMIT_AS, &memory))
            throw std::runtime_error("Cannot bound conversion worker memory");
#endif
        alarm(300);
        setenv("OMP_THREAD_LIMIT", "1", 0);
        if (fs::file_size(argv[4]) > static_cast<unsigned>(integer(argv[7], "upload limit")))
            throw std::runtime_error("Document exceeds the upload limit");
        return prepare(argv[4], argv[5], directory, job_settings(json::parse(argv[6])),
                       argc >= 9 ? integer(argv[8], "copies") : 1, argc == 10 ? argv[9] : "") ? 20 : 0;
    } catch (const std::exception& error) {
        atomic_write(directory / "failed", error.what());
        return 1;
    }
}

std::string html(std::string_view text) {
    std::string result;
    for (char c : text) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&#39;"; break;
            default: result += c;
        }
    }
    return result;
}

std::string unquote(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            result += text[i];
            continue;
        }
        if (i + 2 >= text.size())
            throw std::runtime_error("Invalid upload metadata encoding");
        auto digit = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            throw std::runtime_error("Invalid upload metadata encoding");
        };
        result += static_cast<char>(16 * digit(text[i + 1]) + digit(text[i + 2]));
        i += 2;
    }
    (void)unicode(result);
    return result;
}

bool token_key(std::string_view text, std::size_t count) {
    return text.size() == count && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string filename(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    name = fs::path(name).filename().string();
    if (name.empty() || name.size() > 200 || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("Choose a document with a readable file name of at most 200 bytes");
    (void)unicode(name);
    return name;
}

std::string name_for(const json& job) {
    auto name = job.value("job-name", "Document");
    auto start = name.rfind(" [sender ");
    if (start != std::string::npos && name.size() == start + 42 && name.back() == ']' && token_key(name.substr(start + 9, 32), 32))
        name.resize(start);
    return name;
}

class Queue {
public:
    Queue(fs::path root, std::string helper, int port, int max_bytes)
        : root_(std::move(root)), helper_(std::move(helper)), port_(std::to_string(port)), max_bytes_(max_bytes) {
        fs::create_directory(root_ / "requests");
    }

    std::string command(std::string operation, std::vector<std::string> data = {}) const {
        Temporary out(root_, "response"), err(root_, "error");
        File output{open(out.path().c_str(), O_WRONLY | O_CLOEXEC)}, errors{open(err.path().c_str(), O_WRONLY | O_CLOEXEC)};
        if (output.fd < 0 || errors.fd < 0)
            throw std::runtime_error("Cannot capture IPP response");
        std::vector<std::string> args{helper_, std::move(operation), "127.0.0.1", port_};
        args.insert(args.end(), std::make_move_iterator(data.begin()), std::make_move_iterator(data.end()));
        Child subprocess(std::move(args), output.fd, errors.fd);
        auto result = subprocess.wait(45s);
        if (!result || *result) {
            auto message = strip(read(err.path()));
            throw std::runtime_error(message.empty() ? "IPP queue did not respond" : message);
        }
        return read(out.path());
    }

    json jobs() const { return json::parse(command("jobs")); }
    fs::path directory(int id) const { return root_ / "jobs" / std::to_string(id); }
    int max_bytes() const { return max_bytes_; }
    const std::string& identity() const { return identity_; }
    void open_identity() {
        auto path = root_ / "printer-id";
        if (fs::exists(path)) identity_ = strip(read(path));
        else {
            identity_ = random_token().substr(0, 32);
            atomic_write(path, identity_ + "\n");
        }
        if (!token_key(identity_, 32))
            throw std::runtime_error("Invalid persisted printer identity");
    }

    json job(int id) const {
        for (auto& job : jobs())
            if (job.value("job-id", 0) == id)
                return job;
        return nullptr;
    }

    int submit(const std::string& orig, const std::string& body, const json& options, const std::string& key = "") {
        auto title = filename(orig);
        auto mime = formats.find(extension(title));
        if (mime == formats.end())
            throw std::runtime_error("Use TXT, DOCX, PDF, MusicXML/MXL, PWG/Apple raster, pre-translated .brl, or a supported raster picture");
        if (body.size() > static_cast<std::size_t>(max_bytes_))
            throw std::runtime_error("Upload exceeds the document limit");
        if (fs::space(root_).available < std::max<std::size_t>(body.size() * 2, 128 * 1024 * 1024))
            throw std::runtime_error("Printer storage is full");
        auto raw_settings = options.dump();
        if (raw_settings.size() > 4096)
            throw std::runtime_error("Conversion settings exceed the printer limit");
        if (key.empty())
            return enqueue(body, mime->second, raw_settings, title);
        std::lock_guard lock(submissions_);
        auto journal = root_ / "requests" / key;
        auto fingerprint = digest(body, raw_settings, title);
        auto suffix = " [sender " + key + "]";
        if (fs::exists(journal)) {
            auto record = json::parse(read(journal));
            if (record.at("digest") != fingerprint)
                throw std::runtime_error("Submission ID was already used for different data");
            if (int id = record.value("job", 0); id > 0)
                return id;
            for (auto& job : jobs()) {
                if (job.value("job-name", "").ends_with(suffix)) {
                    int id = job.at("job-id");
                    record["job"] = id;
                    atomic_write(journal, record.dump());
                    return id;
                }
            }
            throw std::runtime_error("Previous submission outcome is unknown. Check the queue before creating another job.");
        }
        json record{{"digest", fingerprint}};
        atomic_write(journal, record.dump());
        int id = enqueue(body, mime->second, raw_settings, title + suffix);
        record["job"] = id;
        atomic_write(journal, record.dump());
        return id;
    }

private:
    int enqueue(const std::string& body, const std::string& mime, const std::string& config, const std::string& title) {
        Temporary file(root_, "upload");
        write(file.path(), body);
        return integer(strip(command("submit", {file.path(), mime, config, title})), "job number");
    }
    fs::path root_;
    std::string helper_, port_, identity_;
    int max_bytes_;
    std::mutex submissions_;
};

std::string phase(const json& job, const fs::path& dir) {
    switch (job.is_object() ? job.value("job-state", 0) : 0) {
        case 7: return "Cancelled";
        case 8: return "Aborted";
        case 9: return "Completed";
        default: break;
    }
    if (fs::exists(dir / "failed")) return "Conversion failed";
    if (fs::exists(dir / "prepared"))
        return fs::exists(dir / "review-required") && !fs::exists(dir / "approved") ? "Held for OCR approval" : "Held for hardware";
    if (fs::exists(dir / "working")) return "Converting document";
    return "Queued for conversion";
}

std::string form_token(const httplib::Request& req) {
    const std::regex pattern(R"((?:^|;\s*)braille-form=([a-f0-9]{64})(?:;|$))");
    auto cookie = req.get_header_value("Cookie");
    std::smatch match;
    return std::regex_search(cookie, match, pattern) ? match[1].str() : "";
}

void web_page(const httplib::Request& req, httplib::Response& res, const std::string& title,
              std::string content, int code = 200) {
    auto token = form_token(req);
    if (token.empty()) token = random_token();
    // Only forms generated by the server contain this placeholder.
    for (std::size_t start = 0; (start = content.find("name=token value=TOKEN_VALUE", start)) != std::string::npos;) {
        start += std::strlen("name=token value=");
        content.replace(start, 11, token);
        start += token.size();
    }
    auto h = html(title);
    res.status = code;
    res.set_header("Set-Cookie", "braille-form=" + token + "; HttpOnly; SameSite=Strict; Path=/");
    res.set_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
    res.set_content("<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<title>" + h + "</title><style>body{font:1.1rem system-ui;max-width:55rem;margin:2rem auto;padding:1rem}"
        "label{display:block;margin:1rem 0}input,select,button,textarea{font:inherit}pre{white-space:pre-wrap}"
        "a,button{margin-right:1rem}:focus-visible{outline:3px solid #164ba1}</style>"
        "<header><a href='/'>Open Braille Embosser</a></header><main><h1>" + h + "</h1>" + content + "</main>",
        "text/html; charset=utf-8");
}

std::string field(const httplib::Request& req, const std::string& key, const std::string& fallback = "") {
    if (req.has_file(key)) return req.get_file_value(key).content;
    return req.has_param(key) ? req.get_param_value(key) : fallback;
}

bool check_csrf(const httplib::Request& req, httplib::Response& res) {
    auto input = field(req, "token"), cookie = form_token(req);
    if (token_key(input, 64) && cookie.size() == input.size() && CRYPTO_memcmp(input.data(), cookie.data(), input.size()) == 0)
        return true;
    web_page(req, res, "Form expired", "Reload the page and submit the form again.", 403);
    return false;
}

std::string job_path(int number) { return "/jobs/" + std::to_string(number); }

std::string job_form(int number, const std::string& action, const std::string& title) {
    return "<form method=post action='" + job_path(number) + "/" + action +
        "'><input type=hidden name=token value=TOKEN_VALUE><p><button>" + title + "</button></p></form>";
}

void reply(httplib::Response& response, const json& data, int status = 200) {
    response.status = status;
    response.set_content(data.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

void routes(httplib::Server& server, Queue& queue) {
    server.set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}});
    server.Get("/", [&](const auto& req, auto& res) {
        try {
            std::string rows;
            for (const auto& job : queue.jobs()) {
                int id = job.at("job-id");
                rows += "<li><a href='" + job_path(id) + "'>Job " + std::to_string(id) + ": " + html(name_for(job)) +
                        "</a> — " + html(phase(job, queue.directory(id))) + "</li>";
            }
            web_page(req, res, "Submit a document",
                "<form method=post enctype='multipart/form-data' action='/submit'><input type=hidden name=token value=TOKEN_VALUE>"
                "<label>Document <input type=file name=document required></label>"
                "<label>Treat as <select name=mode><option value=auto>Auto</option><option value=text>Text or scan</option>"
                "<option value=picture>Picture or diagram</option><option value=map>Map</option></select></label>"
                "<label>Optional map legend <textarea name=legend maxlength=1000></textarea></label>"
                "<label>Cells per line <input type=number name=cells min=8 max=120 value=40 required></label>"
                "<label>Lines per Braille page <input type=number name=lines min=4 max=100 value=25 required></label>"
                "<label>Braille table <select name=table><option value=en-ueb-g1.ctb>English UEB uncontracted</option>"
                "<option value=en-ueb-g2.ctb>English UEB contracted</option>"
                "<option value=es-g1.ctb>Spanish uncontracted (Español)</option></select></label>"
                "<label>Text recognition <select name=ocr><option value=auto>Automatic for scanned pages</option>"
                "<option value=force>Recognize entire PDF pages</option><option value=off>Off</option></select></label>"
                "<label>Picture style <select name=image_mode><option>outline</option><option>threshold</option><option>dither</option></select></label>"
                "<label><input type=checkbox name=images_only value=yes>Depict pictures only, without recognizing words</label>"
                "<p>Maximum document size " + std::to_string(queue.max_bytes() / (1024 * 1024)) +
                " MiB. OCR jobs require review.</p>"
                "<button>Convert document</button></form><h2>Jobs</h2><ul>" + rows + "</ul>");
        } catch (const std::exception& error) {
            web_page(req, res, "Cannot show jobs", html(error.what()), 503);
        }
    });
    server.Get(R"(/jobs/([1-9][0-9]{0,9})(?:/(converted\.brl|preview\.pdf|imported\.txt))?)", [&](const auto& req, auto& res) {
        try {
            int id = integer(req.matches[1].str(), "job number");
            auto job = queue.job(id);
            auto dir = queue.directory(id);
            auto artifact = req.matches[2].str();
            bool prepared = fs::exists(dir / "prepared");
            if (job.is_null() || (!artifact.empty() && !prepared)) {
                web_page(req, res, "Not found", "Job or result is unavailable.", 404);
                return;
            }
            if (!artifact.empty()) {
                res.set_content(read(dir / artifact), artifact == "preview.pdf" ? "application/pdf" : "text/plain; charset=utf-8");
                return;
            }
            std::string content = "<p>" + html(phase(job, dir)) + "</p><p><a href=''>Refresh status</a></p>";
            if (fs::exists(dir / "failed")) content += "<p role=alert>" + html(read(dir / "failed").substr(0, 4000)) + "</p>";
            if (prepared) {
                auto info = json::parse(read(dir / "result.json"));
                content += "<p>" + info.at("braille_pages").dump() + " Braille pages per copy. " +
                    std::to_string(info.value("copies", 1)) + " requested copies.</p>";
                auto url = job_path(id);
                content += "<p><a href='" + url + "/imported.txt'>Readable text</a><a href='" + url +
                    "/converted.brl'>Unicode Braille</a><a href='" + url + "/preview.pdf'>PDF preview</a></p>";
                if (info.value("used_ocr", false)) {
                    std::vector<std::string> sources;
                    for (auto& item : info["ocr_pages"]) sources.push_back(item.is_string() ? item.get<std::string>() : item.dump());
                    content += "<p>OCR was used for " + html(join(sources, ", ")) + ". Proofread the recovered text and inspect figures.</p>"
                        "<h2>Recovered text</h2><pre>" + html(read(dir / "imported.txt").substr(0, 100000)) + "</pre>";
                    if (!fs::exists(dir / "approved") && job.value("job-state", 0) < 7)
                        content += job_form(id, "approve", "Approve reviewed OCR result");
                }
            }
            if (job.value("job-state", 0) < 7) content += job_form(id, "cancel", "Cancel job");
            web_page(req, res, "Job " + std::to_string(id) + ": " + name_for(job), std::move(content));
        } catch (const std::exception& error) {
            web_page(req, res, "Cannot show job", html(error.what()), 503);
        }
    });
    server.Post("/submit", [&](const auto& req, auto& res) {
        try {
            if (!check_csrf(req, res)) return;
            if (req.files.count("document") != 1)
                throw std::runtime_error("Submit one document at a time");
            auto file = req.get_file_value("document");
            auto options = json{{"cells", integer(field(req, "cells", "40"), "cells")},
                {"lines", integer(field(req, "lines", "25"), "lines")}, {"table", field(req, "table", "en-ueb-g1.ctb")},
                {"ocr", field(req, "ocr", "auto")}, {"image_mode", field(req, "image_mode", "outline")},
                {"images_only", field(req, "images_only") == "yes"}, {"mode", field(req, "mode", "auto")}};
            if (auto legend = strip(field(req, "legend")); !legend.empty()) options["legend"] = legend;
            int id = queue.submit(file.filename, file.content, job_settings(options));
            res.set_redirect(job_path(id), 303);
        } catch (const std::exception& error) {
            web_page(req, res, "Cannot submit job", html(error.what()), 400);
        }
    });
    server.Post(R"(/jobs/([1-9][0-9]{0,9})/(approve|cancel))", [&](const auto& req, auto& res) {
        try {
            if (!check_csrf(req, res)) return;
            int id = integer(req.matches[1].str(), "job number");
            auto job = queue.job(id);
            auto dir = queue.directory(id);
            if (job.is_null() || job.value("job-state", 0) >= 7)
                throw std::runtime_error("Job is unavailable or has ended");
            if (req.matches[2] == "cancel") {
                queue.command("cancel", {std::to_string(id)});
            } else {
                if (!fs::exists(dir / "prepared") || !fs::exists(dir / "review-required"))
                    throw std::runtime_error("This job is not awaiting OCR approval");
                atomic_write(dir / "approved", "");
            }
            res.set_redirect(job_path(id), 303);
        } catch (const std::exception& error) {
            web_page(req, res, "Cannot update job", html(error.what()), 400);
        }
    });
    server.Get("/api/capabilities", [&](const auto&, auto& res) {
        std::vector<std::string> extensions;
        for (auto& [suffix, mime] : formats) extensions.push_back(suffix);
        reply(res, {{"name", "Open Braille Embosser"}, {"printer_id", queue.identity()}, {"protocol", 1}, {"max_bytes", queue.max_bytes()},
            {"formats", extensions}, {"modes", {"auto", "text", "picture", "map"}}});
    });
    server.Post("/api/jobs", [&](const auto& req, auto& res) {
        try {
            auto key = req.get_header_value("X-Braille-Request");
            if (!token_key(key, 32))
                throw std::runtime_error("A 32-digit hexadecimal submission ID is required");
            if (req.get_header_value("Content-Type") != "application/octet-stream")
                throw std::runtime_error("Send original document bytes as application/octet-stream");
            auto expected = req.get_header_value("X-Braille-Printer");
            if (!expected.empty() && expected != queue.identity())
                throw std::runtime_error("Printer identity changed. Find the printer again.");
            auto mode = req.get_header_value("X-Braille-Mode");
            json values{{"mode", mode.empty() ? "auto" : mode}};
            auto legend = unquote(req.get_header_value("X-Braille-Legend"));
            if (!legend.empty()) values["legend"] = legend;
            int id = queue.submit(unquote(req.get_header_value("X-Braille-Filename")), req.body, job_settings(values), key);
            reply(res, {{"id", id}, {"review_path", job_path(id)}, {"status", phase(queue.job(id), queue.directory(id))}}, 202);
        } catch (const std::exception& error) {
            auto message = std::string(error.what());
            int code = message.starts_with("Previous submission") || message.starts_with("Submission ID") ? 409 : 400;
            reply(res, {{"error", message}}, code);
        }
    });
    server.Get(R"(/api/jobs/([1-9][0-9]{0,9}))", [&](const auto& req, auto& res) {
        try {
            int id = integer(req.matches[1].str(), "job number");
            auto job = queue.job(id);
            if (job.is_null()) {
                reply(res, {{"error", "Job not found"}}, 404);
                return;
            }
            auto dir = queue.directory(id);
            auto data = json{{"id", id}, {"name", name_for(job)}, {"status", phase(job, dir)}, {"review_path", job_path(id)}};
            if (fs::exists(dir / "failed")) data["error"] = read(dir / "failed").substr(0, 4000);
            reply(res, data);
        } catch (const std::exception& error) {
            reply(res, {{"error", error.what()}}, 503);
        }
    });
    server.set_error_handler([](const auto& req, auto& res) {
        if (res.body.empty() && req.path.starts_with("/api/"))
            reply(res, {{"error", res.status == 413 ? "Upload exceeds the document limit" : "Request failed"}}, res.status);
    });
}

class Discovery {
public:
    explicit Discovery(int port) : port_(port), loop_(avahi_threaded_poll_new()) {
        if (loop_) {
            int error;
            client_ = avahi_client_new(avahi_threaded_poll_get(loop_), AvahiClientFlags(0), client_changed, this, &error);
            if (client_) started_ = avahi_threaded_poll_start(loop_) == 0;
        }
    }

    ~Discovery() {
        if (started_) avahi_threaded_poll_stop(loop_);
        if (client_) avahi_client_free(client_);
        if (loop_) avahi_threaded_poll_free(loop_);
    }

private:
    static void client_changed(AvahiClient* client, AvahiClientState state, void* owner) {
        auto& self = *static_cast<Discovery*>(owner);
        if (state == AVAHI_CLIENT_S_RUNNING) {
            if (!self.group_) self.group_ = avahi_entry_group_new(client, group_changed, owner);
            if (self.group_ && avahi_entry_group_is_empty(self.group_)) self.advertise();
        } else if ((state == AVAHI_CLIENT_S_COLLISION || state == AVAHI_CLIENT_S_REGISTERING) && self.group_) {
            avahi_entry_group_reset(self.group_);
        }
    }

    static void group_changed(AvahiEntryGroup*, AvahiEntryGroupState state, void* owner) {
        auto& self = *static_cast<Discovery*>(owner);
        if (state == AVAHI_ENTRY_GROUP_COLLISION) {
            std::unique_ptr<char, decltype(&avahi_free)> alternative(avahi_alternative_service_name(self.name_.c_str()), avahi_free);
            if (alternative) {
                self.name_ = alternative.get();
                self.advertise();
            }
        }
    }

    void advertise() {
        int rc = avahi_entry_group_add_service(group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, AvahiPublishFlags(0),
            name_.c_str(), discovery_type, nullptr, nullptr, port_, "api=1", nullptr);
        if (rc >= 0) avahi_entry_group_commit(group_);
    }

    int port_;
    std::string name_ = "Open Braille Embosser";
    AvahiThreadedPoll* loop_ = nullptr;
    AvahiClient* client_ = nullptr;
    AvahiEntryGroup* group_ = nullptr;
    bool started_ = false;
};

class FirmwareRpc {
public:
    explicit FirmwareRpc(const fs::path& path) {
        sockaddr_un address{};
        auto name = path.string();
        if (name.size() >= sizeof(address.sun_path))
            throw std::runtime_error("Firmware RPC socket path is too long");
        address.sun_family = AF_UNIX;
        std::copy(name.begin(), name.end(), address.sun_path);
        const timeval timeout{3, 0};
        if (socket_.fd < 0 || setsockopt(socket_.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
            setsockopt(socket_.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
            connect(socket_.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            throw std::runtime_error("Cannot connect to firmware RPC socket " + name + ": " + std::strerror(errno));
    }

    FirmwareRpc(const FirmwareRpc&) = delete;
    FirmwareRpc& operator=(const FirmwareRpc&) = delete;

    int call(std::string_view method, std::initializer_list<int> args) {
        auto id = ++counter_;
        auto packet = embosser::rpc::request(id, method, json(args));
        for (std::size_t offset = 0; offset < packet.size();) {
            auto written = send(socket_.fd, packet.data() + offset, packet.size() - offset, MSG_NOSIGNAL);
            if (written <= 0) throw std::runtime_error("Cannot send firmware RPC " + std::string(method));
            offset += written;
        }
        std::array<std::uint8_t, embosser::rpc::reply_limit> buffer;
        std::size_t size = 0;
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (size < buffer.size() && std::chrono::steady_clock::now() < deadline) {
            auto received = recv(socket_.fd, buffer.data() + size, buffer.size() - size, 0);
            if (received <= 0) break;
            size += received;
            auto reply = embosser::rpc::response(buffer.begin(), buffer.begin() + size, id, method);
            if (!reply) continue;
            if (*reply < -7 || *reply > 2'000'000)
                throw std::runtime_error("Invalid firmware acknowledgment for " + std::string(method));
            return *reply;
        }
        throw std::runtime_error("Firmware RPC reply missing or incomplete for " + std::string(method));
    }

private:
    File socket_{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    int counter_ = 0;
};

void simulate(Queue& queue, int id, std::optional<int> cancel_after, const fs::path& rpc_socket) {
    auto dir = queue.directory(id);
    auto job = queue.job(id);
    auto status = job.is_null() ? "" : phase(job, dir);
    if (status != "Held for hardware")
        throw std::runtime_error("A prepared job held for hardware is required for simulation");
    auto info = json::parse(read(dir / "result.json"));
    auto options = job_options(job_settings(info.at("settings")), "");
    options.ranges.clear();
    options.max_pages = std::max(options.max_pages, info.value("braille_pages", 0));
    auto bytes = read(dir / "converted.brl");
    (void)parse_cells(bytes, options, true);
    auto characters = unicode(bytes);
    if (characters.empty() || characters.size() > 2'000'000)
        throw std::runtime_error("Invalid number of Braille cells to simulate");
    embosser::DryRun firmware;
    using namespace embosser;
    std::optional<FirmwareRpc> rpc;
    if (!rpc_socket.empty()) rpc.emplace(rpc_socket);
    auto expect = [&](int local, int expected, std::string_view method, std::initializer_list<int> args) {
        auto actual = rpc ? rpc->call(method, args) : local;
        if (actual != expected || local != expected)
            throw std::runtime_error(std::string(method) + " returned " + std::to_string(actual) +
                                     ", expected " + std::to_string(expected));
    };
    int count = static_cast<int>(characters.size());
    expect(firmware.begin(id, count), accepted, "braille_begin", {id, count});
    int sequence = 0;
    for (auto c : characters) {
        if (cancel_after && sequence == *cancel_after) break;
        int operation = c == U'\n' ? end_line : c == U'\f' ? end_page : c - blank;
        expect(firmware.cell(id, sequence, operation), sequence + 1, "braille_cell", {id, sequence, operation});
        ++sequence;
    }
    if (cancel_after && sequence == *cancel_after) {
        expect(firmware.cancel(id), cancelled, "braille_cancel", {id});
        expect(firmware.finish(id), cancelled, "braille_finish", {id});
        std::cout << json{{"job", id}, {"simulation", "cancelled"}, {"acknowledged", sequence},
            {"queue_status", status}}.dump() << '\n';
        return;
    }
    expect(firmware.finish(id), complete, "braille_finish", {id});
    std::cout << json{{"job", id}, {"simulation", "complete"}, {"acknowledged", sequence},
        {"cells", firmware.cells()}, {"dots", firmware.dots()}, {"lines", firmware.lines()},
        {"page_breaks", firmware.page_breaks()}, {"queue_status", status}}.dump() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    umask(0077);
    try {
        if (argc > 1 && std::string_view(argv[1]) == "worker")
            return worker(argc, argv);
        auto self = fs::canonical("/proc/self/exe");
        fs::path state, rpc_socket, helper = self.parent_path() / "braille-printer";
        std::string bind = "127.0.0.1", hostname;
        int port = 8000, ipp_port = 8631, max_bytes = upload_limit, dry_run = 0;
        std::optional<int> cancel_after;
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "--help" || option == "-h") {
                std::cout << "usage: braille-service --state DIRECTORY [--bind ADDRESS] [--port PORT] [--ipp-port PORT]\n"
                    "       braille-service --state DIRECTORY [--ipp-port PORT] --dry-run JOB [--cancel-after N] [--rpc-socket PATH]\n";
                return 0;
            }
            if (i + 1 == argc) throw std::runtime_error("Missing value for " + option);
            std::string value = argv[++i];
            if (option == "--state") state = value;
            else if (option == "--native") helper = fs::path(value) / "braille-printer";
            else if (option == "--bind") bind = value;
            else if (option == "--hostname") hostname = value;
            else if (option == "--port") port = integer(value, "HTTP port");
            else if (option == "--ipp-port") ipp_port = integer(value, "IPP port");
            else if (option == "--max-bytes") max_bytes = integer(value, "upload limit");
            else if (option == "--dry-run") dry_run = integer(value, "simulation job");
            else if (option == "--cancel-after") cancel_after = integer(value, "cancellation point");
            else if (option == "--rpc-socket") rpc_socket = value;
            else throw std::runtime_error("Unknown option " + option);
        }
        if (state.empty() || max_bytes < 1 || max_bytes > upload_limit || port < 1 || port > 65535 ||
            ipp_port < 1 || ipp_port > 65535 || port == ipp_port || dry_run < 0 || (cancel_after && *cancel_after < 0))
            throw std::runtime_error("Specify a private --state directory and valid port and size options");
        if (!dry_run && (!rpc_socket.empty() || cancel_after))
            throw std::runtime_error("--rpc-socket and --cancel-after require --dry-run with a held job ID");
        fs::create_directories(state);
        if (dry_run) {
            Queue queue(fs::canonical(state), fs::absolute(helper).string(), ipp_port, max_bytes);
            simulate(queue, dry_run, cancel_after, rpc_socket);
            return 0;
        }
        state = fs::canonical(state);
        File lock{open((state / ".service.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
        if (lock.fd < 0 || flock(lock.fd, LOCK_EX | LOCK_NB))
            throw std::runtime_error("The state directory is already in use");
        Queue queue(state, fs::absolute(helper).string(), ipp_port, max_bytes);
        queue.open_identity();
        if (!hostname.empty()) setenv("BRAILLE_HOSTNAME", hostname.c_str(), 1);
        if (hostname.empty() && bind == "0.0.0.0") {
            std::array<char, 256> name{};
            if (gethostname(name.data(), name.size() - 1) == 0)
                setenv("BRAILLE_HOSTNAME", (std::string(name.data()) + ".local").c_str(), 0);
        }
        File log{open((state / "ipp.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600)};
        if (log.fd < 0) throw std::runtime_error("Cannot open IPP log");
        Child ipp({fs::absolute(helper).string(), "serve", state.string(), self.string(), std::to_string(ipp_port),
                   std::to_string(max_bytes), bind}, log.fd, log.fd);
        for (int attempt = 0;; ++attempt) {
            if (ipp.wait(0ms) || attempt == 60)
                throw std::runtime_error("IPP listener did not start; see " + (state / "ipp.log").string());
            try {
                (void)queue.jobs();
                break;
            } catch (const std::exception&) { std::this_thread::sleep_for(100ms); }
        }
        httplib::Server server;
        server.new_task_queue = [] { return new httplib::ThreadPool(3); };
        server.set_keep_alive_max_count(1);
        server.set_read_timeout(30, 0);
        server.set_write_timeout(30, 0);
        server.set_payload_max_length(max_bytes + 65536);
        routes(server, queue);
        if (!server.bind_to_port(bind, port))
            throw std::runtime_error("Cannot bind upload listener");
        std::optional<Discovery> discovery;
        if (bind != "127.0.0.1" && bind != "::1")
            discovery.emplace(port);
        signal(SIGTERM, request_stop);
        signal(SIGINT, request_stop);
        std::thread listener([&] { server.listen_after_bind(); });
        std::cout << "Upload/review at http://" << bind << ':' << port << "/ (IPP port " << ipp_port << ")\n" << std::flush;
        while (!stopping && !ipp.wait(0ms)) std::this_thread::sleep_for(100ms);
        server.stop();
        listener.join();
        if (!stopping)
            throw std::runtime_error("IPP queue stopped; see " + (state / "ipp.log").string());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
