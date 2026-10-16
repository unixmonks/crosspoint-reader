#pragma once

#include <TaskSyncEngine.h>

#include <memory>

namespace freeink {
class SecureHttpClient;
}

// Device bindings for tw::TaskSyncEngine: SD card files under TASKS_DIR and
// HTTPS through the SDK client. The sync config is read from CONFIG_PATH.
namespace taskwarrior {

constexpr char TASKS_DIR[] = "/.crosspoint/tasks";
constexpr char CONFIG_PATH[] = "/taskwarrior.conf";

class SdFs final : public tw::Fs {
 public:
  SdFs();
  std::unique_ptr<tw::File> open(const char* name, Mode mode) override;
  bool remove(const char* name) override;
  bool rename(const char* from, const char* to) override;
};

class HttpsTransport final : public tw::Transport {
 public:
  HttpsTransport();
  ~HttpsTransport() override;
  Response get(const std::string& url, const std::string& clientId, const Sink& sink) override;
  Response post(const std::string& url, const std::string& clientId, const char* contentType, const uint8_t* body,
                size_t len) override;
  void close() override;

 private:
  Response finish(int status);
  std::unique_ptr<freeink::SecureHttpClient> http;
};

void fillRandom(uint8_t* buf, size_t len);

// Reads CONFIG_PATH. False when the file is missing or incomplete.
bool loadConfig(tw::SyncConfig& out);

}  // namespace taskwarrior
