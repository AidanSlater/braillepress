#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace embosser::rpc {

using nlohmann::json;
inline constexpr int reply_limit = 4096;

inline std::vector<std::uint8_t> request(int id, std::string_view method, const json& args) {
    return json::to_msgpack(json::array({0, id, method, args}));
}

// One call at a time on the streaming socket. An incomplete frame has no result yet.
template<class Iterator>
std::optional<int> response(Iterator first, Iterator last, int id, std::string_view method) {
    json reply = json::from_msgpack(first, last, true, false);
    if (reply.is_discarded()) return std::nullopt;
    if (!reply.is_array() || reply.size() != 4 || reply[0] != 1 || reply[1] != id)
        throw std::runtime_error("Unexpected firmware RPC reply for " + std::string(method));
    if (!reply[2].is_null())
        throw std::runtime_error("Firmware RPC " + std::string(method) + ": " + reply[2].dump());
    const json& value = reply[3];
    bool fits = value.is_number_unsigned() ? value.get<std::uint64_t>() <= std::numeric_limits<int>::max() :
        value.is_number_integer() && value.get<std::int64_t>() >= std::numeric_limits<int>::min() &&
        value.get<std::int64_t>() <= std::numeric_limits<int>::max();
    if (!fits)
        throw std::runtime_error("Invalid firmware acknowledgment for " + std::string(method));
    return value.get<int>();
}

}
