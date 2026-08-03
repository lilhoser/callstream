#pragma once

namespace callstream_v3 {

inline const char* channel_assignment_start(bool started_from_update) {
  return started_from_update ? "update" : "grant";
}

inline const char* transmission_start_status(
    bool started_from_update,
    bool is_first_transmission) {
  return started_from_update && is_first_transmission
      ? "possibly_incomplete"
      : "observed_boundary";
}

}  // namespace callstream_v3
