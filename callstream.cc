#include "../../trunk-recorder/call_concluder/call_concluder.h"
#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../../trunk-recorder/recorders/recorder.h"
#include <boost/dll/alias.hpp> // for BOOST_DLL_ALIAS
#include <boost/foreach.hpp>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/unordered_map.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/thread/locks.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <curl/curl.h>
#include <chrono>
#include <cmath>
#include <deque>

using namespace boost::asio;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

enum callstream_err {
  e_success = 0,
  e_call_already_exists,
  e_call_doesnt_exist,
  e_insertion_failure,
  e_max = 0xffffffff
};

// Audio filtering configuration
struct audio_filter_config_t {
  bool enabled = true;

  // Spike clipping configuration
  struct {
    bool enabled = true;
    int threshold_percent = 85;  // Percentage of INT16_MAX (32767)
    float clip_factor = 0.9f;    // How much to reduce spikes (0.5-1.0)
    int16_t threshold = 27852;   // Calculated threshold value (85% of 32767)
  } spike_clipping;

  // Smoothing configuration
  struct {
    bool enabled = false;
    int window_size = 5;         // Number of samples for moving average
  } smoothing;

  // High-pass filter configuration
  struct {
    bool enabled = false;
    int cutoff_hz = 200;         // Cutoff frequency in Hz
    float alpha = 0.0f;          // Filter coefficient (calculated from cutoff)
  } high_pass_filter;
};

// Parse audio filtering configuration from JSON
audio_filter_config_t parse_audio_filter_config(json config_data) {
  audio_filter_config_t cfg;

  if (!config_data.contains("audio_filtering")) {
    return cfg;
  }

  auto& af = config_data["audio_filtering"];

  // Main enabled flag
  cfg.enabled = af.value("enabled", true);

  // Spike clipping
  if (af.contains("spike_clipping")) {
    auto& sc = af["spike_clipping"];
    cfg.spike_clipping.enabled = sc.value("enabled", true);
    cfg.spike_clipping.threshold_percent = sc.value("threshold_percent", 85);
    cfg.spike_clipping.clip_factor = sc.value("clip_factor", 0.9f);
    // Calculate threshold value from percentage (INT16_MAX = 32767)
    cfg.spike_clipping.threshold = static_cast<int16_t>(32767 * cfg.spike_clipping.threshold_percent / 100.0f);
  } else {
    // Default threshold calculation
    cfg.spike_clipping.threshold = static_cast<int16_t>(32767 * cfg.spike_clipping.threshold_percent / 100.0f);
  }

  // Smoothing
  if (af.contains("smoothing")) {
    auto& sm = af["smoothing"];
    cfg.smoothing.enabled = sm.value("enabled", false);
    cfg.smoothing.window_size = sm.value("window_size", 5);
  }

  // High-pass filter
  if (af.contains("high_pass_filter")) {
    auto& hp = af["high_pass_filter"];
    cfg.high_pass_filter.enabled = hp.value("enabled", false);
    cfg.high_pass_filter.cutoff_hz = hp.value("cutoff_hz", 200);
  }

  return cfg;
}

class calldata_t {
public:
  calldata_t() {
    mutex = std::make_shared<boost::shared_mutex>();
    magic = 0;
    json_length = 0;
    sample_count = 0;
    // Filter state initialization
    hp_state = 0.0f;
    prev_sample = 0;
    hp_alpha = 0.0f;
    smoothing_buffer.clear();
  }
  int magic;
  size_t json_length;
  int sample_count;
  std::string json_string;
  std::shared_ptr<boost::shared_mutex> mutex; // protects samples
  std::vector<int16_t> samples;

  // Audio filtering state
  std::deque<int16_t> smoothing_buffer;
  int smoothing_window = 5;
  float hp_state;      // High-pass filter state (y[n-1])
  int16_t prev_sample; // Previous sample for high-pass filter (x[n-1])
  float hp_alpha;      // High-pass filter coefficient
};

class sftp_buffer_info_t {
  public:
    const char* pointer;
    size_t size_left;
};

class sftp_client_info_t {
  public:
    std::string server_address;
    std::string user;
    std::string password;
    std::string dest;
    bool enabled;
    bool verbose;

    sftp_client_info_t() {
      enabled = false;
      verbose = false;
    }
};

class callstream_t { 
  public:

    callstream_t() {
      mutex = std::make_shared<boost::shared_mutex>();
    }

    unsigned long TGID;
    std::string short_name;
    std::shared_ptr<boost::shared_mutex> mutex; // protects call_data
    boost::unordered_map<std::string, std::shared_ptr<calldata_t>> call_data;
};

std::vector<std::shared_ptr<callstream_t>> g_callstreams;
asio::io_context g_context;
asio::io_service::work g_work(g_context);
std::thread g_WorkerThread;
audio_filter_config_t g_audio_filter_config;  // Global audio filter configuration

class Call_Stream : public Plugin_Api {

  public:
  
  Call_Stream() {
    num_clients = 0;
  }

  int parse_config(json config_data) override {
    if (!config_data.contains("clients")) {
      BOOST_LOG_TRIVIAL(info) << "Invalid plugin configuration: clients block is required";
      return 1;
    }
    if (!config_data.contains("streams")) {
      BOOST_LOG_TRIVIAL(info) << "Invalid plugin configuration: at least one stream is required";
      return 1;
    }
    if (config_data.contains("sftp_info")) {
      auto info = config_data["sftp_info"];
      if (!info.contains("server_address") || !info.contains("user") ||
          !info.contains("password") || !info.contains("dest")) {
            BOOST_LOG_TRIVIAL(info) << "Invalid plugin configuration: invalid SFTP info.";
            return 1;
      }
      if (info.contains("verbose")) {
        sftp_client_info.verbose = info.value("verbose", false);
      }
      sftp_client_info.server_address = info["server_address"];
      sftp_client_info.user = info["user"];
      sftp_client_info.password = info["password"];
      sftp_client_info.dest = info["dest"];
      sftp_client_info.enabled = true;
    }
    for (json client : config_data["clients"]) {
      if (++num_clients > 6)
      {
        BOOST_LOG_TRIVIAL(info) << "Invalid plugin configuration: max 6 clients.";
        return 1;
      }
      if (!client.contains("address") || !client.contains("port")) {
        BOOST_LOG_TRIVIAL(info) << "Invalid plugin configuration: client object must have address and port.";
        return 1;
      }
      clients[num_clients-1].address(asio::ip::address::from_string(client["address"]));
      clients[num_clients-1].port(client["port"]);
      BOOST_LOG_TRIVIAL(info) << "streaming to client " << clients[num_clients-1].address() <<":" << clients[num_clients-1].port();
    }
    for (json element : config_data["streams"]) {
      auto callstream = std::make_shared<callstream_t>();
      callstream->TGID = element["TGID"];
      callstream->short_name = element.value("shortName", "");
      BOOST_LOG_TRIVIAL(info) << "streaming from TGID " << callstream->TGID << " on System " << callstream->short_name;
      g_callstreams.push_back(callstream);
    }

    // Parse audio filtering configuration
    g_audio_filter_config = parse_audio_filter_config(config_data);

    // Log audio filter configuration
    BOOST_LOG_TRIVIAL(info) << "libcallstream: audio filtering " << (g_audio_filter_config.enabled ? "enabled" : "disabled");
    if (g_audio_filter_config.enabled) {
      BOOST_LOG_TRIVIAL(info) << "libcallstream:   spike_clipping: "
        << (g_audio_filter_config.spike_clipping.enabled ? "enabled" : "disabled")
        << " (threshold=" << g_audio_filter_config.spike_clipping.threshold_percent << "%, factor="
        << g_audio_filter_config.spike_clipping.clip_factor << ")";
      BOOST_LOG_TRIVIAL(info) << "libcallstream:   smoothing: "
        << (g_audio_filter_config.smoothing.enabled ? "enabled" : "disabled")
        << " (window=" << g_audio_filter_config.smoothing.window_size << ")";
      BOOST_LOG_TRIVIAL(info) << "libcallstream:   high_pass_filter: "
        << (g_audio_filter_config.high_pass_filter.enabled ? "enabled" : "disabled")
        << " (cutoff=" << g_audio_filter_config.high_pass_filter.cutoff_hz << "Hz)";
    }

    // Calculate high-pass filter coefficient if enabled
    // Sample rate is 8000 Hz for P25 digital audio
    const float sample_rate = 8000.0f;
    if (g_audio_filter_config.high_pass_filter.enabled) {
      float fc = static_cast<float>(g_audio_filter_config.high_pass_filter.cutoff_hz);
      // First-order high-pass filter coefficient: alpha = (fc / sr) / (1 + fc / sr)
      g_audio_filter_config.high_pass_filter.alpha = (fc / sample_rate) / (1.0f + fc / sample_rate);
      BOOST_LOG_TRIVIAL(debug) << "libcallstream: calculated high-pass filter alpha="
        << g_audio_filter_config.high_pass_filter.alpha;
    }

    return 0;
  }

  int call_start(Call *call) override {
    auto callstream = find_callstream(call);
    if (callstream == nullptr) { // not interested in this call's system/TG
      return 0;
    }
    auto unique_id = make_unique_id(call->get_call_num());
    if (add_call(callstream, unique_id) == e_call_already_exists) {
      // this callback can be invoked multiple times for a "call" (eg, in the case of a grant)
      BOOST_LOG_TRIVIAL(debug) << "libcallstream: call_start: call id " << unique_id << " already exists in callstream list.";
      return 0;
    }
    BOOST_LOG_TRIVIAL(debug) << "libcallstream: call_start: inserted callstream for call id " << unique_id;
    return 0;
  }

  int call_end(Call_Data_t call_info) override {
    auto callstream = find_callstream(call_info);
    if (callstream == nullptr) { // not interested in this call's system/TG
      return 0;
    }
    auto unique_id = make_unique_id(call_info.call_num);
    auto call_data = get_call_data(callstream, unique_id);
    if (call_data == nullptr) { // this is unexpected
      BOOST_LOG_TRIVIAL(error) << "libcallstream: call_end: call id " << unique_id << " not found.";
      return 0;
    }
    if (call_data->sample_count == 0 || call_data->samples.size() == 0) {
      BOOST_LOG_TRIVIAL(warning) << "libcallstream: call_end callstream with call id " << unique_id << " has no samples.";
      destroy_call(callstream, unique_id);
      return 0;
    }
    json json_object = {
        {"Source", call_info.sys_num},
        {"Talkgroup", call_info.talkgroup},
        {"PatchedTalkgroups",call_info.patched_talkgroups},
        {"Frequency", call_info.freq},
        {"SystemShortName", call_info.short_name},
        {"CallId", call_info.call_num},
        {"StartTime", call_info.start_time},
        {"StopTime", call_info.stop_time},
    };
    call_data->magic = 0x415A5A50; // 'pzza'
    call_data->json_string = json_object.dump();
    call_data->json_length = call_data->json_string.length();
    send(callstream, call_data, unique_id);
    return 0;
  }

  int audio_stream(Call *call, Recorder *recorder, int16_t *samples, int sampleCount) override {
    auto callstream = find_callstream(call);
    if (callstream == nullptr) { // not interested in this call's system/TG
      return 0;
    }
    auto unique_id = make_unique_id(call->get_call_num());
    auto call_data = get_call_data(callstream, unique_id);
    if (call_data == nullptr) { // this is unexpected
      BOOST_LOG_TRIVIAL(error) << "libcallstream: audio_stream: call id " << unique_id << " not found.";
      return 0;
    }
    add_call_samples(call_data, samples, sampleCount);
    BOOST_LOG_TRIVIAL(debug) << "libcallstream: audio_stream: inserted " << sampleCount << " samples for call id " << unique_id;
    return 0;
  }
  
  int start() override {
    BOOST_LOG_TRIVIAL(info) << "libcallstream: starting plugin...";
    // launch an I/O worker thread to handle socket operations
    g_WorkerThread = std::thread([this](){ g_context.run(); });
    BOOST_LOG_TRIVIAL(info) << "libcallstream: successfully started.";
    return 0;
  }
  
  int stop() override {
    BOOST_LOG_TRIVIAL(info) << "libcallstream: stopping plugin...";
    g_context.stop(); // stop I/O work
    g_WorkerThread.join();
    BOOST_LOG_TRIVIAL(info) << "libcallstream: successfully stopped.";
    return 0;
  }

  static boost::shared_ptr<Call_Stream> create() {
    return boost::shared_ptr<Call_Stream>(
        new Call_Stream());
  }

private:

  inline std::string make_unique_id(long call_id) {
    // NB: TR engine should really provide this to us as UUID so plugins arent all doing different IDs.
    return boost::str(boost::format("%1%") % call_id);
  }

  callstream_t* find_callstream(Call_Data_t call_info) {
    auto short_name = call_info.short_name;
    auto patched_talkgroups = call_info.patched_talkgroups;
    if (patched_talkgroups.size() == 0){
      patched_talkgroups.push_back(call_info.talkgroup);
    }
    return find_callstream_internal(short_name, patched_talkgroups);
  }

  callstream_t* find_callstream(Call *call) {
    auto short_name = call->get_system()->get_short_name();
    auto patched_talkgroups = call->get_system()->get_talkgroup_patch(call->get_talkgroup());
    if (patched_talkgroups.size() == 0){
      patched_talkgroups.push_back(call->get_talkgroup());
    }
    return find_callstream_internal(short_name, patched_talkgroups);
  }

  callstream_t* find_callstream_internal(std::string short_name, std::vector<unsigned long> patched_talkgroups) {
    BOOST_FOREACH (auto& callstream, g_callstreams){
      if (0==callstream->short_name.compare(short_name) || (0==callstream->short_name.compare(""))){
        BOOST_FOREACH (auto& TGID, patched_talkgroups){
          if ((TGID==callstream->TGID || callstream->TGID==0)){  //setting TGID to 0 in the config file will stream everything
            return callstream.get();
          }
        }
      }
    }
    return nullptr;
  }

  callstream_err add_call(callstream_t* callstream, std::string unique_id) {
    auto mut = callstream->mutex;
    boost::lock_guard<boost::shared_mutex> lock2(*mut); // we need exclusive access while modifying the vector
    auto match = callstream->call_data.find(unique_id);
    if (match != callstream->call_data.end()) {
      return e_call_already_exists;
    }
    auto [_, success] = callstream->call_data.try_emplace(unique_id, std::make_shared<calldata_t>());
    return success ? e_success : e_insertion_failure;
  }

  calldata_t* get_call_data(callstream_t* callstream, std::string unique_id) {
    auto mut = callstream->mutex;
    boost::shared_lock<boost::shared_mutex> lock(*mut); // we need shared access while reading the vector
    auto match = callstream->call_data.find(unique_id);
    if (match == callstream->call_data.end()) {
      return nullptr;
    }
    return match->second.get();
  }

  void add_call_samples(calldata_t* call_data, int16_t *samples, int sample_count) {
    auto mut = call_data->mutex;
    boost::lock_guard<boost::shared_mutex> lock(*mut); // we need exclusive access while updating
    call_data->sample_count += sample_count;

    // Initialize filter parameters from global config
    call_data->smoothing_window = g_audio_filter_config.smoothing.window_size;
    call_data->hp_alpha = g_audio_filter_config.high_pass_filter.alpha;

    for (int i = 0; i < sample_count; i++) {
      int16_t sample = samples[i];
      int16_t original_sample = sample;  // Keep original for all filters

      if (g_audio_filter_config.enabled) {
        // Strategy 3: First-order High-Pass Filter (applied first on original signal)
        if (g_audio_filter_config.high_pass_filter.enabled) {
          float sample_f = static_cast<float>(original_sample);
          // y[n] = alpha * (y[n-1] + x[n] - x[n-1])
          float filtered = call_data->hp_alpha * (call_data->hp_state + sample_f - static_cast<float>(call_data->prev_sample));
          call_data->prev_sample = original_sample;
          call_data->hp_state = filtered;
          sample = static_cast<int16_t>(filtered);
        }

        // Strategy 1: Spike Detection and Soft Clipping (applied after high-pass)
        if (g_audio_filter_config.spike_clipping.enabled) {
          int32_t abs_sample = std::abs(static_cast<int32_t>(sample));
          if (abs_sample > g_audio_filter_config.spike_clipping.threshold) {
            // Soft clip: reduce amplitude proportionally
            float scaled = static_cast<float>(sample) * g_audio_filter_config.spike_clipping.clip_factor;
            sample = static_cast<int16_t>(scaled);
          }
        }

        // Strategy 2: Moving Average Smoothing (applied last for final smoothing)
        if (g_audio_filter_config.smoothing.enabled) {
          call_data->smoothing_buffer.push_back(sample);
          if (static_cast<int>(call_data->smoothing_buffer.size()) > call_data->smoothing_window) {
            call_data->smoothing_buffer.pop_front();
          }

          // Calculate moving average
          int32_t sum = 0;
          for (auto s : call_data->smoothing_buffer) {
            sum += s;
          }
          sample = static_cast<int16_t>(sum / call_data->smoothing_buffer.size());
        }
      }

      call_data->samples.push_back(sample);
    }
  }

  void destroy_call(callstream_t* callstream, std::string unique_id) {
    auto mut = callstream->mutex;
    boost::lock_guard<boost::shared_mutex> lock(*mut); // we need exclusive access while modifying the vector
    callstream->call_data.erase(unique_id);
  }

  void send(callstream_t* callstream, calldata_t* call_data, std::string unique_id) {
    //
    // Optionally send to an sftp server (sync)
    //
    if (sftp_client_info.enabled) {
      send_sftp(call_data);
    }

    for (int i = 0; i < num_clients; i++) {
      send_to_client(clients[i], callstream, call_data, unique_id);
    }
    
    destroy_call(callstream, unique_id);
  }

  void send_to_client(tcp::endpoint& client, callstream_t* callstream, calldata_t* call_data, std::string unique_id) {
    auto address = client.address().to_string();
    try {
      auto sock = std::make_unique<tcp::socket>(g_context); // run on I/O worker thread we created
      sock->open(tcp::v4());
      sock->set_option(boost::asio::socket_base::keep_alive(true));
      std::chrono::milliseconds span(1000);
      std::future<void> status = sock->async_connect(client, asio::use_future);
      if (status.wait_for(span) == std::future_status::timeout) {
        BOOST_LOG_TRIVIAL(error) << "libcallstream: client " << address << " unavailable, ignoring";
        destroy_call(callstream, unique_id);
        return;
      }
      std::vector<boost::asio::const_buffer> send_buffers; // order matters!
      send_buffers.push_back(buffer(&call_data->magic, sizeof(call_data->magic)));
      send_buffers.push_back(buffer(&call_data->json_length, sizeof(call_data->json_length)));
      send_buffers.push_back(buffer(&call_data->sample_count, sizeof(call_data->sample_count)));
      send_buffers.push_back(buffer(call_data->json_string));
      send_buffers.push_back(buffer(call_data->samples.data(), call_data->sample_count * sizeof(int16_t)));
      auto result = async_write(*sock, send_buffers, asio::use_future);
      auto bytesSent = result.get(); // block on I/O thread
      size_t total_size = 0;
      BOOST_FOREACH(auto& entry, send_buffers) {
        total_size += entry.size();
      }
      if (bytesSent != total_size) {
        BOOST_LOG_TRIVIAL(error) << "libcallstream: only sent " << bytesSent << " of " << total_size << " to client " << address;
      }
      else {
        BOOST_LOG_TRIVIAL(info) << "libcallstream: sent call data (" << total_size << " bytes) to client " << address;
      }
    }
    catch(const boost::system::system_error& ex) {
      if ((boost::asio::error::eof == ex.code()) ||
          (boost::asio::error::connection_reset == ex.code()) ||
          (boost::asio::error::broken_pipe == ex.code())) {
        BOOST_LOG_TRIVIAL(warning) << "libcallstream: socket send failed, client " << address << " has disconnected";
      }
      else {
        BOOST_LOG_TRIVIAL(error) << "libcallstream: socket send failed for client " << address << ": " << ex.what();
      }
    }
  }

  void send_sftp(calldata_t* call_data) {
    //
    // Init CURL
    //
    CURL* curl = curl_easy_init();
    if(!curl) {
      BOOST_LOG_TRIVIAL(error) << "libcallstream: CURL object null";
      return;
    }

    //
    // Create a flattened buffer
    //
    std::vector<boost::asio::const_buffer> send_buffers; // order matters!
    send_buffers.push_back(buffer(&call_data->magic, sizeof(call_data->magic)));
    send_buffers.push_back(buffer(&call_data->json_length, sizeof(call_data->json_length)));
    send_buffers.push_back(buffer(&call_data->sample_count, sizeof(call_data->sample_count)));
    send_buffers.push_back(buffer(call_data->json_string));
    send_buffers.push_back(buffer(call_data->samples.data(), call_data->sample_count * sizeof(int16_t)));
    size_t total_size = 0;
    BOOST_FOREACH(auto& entry, send_buffers) {
      total_size += entry.size();
    }
    auto buffer = (char*)calloc(total_size, 1);
    if (buffer == nullptr) {
      BOOST_LOG_TRIVIAL(error) << "libcallstream: out of memory";
      return;
    }
    auto ptr = (char*)buffer;
    BOOST_FOREACH(auto& entry, send_buffers) {
      memcpy(ptr, (char*)entry.data(), entry.size());
      ptr += entry.size();
    }

    //
    // Destination folder on server is in the format:
    //    [destination specified in config]/Year/Month/Day/Hour
    //
    time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    tm local_tm = *localtime(&tt);
    auto year = 1900 + local_tm.tm_year;
    auto month = local_tm.tm_mon + 1;
    auto day = local_tm.tm_mday;
    auto hour = local_tm.tm_hour;
    auto min = local_tm.tm_min;
    auto sec = local_tm.tm_sec;
    //
    // Setup CURL
    //
    try {
      sftp_buffer_info_t buffer_info;
      buffer_info.pointer = (char*)buffer;
      buffer_info.size_left = total_size;
      std::string config_dest = sftp_client_info.dest;
      std::string filename = fmt::format("{}-{}-{}.{}{}{}.bin", year, month, day, hour, min, sec);
      std::string destination;
      if (!config_dest.empty()) {
        destination = fmt::format("{}/{}/{}/{}/{}/{}",
          config_dest, year, month, day, hour, filename);
      }
      else {
        destination = fmt::format("{}/{}/{}/{}/{}",
          year, month, day, hour, filename);
      }
      std::string url = fmt::format("sftp://{}:{}@{}/{}",
        sftp_client_info.user , sftp_client_info.password, sftp_client_info.server_address, destination);
      if (sftp_client_info.verbose) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
      }
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, (long)CURLFTP_CREATE_DIR_RETRY);
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(curl, CURLOPT_READFUNCTION, +[](char *ptr, size_t size, size_t nmemb, void* ptrbuffer_info) -> size_t {
        auto info = (sftp_buffer_info_t*)ptrbuffer_info;
        size_t max = size * nmemb;
        if(max < 1) {
          return 0;
        }
        if(info->size_left) {
          size_t copylen = max;
          if(copylen > info->size_left) {
            copylen = info->size_left;
          }
          memcpy(ptr, info->pointer, copylen);
          info->pointer += copylen;
          info->size_left -= copylen;
          return copylen;
        }
        return 0; 
      });
      curl_easy_setopt(curl, CURLOPT_READDATA, &buffer_info);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)total_size);
      auto result = curl_easy_perform(curl);
      if(result != CURLE_OK) {
        BOOST_LOG_TRIVIAL(error) << "libcallstream: CURL failure: " << curl_easy_strerror(result);
      }
      else {
        BOOST_LOG_TRIVIAL(info) << "libcallstream: sent call data (" << total_size << " bytes) to SFTP server";
      }
    }
    catch(...) {
      auto ex = boost::current_exception_diagnostic_information();
      BOOST_LOG_TRIVIAL(info) << "libcallstream: CURL exception" << ex;
    }
    curl_easy_cleanup(curl);
    free(buffer);
  }

  tcp::endpoint clients[6];
  int num_clients;
  sftp_client_info_t sftp_client_info;
};

BOOST_DLL_ALIAS(
    Call_Stream::create, // <-- this function is exported with...
    create_plugin        // <-- ...this alias name
)
