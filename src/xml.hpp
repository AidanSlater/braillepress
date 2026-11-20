#pragma once

#include "braille.hpp"

#include <libxml/parser.h>
#include <zip.h>

#include <memory>
#include <stdexcept>

namespace braille::xml {

using Node = xmlNode*;
using Tree = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
constexpr auto word = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr auto math = "http://schemas.openxmlformats.org/officeDocument/2006/math";
constexpr auto relations = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

inline Tree parse(std::string_view bytes, const char* kind) {
    if (bytes.empty() || bytes.size() > 64 * 1024 * 1024)
        throw std::runtime_error(std::string(kind) + " XML is empty or exceeds the document limit");
    Tree xml(xmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr,
        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
    if (!xml || !xmlDocGetRootElement(xml.get()))
        throw std::runtime_error(std::string("Invalid ") + kind + " XML");
    auto subset = xmlGetIntSubset(xml.get());
    if (subset && (subset->entities || subset->pentities))
        throw std::runtime_error(std::string(kind) + " XML entity definitions are unsupported");
    return xml;
}

inline std::string_view name(Node node) {
    return node && node->name ? reinterpret_cast<const char*>(node->name) : "";
}

inline bool is(Node node, std::string_view key, const char* ns = nullptr) {
    return node && node->type == XML_ELEMENT_NODE && name(node) == key &&
        (!ns || (node->ns && xmlStrEqual(node->ns->href, BAD_CAST ns)));
}

inline Node child(Node node, std::string_view key, const char* ns = nullptr) {
    for (auto item = node ? node->children : nullptr; item; item = item->next)
        if (is(item, key, ns))
            return item;
    return nullptr;
}

inline std::vector<Node> children(Node node, std::string_view key = {}, const char* ns = nullptr) {
    std::vector<Node> result;
    for (auto item = node ? node->children : nullptr; item; item = item->next)
        if (item->type == XML_ELEMENT_NODE && (key.empty() || is(item, key, ns)))
            result.push_back(item);
    return result;
}

inline std::string owned(xmlChar* data) {
    std::unique_ptr<xmlChar, xmlFreeFunc> value(data, xmlFree);
    return value ? reinterpret_cast<const char*>(value.get()) : "";
}

inline std::string attribute(Node node, const char* key, std::string fallback = {}, const char* ns = nullptr) {
    if (!node)
        return fallback;
    auto value = ns ? xmlGetNsProp(node, BAD_CAST key, BAD_CAST ns)
                    : xmlGetNoNsProp(node, BAD_CAST key);
    return value ? owned(value) : fallback;
}

inline std::string content(Node node) {
    return node ? owned(xmlNodeGetContent(node)) : "";
}

inline std::string text(Node node, std::string_view key, std::string fallback = {}) {
    auto element = child(node, key);
    return element ? strip(content(element)) : fallback;
}

inline std::vector<Node> descendants(Node root, std::string_view key, const char* ns = nullptr) {
    std::vector<Node> result;
    for (auto node = root ? root->children : nullptr; node;) {
        if (is(node, key, ns))
            result.push_back(node);
        if (node->children) {
            node = node->children;
        } else {
            while (node != root && !node->next)
                node = node->parent;
            node = node == root ? nullptr : node->next;
        }
    }
    return result;
}

class Archive {
public:
    explicit Archive(const fs::path& path) : archive_(zip_open(path.string().c_str(), ZIP_RDONLY, nullptr), zip_discard) {
        if (!archive_)
            throw std::runtime_error("Cannot open compressed document " + path.string());
        std::uint64_t size = 0;
        auto count = zip_get_num_entries(archive_.get(), 0);
        if (count < 0 || count > 20000)
            throw std::runtime_error("Compressed document exceeds the file count limit");
        for (zip_int64_t entry = 0; entry < count; ++entry) {
            zip_stat_t info;
            if (zip_stat_index(archive_.get(), entry, 0, &info) < 0 || !(info.valid & ZIP_STAT_SIZE)
                || info.size > 64 * 1024 * 1024 - size)
                throw std::runtime_error("Expanded document exceeds the limit");
            size += info.size;
        }
    }

    std::string part(const std::string& name, bool optional = false) const {
        zip_stat_t info;
        if (zip_stat(archive_.get(), name.c_str(), 0, &info) < 0) {
            if (optional)
                return {};
            throw std::runtime_error("Compressed document is missing " + name);
        }
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(zip_fopen_index(archive_.get(), info.index, 0), zip_fclose);
        if (!file)
            throw std::runtime_error("Cannot read compressed document part " + name);
        std::string bytes(info.size, '\0');
        if (zip_fread(file.get(), bytes.data(), info.size) != static_cast<zip_int64_t>(info.size))
            throw std::runtime_error("Incomplete compressed document part " + name);
        return bytes;
    }

private:
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive_;
};

} // namespace braille::xml
