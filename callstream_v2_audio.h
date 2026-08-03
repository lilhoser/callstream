#ifndef CALLSTREAM_V2_AUDIO_H
#define CALLSTREAM_V2_AUDIO_H

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace callstream_v2 {

// Decoder shutdown can produce a very short source-less WAV containing only a
// handful of quantization-level values. Treat audio as non-empty as soon as any
// sample rises above that observed decoder floor. This intentionally favors
// retention: a click, weak voice, or otherwise questionable signal is kept.
inline bool contains_audio_above_decoder_floor(const std::vector<int16_t>& samples) {
  constexpr int32_t decoder_floor_peak = 32;
  for (int16_t sample : samples) {
    const int32_t value = static_cast<int32_t>(sample);
    const int32_t magnitude = value < 0 ? -value : value;
    if (magnitude > decoder_floor_peak) return true;
  }
  return false;
}

inline bool should_retain_transmission(long source_id, const std::vector<int16_t>& samples) {
  return source_id > 0 || contains_audio_above_decoder_floor(samples);
}

inline uint16_t read_le16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) |
         (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

inline uint32_t read_le32(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

inline bool read_pcm16_mono_wav(const std::string& filename, std::vector<int16_t>& samples,
                                unsigned int& sample_rate) {
  std::ifstream input(filename, std::ios::binary | std::ios::ate);
  if (!input) return false;

  const std::streamsize file_size = input.tellg();
  if (file_size < 44 || file_size > static_cast<std::streamsize>(std::numeric_limits<int>::max())) return false;
  input.seekg(0, std::ios::beg);

  std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
  if (!input.read(reinterpret_cast<char*>(bytes.data()), file_size)) return false;
  if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;

  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint16_t bits_per_sample = 0;
  uint32_t parsed_sample_rate = 0;
  size_t data_offset = 0;
  size_t data_size = 0;

  size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    const uint32_t chunk_size = read_le32(bytes, offset + 4);
    const size_t chunk_data = offset + 8;
    if (chunk_data > bytes.size() || chunk_size > bytes.size() - chunk_data) return false;

    if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunk_size >= 16) {
      audio_format = read_le16(bytes, chunk_data);
      channels = read_le16(bytes, chunk_data + 2);
      parsed_sample_rate = read_le32(bytes, chunk_data + 4);
      bits_per_sample = read_le16(bytes, chunk_data + 14);
    } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
      data_offset = chunk_data;
      data_size = chunk_size;
    }

    offset = chunk_data + chunk_size + (chunk_size & 1U);
  }

  if (audio_format != 1 || channels != 1 || bits_per_sample != 16 || parsed_sample_rate == 0 ||
      data_offset == 0 || data_size == 0 || (data_size & 1U) != 0) return false;

  samples.clear();
  samples.reserve(data_size / 2);
  for (size_t i = data_offset; i < data_offset + data_size; i += 2) {
    samples.push_back(static_cast<int16_t>(read_le16(bytes, i)));
  }
  sample_rate = parsed_sample_rate;
  return true;
}

} // namespace callstream_v2

#endif
