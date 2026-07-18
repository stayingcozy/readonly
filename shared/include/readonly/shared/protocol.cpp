#pragma once
#include <cstdint>

namespace readonly::shared {

    inline constexpr std::uint8_t  kProtocolVersion = 1;
    inline constexpr std::uint32_t kVsockPort       = 5555; // supervisor listen port

    // Wire frame: [type:u8][len:u32 little-endian][payload:len bytes]
    enum class FrameType : std::uint8_t {
        Run   = 1, // host->guest : payload = command string, exec'd under a PTY
        Winsz = 2, // host->guest : payload = WinSize (rows,cols)
        Data  = 3, // bidirectional : raw bytes (stdin out / PTY output back)
        Exit  = 4, // guest->host : payload = int32 exit code
    };

    struct WinSize {
        std::uint16_t rows;
        std::uint16_t cols;
    };

    // Serialization is core concern (manual, no struct packing over the wire)
    // This header only fixes the vocab + const both sides share

} // namespace readonly::shared