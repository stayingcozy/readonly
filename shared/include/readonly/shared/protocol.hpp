#pragma once

/* Raw wire constants - src of truth, C(guest) mirrors theses */
#define RO_PROTOCOL_VERSION 1u
#define RO_VSOCK_PORT 5555u
#define RO_FRAME_RUN 1u   /* host->guest: payload = command, run under a PTY */
#define RO_FRAME_WINSZ 2u /* host->guest: payload = rows:u16le, cols:u16le */
#define RO_FRAME_DATA 3u  /* bidirectional: raw bytes (stdin / PTY output)   */
#define RO_FRAME_EXIT 4u  /* guest->host: payload = int32le exit code        */

#ifdef __cplusplus
#include <cstdint>
namespace readonly::shared {

inline constexpr std::uint8_t kProtocolVersion = RO_PROTOCOL_VERSION;
inline constexpr std::uint32_t kVsockPort = RO_VSOCK_PORT;

// Wire frame: [type:u8][len:u32 little-endian][payload:len bytes]
enum class FrameType : std::uint8_t {
  Run = RO_FRAME_RUN,
  Winsz = RO_FRAME_WINSZ,
  Data = RO_FRAME_DATA,
  Exit = RO_FRAME_EXIT,
};

struct WinSize {
  std::uint16_t rows;
  std::uint16_t cols;
};

} // namespace readonly::shared
#endif