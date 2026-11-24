#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "core/protocol/generated/axtp_input_stream_generated.h"

namespace axtp::input {

enum class InputType : std::uint8_t {
    Touch = input_wire::kTypeTouch,
    Mouse = input_wire::kTypeMouse
};

struct TouchContact {
    std::uint16_t contactId = 0;
    std::uint8_t phase = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
};

struct MouseEvent {
    std::uint8_t eventKind = 0;
    std::uint16_t modifiers = 0;
    std::uint32_t buttons = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t wheelUnit = 0;
    std::uint8_t wheelPhase = 0;
    std::int32_t wheelX = 0;
    std::int32_t wheelY = 0;
};

struct InputSample {
    InputType type = InputType::Touch;
    std::vector<TouchContact> contacts;
    MouseEvent mouse;
};

// Limits are supplied by the successfully opened immutable Stream Context.
struct InputLimits {
    std::size_t maxDataBytes = input_wire::kMaxDataBytes;
    std::size_t maxContacts = input_wire::kMaxContacts;
    bool allowTouch = true;
    bool allowMouse = true;
    std::uint32_t buttonsMask = 31;
    std::uint16_t modifierMask = 15;
    bool allowChords = true;
    bool allowDetentWheel = true;
    bool allowContinuousWheel = true;
};

// Logical validation state only. Native submitted ownership belongs to the host.
struct InputState {
    std::set<std::uint16_t> contacts;
    std::uint32_t buttons = 0;
    std::uint16_t modifiers = 0;
    bool scrollActive = false;
    std::uint16_t scrollX = 0;
    std::uint16_t scrollY = 0;
};

} // namespace axtp::input
