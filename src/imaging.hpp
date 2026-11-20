#pragma once

// Pillow-compatible raster operations for the picture pipeline.
//
// The Python reference converter processed pictures with Pillow 13.0.0. These
// functions port the integer and fixed-point arithmetic of the Pillow
// operations it used, so the native converter produces the same pixels.
//
// Portions derived from Pillow 13.0.0 (src/libImaging: Resample.c, Reduce.c,
// BoxBlur.c, Filter.c, Convert.c, AlphaComposite.c, Draw.c, Geometry.c;
// src/PIL: Image.py, ImageOps.py, PngImagePlugin.py) are covered by this notice:
//
// The Python Imaging Library (PIL) is
//
//     Copyright © 1997-2011 by Secret Labs AB
//     Copyright © 1995-2011 by Fredrik Lundh and contributors
//
// Pillow is the friendly PIL fork. It is
//
//     Copyright © 2010 by Jeffrey 'Alex' Clark and contributors
//
// Like PIL, Pillow is licensed under the open source MIT-CMU License:
//
// By obtaining, using, and/or copying this software and/or its associated
// documentation, you agree that you have read, understood, and will comply
// with the following terms and conditions:
//
// Permission to use, copy, modify and distribute this software and its
// documentation for any purpose and without fee is hereby granted,
// provided that the above copyright notice appears in all copies, and that
// both that copyright notice and this permission notice appear in supporting
// documentation, and that the name of Secret Labs AB or the author not be
// used in advertising or publicity pertaining to distribution of the software
// without specific, written prior permission.
//
// SECRET LABS AB AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
// IN NO EVENT SHALL SECRET LABS AB OR THE AUTHOR BE LIABLE FOR ANY SPECIAL,
// INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace braille::imaging {

// An 8-bit raster with one (Pillow "L") or three (Pillow "RGB") interleaved bands.
struct Bitmap {
    int width = 0, height = 0, bands = 1;
    std::vector<std::uint8_t> data;

    Bitmap() = default;
    Bitmap(int width, int height, int bands = 1, std::uint8_t fill = 0);
    std::uint8_t* row(int y) { return data.data() + static_cast<std::size_t>(y) * width * bands; }
    const std::uint8_t* row(int y) const { return data.data() + static_cast<std::size_t>(y) * width * bands; }
    // The first band of one pixel; the value of an "L" image.
    int at(int x, int y) const { return data[(static_cast<std::size_t>(y) * width + x) * bands]; }
};

enum class Filter { nearest, box, bicubic, lanczos };
enum class Transpose { flip_left_right, flip_top_bottom, rotate_90, rotate_180, rotate_270, transpose, transverse };
using Lut = std::array<std::uint8_t, 256>;

// Image.convert("L") from "RGB", ITU-R 601-2 luma in Pillow's fixed point.
Bitmap luminance(const Bitmap& image);
// Image.convert("RGB") from "L".
Bitmap rgb(const Bitmap& image);
// Image.crop: areas outside the source are zero.
Bitmap crop(const Bitmap& image, int left, int top, int right, int bottom);
// ImageOps.expand with a uniform border colour.
Bitmap expand(const Bitmap& image, int border, std::uint8_t fill);
Bitmap transpose(const Bitmap& image, Transpose method);
// Image.resize; box is the source rectangle (Pillow's float box).
Bitmap resize(const Bitmap& image, int width, int height, Filter filter);
Bitmap resize(const Bitmap& image, int width, int height, Filter filter, std::array<float, 4> box);
// Image.reduce over the integer box (left, top, right, bottom).
Bitmap reduce(const Bitmap& image, int xscale, int yscale, std::array<int, 4> box);
// Image.thumbnail with its default BICUBIC filter and reducing_gap=2.0.
void thumbnail(Bitmap& image, int width, int height);
// ImageDraw.rectangle(fill=...) with a value for every band.
void fill_rectangle(Bitmap& image, int x0, int y0, int x1, int y1, std::uint8_t value);
// Image.paste of a same-mode image at (x, y).
void paste(Bitmap& image, const Bitmap& piece, int x, int y);
Bitmap point(const Bitmap& image, const Lut& lut);
std::array<long long, 256> histogram(const Bitmap& image);
Bitmap autocontrast(const Bitmap& image);
// ImageFilter.GaussianBlur(radius), three box-blur passes.
Bitmap gaussian_blur(const Bitmap& image, float radius);
// ImageFilter.FIND_EDGES.
Bitmap find_edges(const Bitmap& image);
// ImageFilter.MinFilter(size) or MaxFilter(size).
Bitmap rank_filter(const Bitmap& image, int size, bool maximum);
// Image.convert("1", dither=Image.Dither.FLOYDSTEINBERG) from "L"; pixels are 0 or 255.
Bitmap floyd_steinberg(const Bitmap& image);
// ImageChops.subtract and ImageChops.lighter.
Bitmap subtract(const Bitmap& first, const Bitmap& second);
Bitmap lighter(const Bitmap& first, const Bitmap& second);

// open_picture + white_picture: decode, apply EXIF orientation, composite any
// alpha onto white, and return "RGB" (or "L" for single-channel sources).
Bitmap decode_picture(std::string_view bytes, long long max_pixels);
// Alpha-composite one RGBA pixel onto opaque white (Image.alpha_composite).
std::array<std::uint8_t, 3> over_white(int r, int g, int b, int a);
// TIFF orientation tag inside an EXIF block, 1 when absent.
int exif_orientation(std::string_view tiff);
// ImageOps.exif_transpose for an orientation value.
Bitmap orient(const Bitmap& image, int orientation);

} // namespace braille::imaging
