#include "../callstream_v2_audio.h"
#include "../callstream_v3_capture.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static void write_le16(std::ofstream& output, uint16_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
}

static void write_le32(std::ofstream& output, uint32_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
  output.put(static_cast<char>((value >> 16) & 0xff));
  output.put(static_cast<char>((value >> 24) & 0xff));
}

static void write_wav(const std::string& path, uint16_t channels, const std::vector<int16_t>& samples) {
  std::ofstream output(path, std::ios::binary);
  const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
  output.write("RIFF", 4);
  write_le32(output, 36 + data_size);
  output.write("WAVEfmt ", 8);
  write_le32(output, 16);
  write_le16(output, 1);
  write_le16(output, channels);
  write_le32(output, 8000);
  write_le32(output, 8000 * channels * 2);
  write_le16(output, channels * 2);
  write_le16(output, 16);
  output.write("data", 4);
  write_le32(output, data_size);
  for (int16_t sample : samples) write_le16(output, static_cast<uint16_t>(sample));
}

int main() {
  const std::string valid_path = "callstream-v2-valid.wav";
  const std::string stereo_path = "callstream-v2-stereo.wav";
  const std::string invalid_path = "callstream-v2-invalid.wav";

  write_wav(valid_path, 1, {1, -2, 32767, -32768});
  write_wav(stereo_path, 2, {1, 2, 3, 4});
  {
    std::ofstream invalid(invalid_path, std::ios::binary);
    invalid.write("not a wav", 9);
  }

  std::vector<int16_t> samples;
  unsigned int sample_rate = 0;
  assert(callstream_v2::read_pcm16_mono_wav(valid_path, samples, sample_rate));
  assert(sample_rate == 8000);
  assert((samples == std::vector<int16_t>{1, -2, 32767, -32768}));
  assert(!callstream_v2::read_pcm16_mono_wav(stereo_path, samples, sample_rate));
  assert(!callstream_v2::read_pcm16_mono_wav(invalid_path, samples, sample_rate));

  const std::vector<int16_t> decoder_floor = {0, 7, -13, 15, -4};
  const std::vector<int16_t> weak_but_real_audio = {0, 8, -33, 9};
  assert(!callstream_v2::contains_audio_above_decoder_floor(decoder_floor));
  assert(callstream_v2::contains_audio_above_decoder_floor(weak_but_real_audio));
  assert(!callstream_v2::should_retain_transmission(-1, decoder_floor));
  assert(callstream_v2::should_retain_transmission(-1, weak_but_real_audio));
  assert(callstream_v2::should_retain_transmission(1002081, decoder_floor));

  assert(std::string(callstream_v3::channel_assignment_start(false)) == "grant");
  assert(std::string(callstream_v3::channel_assignment_start(true)) == "update");
  assert(std::string(callstream_v3::transmission_start_status(true, true)) == "possibly_incomplete");
  assert(std::string(callstream_v3::transmission_start_status(true, false)) == "observed_boundary");
  assert(std::string(callstream_v3::transmission_start_status(false, true)) == "observed_boundary");

  std::remove(valid_path.c_str());
  std::remove(stereo_path.c_str());
  std::remove(invalid_path.c_str());
  return 0;
}
