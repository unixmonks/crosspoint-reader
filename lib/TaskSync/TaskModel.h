#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A minimal TaskChampion replica for a read-mostly task list.
//
// Only tasks that can still show up in a list (status pending, recurring or not
// yet set) are kept in RAM. Completed and deleted tasks go to an archive the
// caller stores (SD on the device), and come back only if another replica
// revives them. Only the properties the UI shows are kept; nothing is ever
// written back as a snapshot, so the dropped properties are never lost on the
// server.
namespace tw {

struct Uuid {
  uint8_t b[16] = {};
  static bool parse(const char* s, size_t len, Uuid& out);
  static bool parse(const std::string& s, Uuid& out) { return parse(s.data(), s.size(), out); }
  std::string str() const;
  bool isNil() const;
  bool operator==(const Uuid& o) const;
  bool operator!=(const Uuid& o) const { return !(*this == o); }
};

enum class Status : uint8_t { Unset, Pending, Completed, Deleted, Recurring, Unknown };
Status parseStatus(const std::string& s);
const char* statusName(Status s);

struct Task {
  Uuid uuid;
  Status status = Status::Unset;
  char priority = 0;  // 'H', 'M', 'L' or 0
  int64_t entry = 0, modified = 0, due = 0, wait = 0, scheduled = 0, start = 0, end = 0;
  std::string description;
  std::string project;
  std::string tags;  // space separated, without the "tag_" prefix

  bool isOpen() const { return status == Status::Unset || status == Status::Pending || status == Status::Recurring; }
  bool isWaiting(int64_t now) const { return wait > now; }
  bool hasTag(const std::string& tag) const;
  void setTag(const std::string& tag, bool on);
  int tagCount() const;
  // Taskwarrior's default urgency, minus dependency/annotation/UDA terms.
  float urgency(int64_t now) const;
  // Apply one property change; returns false for properties we don't keep.
  bool setProperty(const std::string& name, const std::string* value);

  // One-line, tab separated serialisation used for the SD files.
  std::string serialize() const;
  static bool deserialize(const std::string& line, Task& out);
};

// Timestamps in sync operations are RFC 3339 strings with optional fractional seconds.
struct OpTime {
  int64_t seconds = 0;
  uint32_t nanos = 0;
  bool operator<(const OpTime& o) const { return seconds < o.seconds || (seconds == o.seconds && nanos < o.nanos); }
};
bool parseRfc3339(const std::string& s, OpTime& out);
std::string formatRfc3339(int64_t seconds);

struct SyncOp {
  enum class Kind : uint8_t { Create, Delete, Update };
  Kind kind = Kind::Update;
  Uuid uuid;
  std::string property;
  bool hasValue = false;
  std::string value;
  std::string timestamp;  // as received / sent

  std::string toJson() const;
};

// TaskChampion's operational transform for two concurrent operations. Either
// output may be dropped (returned false).
void transform(const SyncOp& a, const SyncOp& b, SyncOp& aOut, bool& keepA, SyncOp& bOut, bool& keepB);

// Streams {"operations":[...]} history segments into SyncOps.
class HistoryParser {
 public:
  using OpFn = std::function<void(const SyncOp&)>;
  explicit HistoryParser(OpFn fn);
  ~HistoryParser();
  HistoryParser(const HistoryParser&) = delete;
  HistoryParser& operator=(const HistoryParser&) = delete;
  void feed(const char* data, size_t len);
  bool ok() const;

 private:
  struct Impl;
  Impl* impl;
};

// Streams snapshot JSON {"<uuid>": {"prop": "value", ...}, ...} into Tasks.
class SnapshotParser {
 public:
  using TaskFn = std::function<void(Task&)>;
  explicit SnapshotParser(TaskFn fn);
  ~SnapshotParser();
  SnapshotParser(const SnapshotParser&) = delete;
  SnapshotParser& operator=(const SnapshotParser&) = delete;
  void feed(const char* data, size_t len);
  bool ok() const;

 private:
  struct Impl;
  Impl* impl;
};

// Where closed tasks live. put() may be called for a uuid already archived;
// take() must return the most recent copy.
class Archive {
 public:
  virtual ~Archive() = default;
  virtual bool put(const Task& task) = 0;
  virtual bool take(const Uuid& uuid, Task& out) = 0;
};

class TaskStore {
 public:
  std::vector<Task>& tasks() { return open; }
  const std::vector<Task>& tasks() const { return open; }
  Task* find(const Uuid& uuid);
  void clear() { open.clear(); }
  void add(Task&& task) { open.push_back(std::move(task)); }

  // Apply a server (or local) operation. archive is consulted when a closed
  // task is reopened.
  void apply(const SyncOp& op, Archive& archive);
  // Move closed tasks to the archive. Call after a batch of operations.
  bool compact(Archive& archive);

 private:
  std::vector<Task> open;
};

// Local changes as operations: applies them to the store and returns them for upload.
std::vector<SyncOp> makeUpdate(const Uuid& uuid, int64_t now,
                               std::initializer_list<std::pair<const char*, const std::string*>> props);

}  // namespace tw
