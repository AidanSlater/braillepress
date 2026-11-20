// Pillow-compatible raster operations; see imaging.hpp for the Pillow notice.

#include "imaging.hpp"

#include <leptonica/allheaders.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>

namespace braille::imaging {
namespace {

[[noreturn]] void undecodable() {
    throw std::runtime_error("Cannot decode picture; use PNG, JPEG, BMP, GIF, TIFF, or JPEG 2000");
}

void check_size(long long width, long long height, long long max_pixels) {
    if (width <= 0 || height <= 0)
        undecodable();
    if (width * height > max_pixels)
        throw std::runtime_error("Picture exceeds the pixel limit");
}

// ---- Resample.c ----------------------------------------------------------

constexpr int precision_bits = 32 - 8 - 2;

struct Kernel {
    double (*function)(double);
    double support;
};

double box_filter(double x) {
    return x > -0.5 && x <= 0.5 ? 1.0 : 0.0;
}

double bicubic_filter(double x) {
    constexpr double a = -0.5;
    if (x < 0.0)
        x = -x;
    if (x < 1.0)
        return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
    if (x < 2.0)
        return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}

double sinc_filter(double x) {
    if (x == 0.0)
        return 1.0;
    x = x * std::numbers::pi;
    return std::sin(x) / x;
}

double lanczos_filter(double x) {
    if (-3.0 <= x && x < 3.0)
        return sinc_filter(x) * sinc_filter(x / 3);
    return 0.0;
}

Kernel kernel(Filter filter) {
    switch (filter) {
    case Filter::box: return {box_filter, 0.5};
    case Filter::bicubic: return {bicubic_filter, 2.0};
    case Filter::lanczos: return {lanczos_filter, 3.0};
    default: throw std::logic_error("unsupported resampling filter");
    }
}

struct Coefficients {
    int size = 0;                 // coefficients per output sample
    std::vector<int> bounds;      // first input sample and count, per output sample
    std::vector<std::int32_t> k;  // normalized fixed-point coefficients
};

Coefficients precompute(int in_size, float in0, float in1, int out_size, const Kernel& filter) {
    double scale = static_cast<double>(in1 - in0) / out_size, filterscale = scale;
    if (filterscale < 1.0)
        filterscale = 1.0;
    double support = filter.support * filterscale;
    Coefficients result;
    result.size = static_cast<int>(std::ceil(support)) * 2 + 1;
    result.bounds.resize(static_cast<std::size_t>(out_size) * 2);
    std::vector<double> kk(static_cast<std::size_t>(out_size) * result.size);
    double inv_filterscale = 1.0 / filterscale;
    for (int xx = 0; xx < out_size; ++xx) {
        double center = in0 + (xx + 0.5) * scale, ww = 0.0;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;
        double* k = &kk[static_cast<std::size_t>(xx) * result.size];
        int x = 0;
        for (; x < xmax; ++x) {
            double w = filter.function((x + xmin - center + 0.5) * inv_filterscale);
            k[x] = w;
            ww += w;
        }
        if (ww != 0.0)
            for (x = 0; x < xmax; ++x)
                k[x] /= ww;
        for (; x < result.size; ++x)
            k[x] = 0;
        result.bounds[xx * 2] = xmin;
        result.bounds[xx * 2 + 1] = xmax;
    }
    result.k.resize(kk.size());
    for (std::size_t i = 0; i < kk.size(); ++i)
        result.k[i] = kk[i] < 0 ? static_cast<int>(-0.5 + kk[i] * (1 << precision_bits))
                                : static_cast<int>(0.5 + kk[i] * (1 << precision_bits));
    return result;
}

std::uint8_t clip8(int value) {
    return static_cast<std::uint8_t>(std::clamp(value >> precision_bits, 0, 255));
}

Bitmap horizontal_pass(const Bitmap& in, int width, int first_row, int rows, const Coefficients& c) {
    Bitmap out(width, rows, in.bands);
    int bands = in.bands;
    for (int yy = 0; yy < rows; ++yy) {
        const std::uint8_t* line = in.row(yy + first_row);
        std::uint8_t* target = out.row(yy);
        for (int xx = 0; xx < width; ++xx) {
            int xmin = c.bounds[xx * 2], xmax = c.bounds[xx * 2 + 1];
            const std::int32_t* k = &c.k[static_cast<std::size_t>(xx) * c.size];
            for (int band = 0; band < bands; ++band) {
                int ss = 1 << (precision_bits - 1);
                for (int x = 0; x < xmax; ++x)
                    ss += line[(x + xmin) * bands + band] * k[x];
                target[xx * bands + band] = clip8(ss);
            }
        }
    }
    return out;
}

Bitmap vertical_pass(const Bitmap& in, int height, const Coefficients& c) {
    Bitmap out(in.width, height, in.bands);
    std::size_t stride = static_cast<std::size_t>(in.width) * in.bands;
    for (int yy = 0; yy < height; ++yy) {
        int ymin = c.bounds[yy * 2], ymax = c.bounds[yy * 2 + 1];
        const std::int32_t* k = &c.k[static_cast<std::size_t>(yy) * c.size];
        std::uint8_t* target = out.row(yy);
        for (std::size_t i = 0; i < stride; ++i) {
            int ss = 1 << (precision_bits - 1);
            for (int y = 0; y < ymax; ++y)
                ss += in.data[(y + ymin) * stride + i] * k[y];
            target[i] = clip8(ss);
        }
    }
    return out;
}

// Geometry.c ImagingScaleAffine, used by resize(NEAREST).
Bitmap resize_nearest(const Bitmap& image, int width, int height, std::array<float, 4> box) {
    auto coordinate = [](double v) { return v < 0.0 ? -1 : static_cast<int>(v); };
    double a0 = static_cast<double>(box[2] - box[0]) / width, a4 = static_cast<double>(box[3] - box[1]) / height;
    double a2 = box[0], a5 = box[1];
    Bitmap out(width, height, image.bands, 0);
    std::vector<int> columns(width, -1);
    double xo = a2 + a0 * 0.5;
    for (int x = 0; x < width; ++x, xo += a0) {
        int xin = coordinate(xo);
        if (xin >= 0 && xin < image.width)
            columns[x] = xin;
    }
    double yo = a5 + a4 * 0.5;
    for (int y = 0; y < height; ++y, yo += a4) {
        int yin = coordinate(yo);
        if (yin < 0 || yin >= image.height)
            continue;
        for (int x = 0; x < width; ++x)
            if (columns[x] >= 0)
                std::copy_n(image.row(yin) + columns[x] * image.bands, image.bands, out.row(y) + x * image.bands);
    }
    return out;
}

// ---- BoxBlur.c ------------------------------------------------------------

float gaussian_radius(float radius, int passes) {
    float sigma2 = radius * radius / passes;
    float L = std::sqrt(12.0 * sigma2 + 1.0);
    float l = std::floor((L - 1.0) / 2.0);
    float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (l + 1) * (l + 1));
    return l + a;
}

void box_blur_line(std::uint8_t* out, const std::uint8_t* in, int lastx, int radius, int edge_a, int edge_b,
                   std::uint32_t ww, std::uint32_t fw) {
    std::uint32_t acc, bulk;
    auto move = [&](int subtract, int add) { acc += in[add] - in[subtract]; };
    auto far = [&](int left, int right) { bulk = (acc * ww) + (in[left] + in[right]) * fw; };
    auto save = [&](int x) { out[x] = static_cast<std::uint8_t>((bulk + (1 << 23)) >> 24); };
    acc = in[0] * (radius + 1);
    for (int x = 0; x < edge_a - 1; ++x)
        acc += in[x];
    acc += in[lastx] * (radius - edge_a + 1);
    if (edge_a <= edge_b) {
        for (int x = 0; x < edge_a; ++x) { move(0, x + radius); far(0, x + radius + 1); save(x); }
        for (int x = edge_a; x < edge_b; ++x) {
            move(x - radius - 1, x + radius);
            far(x - radius - 1, x + radius + 1);
            save(x);
        }
        for (int x = edge_b; x <= lastx; ++x) { move(x - radius - 1, lastx); far(x - radius - 1, lastx); save(x); }
    } else {
        for (int x = 0; x < edge_b; ++x) { move(0, x + radius); far(0, x + radius + 1); save(x); }
        for (int x = edge_b; x < edge_a; ++x) { move(0, lastx); far(0, lastx); save(x); }
        for (int x = edge_a; x <= lastx; ++x) { move(x - radius - 1, lastx); far(x - radius - 1, lastx); save(x); }
    }
}

void horizontal_box_blur(Bitmap& image, float float_radius) {
    int radius = static_cast<int>(float_radius);
    std::uint32_t ww = static_cast<std::uint32_t>(static_cast<std::uint32_t>(1 << 24) / (float_radius * 2 + 1));
    std::uint32_t fw = ((1 << 24) - (radius * 2 + 1) * ww) / 2;
    int edge_a = std::min(radius + 1, image.width), edge_b = std::max(image.width - radius - 1, 0);
    std::vector<std::uint8_t> line(image.width);
    for (int y = 0; y < image.height; ++y) {
        box_blur_line(line.data(), image.row(y), image.width - 1, radius, edge_a, edge_b, ww, fw);
        std::copy(line.begin(), line.end(), image.row(y));
    }
}

// ---- PNG (PngImagePlugin.py, ZipDecode.c, Unpack.c and Convert.c) ---------

std::uint32_t be32(const unsigned char* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

Bitmap decode_png(std::string_view bytes, long long max_pixels) {
    auto data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t size = bytes.size(), at = 8;
    long long width = 0, height = 0;
    int depth = 0, type = -1, interlace = 0, orientation = 1;
    std::vector<unsigned char> idat, palette, trns;
    bool has_trns = false;
    while (at + 12 <= size) {
        std::size_t length = be32(data + at);
        if (length > size - at - 12)
            undecodable();
        std::string_view kind(bytes.data() + at + 4, 4);
        const unsigned char* body = data + at + 8;
        if (kind == "IHDR") {
            if (length < 13 || body[10] || body[11] || body[12] > 1)
                undecodable();
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            type = body[9];
            interlace = body[12];
        } else if (kind == "PLTE") {
            palette.assign(body, body + length);
        } else if (kind == "tRNS") {
            trns.assign(body, body + length);
            has_trns = true;
        } else if (kind == "IDAT") {
            idat.insert(idat.end(), body, body + length);
        } else if (kind == "eXIf") {
            std::string_view exif(bytes.data() + at + 8, length);
            if (exif.starts_with(std::string_view("Exif\0\0", 6)))
                exif.remove_prefix(6);
            orientation = exif_orientation(exif);
        } else if (kind == "IEND") {
            break;
        }
        at += length + 12;
    }
    bool valid = (type == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                 ((type == 2 || type == 4 || type == 6) && (depth == 8 || depth == 16)) ||
                 (type == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8));
    if (!valid || width > INT32_MAX || height > INT32_MAX)
        undecodable();
    check_size(width, height, max_pixels);
    int channels = type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 1;
    int pixel_bits = channels * depth, step = std::max(1, pixel_bits / 8);
    struct Pass { int x, y, dx, dy; };
    std::vector<Pass> passes{{0, 0, 1, 1}};
    if (interlace)
        passes = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    std::size_t expected = 0;
    for (auto pass : passes) {
        long long w = width > pass.x ? (width - pass.x + pass.dx - 1) / pass.dx : 0;
        long long h = height > pass.y ? (height - pass.y + pass.dy - 1) / pass.dy : 0;
        if (w && h)
            expected += static_cast<std::size_t>(h) * (1 + (w * pixel_bits + 7) / 8);
    }
    std::size_t inflated = 0;
    std::unique_ptr<l_uint8, decltype(&lept_free)> raw(
        idat.empty() ? nullptr : zlibUncompress(idat.data(), idat.size(), &inflated), lept_free);
    if (!raw || inflated < expected)
        undecodable();

    // Pillow mode semantics: transparency compares the converted 8-bit RGB with
    // the low byte of each tRNS sample; palettes keep per-entry alpha.
    int key_r = -1, key_g = -1, key_b = -1;
    std::vector<std::array<std::uint8_t, 4>> colors(256, {0, 0, 0, 255});
    if (type == 3) {
        for (std::size_t i = 0; i < 256 && i * 3 + 2 < palette.size(); ++i)
            colors[i] = {palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2], 255};
        if (has_trns) {
            std::size_t zeros = std::count(trns.begin(), trns.end(), 0);
            bool simple = zeros == 1 && std::count(trns.begin(), trns.end(), 255) + 1 == static_cast<long>(trns.size());
            if (simple)
                colors[std::find(trns.begin(), trns.end(), 0) - trns.begin()][3] = 0;
            else if (zeros || trns.size() != static_cast<std::size_t>(std::count(trns.begin(), trns.end(), 255)))
                for (std::size_t i = 0; i < trns.size() && i < 256; ++i)
                    colors[i][3] = trns[i];
        }
    } else if (has_trns && (type == 0 || type == 2)) {
        if (trns.size() < (type == 2 ? 6u : 2u))
            undecodable();
        if (type == 0 && depth == 1) {
            key_r = key_g = key_b = (trns[0] << 8 | trns[1]) ? 255 : 0;
        } else if (type == 0) {
            key_r = key_g = key_b = trns[1];
        } else {
            key_r = trns[1];
            key_g = trns[3];
            key_b = trns[5];
        }
    }
    bool gray = type == 0 || type == 4;
    Bitmap out(static_cast<int>(width), static_cast<int>(height), gray ? 1 : 3, 255);
    std::vector<unsigned char> previous, current;
    const unsigned char* source = raw.get();
    for (auto pass : passes) {
        int w = width > pass.x ? static_cast<int>((width - pass.x + pass.dx - 1) / pass.dx) : 0;
        int h = height > pass.y ? static_cast<int>((height - pass.y + pass.dy - 1) / pass.dy) : 0;
        if (!w || !h)
            continue;
        std::size_t row_bytes = (static_cast<std::size_t>(w) * pixel_bits + 7) / 8;
        previous.assign(row_bytes, 0);
        current.resize(row_bytes);
        for (int j = 0; j < h; ++j) {
            int filter = *source++;
            std::copy_n(source, row_bytes, current.begin());
            source += row_bytes;
            for (std::size_t i = 0; i < row_bytes; ++i) {
                int left = i >= static_cast<std::size_t>(step) ? current[i - step] : 0;
                int up = previous[i], corner = i >= static_cast<std::size_t>(step) ? previous[i - step] : 0;
                switch (filter) {
                case 0: break;
                case 1: current[i] += left; break;
                case 2: current[i] += up; break;
                case 3: current[i] += (left + up) / 2; break;
                case 4: current[i] += paeth(left, up, corner); break;
                default: undecodable();
                }
            }
            auto sample = [&](int index) -> int {
                if (depth == 8)
                    return current[index];
                if (depth == 16)
                    return current[index * 2] << 8 | current[index * 2 + 1];
                int bit = index * depth;
                return (current[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
            };
            for (int i = 0; i < w; ++i) {
                int r, g, b, a = 255;
                if (type == 0 || type == 4) {
                    int v = sample(i * channels);
                    if (depth == 16)
                        v = type == 0 ? (v >> 8 ? 255 : v) : v >> 8;  // "I;16" clips; "LA;16B" keeps the high byte
                    else if (depth < 8)
                        v = v * 255 / ((1 << depth) - 1);
                    r = g = b = v;
                    if (type == 4)
                        a = depth == 16 ? sample(i * 2 + 1) >> 8 : sample(i * 2 + 1);
                } else if (type == 3) {
                    auto& color = colors[sample(i)];
                    r = color[0], g = color[1], b = color[2], a = color[3];
                } else {
                    int shift = depth == 16 ? 8 : 0;
                    r = sample(i * channels) >> shift;
                    g = sample(i * channels + 1) >> shift;
                    b = sample(i * channels + 2) >> shift;
                    if (type == 6)
                        a = sample(i * channels + 3) >> shift;
                }
                if (r == key_r && g == key_g && b == key_b)
                    a = 0;
                auto pixel = over_white(r, g, b, a);
                std::uint8_t* target = out.row(pass.y + j * pass.dy) + (pass.x + i * pass.dx) * out.bands;
                std::copy_n(pixel.begin(), out.bands, target);
            }
            previous.swap(current);
            current.resize(row_bytes);
        }
    }
    return orient(out, orientation);
}

int jpeg_orientation(std::string_view data) {
    auto byte = [&](std::size_t at) { return static_cast<unsigned char>(data[at]); };
    for (std::size_t at = 2; at + 4 <= data.size() && byte(at) == 255;) {
        unsigned marker = byte(at + 1);
        if (marker == 255) {
            ++at;
            continue;
        }
        if (marker == 218 || marker == 217)
            break;
        std::size_t length = byte(at + 2) * 256 + byte(at + 3);
        if (length < 2 || length > data.size() - at - 2)
            break;
        auto segment = data.substr(at + 4, length - 2);
        if (marker == 225 && segment.starts_with(std::string_view("Exif\0\0", 6)))
            return exif_orientation(segment.substr(6));
        at += length + 2;
    }
    return 1;
}

// JPEG, GIF, BMP, TIFF and JPEG 2000 through Leptonica's codecs, with Pillow's
// conversion of each pixel layout to white-composited RGB.
Bitmap decode_leptonica(std::string_view bytes, long long max_pixels) {
    auto data = reinterpret_cast<const unsigned char*>(bytes.data());
    l_int32 format = 0, w = 0, h = 0, bits = 0, spp = 0, colormapped = 0;
    if (pixReadHeaderMem(data, bytes.size(), &format, &w, &h, &bits, &spp, &colormapped) != 0)
        undecodable();
    check_size(w, h, max_pixels);
    auto destroy = [](PIX* pix) { pixDestroy(&pix); };
    std::unique_ptr<PIX, decltype(destroy)> pix(pixReadMem(data, bytes.size()), destroy);
    if (!pix)
        undecodable();
    w = pixGetWidth(pix.get());
    h = pixGetHeight(pix.get());
    int depth = pixGetDepth(pix.get());
    PIXCMAP* cmap = pixGetColormap(pix.get());
    bool gray = !cmap && depth <= 16;
    // Pillow reads uncompressed 32-bit BMP pixels as BGRX, ignoring the fourth byte.
    bool bmp_rgb = format == IFF_BMP && bytes.size() >= 34 && data[30] == 0 && data[31] == 0 && data[32] == 0 && data[33] == 0;
    bool alpha = pixGetSpp(pix.get()) == 4 && !bmp_rgb;
    Bitmap out(w, h, gray ? 1 : 3);
    for (int y = 0; y < h; ++y) {
        l_uint32* line = pixGetData(pix.get()) + y * pixGetWpl(pix.get());
        std::uint8_t* target = out.row(y);
        for (int x = 0; x < w; ++x) {
            if (cmap) {
                l_int32 index = depth == 1 ? GET_DATA_BIT(line, x) : depth == 2 ? GET_DATA_DIBIT(line, x)
                    : depth == 4 ? GET_DATA_QBIT(line, x) : GET_DATA_BYTE(line, x);
                l_int32 r = 0, g = 0, b = 0, a = 255;
                if (index < pixcmapGetCount(cmap))
                    pixcmapGetRGBA(cmap, index, &r, &g, &b, &a);
                auto pixel = over_white(r, g, b, a);
                std::copy_n(pixel.begin(), 3, target + x * 3);
            } else if (depth == 1) {
                target[x] = GET_DATA_BIT(line, x) ? 0 : 255;  // Leptonica: 1 is black
            } else if (depth == 2) {
                target[x] = GET_DATA_DIBIT(line, x) * 85;
            } else if (depth == 4) {
                target[x] = GET_DATA_QBIT(line, x) * 17;
            } else if (depth == 8) {
                target[x] = GET_DATA_BYTE(line, x);
            } else if (depth == 16) {
                target[x] = GET_DATA_TWO_BYTES(line, x) >> 8;
            } else if (depth == 32) {
                l_int32 r, g, b, a;
                extractRGBAValues(line[x], &r, &g, &b, &a);
                auto pixel = over_white(r, g, b, alpha ? a : 255);
                std::copy_n(pixel.begin(), 3, target + x * 3);
            } else {
                undecodable();
            }
        }
    }
    int orientation = format == IFF_JFIF_JPEG ? jpeg_orientation(bytes) : 1;
    return orient(out, orientation);
}

} // namespace

Bitmap::Bitmap(int w, int h, int b, std::uint8_t fill)
    : width(w), height(h), bands(b), data(static_cast<std::size_t>(w) * h * b, fill) {}

Bitmap luminance(const Bitmap& image) {
    if (image.bands == 1)
        return image;
    Bitmap out(image.width, image.height, 1);
    for (std::size_t i = 0, n = out.data.size(); i < n; ++i) {
        const std::uint8_t* p = &image.data[i * 3];
        out.data[i] = static_cast<std::uint8_t>((p[0] * 19595 + p[1] * 38470 + p[2] * 7471 + 0x8000) >> 16);
    }
    return out;
}

Bitmap rgb(const Bitmap& image) {
    if (image.bands == 3)
        return image;
    Bitmap out(image.width, image.height, 3);
    for (std::size_t i = 0, n = image.data.size(); i < n; ++i)
        out.data[i * 3] = out.data[i * 3 + 1] = out.data[i * 3 + 2] = image.data[i];
    return out;
}

Bitmap crop(const Bitmap& image, int left, int top, int right, int bottom) {
    if (right < left)
        throw std::runtime_error("Coordinate 'right' is less than 'left'");
    if (bottom < top)
        throw std::runtime_error("Coordinate 'lower' is less than 'upper'");
    Bitmap out(right - left, bottom - top, image.bands, 0);
    int x0 = std::max(0, left), x1 = std::min(image.width, right);
    for (int y = std::max(0, top); y < std::min(image.height, bottom); ++y)
        if (x1 > x0)
            std::copy_n(image.row(y) + x0 * image.bands, (x1 - x0) * image.bands,
                        out.row(y - top) + (x0 - left) * image.bands);
    return out;
}

Bitmap expand(const Bitmap& image, int border, std::uint8_t fill) {
    Bitmap out(image.width + 2 * border, image.height + 2 * border, image.bands, fill);
    paste(out, image, border, border);
    return out;
}

Bitmap transpose(const Bitmap& image, Transpose method) {
    bool swap = method == Transpose::rotate_90 || method == Transpose::rotate_270 ||
                method == Transpose::transpose || method == Transpose::transverse;
    int w = image.width, h = image.height;
    Bitmap out(swap ? h : w, swap ? w : h, image.bands);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            int sx = x, sy = y;
            switch (method) {
            case Transpose::flip_left_right: sx = w - 1 - x; break;
            case Transpose::flip_top_bottom: sy = h - 1 - y; break;
            case Transpose::rotate_90: sx = w - 1 - y; sy = x; break;
            case Transpose::rotate_180: sx = w - 1 - x; sy = h - 1 - y; break;
            case Transpose::rotate_270: sx = y; sy = h - 1 - x; break;
            case Transpose::transpose: sx = y; sy = x; break;
            case Transpose::transverse: sx = w - 1 - y; sy = h - 1 - x; break;
            }
            std::copy_n(image.row(sy) + sx * image.bands, image.bands, out.row(y) + x * image.bands);
        }
    return out;
}

Bitmap resize(const Bitmap& image, int width, int height, Filter filter) {
    return resize(image, width, height, filter, {0.f, 0.f, float(image.width), float(image.height)});
}

Bitmap resize(const Bitmap& image, int width, int height, Filter filter, std::array<float, 4> box) {
    if (width == image.width && height == image.height && box[0] == 0 && box[1] == 0 &&
        box[2] == image.width && box[3] == image.height)
        return image;
    if (filter == Filter::nearest)
        return resize_nearest(image, width, height, box);
    auto f = kernel(filter);
    bool need_horizontal = width != image.width || box[0] || box[2] != width;
    bool need_vertical = height != image.height || box[1] || box[3] != height;
    bool horizontal_first = !((image.height - height) > 0 && (image.height - height) > (image.width - width) * 2);
    Coefficients vertical, horizontal;
    if ((need_horizontal && horizontal_first) || need_vertical)
        vertical = precompute(image.height, box[1], box[3], height, f);
    int first = 0, last = image.height;
    if (need_horizontal) {
        if (horizontal_first) {
            first = vertical.bounds[0];
            last = vertical.bounds[height * 2 - 2] + vertical.bounds[height * 2 - 1];
            for (int i = 0; i < height; ++i)
                vertical.bounds[i * 2] -= first;
        }
        horizontal = precompute(image.width, box[0], box[2], width, f);
    }
    Bitmap out = image;
    if (horizontal_first) {
        if (need_horizontal)
            out = horizontal_pass(out, width, first, last - first, horizontal);
        if (need_vertical)
            out = vertical_pass(out, height, vertical);
    } else {
        if (need_vertical)
            out = vertical_pass(out, height, vertical);
        if (need_horizontal)
            out = horizontal_pass(out, width, 0, height, horizontal);
    }
    return out;
}

Bitmap reduce(const Bitmap& image, int xscale, int yscale, std::array<int, 4> box) {
    if (xscale == 1 && yscale == 1)
        return crop(image, box[0], box[1], box[2], box[3]);
    int bw = box[2] - box[0], bh = box[3] - box[1];
    Bitmap out((bw + xscale - 1) / xscale, (bh + yscale - 1) / yscale, image.bands);
    // Every Reduce.c specialization computes ((sum + n / 2) * multiplier(n)) >> 24.
    auto multiplier = [](int n) {
        std::uint32_t max_dividend = (1u << 8) * n;
        float max_int = (1 << 30) * 4.0;
        return static_cast<std::uint32_t>(max_int / max_dividend);
    };
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            int x0 = box[0] + x * xscale, y0 = box[1] + y * yscale;
            int x1 = std::min(x0 + xscale, box[0] + bw), y1 = std::min(y0 + yscale, box[1] + bh);
            std::uint32_t n = (x1 - x0) * (y1 - y0);
            for (int band = 0; band < image.bands; ++band) {
                std::uint32_t sum = n / 2;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx)
                        sum += image.row(yy)[xx * image.bands + band];
                out.row(y)[x * image.bands + band] = static_cast<std::uint8_t>((sum * multiplier(n)) >> 24);
            }
        }
    return out;
}

void thumbnail(Bitmap& image, int width, int height) {
    int x = width, y = height;
    if (x >= image.width && y >= image.height)
        return;
    double aspect = static_cast<double>(image.width) / image.height;
    auto round_aspect = [](double number, auto key) {
        int low = static_cast<int>(std::floor(number)), high = static_cast<int>(std::ceil(number));
        return std::max(key(high) < key(low) ? high : low, 1);
    };
    if (static_cast<double>(x) / y >= aspect)
        x = round_aspect(y * aspect, [&](int n) { return std::abs(aspect - static_cast<double>(n) / y); });
    else
        y = round_aspect(x / aspect, [&](int n) { return n == 0 ? 0.0 : std::abs(aspect - static_cast<double>(x) / n); });
    if (x == image.width && y == image.height)
        return;
    // Image.resize(size, BICUBIC, reducing_gap=2.0)
    double box[4] = {0, 0, double(image.width), double(image.height)};
    int factor_x = std::max(1, static_cast<int>((box[2] - box[0]) / x / 2.0));
    int factor_y = std::max(1, static_cast<int>((box[3] - box[1]) / y / 2.0));
    Bitmap source = image;
    if (factor_x > 1 || factor_y > 1) {
        double support = 2.0 - 0.5, scale_x = (box[2] - box[0]) / x, scale_y = (box[3] - box[1]) / y;
        std::array<int, 4> safe{std::max(0, static_cast<int>(box[0] - support * scale_x)),
                                std::max(0, static_cast<int>(box[1] - support * scale_y)),
                                std::min(image.width, static_cast<int>(std::ceil(box[2] + support * scale_x))),
                                std::min(image.height, static_cast<int>(std::ceil(box[3] + support * scale_y)))};
        source = reduce(image, factor_x, factor_y, safe);
        box[0] = (box[0] - safe[0]) / factor_x;
        box[1] = (box[1] - safe[1]) / factor_y;
        box[2] = (box[2] - safe[0]) / factor_x;
        box[3] = (box[3] - safe[1]) / factor_y;
    }
    image = resize(source, x, y, Filter::bicubic, {float(box[0]), float(box[1]), float(box[2]), float(box[3])});
}

void fill_rectangle(Bitmap& image, int x0, int y0, int x1, int y1, std::uint8_t value) {
    if (x1 < x0 || y1 < y0)
        throw std::runtime_error(x1 < x0 ? "x1 must be greater than or equal to x0" : "y1 must be greater than or equal to y0");
    // Draw.c: rows clip to [0, ysize]; hline clips columns to the image.
    if (y0 < 0)
        y0 = 0;
    else if (y0 >= image.height)
        return;
    if (y1 < 0)
        return;
    y1 = std::min(y1, image.height - 1);
    if (x0 < 0)
        x0 = 0;
    else if (x0 >= image.width)
        return;
    if (x1 < 0)
        return;
    x1 = std::min(x1, image.width - 1);
    for (int y = y0; y <= y1; ++y)
        std::fill(image.row(y) + x0 * image.bands, image.row(y) + (x1 + 1) * image.bands, value);
}

void paste(Bitmap& image, const Bitmap& piece, int x, int y) {
    int x0 = std::max(0, x), x1 = std::min(image.width, x + piece.width);
    for (int row = std::max(0, y); row < std::min(image.height, y + piece.height); ++row)
        if (x1 > x0)
            std::copy_n(piece.row(row - y) + (x0 - x) * piece.bands, (x1 - x0) * piece.bands,
                        image.row(row) + x0 * image.bands);
}

Bitmap point(const Bitmap& image, const Lut& lut) {
    Bitmap out = image;
    for (auto& value : out.data)
        value = lut[value];
    return out;
}

std::array<long long, 256> histogram(const Bitmap& image) {
    std::array<long long, 256> counts{};
    for (auto value : image.data)
        ++counts[value];
    return counts;
}

Bitmap autocontrast(const Bitmap& image) {
    auto h = histogram(image);
    int lo = 0, hi = 255;
    for (lo = 0; lo < 255 && !h[lo]; ++lo) {}
    for (hi = 255; hi > 0 && !h[hi]; --hi) {}
    Lut lut;
    if (hi <= lo) {
        for (int i = 0; i < 256; ++i)
            lut[i] = static_cast<std::uint8_t>(i);
    } else {
        double scale = 255.0 / (hi - lo), offset = -lo * scale;
        for (int i = 0; i < 256; ++i)
            lut[i] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(i * scale + offset), 0, 255));
    }
    return point(image, lut);
}

Bitmap gaussian_blur(const Bitmap& image, float radius) {
    constexpr int passes = 3;
    Bitmap out = image;
    if (out.width == 0 || out.height == 0)
        return out;
    float r = gaussian_radius(radius, passes);
    if (r != 0) {
        for (int i = 0; i < passes; ++i)
            horizontal_box_blur(out, r);
        out = transpose(out, Transpose::transpose);
        for (int i = 0; i < passes; ++i)
            horizontal_box_blur(out, r);
        out = transpose(out, Transpose::transpose);
    }
    return out;
}

Bitmap find_edges(const Bitmap& image) {
    if (image.width < 3 || image.height < 3)
        return image;
    Bitmap out = image;
    for (int y = 1; y < image.height - 1; ++y) {
        const std::uint8_t *up = image.row(y - 1), *mid = image.row(y), *down = image.row(y + 1);
        std::uint8_t* target = out.row(y);
        for (int x = 1; x < image.width - 1; ++x) {
            float ss = 0.5f;
            ss += float(down[x - 1]) * -1.f + float(down[x]) * -1.f + float(down[x + 1]) * -1.f;
            ss += float(mid[x - 1]) * -1.f + float(mid[x]) * 8.f + float(mid[x + 1]) * -1.f;
            ss += float(up[x - 1]) * -1.f + float(up[x]) * -1.f + float(up[x + 1]) * -1.f;
            ss = ss < 0.0f ? 0.0f : ss;
            ss = ss > 255.0f ? 255.0f : ss;
            target[x] = static_cast<std::uint8_t>(ss);
        }
    }
    return out;
}

Bitmap rank_filter(const Bitmap& image, int size, bool maximum) {
    // Pillow expands the borders by edge replication, so the extreme of the
    // clipped window is exact. Rows, then columns.
    int radius = size / 2;
    auto pick = [maximum](std::uint8_t a, std::uint8_t b) { return maximum ? std::max(a, b) : std::min(a, b); };
    Bitmap across = image;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            std::uint8_t value = image.row(y)[x];
            for (int i = std::max(0, x - radius); i <= std::min(image.width - 1, x + radius); ++i)
                value = pick(value, image.row(y)[i]);
            across.row(y)[x] = value;
        }
    Bitmap out = across;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            std::uint8_t value = across.row(y)[x];
            for (int j = std::max(0, y - radius); j <= std::min(image.height - 1, y + radius); ++j)
                value = pick(value, across.row(j)[x]);
            out.row(y)[x] = value;
        }
    return out;
}

Bitmap floyd_steinberg(const Bitmap& image) {
    Bitmap out(image.width, image.height, 1);
    std::vector<int> errors(image.width + 1, 0);
    for (int y = 0; y < image.height; ++y) {
        const std::uint8_t* in = image.row(y);
        std::uint8_t* target = out.row(y);
        int l = 0, l0 = 0, l1 = 0, l2, d2, x = 0;
        for (; x < image.width; ++x) {
            l = std::clamp(in[x] + (l + errors[x + 1]) / 16, 0, 255);
            target[x] = l > 128 ? 255 : 0;
            l -= target[x];
            l2 = l;
            d2 = l + l;
            l += d2;
            errors[x] = l + l0;
            l += d2;
            l0 = l + l1;
            l1 = l2;
            l += d2;
        }
        errors[x] = l0;
    }
    return out;
}

Bitmap subtract(const Bitmap& first, const Bitmap& second) {
    Bitmap out = first;
    for (std::size_t i = 0; i < out.data.size(); ++i)
        out.data[i] = static_cast<std::uint8_t>(std::clamp(first.data[i] - second.data[i], 0, 255));
    return out;
}

Bitmap lighter(const Bitmap& first, const Bitmap& second) {
    Bitmap out = first;
    for (std::size_t i = 0; i < out.data.size(); ++i)
        out.data[i] = std::max(first.data[i], second.data[i]);
    return out;
}

std::array<std::uint8_t, 3> over_white(int r, int g, int b, int a) {
    if (a == 255)
        return {std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)};
    if (a == 0)
        return {255, 255, 255};
    // AlphaComposite.c with an opaque white destination.
    constexpr std::uint32_t bits = 7;
    std::uint32_t blend = 255u * (255 - a), outa255 = a * 255u + blend;
    std::uint32_t coef1 = a * 255u * 255u * (1u << bits) / outa255, coef2 = 255u * (1u << bits) - coef1;
    auto channel = [&](std::uint32_t value) {
        std::uint32_t t = value * coef1 + 255u * coef2 + (0x80u << bits);
        return static_cast<std::uint8_t>(((((t >> 8) + t) >> 8)) >> bits);
    };
    return {channel(r), channel(g), channel(b)};
}

int exif_orientation(std::string_view data) {
    if (data.size() < 8 || (data.substr(0, 2) != "II" && data.substr(0, 2) != "MM"))
        return 1;
    bool little = data[0] == 'I';
    auto word = [&](std::size_t offset, unsigned length) {
        if (offset > data.size() || length > data.size() - offset)
            return std::uint32_t{0};
        std::uint32_t value = 0;
        for (unsigned i = 0; i < length; ++i) {
            auto at = little ? offset + length - i - 1 : offset + i;
            value = (value << 8) | static_cast<unsigned char>(data[at]);
        }
        return value;
    };
    if (word(2, 2) != 42)
        return 1;
    std::size_t offset = word(4, 4);
    if (offset < 8 || offset > data.size() || 2 > data.size() - offset)
        return 1;
    auto entries = word(offset, 2);
    offset += 2;
    if (entries > (data.size() - offset) / 12)
        return 1;
    for (unsigned i = 0; i < entries; ++i, offset += 12) {
        if (word(offset, 2) == 0x0112 && word(offset + 2, 2) == 3 && word(offset + 4, 4) == 1) {
            auto orientation = word(offset + 8, 2);
            return orientation >= 1 && orientation <= 8 ? static_cast<int>(orientation) : 1;
        }
    }
    return 1;
}

Bitmap orient(const Bitmap& image, int orientation) {
    switch (orientation) {
    case 2: return transpose(image, Transpose::flip_left_right);
    case 3: return transpose(image, Transpose::rotate_180);
    case 4: return transpose(image, Transpose::flip_top_bottom);
    case 5: return transpose(image, Transpose::transpose);
    case 6: return transpose(image, Transpose::rotate_270);
    case 7: return transpose(image, Transpose::transverse);
    case 8: return transpose(image, Transpose::rotate_90);
    default: return image;
    }
}

Bitmap decode_picture(std::string_view bytes, long long max_pixels) {
    if (bytes.starts_with("\x89PNG\r\n\x1a\n"))
        return decode_png(bytes, max_pixels);
    if (bytes.empty())
        undecodable();
    return decode_leptonica(bytes, max_pixels);
}

} // namespace braille::imaging
