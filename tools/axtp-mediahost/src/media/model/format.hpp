#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace axtp::mediahost {

inline std::string toHexU32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

} // namespace axtp::mediahost
