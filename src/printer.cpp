/* IPP queue, libcups client, and bounded raster decoding. No motor control. */
#include <pappl/pappl.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <string>
#include <stdexcept>
#include "raster.hpp"

extern char **environ;
static char root[PATH_MAX / 2], statefile[PATH_MAX];
static const char *worker;
static pappl_job_t *running;
static pid_t child;
static volatile sig_atomic_t stopping;
static size_t max_bytes = 64 * 1024 * 1024;
static const char *formats[] = {
    "text/plain", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    "application/pdf", "image/pwg-raster", "image/urf", "image/png", "image/jpeg", "image/bmp",
    "image/gif", "image/tiff", "image/jp2", "application/vnd.recordare.musicxml+xml", "application/vnd.recordare.musicxml"
};

static void json_string(const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    putchar('"');
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void job_path(char *out, size_t size, int id, const char *name)
{
    snprintf(out, size, "%s/jobs/%d/%s", root, id, name);
}

static bool exists(int id, const char *name)
{
    char path[PATH_MAX];
    job_path(path, sizeof(path), id, name);
    return access(path, F_OK) == 0;
}

static void marker(int id, const char *name, const char *message)
{
    char path[PATH_MAX];
    int fd;
    job_path(path, sizeof(path), id, name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        if (message && write(fd, message, strlen(message)) < 0) perror("job marker");
        close(fd);
    }
}

static bool save_state(pappl_system_t *system, void *data)
{
    char temp[PATH_MAX + 8];
    (void)data;
    snprintf(temp, sizeof(temp), "%s.new", statefile);
    return papplSystemSaveState(system, temp) && rename(temp, statefile) == 0;
}

static void stop_signal(int sig) { (void)sig; stopping = 1; }

/* All jobs remain held. Only this timer launches a single conversion worker;
   PAPPL retains the original file and remains the authority for IDs/cancel. */
static void prepare_job(pappl_job_t *job, void *data)
{
    int id = papplJobGetID(job);
    const char *input = papplJobGetFilename(job), *settings = "{}";
    char directory[PATH_MAX], number[32], limit[32], copies[32], ranges[2048] = "";
    struct stat st;
    ipp_attribute_t *attr;
    posix_spawnattr_t spawn_attr;
        (void)data;
    if (child || papplJobGetState(job) != IPP_JSTATE_HELD || !input ||
        (papplJobGetReasons(job) & PAPPL_JREASON_JOB_INCOMING)) return;
    if (exists(id, "prepared")) {
        const char *message = exists(id, "review-required") && !exists(id, "approved") ?
            "OCR review required; open the document upload/review page" : "Ready; awaiting printer hardware";
        const char *current = papplJobGetMessage(job);
        if (!current || strcmp(current, message)) papplJobSetMessage(job, "%s", message);
        return;
    }
    if (exists(id, "failed")) return;
    job_path(directory, sizeof(directory), id, "");
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return;
    if (exists(id, "working")) {
        marker(id, "failed", "Conversion interrupted; cancel and resubmit the document.");
        papplJobSetMessage(job, "Conversion interrupted; cancel and resubmit");
        return;
    }
    if (stat(input, &st) < 0 || st.st_size < 0 || (uint64_t)st.st_size > max_bytes) {
        marker(id, "failed", "Document is missing or exceeds the upload limit.");
        papplJobSetMessage(job, "Document is missing or exceeds the upload limit");
        papplJobSetReasons(job, PAPPL_JREASON_DOCUMENT_FORMAT_ERROR, 0);
        return;
    }
    attr = papplJobGetAttribute(job, "braille-settings");
    if (attr && ippGetString(attr, 0, NULL)) settings = ippGetString(attr, 0, NULL);
    snprintf(number, sizeof(number), "%d", id);
    snprintf(limit, sizeof(limit), "%zu", max_bytes);
    attr = papplJobGetAttribute(job, "copies");
    snprintf(copies, sizeof(copies), "%d", attr ? ippGetInteger(attr, 0) : 1);
    attr = papplJobGetAttribute(job, "page-ranges");
    if (attr) ippAttributeString(attr, ranges, sizeof(ranges));
    std::array<std::string, 10> arguments{worker, "worker", root, number, input, papplJobGetFormat(job),
                                           settings, limit, copies, ranges};
    std::array<char*, 11> args{};
    for (std::size_t i = 0; i < arguments.size(); ++i) args[i] = arguments[i].data();
    marker(id, "working", NULL);
    posix_spawnattr_init(&spawn_attr);
    posix_spawnattr_setflags(&spawn_attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&spawn_attr, 0);
    if (posix_spawn(&child, worker, nullptr, &spawn_attr, args.data(), environ)) {
        child = 0;
        marker(id, "failed", "Could not start conversion worker.");
    } else {
        running = job;
        papplJobSetMessage(job, "Converting document");
    }
    posix_spawnattr_destroy(&spawn_attr);
}

static bool tick(pappl_system_t *system, void *data)
{
    auto printer = static_cast<pappl_printer_t *>(data);
    int status;
    if (child) {
        if (stopping || papplJobIsCanceled(running)) kill(-child, SIGTERM);
        if (waitpid(child, &status, WNOHANG) == child) {
            int id = papplJobGetID(running);
            char path[PATH_MAX];
            job_path(path, sizeof(path), id, "working");
            unlink(path);
            if (!papplJobIsCanceled(running)) {
                if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 20)) {
                    if (!exists(id, "failed")) marker(id, "failed", "Conversion worker failed or was interrupted.");
                    papplJobSetMessage(running, "Conversion failed; see the review page");
                    papplJobSetReasons(running, PAPPL_JREASON_DOCUMENT_FORMAT_ERROR, 0);
                }
            }
            child = 0;
            running = NULL;
            save_state(system, NULL);
        }
    }
    if (stopping) {
        if (!child) {
            save_state(system, NULL);
            papplSystemShutdown(system);
        }
        return true;
    }
    if (!child) papplPrinterIterateActiveJobs(printer, prepare_job, NULL, 1, 0);
    return true;
}

static bool unused_filter(pappl_job_t *job, pappl_device_t *device, void *data)
{
    (void)device; (void)data;
    papplJobSetMessage(job, "Awaiting printer hardware");
    return false;
}

/* Required by PAPPL's capability model. Jobs are held before these callbacks;
   releasing one through another IPP client must still never send motor data. */
static bool no_hardware_job(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device)
{
    (void)options; (void)device;
    papplJobSetMessage(job, "Printer hardware is not implemented");
    return false;
}

static bool no_hardware_page(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned page)
{
    (void)page;
    return no_hardware_job(job, options, device);
}

static bool no_hardware_line(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device,
                             unsigned y, const unsigned char *line)
{
    (void)y; (void)line;
    return no_hardware_job(job, options, device);
}

static bool driver(pappl_system_t *system, const char *name, const char *uri,
                   const char *device_id, pappl_pr_driver_data_t *d, ipp_t **attrs, void *data)
{
    (void)system; (void)name; (void)uri; (void)device_id; (void)data;
    snprintf(d->make_and_model, sizeof(d->make_and_model), "Open Braille Embosser Preview");
    d->format = "application/vnd.openbraille";
    d->printfile_cb = d->rstartjob_cb = d->rendjob_cb = no_hardware_job;
    d->rstartpage_cb = d->rendpage_cb = no_hardware_page;
    d->rwriteline_cb = no_hardware_line;
    d->kind = PAPPL_KIND_DOCUMENT;
    d->ppm = 1;
    d->color_supported = d->color_default = PAPPL_COLOR_MODE_MONOCHROME;
    d->content_default = PAPPL_CONTENT_TEXT_AND_GRAPHIC;
    d->quality_default = IPP_QUALITY_NORMAL;
    d->orient_default = IPP_ORIENT_PORTRAIT;
    d->raster_types = PAPPL_PWG_RASTER_TYPE_BLACK_1 | PAPPL_PWG_RASTER_TYPE_SGRAY_8;
    d->sides_supported = d->sides_default = PAPPL_SIDES_ONE_SIDED;
    d->num_resolution = 1;
    d->x_resolution[0] = d->y_resolution[0] = d->x_default = d->y_default = 300;
    d->num_media = 2;
    d->media[0] = "na_letter_8.5x11in"; d->media[1] = "iso_a4_210x297mm";
    /* Source print canvas only; physical Braille spacing is not calibrated here.
       Positive margins keep generated CUPS paper names/defaults consistent. */
    d->left_right = d->bottom_top = 635;
    d->num_source = 1; d->source[0] = "auto";
    d->num_type = 1; d->type[0] = "stationery";
    snprintf(d->media_default.size_name, sizeof(d->media_default.size_name), "%s", d->media[0]);
    d->media_default.size_width = 21590; d->media_default.size_length = 27940;
    snprintf(d->media_default.source, sizeof(d->media_default.source), "auto");
    snprintf(d->media_default.type, sizeof(d->media_default.type), "stationery");
    d->media_default.left_margin = d->media_default.right_margin = d->left_right;
    d->media_default.bottom_margin = d->media_default.top_margin = d->bottom_top;
    d->media_ready[0] = d->media_default;
    d->num_vendor = 1; d->vendor[0] = "braille-settings";
    *attrs = ippNew();
    ippAddString(*attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT, "braille-settings-default", NULL, "{}");
    ippAddRange(*attrs, IPP_TAG_PRINTER, "braille-settings-supported", 0, 4096);
    return true;
}

static int serve(int argc, char **argv)
{
    pappl_system_t *system;
    pappl_printer_t *printer;
    pappl_pr_driver_t description = {"open-braille", "Open Braille Embosser Preview", "MFG:OpenBraille;MDL:Preview;", NULL};
    char path[PATH_MAX];
    struct rlimit limit;
    if (argc != 7) return 2;
    pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) return 1;
    char *resolved = realpath(argv[2], NULL);
    if (!resolved || strlen(resolved) >= sizeof(root)) { free(resolved); fprintf(stderr, "Invalid state directory\n"); return 1; }
    strcpy(root, resolved);
    free(resolved);
    worker = argv[3];
    max_bytes = (size_t)strtoull(argv[5], NULL, 10);
    snprintf(statefile, sizeof(statefile), "%s/queue.state", root);
    snprintf(path, sizeof(path), "%s/jobs", root); mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/ipp-spool", root); mkdir(path, 0700);
    signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal); signal(SIGXFSZ, SIG_IGN);
    limit.rlim_cur = limit.rlim_max = max_bytes;
    if (setrlimit(RLIMIT_FSIZE, &limit)) { perror("file limit"); return 1; }
    system = papplSystemCreate(PAPPL_SOPTIONS_NONE, "Open Braille", atoi(argv[4]), "_print", path,
                              "-", PAPPL_LOGLEVEL_WARN, NULL, false);
    if (!system) return 1;
    papplSystemSetPrinterDrivers(system, 1, &description, NULL, NULL, driver, NULL);
    const char *hostname = getenv("BRAILLE_HOSTNAME");
    if (hostname && *hostname) papplSystemSetHostName(system, hostname);
    for (size_t n = 0; n < sizeof(formats) / sizeof(*formats); n++)
        papplSystemAddMIMEFilter(system, formats[n], "application/vnd.openbraille", unused_filter, NULL);
    papplSystemSetMaxImageSize(system, 20 * 1000 * 1000, 20000, 20000);
    papplSystemLoadState(system, statefile);
    printer = papplSystemFindPrinter(system, NULL, 1, NULL);
    if (!printer) printer = papplPrinterCreate(system, 1, "Open Braille Embosser Preview", "open-braille",
                                              description.device_id, "file:/dev/null");
    if (!printer) { papplSystemDelete(system); return 1; }
    /* Older state files stored a zero-margin canvas. Migrate those defaults
       without discarding the queue, job IDs, documents, or review artifacts. */
    pappl_pr_driver_data_t defaults;
    papplPrinterGetDriverData(printer, &defaults);
    defaults.media_default.left_margin = defaults.media_default.right_margin = defaults.left_right;
    defaults.media_default.bottom_margin = defaults.media_default.top_margin = defaults.bottom_top;
    defaults.media_ready[0] = defaults.media_default;
    if (!papplPrinterSetReadyMedia(printer, 1, defaults.media_ready) ||
        !papplPrinterSetDriverDefaults(printer, &defaults, 0, NULL)) {
        fprintf(stderr, "Cannot set the source print canvas\n");
        papplSystemDelete(system); return 1;
    }
    papplPrinterHoldNewJobs(printer);
    papplPrinterSetMaxActiveJobs(printer, 100);
    papplPrinterSetMaxCompletedJobs(printer, 100);
    papplPrinterSetMaxPreservedJobs(printer, 100);
    papplSystemSetSaveCallback(system, save_state, NULL);
    papplSystemAddTimerCallback(system, time(NULL) + 1, 1, tick, printer);
    if (!papplSystemAddListeners(system, !strcmp(argv[6], "0.0.0.0") ? nullptr : argv[6])) {
        fprintf(stderr, "Cannot bind the IPP listener\n");
        papplSystemDelete(system); return 1;
    }
    papplSystemRun(system);
    save_state(system, NULL);
    papplSystemDelete(system);
    return 0;
}

static int client(int argc, char **argv)
{
    http_t *http;
    ipp_t *request, *response;
    ipp_attribute_t *attr;
    char uri[256];
    const char *op = argv[1];
    if (argc < 4) return 2;
    snprintf(uri, sizeof(uri), "ipp://%s:%s/ipp/print", argv[2], argv[3]);
    http = httpConnect2(argv[2], atoi(argv[3]), NULL, AF_UNSPEC, HTTP_ENCRYPTION_IF_REQUESTED, 1, 10000, NULL);
    if (!http) { fprintf(stderr, "Cannot reach the local IPP queue\n"); return 1; }
    request = ippNewRequest(!strcmp(op, "submit") ? IPP_OP_PRINT_JOB :
                            !strcmp(op, "cancel") ? IPP_OP_CANCEL_JOB : IPP_OP_GET_JOBS);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
    if (!strcmp(op, "submit")) {
        if (argc != 8 && argc != 10) { ippDelete(request); httpClose(http); return 2; }
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name", NULL, argv[7]);
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, argv[5]);
        ippAddString(request, IPP_TAG_JOB, IPP_TAG_TEXT, "braille-settings", NULL, argv[6]);
        ippAddString(request, IPP_TAG_JOB, IPP_TAG_KEYWORD, "job-hold-until", NULL, "indefinite");
        if (argc == 10) {
            cups_option_t *options = NULL;
            int count = cupsAddOption("copies", argv[8], 0, &options);
            if (*argv[9]) count = cupsAddOption("page-ranges", argv[9], count, &options);
            cupsEncodeOptions2(request, count, options, IPP_TAG_JOB);
            cupsFreeOptions(count, options);
        }
        response = cupsDoFileRequest(http, request, "/ipp/print", argv[4]);
    } else {
        if (!strcmp(op, "cancel")) {
            if (argc != 5 || atoi(argv[4]) < 1) { ippDelete(request); httpClose(http); return 2; }
            ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", atoi(argv[4]));
        } else {
            const char *attributes[] = {"job-id", "job-name", "job-state", "job-state-message"};
            ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", NULL, "all");
            ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 4, NULL, attributes);
        }
        response = cupsDoRequest(http, request, "/ipp/print");
    }
    if (!response || ippGetStatusCode(response) >= IPP_STATUS_ERROR_BAD_REQUEST) {
        fprintf(stderr, "%s\n", cupsLastErrorString());
        ippDelete(response); httpClose(http); return 1;
    }
    if (!strcmp(op, "submit")) {
        attr = ippFindAttribute(response, "job-id", IPP_TAG_INTEGER);
        if (!attr) { ippDelete(response); httpClose(http); return 1; }
        printf("%d\n", ippGetInteger(attr, 0));
    } else if (!strcmp(op, "jobs")) {
        bool open = false, first = true;
        unsigned seen = 0;
        printf("[");
        for (attr = ippFirstAttribute(response); attr; attr = ippNextAttribute(response)) {
            const char *name = ippGetName(attr);
            if (!name) { if (open) { putchar('}'); open = false; } seen = 0; continue; }
            if (ippGetGroupTag(attr) != IPP_TAG_JOB) continue;
            unsigned bit = !strcmp(name, "job-id") ? 1 : !strcmp(name, "job-name") ? 2 :
                           !strcmp(name, "job-state") ? 4 : !strcmp(name, "job-state-message") ? 8 : 0;
            if (!bit || (seen & bit)) continue;
            seen |= bit;
            if (!open) { if (!first) putchar(','); first = false; putchar('{'); open = true; }
            else putchar(',');
            json_string(name); putchar(':');
            if (ippGetValueTag(attr) == IPP_TAG_INTEGER || ippGetValueTag(attr) == IPP_TAG_ENUM)
                printf("%d", ippGetInteger(attr, 0));
            else if (ippGetValueTag(attr) == IPP_TAG_BOOLEAN) printf("%s", ippGetBoolean(attr, 0) ? "true" : "false");
            else {
                // ippAttributeString() escapes '[', '"' and '\' for its own syntax; names are
                // reported as submitted so the sender suffix and file names stay intact.
                const char *text = ippGetString(attr, 0, NULL);
                char value[2048];
                if (!text) { ippAttributeString(attr, value, sizeof(value)); text = value; }
                json_string(text);
            }
        }
        if (open) putchar('}');
        printf("]\n");
    }
    ippDelete(response); httpClose(http);
    return 0;
}

static int raster(int argc, char** argv)
{
    if (argc != 6) return 2;
    try {
        int max_pages = braille::integer(argv[4], "maximum pages");
        int max_pixels = braille::integer(argv[5], "maximum pixels");
        int count = 0;
        braille::read_raster(argv[2], max_pages, max_pixels, [&](int page, braille::Image image) {
            std::string bytes = "P5\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
            bytes.append(reinterpret_cast<const char*>(image.pixels.data()), image.pixels.size());
            if (!strcmp(argv[3], "-")) {
                if (fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || fflush(stdout) != 0)
                    throw std::runtime_error("Cannot write raster output");
            } else {
                char name[32];
                snprintf(name, sizeof name, "page-%04d.pgm", page);
                braille::write(braille::fs::path(argv[3]) / name, bytes);
            }
            count = page;
        });
        if (strcmp(argv[3], "-")) printf("%d\n", count);
        return 0;
    } catch (const std::exception& problem) {
        fprintf(stderr, "%s\n", problem.what());
        return 1;
    }
}

int main(int argc, char **argv)
{
    umask(0077);
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "serve")) return serve(argc, argv);
    if (!strcmp(argv[1], "raster")) return raster(argc, argv);
    if (!strcmp(argv[1], "submit") || !strcmp(argv[1], "jobs") || !strcmp(argv[1], "cancel")) return client(argc, argv);
    fprintf(stderr, "Commands: serve, submit, jobs, cancel, raster\n");
    return 2;
}
