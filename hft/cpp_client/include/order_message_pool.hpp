#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>



namespace arena::detail {

// These are application limits. 
inline constexpr std::size_t kPoolSlots = 1;
inline constexpr std::size_t kMaxSymbolBytes = 16;
inline constexpr std::size_t kMaxTeamIdBytes = 32;
inline constexpr std::size_t kMaxOrderIdBytes = 128;

// Conservative numeric formatting budgets.
inline constexpr std::size_t kPriceTextBytes = 128;
inline constexpr std::size_t kQuantityTextBytes = 32;

// Skeletons used only to calculate fixed JSON overhead.
// Numeric values are intentionally absent in the place-order skeleton.
inline constexpr char kPlaceSkeleton[] =
    "{\"type\":\"place_order\",\"team_id\":\"\",\"symbol\":\"\","
    "\"side\":\"\",\"order_type\":\"\",\"price\":,\"quantity\":}";

inline constexpr char kCancelSkeleton[] =
    "{\"type\":\"cancel_order\",\"team_id\":\"\","
    "\"order_id\":\"\",\"symbol\":\"\"}";

// JSON escaping can expand one input byte to six output bytes.
inline constexpr std::size_t kPlaceWireBytes =
    sizeof(kPlaceSkeleton) - 1
    + 6 * kMaxTeamIdBytes
    + 6 * kMaxSymbolBytes
    + 4  // "sell"
    + 6  // "market"
    + kPriceTextBytes
    + kQuantityTextBytes;

inline constexpr std::size_t kCancelWireBytes =
    sizeof(kCancelSkeleton) - 1
    + 6 * kMaxTeamIdBytes
    + 6 * kMaxOrderIdBytes
    + 6 * kMaxSymbolBytes;

inline constexpr std::size_t kMaxWireBytes =
    kPlaceWireBytes > kCancelWireBytes
        ? kPlaceWireBytes
        : kCancelWireBytes;

static_assert(kPlaceWireBytes == 552);
static_assert(kCancelWireBytes == 1118);

enum class MessageKind {
    Limit,
    Market,
    Cancel
};

struct OutboundMessage {
    MessageKind kind = MessageKind::Limit;

    std::array<char, kMaxSymbolBytes> symbol{};
    std::size_t symbol_size = 0;

    std::array<char, kMaxOrderIdBytes> order_id{};
    std::size_t order_id_size = 0;

    bool is_buy = false;
    int quantity = 0;
    double price = 0.0;

    std::string wire;
    bool in_use = false;
};

struct MessagePool {
    std::array<OutboundMessage, kPoolSlots> slots{};

    std::size_t exhausted = 0;
    std::size_t rejected = 0;

    MessagePool() {
        for (auto& slot : slots) {
            slot.wire.reserve(kMaxWireBytes);
        }
    }

    OutboundMessage* try_acquire() {
        for (auto& slot : slots) {
            if (!slot.in_use) {
                slot.in_use = true;

                slot.symbol_size = 0;
                slot.order_id_size = 0;
                slot.is_buy = false;
                slot.quantity = 0;
                slot.price = 0.0;
                slot.wire.clear();

                return &slot;
            }
        }

        ++exhausted;
        return nullptr;
    }
};

// Separate storage for each sending thread.
inline MessagePool& message_pool_for_thread() {
    thread_local MessagePool pool;
    return pool;
}

// Return a borrowed slot automatically on every exit path.
class MessageLease {
public:
    explicit MessageLease(OutboundMessage& message)
        : message_(message) {}

    MessageLease(const MessageLease&) = delete;
    MessageLease& operator=(const MessageLease&) = delete;

    ~MessageLease() {
        message_.in_use = false;
    }

private:
    OutboundMessage& message_;
};

template <std::size_t N>
bool copy_text(std::string_view source,
               std::array<char, N>& destination,
               std::size_t& length) {
    if (source.size() > N) {
        return false;
    }

    if (!source.empty()) {
        std::memcpy(
            destination.data(), source.data(), source.size());
    }

    length = source.size();
    return true;
}

class WireWriter {
public:
    explicit WireWriter(std::string& output)
        : output_(output) {
        output_.clear();
    }

    bool append(std::string_view text) {
        if (text.size() > kMaxWireBytes - output_.size()) {
            return false;
        }

        output_.append(text.data(), text.size());
        return true;
    }

    bool quoted(std::string_view text) {
        if (!append("\"")) {
            return false;
        }

        constexpr char hex[] = "0123456789abcdef";

        for (unsigned char c : text) {
            if (c == '"' || c == '\\') {
                const char escaped[] = {
                    '\\', static_cast<char>(c)
                };

                if (!append(std::string_view(escaped, 2))) {
                    return false;
                }
            } else if (c < 0x20) {
                const char escaped[] = {
                    '\\', 'u', '0', '0',
                    hex[c >> 4], hex[c & 0x0f]
                };

                if (!append(std::string_view(escaped, 6))) {
                    return false;
                }
            } else {
                const char byte = static_cast<char>(c);

                if (!append(std::string_view(&byte, 1))) {
                    return false;
                }
            }
        }

        return append("\"");
    }

    bool integer(int value) {
        char buffer[kQuantityTextBytes];

        const auto result = std::to_chars(
            buffer, buffer + sizeof(buffer), value);

        if (result.ec != std::errc{}) {
            return false;
        }

        return append(std::string_view(
            buffer,
            static_cast<std::size_t>(result.ptr - buffer)));
    }

    bool number(double value) {
        if (!std::isfinite(value)) {
            return false;
        }

        char buffer[kPriceTextBytes];

        const auto result = std::to_chars(
            buffer,
            buffer + sizeof(buffer),
            value,
            std::chars_format::general);

        if (result.ec != std::errc{}) {
            return false;
        }

        return append(std::string_view(
            buffer,
            static_cast<std::size_t>(result.ptr - buffer)));
    }

private:
    std::string& output_;
};

inline bool encode_message(OutboundMessage& message,
                    std::string_view team_id) {
    if (team_id.size() > kMaxTeamIdBytes) {
        return false;
    }

    WireWriter writer(message.wire);

    const std::string_view symbol(
        message.symbol.data(), message.symbol_size);

    if (message.kind == MessageKind::Cancel) {
        const std::string_view order_id(
            message.order_id.data(), message.order_id_size);

        return
            writer.append(
                "{\"type\":\"cancel_order\",\"team_id\":") &&
            writer.quoted(team_id) &&
            writer.append(",\"order_id\":") &&
            writer.quoted(order_id) &&
            writer.append(",\"symbol\":") &&
            writer.quoted(symbol) &&
            writer.append("}");
    }

    const bool market = message.kind == MessageKind::Market;

    return
        writer.append(
            "{\"type\":\"place_order\",\"team_id\":") &&
        writer.quoted(team_id) &&
        writer.append(",\"symbol\":") &&
        writer.quoted(symbol) &&
        writer.append(",\"side\":") &&
        writer.quoted(message.is_buy ? "buy" : "sell") &&
        writer.append(",\"order_type\":") &&
        writer.quoted(market ? "market" : "limit") &&
        writer.append(",\"price\":") &&
        writer.number(market ? 0.0 : message.price) &&
        writer.append(",\"quantity\":") &&
        writer.integer(message.quantity) &&
        writer.append("}");
}

} 