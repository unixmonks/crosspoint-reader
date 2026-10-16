#include "TaskSyncEngine.h"

#include <InflateStream.h>

#include <algorithm>
#include <cstring>

namespace tw {

namespace {

constexpr char STATE_FILE[] = "state";
constexpr char KEY_FILE[] = "key";
constexpr char OPEN_FILE[] = "open.tsv";
constexpr char OPEN_TMP[] = "open.tmp";
constexpr char ARCHIVE_FILE[] = "archive.tsv";
constexpr char PENDING_FILE[] = "pending.jsonl";
constexpr char PENDING_TMP[] = "pending.tmp";
constexpr char DOWNLOAD_FILE[] = "download.tmp";
constexpr char SNAPSHOT_TMP[] = "snapshot.tmp";

constexpr char HISTORY_TYPE[] = "application/vnd.taskchampion.history-segment";
constexpr int MAX_VERSIONS_PER_SYNC = 2000;
constexpr int MAX_CONFLICT_RETRIES = 5;
// Stack buffers: the C3 loop task has an 8 KB stack.
constexpr size_t IO_CHUNK = 512;

// Buffered line reader over a File.
class LineReader {
 public:
  explicit LineReader(File& f) : file(f) {}
  bool next(std::string& line) {
    line.clear();
    for (;;) {
      if (pos == len) {
        const long n = file.read(buf, sizeof(buf));
        if (n <= 0) return !line.empty();
        len = static_cast<size_t>(n);
        pos = 0;
      }
      const auto* nl = static_cast<const uint8_t*>(memchr(buf + pos, '\n', len - pos));
      const size_t end = nl ? static_cast<size_t>(nl - buf) : len;
      line.append(reinterpret_cast<const char*>(buf + pos), end - pos);
      pos = end;
      if (nl) {
        ++pos;
        return true;
      }
    }
  }

 private:
  File& file;
  uint8_t buf[512];
  size_t pos = 0, len = 0;
};

// Closed tasks, one line each, appended; the last line for a uuid wins.
class FileArchive final : public Archive {
 public:
  explicit FileArchive(Fs& fs) : fs(fs) {}
  bool truncate() {
    appendHandle.reset();
    appendHandle = fs.open(ARCHIVE_FILE, Fs::Mode::Write);
    return appendHandle != nullptr;
  }
  bool put(const Task& task) override {
    if (!appendHandle) appendHandle = fs.open(ARCHIVE_FILE, Fs::Mode::Append);
    if (!appendHandle) return false;
    return appendHandle->write(task.serialize() + "\n");
  }
  bool take(const Uuid& uuid, Task& out) override {
    appendHandle.reset();  // flush before scanning
    auto f = fs.open(ARCHIVE_FILE, Fs::Mode::Read);
    if (!f) return false;
    const std::string prefix = uuid.str();
    LineReader reader(*f);
    std::string line;
    bool found = false;
    while (reader.next(line)) {
      if (line.compare(0, prefix.size(), prefix) == 0) found = Task::deserialize(line, out) || found;
    }
    return found;
  }

 private:
  Fs& fs;
  std::unique_ptr<File> appendHandle;
};

// Pull-based decryption of a verified download.
class DecryptReader {
 public:
  DecryptReader(File& f, size_t cipherLen) : file(f), remaining(cipherLen) {}
  void begin(const uint8_t key[32], const uint8_t nonce[12], const uint8_t aad[twcrypto::AAD_LEN]) {
    aead.begin(key, nonce, aad, twcrypto::AAD_LEN);
  }
  long read(uint8_t* out, size_t len) {
    if (remaining == 0) return 0;
    const size_t want = std::min(len, remaining);
    const long n = file.read(out, want);
    if (n <= 0) return -1;
    aead.decrypt(out, out, static_cast<size_t>(n));
    remaining -= static_cast<size_t>(n);
    return n;
  }

 private:
  File& file;
  size_t remaining;
  twcrypto::AeadStream aead;
};

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\"'");
  if (a == std::string::npos) return "";
  const size_t b = s.find_last_not_of(" \t\r\"'");
  return s.substr(a, b - a + 1);
}

std::string endpoint(const SyncConfig& config, const std::string& path) {
  std::string url = config.url;
  if (url.empty() || url.back() != '/') url += '/';
  return url + path;
}

}  // namespace

const char* syncResultName(const SyncResult r) {
  switch (r) {
    case SyncResult::Ok:
      return "ok";
    case SyncResult::Aborted:
      return "aborted";
    case SyncResult::Network:
      return "network error";
    case SyncResult::NoAccess:
      return "no access (check plan / client id)";
    case SyncResult::BadSecret:
      return "cannot decrypt (check encryption secret)";
    case SyncResult::ServerError:
      return "unexpected server response";
    case SyncResult::StorageError:
      return "SD card error";
    case SyncResult::OutOfSync:
      return "out of sync";
  }
  return "?";
}

bool SyncConfig::parse(const std::string& text, SyncConfig& out) {
  std::string clientId;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.find('#') == line.find_first_not_of(" \t")) continue;
    const std::string name = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));
    // .taskrc escapes slashes written by `task config`: https:\/\/host
    for (size_t i; (i = value.find("\\/")) != std::string::npos;) value.erase(i, 1);
    if (name == "sync.server.url" || name == "sync.server.origin") {
      out.url = value;
    } else if (name == "sync.server.client_id") {
      clientId = value;
    } else if (name == "sync.encryption_secret") {
      out.secret = value;
    }
  }
  return !out.url.empty() && Uuid::parse(clientId, out.clientId) && !out.secret.empty();
}

TaskSyncEngine::TaskSyncEngine(Fs& fs, Transport& net, RandomFn random) : fs(fs), net(net), random(std::move(random)) {}

bool TaskSyncEngine::load() {
  tasks.clear();
  synced = false;
  lastSyncTime = 0;
  baseVersion = Uuid();
  if (auto f = fs.open(STATE_FILE, Fs::Mode::Read)) {
    LineReader reader(*f);
    std::string line;
    while (reader.next(line)) {
      if (line.compare(0, 5, "base=") == 0) synced = Uuid::parse(line.substr(5), baseVersion);
      if (line.compare(0, 5, "last=") == 0) lastSyncTime = strtoll(line.c_str() + 5, nullptr, 10);
    }
  }
  if (auto f = fs.open(OPEN_FILE, Fs::Mode::Read)) {
    LineReader reader(*f);
    std::string line;
    while (reader.next(line)) {
      Task t;
      if (Task::deserialize(line, t)) tasks.add(std::move(t));
    }
  }
  if (auto f = fs.open(KEY_FILE, Fs::Mode::Read)) {
    uint8_t buf[40];
    if (f->read(buf, sizeof(buf)) == sizeof(buf)) {
      memcpy(keyFingerprint, buf, 8);
      memcpy(key, buf + 8, 32);
      haveKey = true;
    }
  }
  return loadPending();
}

bool TaskSyncEngine::loadPending() {
  pending.clear();
  auto f = fs.open(PENDING_FILE, Fs::Mode::Read);
  if (!f) return true;
  HistoryParser parser([this](const SyncOp& op) { pending.push_back(op); });
  static constexpr char HEAD[] = "{\"operations\":[";
  parser.feed(HEAD, sizeof(HEAD) - 1);
  LineReader reader(*f);
  std::string line;
  bool first = true;
  while (reader.next(line)) {
    if (line.empty()) continue;
    if (!first) parser.feed(",", 1);
    parser.feed(line.data(), line.size());
    first = false;
  }
  parser.feed("]}", 2);
  return parser.ok();
}

bool TaskSyncEngine::savePending() {
  if (pending.empty()) {
    fs.remove(PENDING_FILE);
    return true;
  }
  {
    auto f = fs.open(PENDING_TMP, Fs::Mode::Write);
    if (!f) return false;
    for (const auto& op : pending) {
      if (!f->write(op.toJson() + "\n")) return false;
    }
  }
  fs.remove(PENDING_FILE);
  return fs.rename(PENDING_TMP, PENDING_FILE);
}

bool TaskSyncEngine::save() {
  {
    auto f = fs.open(OPEN_TMP, Fs::Mode::Write);
    if (!f) return false;
    for (const auto& t : tasks.tasks()) {
      if (!f->write(t.serialize() + "\n")) return false;
    }
  }
  fs.remove(OPEN_FILE);
  if (!fs.rename(OPEN_TMP, OPEN_FILE)) return false;
  auto f = fs.open(STATE_FILE, Fs::Mode::Write);
  if (!f) return false;
  std::string state;
  if (synced) state += "base=" + baseVersion.str() + "\n";
  state += "last=" + std::to_string(lastSyncTime) + "\n";
  return f->write(state);
}

bool TaskSyncEngine::reset() {
  tasks.clear();
  synced = false;
  baseVersion = Uuid();
  fs.remove(ARCHIVE_FILE);
  return save();
}

bool TaskSyncEngine::edit(const Uuid& uuid, std::vector<SyncOp>&& ops) {
  if (!tasks.find(uuid)) return false;
  FileArchive archive(fs);
  for (auto& op : ops) {
    tasks.apply(op, archive);
    pending.push_back(std::move(op));
  }
  // Keep the closed task listed until the next sync so the change can be seen.
  return savePending() && save();
}

bool TaskSyncEngine::complete(const Uuid& uuid, const int64_t now) {
  const std::string status = "completed", ts = std::to_string(now);
  return edit(uuid, makeUpdate(uuid, now, {{"status", &status}, {"end", &ts}, {"start", nullptr}, {"modified", &ts}}));
}

bool TaskSyncEngine::remove(const Uuid& uuid, const int64_t now) {
  const std::string status = "deleted", ts = std::to_string(now);
  return edit(uuid, makeUpdate(uuid, now, {{"status", &status}, {"end", &ts}, {"start", nullptr}, {"modified", &ts}}));
}

bool TaskSyncEngine::setStarted(const Uuid& uuid, const bool started, const int64_t now) {
  const std::string ts = std::to_string(now);
  return edit(uuid, makeUpdate(uuid, now, {{"start", started ? &ts : nullptr}, {"modified", &ts}}));
}

bool TaskSyncEngine::waitUntil(const Uuid& uuid, const int64_t until, const int64_t now) {
  const std::string ts = std::to_string(now), wait = std::to_string(until);
  return edit(uuid, makeUpdate(uuid, now, {{"wait", until ? &wait : nullptr}, {"modified", &ts}}));
}

bool TaskSyncEngine::ensureKey(const SyncConfig& config, const Progress& progress) {
  // Fingerprint = SHA-256(client id || secret), so a config change re-derives the key.
  twcrypto::Sha256 h;
  h.init();
  h.update(config.clientId.b, 16);
  h.update(reinterpret_cast<const uint8_t*>(config.secret.data()), config.secret.size());
  uint8_t digest[32];
  h.final(digest);
  if (haveKey && memcmp(digest, keyFingerprint, 8) == 0) return true;

  uint8_t derived[32];
  const bool ok = twcrypto::pbkdf2Sha256(
      reinterpret_cast<const uint8_t*>(config.secret.data()), config.secret.size(), config.clientId.b, 16,
      twcrypto::PBKDF2_ITERATIONS, derived, sizeof(derived), [&progress](uint32_t done, uint32_t total) {
        return !progress || progress("Deriving key (one time)", static_cast<int>(uint64_t(done) * 100 / total));
      });
  if (!ok) return false;
  memcpy(key, derived, 32);
  memcpy(keyFingerprint, digest, 8);
  haveKey = true;
  if (auto f = fs.open(KEY_FILE, Fs::Mode::Write)) {
    f->write(keyFingerprint, 8);
    f->write(key, 32);
  }
  return true;
}

Transport::Response TaskSyncEngine::download(const std::string& url, const SyncConfig& config,
                                             const Progress& progress) {
  auto f = fs.open(DOWNLOAD_FILE, Fs::Mode::Write);
  if (!f) {
    Transport::Response r;
    r.status = -2;
    return r;
  }
  size_t received = 0;
  bool writeOk = true;
  auto response = net.get(url, config.clientId.str(), [&](const uint8_t* data, size_t len) {
    received += len;
    writeOk = f->write(data, len);
    // Report every ~16 KB so big snapshots show movement.
    if (progress && (received >> 14) != ((received - len) >> 14) && !progress("Downloading", -1)) return false;
    return writeOk;
  });
  if (!writeOk) response.status = -2;
  return response;
}

bool TaskSyncEngine::openDownload(const uint8_t versionId[16], const Transport::Sink& sink) {
  auto f = fs.open(DOWNLOAD_FILE, Fs::Mode::Read);
  if (!f) return false;
  const size_t total = f->size();
  if (total < twcrypto::ENVELOPE_HEADER + twcrypto::TAG_LEN) return false;
  const size_t cipherLen = total - twcrypto::ENVELOPE_HEADER - twcrypto::TAG_LEN;
  uint8_t header[twcrypto::ENVELOPE_HEADER];
  if (f->read(header, sizeof(header)) != sizeof(header) || header[0] != 1) return false;
  uint8_t aad[twcrypto::AAD_LEN];
  twcrypto::makeAad(versionId, aad);

  // Pass 1: authenticate before acting on any plaintext.
  {
    twcrypto::AeadStream aead;
    aead.begin(key, header + 1, aad, sizeof(aad));
    uint8_t buf[IO_CHUNK];
    size_t left = cipherLen;
    while (left > 0) {
      const long n = f->read(buf, std::min(left, sizeof(buf)));
      if (n <= 0) return false;
      aead.authOnly(buf, static_cast<size_t>(n));
      left -= static_cast<size_t>(n);
    }
    uint8_t tag[twcrypto::TAG_LEN], expected[twcrypto::TAG_LEN];
    if (f->read(expected, sizeof(expected)) != sizeof(expected)) return false;
    aead.finish(tag);
    if (!twcrypto::constantTimeEqual(tag, expected, sizeof(tag))) return false;
  }

  // Pass 2: decrypt.
  f.reset();
  f = fs.open(DOWNLOAD_FILE, Fs::Mode::Read);
  if (!f || f->read(header, sizeof(header)) != sizeof(header)) return false;
  DecryptReader reader(*f, cipherLen);
  reader.begin(key, header + 1, aad);
  uint8_t buf[IO_CHUNK];
  for (;;) {
    const long n = reader.read(buf, sizeof(buf));
    if (n < 0) return false;
    if (n == 0) return true;
    if (!sink(buf, static_cast<size_t>(n))) return false;
  }
}

bool TaskSyncEngine::fetchSnapshot(const SyncConfig& config, const Progress& progress, SyncResult& result) {
  if (progress && !progress("Fetching snapshot", -1)) {
    result = SyncResult::Aborted;
    return false;
  }
  const auto response = download(endpoint(config, "v1/client/snapshot"), config, progress);
  FileArchive archive(fs);
  if (!archive.truncate()) {
    result = SyncResult::StorageError;
    return false;
  }
  tasks.clear();
  if (response.status == 404) {
    baseVersion = Uuid();  // empty server: start from the nil version
    synced = true;
    return true;
  }
  if (response.status != 200) {
    result = response.status <= 0                                                         ? SyncResult::Network
             : response.status == 401 || response.status == 402 || response.status == 403 ? SyncResult::NoAccess
                                                                                          : SyncResult::ServerError;
    return false;
  }
  Uuid version;
  if (!Uuid::parse(response.versionId, version)) {
    result = SyncResult::ServerError;
    return false;
  }
  // Free the TLS session before the 32 KB inflate window is allocated.
  net.close();
  if (progress && !progress("Unpacking snapshot", -1)) {
    result = SyncResult::Aborted;
    return false;
  }

  // The snapshot is zlib-compressed JSON. It is authenticated and decrypted
  // to a second temp file first, because the inflater pulls its input.
  const bool decrypted = [&] {
    auto plain = fs.open(SNAPSHOT_TMP, Fs::Mode::Write);
    return plain && openDownload(version.b, [&](const uint8_t* d, size_t n) { return plain->write(d, n); });
  }();
  if (!decrypted) {
    fs.remove(SNAPSHOT_TMP);
    result = SyncResult::BadSecret;
    return false;
  }

  SnapshotParser parser([&](Task& t) {
    if (t.isOpen()) {
      tasks.add(std::move(t));
    } else {
      archive.put(t);
    }
  });
  InflateStream inflate;
  if (!inflate.init(true)) {
    fs.remove(SNAPSHOT_TMP);
    result = SyncResult::StorageError;
    return false;
  }
  inflate.setZlibWrapped();
  bool inflateOk = true;
  uint8_t out[IO_CHUNK];
  {
    auto plain = fs.open(SNAPSHOT_TMP, Fs::Mode::Read);
    if (!plain) {
      result = SyncResult::StorageError;
      return false;
    }
    struct FileFill {
      File* file;
      uint8_t buf[IO_CHUNK];
    };
    auto fill = std::make_unique<FileFill>();
    fill->file = plain.get();
    inflate.setFill(
        [](void* ctx, const uint8_t** data) -> size_t {
          auto* ff = static_cast<FileFill*>(ctx);
          const long n = ff->file->read(ff->buf, sizeof(ff->buf));
          if (n <= 0) return 0;
          *data = ff->buf;
          return static_cast<size_t>(n);
        },
        fill.get());
    for (;;) {
      size_t produced = 0;
      const auto status = inflate.readAtMost(out, sizeof(out), &produced);
      if (produced) parser.feed(reinterpret_cast<const char*>(out), produced);
      if (status == InflateStream::Status::Error) {
        inflateOk = false;
        break;
      }
      if (status == InflateStream::Status::Done) break;
    }
  }
  fs.remove(SNAPSHOT_TMP);
  if (!inflateOk || !parser.ok()) {
    result = SyncResult::ServerError;
    return false;
  }
  baseVersion = version;
  synced = true;
  return true;
}

void TaskSyncEngine::applyServerOp(const SyncOp& serverOp, Archive& archive) {
  // Rebase local operations onto the server operation, as TaskChampion does.
  SyncOp svr = serverOp;
  bool haveSvr = true;
  std::vector<SyncOp> rebased;
  rebased.reserve(pending.size());
  for (const auto& local : pending) {
    if (!haveSvr) {
      rebased.push_back(local);
      continue;
    }
    SyncOp svrOut, localOut;
    bool keepSvr, keepLocal;
    transform(svr, local, svrOut, keepSvr, localOut, keepLocal);
    haveSvr = keepSvr;
    if (keepSvr) svr = std::move(svrOut);
    if (keepLocal) rebased.push_back(std::move(localOut));
  }
  pending = std::move(rebased);
  if (haveSvr) tasks.apply(svr, archive);
}

SyncResult TaskSyncEngine::sync(const SyncConfig& config, const int64_t now, const Progress& progress) {
  if (!ensureKey(config, progress)) return SyncResult::Aborted;

  SyncResult result = SyncResult::Ok;
  bool resetDone = false;
  if (!synced) {
    if (!fetchSnapshot(config, progress, result)) return result;
    resetDone = true;
    // Edits made before a full refetch are re-applied on top of it.
    FileArchive archive(fs);
    for (const auto& op : pending) tasks.apply(op, archive);
  }

  FileArchive archive(fs);
  int versions = 0;
  int conflicts = 0;
  for (;;) {
    // Pull every version after our base.
    for (;;) {
      if (++versions > MAX_VERSIONS_PER_SYNC) return SyncResult::OutOfSync;
      if (progress && !progress("Fetching changes", -1)) return SyncResult::Aborted;
      const auto response =
          download(endpoint(config, "v1/client/get-child-version/" + baseVersion.str()), config, progress);
      if (response.status == 404) break;
      if (response.status == 410 && !resetDone) {
        // Our base is gone from the server: start over from its snapshot.
        resetDone = true;
        if (!fetchSnapshot(config, progress, result)) return result;
        for (const auto& op : pending) tasks.apply(op, archive);
        continue;
      }
      if (response.status <= 0) return response.status == -2 ? SyncResult::StorageError : SyncResult::Network;
      if (response.status == 401 || response.status == 402 || response.status == 403) return SyncResult::NoAccess;
      if (response.status != 200) return SyncResult::ServerError;
      Uuid parent, version;
      if (!Uuid::parse(response.parentVersionId, parent) || !Uuid::parse(response.versionId, version)) {
        return SyncResult::ServerError;
      }
      HistoryParser parser([&](const SyncOp& op) { applyServerOp(op, archive); });
      if (!openDownload(parent.b, [&](const uint8_t* d, size_t n) {
            parser.feed(reinterpret_cast<const char*>(d), n);
            return true;
          })) {
        return SyncResult::BadSecret;
      }
      if (!parser.ok()) return SyncResult::ServerError;
      baseVersion = version;
      synced = true;
    }

    if (pending.empty()) break;

    // Push local edits as one new version.
    if (progress && !progress("Uploading changes", -1)) return SyncResult::Aborted;
    std::string body = "{\"operations\":[";
    for (size_t i = 0; i < pending.size(); ++i) {
      if (i) body += ',';
      body += pending[i].toJson();
    }
    body += "]}";
    std::vector<uint8_t> sealed(body.size() + twcrypto::ENVELOPE_HEADER + twcrypto::TAG_LEN);
    uint8_t nonce[twcrypto::NONCE_LEN];
    random(nonce, sizeof(nonce));
    const size_t sealedLen = twcrypto::sealEnvelope(
        key, baseVersion.b, nonce, reinterpret_cast<const uint8_t*>(body.data()), body.size(), sealed.data());
    body.clear();
    body.shrink_to_fit();
    const auto response = net.post(endpoint(config, "v1/client/add-version/" + baseVersion.str()),
                                   config.clientId.str(), HISTORY_TYPE, sealed.data(), sealedLen);
    if (response.status == 409) {
      if (++conflicts > MAX_CONFLICT_RETRIES) return SyncResult::OutOfSync;
      continue;  // someone else pushed first: pull and rebase
    }
    if (response.status <= 0) return SyncResult::Network;
    if (response.status == 401 || response.status == 402 || response.status == 403) return SyncResult::NoAccess;
    if (response.status != 200) return SyncResult::ServerError;
    Uuid version;
    if (!Uuid::parse(response.versionId, version)) return SyncResult::ServerError;
    baseVersion = version;
    pending.clear();
    break;
  }

  fs.remove(DOWNLOAD_FILE);
  if (!tasks.compact(archive)) return SyncResult::StorageError;
  lastSyncTime = now;
  if (!savePending() || !save()) return SyncResult::StorageError;
  return SyncResult::Ok;
}

}  // namespace tw
