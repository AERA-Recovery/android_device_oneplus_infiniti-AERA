/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Narrow plugin-to-AGM PCM bridge for Infiniti / OnePlus 15 recovery. Untrusted
 * plugins never receive access to ALSA, Binder, stock partitions, or this
 * process. Only the dedicated browser and media UIDs may connect and send
 * fixed-format audio over one abstract local socket.
 */
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace {
constexpr uid_t kBrowserUid = 99090;
constexpr uid_t kMediaUid = 99092;
constexpr uint32_t kMagic = 0x41525041U;
constexpr uint32_t kRate = 48000U;
constexpr uint32_t kChannels = 2U;
constexpr uint32_t kBits = 16U;
constexpr uint32_t kHardwareBits = 32U;
constexpr int kDefaultVolumePercent = 30;
constexpr char kSocketName[] = "aera-browser-audio-v1";
constexpr char kBootstrap[] = "/system/bin/aera-audio-bootstrap";
constexpr char kSelf[] = "/proc/self/exe";
constexpr char kAudioInterface[] = "MI2S-LPAIF-RX-PRIMARY";
constexpr unsigned int kAgmCard = 100;
constexpr unsigned int kAgmDevice = 100;
constexpr uint32_t kSpeakerKv = 0xA2000001U;
constexpr uint32_t kPcmLowLatencyKv = 0xA100000EU;
constexpr uint32_t kSpeakerMbDrcKv = 0xAC000002U;
volatile sig_atomic_t g_stop = 0;

void Stop(int) { g_stop = 1; }

struct Mixer;
struct MixerCtl;
struct Pcm;

struct PcmConfig {
  unsigned int channels;
  unsigned int rate;
  unsigned int period_size;
  unsigned int period_count;
  int format;
  unsigned int start_threshold;
  unsigned int stop_threshold;
  unsigned int silence_threshold;
  unsigned int silence_size;
  int avail_min;
};

struct AgmDeviceConfig {
  char name[80];
  unsigned int rate;
  unsigned int channels;
  unsigned int bits;
  int format;
};

static_assert(offsetof(AgmDeviceConfig, rate) == 0x50);
static_assert(offsetof(AgmDeviceConfig, format) == 0x5c);

struct AudioApi {
  using MixerOpen = Mixer* (*)(unsigned int);
  using MixerClose = void (*)(Mixer*);
  using MixerGetCtlByName = MixerCtl* (*)(Mixer*, const char*);
  using MixerCtlSetValue = int (*)(MixerCtl*, unsigned int, int);
  using MixerCtlSetEnumByString = int (*)(MixerCtl*, const char*);
  using PcmOpen = Pcm* (*)(unsigned int, unsigned int, unsigned int,
                           PcmConfig*);
  using PcmClose = void (*)(Pcm*);
  using PcmIsReady = int (*)(Pcm*);
  using PcmGetError = const char* (*)(Pcm*);
  using PcmStart = int (*)(Pcm*);
  using PcmStop = int (*)(Pcm*);
  using PcmWrite = int (*)(Pcm*, const void*, unsigned int);
  using SetDeviceMediaConfig = int (*)(Mixer*, char*, AgmDeviceConfig*);
  using SetAudioInterfaceMetadata =
      int (*)(Mixer*, char*, unsigned int, int, int, int, uint32_t);
  using SetStreamMetadata =
      int (*)(Mixer*, int, uint32_t, int, int, unsigned int);
  using SetStreamDeviceMetadata =
      int (*)(Mixer*, int, uint32_t, int, int, char*, unsigned int);
  using ConnectAudioInterface =
      int (*)(Mixer*, unsigned int, char*, int, bool);

  void* tinyalsa = nullptr;
  void* agmmixer = nullptr;
  MixerOpen mixer_open = nullptr;
  MixerClose mixer_close = nullptr;
  MixerGetCtlByName mixer_get_ctl_by_name = nullptr;
  MixerCtlSetValue mixer_ctl_set_value = nullptr;
  MixerCtlSetEnumByString mixer_ctl_set_enum_by_string = nullptr;
  PcmOpen pcm_open = nullptr;
  PcmClose pcm_close = nullptr;
  PcmIsReady pcm_is_ready = nullptr;
  PcmGetError pcm_get_error = nullptr;
  PcmStart pcm_start = nullptr;
  PcmStop pcm_stop = nullptr;
  PcmWrite pcm_write = nullptr;
  SetDeviceMediaConfig set_device_media_config = nullptr;
  SetAudioInterfaceMetadata set_audio_interface_metadata = nullptr;
  SetStreamMetadata set_stream_metadata = nullptr;
  SetStreamDeviceMetadata set_stream_device_metadata = nullptr;
  ConnectAudioInterface connect_audio_interface = nullptr;

  ~AudioApi() {
    if (agmmixer) dlclose(agmmixer);
    if (tinyalsa) dlclose(tinyalsa);
  }

  template <typename T>
  bool LoadSymbol(void* library, const char* name, T* target) {
    *target = reinterpret_cast<T>(dlsym(library, name));
    if (*target) return true;
    fprintf(stderr, "AERA audio: missing stock symbol %s: %s\n", name,
            dlerror());
    return false;
  }

  bool Load() {
    tinyalsa = dlopen("libtinyalsa.so", RTLD_NOW | RTLD_GLOBAL);
    if (!tinyalsa) {
      fprintf(stderr, "AERA audio: could not load stock libtinyalsa: %s\n",
              dlerror());
      return false;
    }
    agmmixer = dlopen("libagmmixer.so", RTLD_NOW | RTLD_LOCAL);
    if (!agmmixer) {
      fprintf(stderr, "AERA audio: could not load stock libagmmixer: %s\n",
              dlerror());
      return false;
    }
    return LoadSymbol(tinyalsa, "mixer_open", &mixer_open) &&
           LoadSymbol(tinyalsa, "mixer_close", &mixer_close) &&
           LoadSymbol(tinyalsa, "mixer_get_ctl_by_name",
                      &mixer_get_ctl_by_name) &&
           LoadSymbol(tinyalsa, "mixer_ctl_set_value",
                      &mixer_ctl_set_value) &&
           LoadSymbol(tinyalsa, "mixer_ctl_set_enum_by_string",
                      &mixer_ctl_set_enum_by_string) &&
           LoadSymbol(tinyalsa, "pcm_open", &pcm_open) &&
           LoadSymbol(tinyalsa, "pcm_close", &pcm_close) &&
           LoadSymbol(tinyalsa, "pcm_is_ready", &pcm_is_ready) &&
           LoadSymbol(tinyalsa, "pcm_get_error", &pcm_get_error) &&
           LoadSymbol(tinyalsa, "pcm_start", &pcm_start) &&
           LoadSymbol(tinyalsa, "pcm_stop", &pcm_stop) &&
           LoadSymbol(tinyalsa, "pcm_write", &pcm_write) &&
           LoadSymbol(agmmixer, "set_agm_device_media_config",
                      &set_device_media_config) &&
           LoadSymbol(agmmixer, "set_agm_audio_intf_metadata",
                      &set_audio_interface_metadata) &&
           LoadSymbol(agmmixer, "set_agm_stream_metadata",
                      &set_stream_metadata) &&
           LoadSymbol(agmmixer, "set_agm_streamdevice_metadata",
                      &set_stream_device_metadata) &&
           LoadSymbol(agmmixer, "connect_agm_audio_intf_to_stream",
                      &connect_audio_interface);
  }
};

bool SetMixerValue(AudioApi& api, Mixer* mixer, const char* name, int value) {
  MixerCtl* control = api.mixer_get_ctl_by_name(mixer, name);
  if (control && api.mixer_ctl_set_value(control, 0, value) == 0) return true;
  fprintf(stderr, "AERA audio: could not set mixer control %s\n", name);
  return false;
}

bool SetMixerEnum(AudioApi& api, Mixer* mixer, const char* name,
                  const char* value) {
  MixerCtl* control = api.mixer_get_ctl_by_name(mixer, name);
  if (control && api.mixer_ctl_set_enum_by_string(control, value) == 0)
    return true;
  fprintf(stderr, "AERA audio: could not set mixer control %s=%s\n", name,
          value);
  return false;
}

[[maybe_unused]] bool ConfigureSpeakerMixer(AudioApi& api, Mixer* mixer,
                                            bool enable) {
  if (!enable) {
    SetMixerEnum(api, mixer, "TFA_CHECK_FEEDBACK", "Off");
    SetMixerValue(api, mixer, "TFA_CHIP_SELECTOR", 0);
    return true;
  }
  return SetMixerValue(api, mixer, "TFA_CHIP_SELECTOR", 3) &&
         SetMixerEnum(api, mixer, "TFA Profile", "speaker") &&
         SetMixerEnum(api, mixer, "TFA_CHECK_FEEDBACK", "On");
}

using PalStreamHandle = uint64_t;

struct PalChannelInfo {
  uint16_t channels;
  uint8_t channel_map[64];
};

struct PalStreamInfo {
  int64_t version;
  int64_t size;
  int64_t duration_us;
  bool has_video;
  bool is_streaming;
  int32_t loopback_type;
  int32_t tx_proxy_type;
  int32_t rx_proxy_type;
  int32_t haptics_type;
  bool is_bit_perfect;
};

union PalStreamInfoUnion {
  PalStreamInfo optional;
  struct {
    int64_t version;
    int64_t size;
    int32_t direction;
  } voice_record;
  struct {
    uint32_t vsid;
    uint32_t tty_mode;
  } voice_call;
  struct {
    bool local_playback;
    int32_t direction;
  } incall_music;
  int32_t hpcm_stream_type;
};

struct PalMediaConfig {
  uint32_t sample_rate;
  uint32_t bit_width;
  int32_t format;
  PalChannelInfo channel_info;
};

struct PalStreamAttributes {
  int32_t type;
  PalStreamInfoUnion info;
  int32_t flags;
  int32_t direction;
  PalMediaConfig input;
  PalMediaConfig output;
  char* address;
};

struct PalDevice {
  int32_t id;
  PalMediaConfig config;
  int32_t usb_card;
  int32_t usb_device;
  char sound_device_name[128];
  char custom_key[128];
  union {
    char id[128];
    uint32_t ipv6[8];
  } address;
};

struct PalBuffer {
  uint8_t* data;
  size_t size;
  size_t offset;
  timespec* timestamp;
  uint32_t flags;
  size_t metadata_size;
  uint8_t* metadata;
  struct {
    int32_t handle;
    uint32_t size;
    uint32_t offset;
  } allocation;
  uint64_t frame_index;
};

static_assert(sizeof(PalStreamAttributes) == 232);
static_assert(offsetof(PalStreamAttributes, input) == 64);
static_assert(sizeof(PalDevice) == 476);
static_assert(sizeof(PalBuffer) == 80);

struct PalApi {
  using Init = int32_t (*)();
  using Deinit = void (*)();
  using StreamOpen = int32_t (*)(PalStreamAttributes*, uint32_t, PalDevice*,
                                  uint32_t, void*, void*, uint64_t,
                                  PalStreamHandle**);
  using StreamOperation = int32_t (*)(PalStreamHandle*);
  using StreamWrite = ssize_t (*)(PalStreamHandle*, PalBuffer*);

  void* library = nullptr;
  Init init = nullptr;
  Deinit deinit = nullptr;
  StreamOpen stream_open = nullptr;
  StreamOperation stream_close = nullptr;
  StreamOperation stream_start = nullptr;
  StreamOperation stream_stop = nullptr;
  StreamWrite stream_write = nullptr;

  ~PalApi() {
    if (library) dlclose(library);
  }

  template <typename T>
  bool LoadSymbol(const char* name, T* target) {
    *target = reinterpret_cast<T>(dlsym(library, name));
    if (*target) return true;
    fprintf(stderr, "AERA audio: missing stock PAL symbol %s: %s\n", name,
            dlerror());
    return false;
  }

  bool Load() {
    library = dlopen("libar-pal.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      fprintf(stderr, "AERA audio: could not load stock PAL: %s\n", dlerror());
      return false;
    }
    return LoadSymbol("pal_init", &init) &&
           LoadSymbol("pal_deinit", &deinit) &&
           LoadSymbol("pal_stream_open", &stream_open) &&
           LoadSymbol("pal_stream_close", &stream_close) &&
           LoadSymbol("pal_stream_start", &stream_start) &&
           LoadSymbol("pal_stream_stop", &stream_stop) &&
           LoadSymbol("pal_stream_write", &stream_write);
  }
};

void SetPalPcmConfig(PalMediaConfig* config) {
  config->sample_rate = kRate;
  config->bit_width = kBits;
  config->format = 1;
  config->channel_info.channels = kChannels;
  config->channel_info.channel_map[0] = 1;
  config->channel_info.channel_map[1] = 2;
}

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t count = write(fd, bytes + done, size - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    done += static_cast<size_t>(count);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t count = read(fd, bytes + done, size - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    done += static_cast<size_t>(count);
  }
  return true;
}

int ReadVolumePercent() {
  const int fd = open("/tmp/aera-audio-volume", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return kDefaultVolumePercent;
  char value[8]{};
  const ssize_t count = read(fd, value, sizeof(value) - 1);
  close(fd);
  if (count <= 0) return kDefaultVolumePercent;
  return std::clamp(atoi(value), 0, 100);
}

bool StartBackend() {
  const pid_t child = fork();
  if (child < 0) return false;
  if (!child) {
    execl(kBootstrap, "aera-audio-bootstrap", nullptr);
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

pid_t StartPlayer(const std::string& fifo) {
  const pid_t child = fork();
  if (child != 0) return child;
  prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
  setenv("LD_LIBRARY_PATH",
         "/vendor/aera-audio-abi:/mnt/aera-stock/vendor/lib64:"
         "/mnt/aera-stock/odm/lib64:/vendor/lib64:/system/lib64", 1);
  setenv("ANDROID_ROOT", "/system", 1);
  setenv("ANDROID_DATA", "/data", 1);
  execl(kSelf, "aera-audio-bridge", "--play-fifo", fifo.c_str(), nullptr);
  _exit(127);
}

int OpenPlayerPipe(const std::string& fifo, pid_t child) {
  for (int attempt = 0; attempt < 250; ++attempt) {
    const int output = open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (output >= 0) {
      const int flags = fcntl(output, F_GETFL);
      if (flags >= 0) fcntl(output, F_SETFL, flags & ~O_NONBLOCK);
      return output;
    }
    if (errno != ENXIO && errno != EINTR) break;
    int status = 0;
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child || (result < 0 && errno == ECHILD)) {
      fprintf(stderr, "AERA audio: stock PCM child exited before opening\n");
      return -1;
    }
    usleep(20000);
  }
  fprintf(stderr, "AERA audio: PCM child did not open its stream: %s\n",
          strerror(errno));
  return -1;
}

void StopPlayer(pid_t child) {
  if (child <= 0) return;
  int status = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child || (result < 0 && errno == ECHILD)) return;
    usleep(25000);
  }
  kill(child, SIGTERM);
  for (int attempt = 0; attempt < 20; ++attempt) {
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child || (result < 0 && errno == ECHILD)) return;
    usleep(25000);
  }
  kill(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

void ServeClient(int client) {
  ucred credentials{};
  socklen_t credentials_size = sizeof(credentials);
  if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials,
                 &credentials_size) != 0 ||
      credentials_size != sizeof(credentials) ||
      (credentials.uid != kBrowserUid && credentials.uid != kMediaUid)) {
    fprintf(stderr, "AERA audio: rejected local UID %u\n", credentials.uid);
    return;
  }
  std::array<uint32_t, 4> hello{};
  if (!ReadAll(client, hello.data(), sizeof(hello)) || hello[0] != kMagic ||
      hello[1] != kRate || hello[2] != kChannels || hello[3] != kBits) {
    fprintf(stderr, "AERA audio: rejected invalid PCM handshake\n");
    return;
  }

  const std::string fifo = "/tmp/aera-browser-audio-" +
                           std::to_string(static_cast<long long>(getpid()));
  unlink(fifo.c_str());
  if (mkfifo(fifo.c_str(), 0600) != 0) return;
  const pid_t player = StartPlayer(fifo);
  if (player < 0) {
    unlink(fifo.c_str());
    return;
  }
  const int output = OpenPlayerPipe(fifo, player);
  if (output < 0) {
    StopPlayer(player);
    unlink(fifo.c_str());
    return;
  }
  bool good = true;
  std::array<uint8_t, 8195> buffer{};
  size_t carry = 0;
  unsigned volume_counter = 0;
  int volume = ReadVolumePercent();
  while (good && !g_stop) {
    const ssize_t count = read(client, buffer.data() + carry,
                               buffer.size() - carry);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    const size_t total = carry + static_cast<size_t>(count);
    const size_t aligned = total & ~static_cast<size_t>(3);
    if ((volume_counter++ % 12U) == 0) volume = ReadVolumePercent();
    for (size_t offset = 0; offset < aligned; offset += 2) {
      int16_t sample = static_cast<int16_t>(
          static_cast<uint16_t>(buffer[offset]) |
          (static_cast<uint16_t>(buffer[offset + 1]) << 8));
      const int32_t scaled =
          (static_cast<int32_t>(sample) * volume * volume) / 10000;
      sample = static_cast<int16_t>(scaled);
      buffer[offset] = static_cast<uint8_t>(sample);
      buffer[offset + 1] = static_cast<uint8_t>(
          static_cast<uint16_t>(sample) >> 8);
    }
    good = WriteAll(output, buffer.data(), aligned);
    carry = total - aligned;
    if (carry) memmove(buffer.data(), buffer.data() + aligned, carry);
  }
  close(output);
  StopPlayer(player);
  unlink(fifo.c_str());
}

int RunPlayer(const char* fifo) {
  struct stat info {};
  if (lstat(fifo, &info) != 0 || !S_ISFIFO(info.st_mode) ||
      info.st_uid != 0) {
    fprintf(stderr, "AERA audio: rejected invalid PCM FIFO\n");
    return 64;
  }
  const int input = open(fifo, O_RDONLY | O_CLOEXEC);
  if (input < 0) return 70;

  PalApi api;
  if (!api.Load()) {
    close(input);
    return 70;
  }
  bool initialized = false;
  bool started = false;
  PalStreamHandle* stream = nullptr;
  int result = 70;

  if (api.init() != 0) {
    fprintf(stderr, "AERA audio: stock PAL initialization failed\n");
    goto done;
  }
  initialized = true;

  {
    PalStreamAttributes attributes{};
    PalDevice speaker{};
    attributes.type = 1;
    attributes.direction = 1;
    SetPalPcmConfig(&attributes.input);
    SetPalPcmConfig(&attributes.output);
    speaker.id = 3;
    SetPalPcmConfig(&speaker.config);
    if (api.stream_open(&attributes, 1, &speaker, 0, nullptr, nullptr, 0,
                        &stream) != 0 ||
        !stream) {
      fprintf(stderr, "AERA audio: protected PAL stream open failed\n");
      goto done;
    }
  }
  if (api.stream_start(stream) != 0) {
    fprintf(stderr, "AERA audio: protected PAL stream start failed\n");
    goto done;
  }
  started = true;

  {
    std::array<uint8_t, 8195> buffer{};
    size_t carry = 0;
    result = 0;
    while (!g_stop) {
      const ssize_t count =
          read(input, buffer.data() + carry, buffer.size() - carry);
      if (count < 0 && errno == EINTR) continue;
      if (count == 0) break;
      if (count < 0) {
        result = 74;
        break;
      }
      const size_t total = carry + static_cast<size_t>(count);
      const size_t aligned = total & ~static_cast<size_t>(3);
      size_t written = 0;
      while (written < aligned) {
        PalBuffer buffer_info{};
        buffer_info.data = buffer.data() + written;
        buffer_info.size = aligned - written;
        const ssize_t bytes = api.stream_write(stream, &buffer_info);
        if (bytes <= 0) break;
        written += static_cast<size_t>(bytes);
      }
      if (written != aligned) {
        fprintf(stderr, "AERA audio: protected PAL stream write failed\n");
        result = 74;
        break;
      }
      carry = total - aligned;
      if (carry) memmove(buffer.data(), buffer.data() + aligned, carry);
    }
  }

done:
  if (started && api.stream_stop(stream) != 0)
    fprintf(stderr, "AERA audio: PAL stream stop failed\n");
  if (stream && api.stream_close(stream) != 0)
    fprintf(stderr, "AERA audio: PAL stream close failed\n");
  if (initialized) api.deinit();
  close(input);
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--play-fifo") == 0 && getuid() == 0)
    return RunPlayer(argv[2]);
  if (argc != 2 || strcmp(argv[1], "--browser-audio") != 0 || getuid() != 0) {
    fprintf(stderr, "Usage (root recovery only): aera-audio-bridge --browser-audio\n");
    return 64;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, Stop);
  signal(SIGINT, Stop);
  prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

  const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server < 0) return 70;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path + 1, kSocketName, sizeof(kSocketName) - 1);
  const socklen_t address_size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + 1 + sizeof(kSocketName) - 1);
  if (bind(server, reinterpret_cast<const sockaddr*>(&address), address_size) != 0 ||
      listen(server, 1) != 0) {
    close(server);
    return 70;
  }
  if (!StartBackend()) {
    fprintf(stderr, "AERA audio: stock AGM backend failed to start\n");
    close(server);
    return 69;
  }
  fprintf(stderr, "AERA audio: browser PCM bridge ready\n");
  while (!g_stop) {
    const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) continue;
      break;
    }
    ServeClient(client);
    close(client);
  }
  close(server);
  return 0;
}
