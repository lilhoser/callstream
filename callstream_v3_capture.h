#pragma once

#include <cstddef>
#include <cstdint>

namespace callstream_v3 {

inline const char* channel_assignment_start(bool was_update) {
  return was_update ? "update" : "grant";
}

inline const char* transmission_start_status(
    bool was_update,
    std::int64_t possibly_incomplete_start_time_ms,
    std::int64_t transmission_start_time_ms) {
  return was_update && possibly_incomplete_start_time_ms > 0 &&
             transmission_start_time_ms == possibly_incomplete_start_time_ms
      ? "possibly_incomplete"
      : "observed_boundary";
}

}  // namespace callstream_v3
