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
constexpr int kDefaultVolumePercent = 30;
constexpr char kSocketName[] = "aera-browser-audio-v1";
constexpr char kBootstrap[] = "/system/bin/aera-audio-bootstrap";
constexpr char kAgmPlay[] = "/mnt/aera-stock/vendor/bin/agmplay";
volatile sig_atomic_t g_stop = 0;

void Stop(int) { g_stop = 1; }

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
         "/vendor/lib64:/system/lib64", 1);
  setenv("ANDROID_ROOT", "/system", 1);
  setenv("ANDROID_DATA", "/data", 1);
  execl(kAgmPlay, "agmplay", fifo.c_str(), "-D", "100", "-d", "100",
        "-c", "2", "-r", "48000", "-b", "16", "-i",
        "MI2S-LPAIF-RX-PRIMARY", nullptr);
  _exit(127);
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

std::array<uint8_t, 44> WavHeader() {
  std::array<uint8_t, 44> header{};
  auto put16 = [&header](size_t offset, uint16_t value) {
    header[offset] = static_cast<uint8_t>(value);
    header[offset + 1] = static_cast<uint8_t>(value >> 8);
  };
  auto put32 = [&header](size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
      header[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  };
  memcpy(header.data(), "RIFF", 4);
  put32(4, 0x7ffff024U);
  memcpy(header.data() + 8, "WAVEfmt ", 8);
  put32(16, 16U);
  put16(20, 1U);
  put16(22, static_cast<uint16_t>(kChannels));
  put32(24, kRate);
  put32(28, kRate * kChannels * (kBits / 8));
  put16(32, static_cast<uint16_t>(kChannels * (kBits / 8)));
  put16(34, static_cast<uint16_t>(kBits));
  memcpy(header.data() + 36, "data", 4);
  put32(40, 0x7ffff000U);
  return header;
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
  // O_RDWR prevents a deadlock if the stock player exits before opening its
  // reader. Open it before forking and keep the pathname until shutdown: if
  // the parent unlinks first, the newly scheduled player can lose the race
  // and fail to open the stream at all.
  const int output = open(fifo.c_str(), O_RDWR | O_CLOEXEC);
  if (output < 0) {
    unlink(fifo.c_str());
    return;
  }
  const pid_t player = StartPlayer(fifo);
  if (player < 0) {
    close(output);
    unlink(fifo.c_str());
    return;
  }
  const auto header = WavHeader();
  bool good = WriteAll(output, header.data(), header.size());
  std::array<uint8_t, 8193> buffer{};
  size_t carry = 0;
  unsigned volume_counter = 0;
  int volume = ReadVolumePercent();
  while (good && !g_stop) {
    const ssize_t count = read(client, buffer.data() + carry,
                               buffer.size() - carry);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    const size_t total = carry + static_cast<size_t>(count);
    const size_t even = total & ~static_cast<size_t>(1);
    if ((volume_counter++ % 12U) == 0) volume = ReadVolumePercent();
    for (size_t offset = 0; offset < even; offset += 2) {
      int16_t sample = static_cast<int16_t>(
          static_cast<uint16_t>(buffer[offset]) |
          (static_cast<uint16_t>(buffer[offset + 1]) << 8));
      sample = static_cast<int16_t>((static_cast<int32_t>(sample) * volume) /
                                    100);
      buffer[offset] = static_cast<uint8_t>(sample);
      buffer[offset + 1] = static_cast<uint8_t>(
          static_cast<uint16_t>(sample) >> 8);
    }
    good = WriteAll(output, buffer.data(), even);
    carry = total - even;
    if (carry) buffer[0] = buffer[even];
  }
  close(output);
  StopPlayer(player);
  unlink(fifo.c_str());
}
}  // namespace

int main(int argc, char** argv) {
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
