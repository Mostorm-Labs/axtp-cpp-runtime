#pragma once

#include <algorithm>
#include <limits>
#include <string>

#include "core/protocol/model/input_sample.hpp"
#include "core/protocol/model/bytes.hpp"
#include "core/support/io/byte_reader.hpp"
#include "core/support/io/byte_writer.hpp"

namespace axtp::input {
namespace detail {
inline bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
inline std::size_t mouseBytes(std::uint8_t kind) {
    switch (kind) {
    case input_wire::kMoveKind: return input_wire::kMoveRecordBytes;
    case input_wire::kButtonsKind: return input_wire::kButtonsRecordBytes;
    case input_wire::kWheelKind: return input_wire::kWheelRecordBytes;
    case input_wire::kCancelKind: return input_wire::kCancelRecordBytes;
    case input_wire::kModifiersKind: return input_wire::kModifiersRecordBytes;
    default: return 0;
    }
}
inline std::int32_t signed32(std::uint32_t value) {
    return value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int32_t>(value)
        : static_cast<std::int32_t>(static_cast<std::int64_t>(value) - 0x100000000LL);
}
}

// Call only on reassembled STREAM.data. Frame/CRC/session gating stays outside.
// Failure leaves output unchanged; no host/native side effects occur here.
inline bool decodeInputSample(const Bytes& bytes, InputSample& output,
                              const InputLimits& limits = {}, std::string* error = nullptr) {
    if (bytes.size() < 2 || bytes.size() > std::min(limits.maxDataBytes, input_wire::kMaxDataBytes))
        return detail::fail(error, "input data length/budget");
    InputSample candidate;
    ByteReader reader(bytes);
    const auto type = reader.readU8().value;
    if (type == input_wire::kTypeTouch) {
        if (!limits.allowTouch || reader.remaining() % input_wire::kTouchRecordBytes != 0)
            return detail::fail(error, "touch type/length");
        const auto count = reader.remaining() / input_wire::kTouchRecordBytes;
        if (!count || count > std::min(limits.maxContacts, input_wire::kMaxContacts))
            return detail::fail(error, "touch count limit");
        candidate.contacts.reserve(count);
        while (!reader.empty()) {
            TouchContact r;
            r.contactId = reader.readU16().value;
            r.phase = reader.readU8().value;
            r.x = reader.readU16().value;
            r.y = reader.readU16().value;
            if (r.phase > input_wire::kPhaseCancel) return detail::fail(error, "touch phase");
            candidate.contacts.push_back(r);
        }
    } else if (type == input_wire::kTypeMouse) {
        if (!limits.allowMouse) return detail::fail(error, "unaccepted Mouse type");
        candidate.type = InputType::Mouse;
        auto& m = candidate.mouse;
        m.eventKind = reader.readU8().value;
        const auto size = detail::mouseBytes(m.eventKind);
        if (!size || bytes.size() != input_wire::kTypeBytes + size)
            return detail::fail(error, "mouse exact length/kind");
        if (m.eventKind != input_wire::kCancelKind) {
            m.modifiers = reader.readU16().value;
            if (m.modifiers & ~std::uint16_t(15) || m.modifiers & ~limits.modifierMask)
                return detail::fail(error, "modifier bits/capability");
            if (m.eventKind != input_wire::kModifiersKind) {
                m.buttons = reader.readU32().value;
                m.x = reader.readU16().value;
                m.y = reader.readU16().value;
                if (m.buttons & ~std::uint32_t(31) || m.buttons & ~limits.buttonsMask)
                    return detail::fail(error, "button bits/capability");
                if (!limits.allowChords && (m.buttons & (m.buttons - 1)))
                    return detail::fail(error, "button chord unsupported");
            }
        }
        if (m.eventKind == input_wire::kWheelKind) {
            m.wheelUnit = reader.readU8().value;
            m.wheelPhase = reader.readU8().value;
            m.wheelX = detail::signed32(reader.readU32().value);
            m.wheelY = detail::signed32(reader.readU32().value);
            if (m.wheelUnit > input_wire::kWheelUnitRegionQ1616 || m.wheelPhase > input_wire::kWheelPhaseCancelled)
                return detail::fail(error, "wheel unit/phase");
            if ((m.wheelUnit == input_wire::kWheelUnitDetent120 && !limits.allowDetentWheel) ||
                (m.wheelUnit == input_wire::kWheelUnitRegionQ1616 && !limits.allowContinuousWheel))
                return detail::fail(error, "wheel capability");
        }
    } else {
        return detail::fail(error, "unknown input type");
    }
    output = std::move(candidate);
    if (error) error->clear();
    return true;
}

inline bool encodeInputSample(const InputSample& sample, Bytes& output,
                              const InputLimits& limits = {}, std::string* error = nullptr) {
    ByteWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(sample.type));
    if (sample.type == InputType::Touch) {
        if (sample.contacts.empty() || sample.contacts.size() > input_wire::kMaxContacts)
            return detail::fail(error, "touch count limit");
        for (const auto& r : sample.contacts) {
            writer.writeU16(r.contactId); writer.writeU8(r.phase);
            writer.writeU16(r.x); writer.writeU16(r.y);
        }
    } else if (sample.type == InputType::Mouse) {
        const auto& m = sample.mouse;
        writer.writeU8(m.eventKind);
        if (m.eventKind != input_wire::kCancelKind) {
            writer.writeU16(m.modifiers);
            if (m.eventKind != input_wire::kModifiersKind) {
                writer.writeU32(m.buttons); writer.writeU16(m.x); writer.writeU16(m.y);
            }
        }
        if (m.eventKind == input_wire::kWheelKind) {
            writer.writeU8(m.wheelUnit); writer.writeU8(m.wheelPhase);
            writer.writeU32(static_cast<std::uint32_t>(m.wheelX));
            writer.writeU32(static_cast<std::uint32_t>(m.wheelY));
        }
    }
    auto bytes = writer.takeBytes();
    InputSample checked;
    if (!decodeInputSample(bytes, checked, limits, error)) return false;
    output = std::move(bytes);
    return true;
}

// Produce a candidate state. The caller commits it only at its chosen admission
// boundary and separately accounts for actual native submission/cleanup.
inline bool validateTransition(const InputSample& sample, const InputState& previous,
                               InputState& output, const InputLimits& limits = {},
                               std::string* error = nullptr) {
    Bytes checked;
    if (!encodeInputSample(sample, checked, limits, error)) return false;
    auto next = previous;
    if (sample.type == InputType::Touch) {
        std::set<std::uint16_t> ids;
        for (const auto& r : sample.contacts)
            if (!ids.insert(r.contactId).second) return detail::fail(error, "duplicate touch contact");
        for (const auto id : previous.contacts)
            if (!ids.count(id)) return detail::fail(error, "missing active touch contact");
        for (const auto& r : sample.contacts) {
            if (r.phase == input_wire::kPhaseDown) {
                if (previous.contacts.count(r.contactId)) return detail::fail(error, "Down on active contact");
                next.contacts.insert(r.contactId);
            } else {
                if (!previous.contacts.count(r.contactId)) return detail::fail(error, "orphan touch contact");
                if (r.phase == input_wire::kPhaseUp || r.phase == input_wire::kPhaseCancel)
                    next.contacts.erase(r.contactId);
            }
        }
    } else {
        const auto& m = sample.mouse;
        if (m.eventKind == input_wire::kCancelKind) {
            next.buttons = 0; next.modifiers = 0; next.scrollActive = false;
        } else {
            next.modifiers = m.modifiers;
            if (m.eventKind != input_wire::kModifiersKind) {
                if ((m.eventKind == input_wire::kButtonsKind) == (m.buttons == previous.buttons))
                    return detail::fail(error, "implicit or missing button transition");
                next.buttons = m.buttons;
            }
            if (m.eventKind == input_wire::kWheelKind) {
                if (m.wheelUnit == input_wire::kWheelUnitDetent120) {
                    if (m.wheelPhase != input_wire::kWheelPhaseNone || (!m.wheelX && !m.wheelY) || previous.scrollActive)
                        return detail::fail(error, "invalid detent/overlapping scroll");
                } else if (m.wheelPhase == input_wire::kWheelPhaseBegan) {
                    if (previous.scrollActive) return detail::fail(error, "overlapping scroll");
                    next.scrollActive = true; next.scrollX = m.x; next.scrollY = m.y;
                } else {
                    if (!previous.scrollActive || m.x != previous.scrollX || m.y != previous.scrollY || !m.wheelPhase)
                        return detail::fail(error, "scroll without Began/anchor mismatch");
                    if (m.wheelPhase == input_wire::kWheelPhaseEnded || m.wheelPhase == input_wire::kWheelPhaseCancelled) {
                        if (m.wheelX || m.wheelY) return detail::fail(error, "scroll terminal delta");
                        next.scrollActive = false;
                    }
                }
            }
        }
    }
    output = std::move(next);
    if (error) error->clear();
    return true;
}
} // namespace axtp::input
