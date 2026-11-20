#pragma once

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <memory>
#include <stdexcept>
#include <type_traits>

namespace braille {

class PdfContext {
public:
    PdfContext() : context_(fz_new_context(nullptr, nullptr, 64 * 1024 * 1024), fz_drop_context) {
        if (!context_)
            throw std::runtime_error("Cannot allocate MuPDF context");
    }

    fz_context* get() const { return context_.get(); }

    // The callable contains only the C API operation. C++ objects owned by
    // the caller stay outside MuPDF's setjmp/longjmp frame.
    template<class Operation>
    auto call(Operation operation) const {
        auto ctx = context_.get();
        using Result = decltype(operation(ctx));
        if constexpr (std::is_void_v<Result>) {
            fz_try(ctx) { operation(ctx); }
            fz_catch(ctx) { throw std::runtime_error(fz_caught_message(ctx)); }
        } else {
            Result value{};
            fz_var(value);
            fz_try(ctx) { value = operation(ctx); }
            fz_catch(ctx) { throw std::runtime_error(fz_caught_message(ctx)); }
            return value;
        }
    }

    template<class T>
    auto own(T* pointer, void (*drop)(fz_context*, T*)) const {
        auto deleter = [ctx = context_.get(), drop](T* p) { drop(ctx, p); };
        return std::unique_ptr<T, decltype(deleter)>(pointer, deleter);
    }

private:
    std::unique_ptr<fz_context, decltype(&fz_drop_context)> context_;
};

inline void add_pdf_page(const PdfContext& mu, pdf_document* doc, float width, float height,
                         const std::string& commands, pdf_obj* resources = nullptr) {
    auto data = mu.own(mu.call([&](auto ctx) {
        return fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(commands.data()), commands.size());
    }), fz_drop_buffer);
    auto page = mu.own(mu.call([&](auto ctx) {
        return pdf_add_page(ctx, doc, {0, 0, width, height}, 0, resources, data.get());
    }), pdf_drop_obj);
    mu.call([&](auto ctx) { pdf_insert_page(ctx, doc, -1, page.get()); });
}

inline void save_pdf(const PdfContext& mu, pdf_document* doc, const std::string& path) {
    pdf_write_options options = pdf_default_write_options;
    options.do_compress = 1;
    mu.call([&](auto ctx) { pdf_save_document(ctx, doc, path.c_str(), &options); });
}

} // namespace braille
