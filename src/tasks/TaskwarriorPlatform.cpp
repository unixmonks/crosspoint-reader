#include "TaskwarriorPlatform.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <esp_random.h>

#include <string>

namespace taskwarrior {

namespace {

constexpr size_t MAX_CONFIG_BYTES = 2048;
constexpr uint32_t HTTP_TIMEOUT_MS = 30000;

std::string pathFor(const char* name) { return std::string(TASKS_DIR) + "/" + name; }

class SdFile final : public tw::File {
 public:
  explicit SdFile(HalFile&& f) : file(std::move(f)) {}
  ~SdFile() override { file.close(); }
  long read(uint8_t* buf, size_t len) override { return file.read(buf, len); }
  bool write(const uint8_t* buf, size_t len) override { return file.write(buf, len) == len; }
  size_t size() override { return file.fileSize(); }

 private:
  HalFile file;
};

}  // namespace

SdFs::SdFs() { Storage.ensureDirectoryExists(TASKS_DIR); }

std::unique_ptr<tw::File> SdFs::open(const char* name, const Mode mode) {
  oflag_t flags = O_RDONLY;
  if (mode == Mode::Write) flags = O_WRONLY | O_CREAT | O_TRUNC;
  if (mode == Mode::Append) flags = O_WRONLY | O_CREAT | O_APPEND;
  HalFile f = Storage.open(pathFor(name).c_str(), flags);
  if (!f) return nullptr;
  return std::make_unique<SdFile>(std::move(f));
}

bool SdFs::remove(const char* name) {
  const std::string path = pathFor(name);
  return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
}

bool SdFs::rename(const char* from, const char* to) {
  return Storage.rename(pathFor(from).c_str(), pathFor(to).c_str());
}

HttpsTransport::HttpsTransport() : http(std::make_unique<freeink::SecureHttpClient>()) {
  // No CA bundle is wired into the TLS client (same as KOSync); task data is
  // end-to-end encrypted and authenticated, so a MITM sees only ciphertext.
  http->setInsecure();
  http->setTimeout(HTTP_TIMEOUT_MS);
  http->setUserAgent("CrossPoint-Tasks");
}

HttpsTransport::~HttpsTransport() = default;

void HttpsTransport::close() { http->end(); }

tw::Transport::Response HttpsTransport::finish(const int status) {
  Response r;
  r.status = status;
  r.versionId = http->getHeader("x-version-id");
  r.parentVersionId = http->getHeader("x-parent-version-id");
  LOG_DBG("TASKS", "HTTP %d", status);
  return r;
}

tw::Transport::Response HttpsTransport::get(const std::string& url, const std::string& clientId, const Sink& sink) {
  if (!http->begin(url)) return finish(-1);
  http->addHeader("X-Client-Id", clientId);
  bool ok200 = false;
  const int status = http->sendRequest("GET", nullptr, 0, [&](const uint8_t* data, size_t len) {
    // Only a 200 body is payload; error bodies are drained and dropped.
    if (!ok200) ok200 = http->getStatus() == 200;
    return !ok200 || sink(data, len);
  });
  if (status == 200 && !http->responseComplete()) return finish(-1);
  return finish(status);
}

tw::Transport::Response HttpsTransport::post(const std::string& url, const std::string& clientId,
                                             const char* contentType, const uint8_t* body, size_t len) {
  if (!http->begin(url)) return finish(-1);
  http->addHeader("X-Client-Id", clientId);
  http->addHeader("Content-Type", contentType);
  return finish(http->sendRequest("POST", body, len));
}

void fillRandom(uint8_t* buf, size_t len) { esp_fill_random(buf, len); }

bool loadConfig(tw::SyncConfig& out) {
  std::string text;
  if (!Storage.exists(CONFIG_PATH) || !Storage.readFileToString("TASKS", CONFIG_PATH, MAX_CONFIG_BYTES, text)) {
    return false;
  }
  return tw::SyncConfig::parse(text, out);
}

}  // namespace taskwarrior
