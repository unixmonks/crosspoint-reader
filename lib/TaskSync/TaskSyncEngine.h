#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TaskModel.h"
#include "TwCrypto.h"

// TaskChampion sync client (https://gothenburgbitfactory.org/taskchampion/sync-protocol.html)
// for a device that only lists tasks and makes small edits. Platform access is
// behind the two interfaces below, so the engine also runs in host tests.
namespace tw {

class File {
 public:
  virtual ~File() = default;
  // Bytes read, 0 at end of file, < 0 on error.
  virtual long read(uint8_t* buf, size_t len) = 0;
  virtual bool write(const uint8_t* buf, size_t len) = 0;
  virtual size_t size() = 0;
  bool write(const std::string& s) { return write(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
};

class Fs {
 public:
  enum class Mode : uint8_t { Read, Write, Append };
  virtual ~Fs() = default;
  // name is relative to the engine's data directory.
  virtual std::unique_ptr<File> open(const char* name, Mode mode) = 0;
  virtual bool remove(const char* name) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
};

class Transport {
 public:
  struct Response {
    int status = 0;  // HTTP status, <= 0 on transport failure
    std::string versionId;
    std::string parentVersionId;
  };
  using Sink = std::function<bool(const uint8_t* data, size_t len)>;
  virtual ~Transport() = default;
  virtual Response get(const std::string& url, const std::string& clientId, const Sink& sink) = 0;
  virtual Response post(const std::string& url, const std::string& clientId, const char* contentType,
                        const uint8_t* body, size_t len) = 0;
  // Drop a kept-alive connection; the next request reconnects.
  virtual void close() {}
};

struct SyncConfig {
  std::string url;  // e.g. https://taskwarriorsync.com
  Uuid clientId;
  std::string secret;

  // Parses taskrc-style lines (sync.server.url=..., sync.server.client_id=...,
  // sync.encryption_secret=...). Returns false when a value is missing.
  static bool parse(const std::string& text, SyncConfig& out);
};

enum class SyncResult : uint8_t {
  Ok,
  Aborted,
  Network,
  NoAccess,     // 401/402/403: account or plan problem
  BadSecret,    // decryption failed: wrong encryption secret
  ServerError,  // unexpected response
  StorageError,
  OutOfSync,
};
const char* syncResultName(SyncResult r);

class TaskSyncEngine {
 public:
  // Phase updates for the UI. percent is -1 when unknown. Return false to abort.
  using Progress = std::function<bool(const char* phase, int percent)>;
  // Source of nonce bytes for uploads.
  using RandomFn = std::function<void(uint8_t* buf, size_t len)>;

  TaskSyncEngine(Fs& fs, Transport& net, RandomFn random);

  // Read the local replica. Missing files mean "never synced".
  bool load();
  TaskStore& store() { return tasks; }
  size_t pendingCount() const { return pending.size(); }
  bool hasSynced() const { return synced; }
  int64_t lastSync() const { return lastSyncTime; }

  // Local edits. Applied at once, uploaded on the next sync.
  bool complete(const Uuid& uuid, int64_t now);
  bool remove(const Uuid& uuid, int64_t now);
  bool setStarted(const Uuid& uuid, bool started, int64_t now);
  bool waitUntil(const Uuid& uuid, int64_t until, int64_t now);

  SyncResult sync(const SyncConfig& config, int64_t now, const Progress& progress);
  // Forget the replica (not pending edits) and fetch everything on the next sync.
  bool reset();

  // Separate step so the caller can report it; sync() calls it if needed.
  bool ensureKey(const SyncConfig& config, const Progress& progress);

 private:
  bool edit(const Uuid& uuid, std::vector<SyncOp>&& ops);
  bool save();
  bool savePending();
  bool loadPending();
  bool fetchSnapshot(const SyncConfig& config, const Progress& progress, SyncResult& result);
  // Downloads to a temp file; returns the response (status <= 0 on failure).
  Transport::Response download(const std::string& url, const SyncConfig& config, const Progress& progress);
  // Verifies and decrypts the temp file, streaming plaintext to sink.
  bool openDownload(const uint8_t versionId[16], const Transport::Sink& sink);
  void applyServerOp(const SyncOp& op, Archive& archive);

  Fs& fs;
  Transport& net;
  RandomFn random;
  TaskStore tasks;
  std::vector<SyncOp> pending;
  Uuid baseVersion;
  bool synced = false;
  int64_t lastSyncTime = 0;
  uint8_t key[32] = {};
  uint8_t keyFingerprint[8] = {};
  bool haveKey = false;
};

}  // namespace tw
