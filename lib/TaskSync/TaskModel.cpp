#include "TaskModel.h"

#include <StreamingJsonParser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tw {

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int64_t parseEpoch(const std::string& s) { return s.empty() ? 0 : strtoll(s.c_str(), nullptr, 10); }

// Days from civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void appendEscaped(std::string& out, const std::string& s) {
  for (const char c : s) {
    switch (c) {
      case '\t':
        out += "\\t";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        out += c;
    }
  }
}

std::string unescape(const char* s, size_t len) {
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    if (s[i] == '\\' && i + 1 < len) {
      const char n = s[++i];
      out += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    } else {
      out += s[i];
    }
  }
  return out;
}

void appendJsonString(std::string& out, const std::string& s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += ch;
    } else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += ch;
    }
  }
  out += '"';
}

}  // namespace

// --- Uuid -------------------------------------------------------------------

bool Uuid::parse(const char* s, size_t len, Uuid& out) {
  size_t j = 0;
  for (size_t i = 0; i < len;) {
    if (s[i] == '-') {
      ++i;
      continue;
    }
    if (j >= 16 || i + 1 >= len) return false;
    const int hi = hexVal(s[i]), lo = hexVal(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.b[j++] = static_cast<uint8_t>(hi << 4 | lo);
    i += 2;
  }
  return j == 16;
}

std::string Uuid::str() const {
  static constexpr char HEX[] = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
    s += HEX[b[i] >> 4];
    s += HEX[b[i] & 0xf];
  }
  return s;
}

bool Uuid::isNil() const {
  return std::all_of(std::begin(b), std::end(b), [](uint8_t v) { return v == 0; });
}

bool Uuid::operator==(const Uuid& o) const { return memcmp(b, o.b, 16) == 0; }

// --- Status -----------------------------------------------------------------

Status parseStatus(const std::string& s) {
  if (s == "pending") return Status::Pending;
  if (s == "completed") return Status::Completed;
  if (s == "deleted") return Status::Deleted;
  if (s == "recurring") return Status::Recurring;
  // Taskwarrior 2.x stored waiting tasks with their own status.
  if (s == "waiting") return Status::Pending;
  return Status::Unknown;
}

const char* statusName(Status s) {
  switch (s) {
    case Status::Pending:
      return "pending";
    case Status::Completed:
      return "completed";
    case Status::Deleted:
      return "deleted";
    case Status::Recurring:
      return "recurring";
    default:
      return "";
  }
}

// --- Task -------------------------------------------------------------------

bool Task::hasTag(const std::string& tag) const {
  size_t pos = 0;
  while (pos < tags.size()) {
    size_t end = tags.find(' ', pos);
    if (end == std::string::npos) end = tags.size();
    if (tags.compare(pos, end - pos, tag) == 0) return true;
    pos = end + 1;
  }
  return false;
}

void Task::setTag(const std::string& tag, const bool on) {
  if (tag.empty() || hasTag(tag) == on) return;
  if (on) {
    if (!tags.empty()) tags += ' ';
    tags += tag;
    return;
  }
  std::string kept;
  size_t pos = 0;
  while (pos < tags.size()) {
    size_t end = tags.find(' ', pos);
    if (end == std::string::npos) end = tags.size();
    if (tags.compare(pos, end - pos, tag) != 0) {
      if (!kept.empty()) kept += ' ';
      kept.append(tags, pos, end - pos);
    }
    pos = end + 1;
  }
  tags = std::move(kept);
}

int Task::tagCount() const {
  if (tags.empty()) return 0;
  return static_cast<int>(std::count(tags.begin(), tags.end(), ' ')) + 1;
}

float Task::urgency(const int64_t now) const {
  float u = 0;
  if (hasTag("next")) u += 15.0f;
  if (due) {
    const float days = static_cast<float>(now - due) / 86400.0f;
    float score;
    if (days >= 7.0f) {
      score = 1.0f;
    } else if (days >= -14.0f) {
      score = ((days + 14.0f) * 0.8f / 21.0f) + 0.2f;
    } else {
      score = 0.2f;
    }
    u += 12.0f * score;
  }
  if (priority == 'H') u += 6.0f;
  if (priority == 'M') u += 3.9f;
  if (priority == 'L') u += 1.8f;
  if (start) u += 4.0f;
  if (scheduled && scheduled <= now) u += 5.0f;
  if (entry) u += 2.0f * std::min(1.0f, static_cast<float>(now - entry) / 86400.0f / 365.0f);
  const int n = tagCount();
  if (n) u += n == 1 ? 0.8f : n == 2 ? 0.9f : 1.0f;
  if (!project.empty()) u += 1.0f;
  if (isWaiting(now)) u -= 3.0f;
  return u;
}

bool Task::setProperty(const std::string& name, const std::string* value) {
  static const std::string EMPTY;
  const std::string& v = value ? *value : EMPTY;
  if (name == "status") {
    status = value ? parseStatus(v) : Status::Unset;
  } else if (name == "description") {
    description = v;
  } else if (name == "project") {
    project = v;
  } else if (name == "priority") {
    priority = v.empty() ? 0 : v[0];
  } else if (name == "entry") {
    entry = parseEpoch(v);
  } else if (name == "modified") {
    modified = parseEpoch(v);
  } else if (name == "due") {
    due = parseEpoch(v);
  } else if (name == "wait") {
    wait = parseEpoch(v);
  } else if (name == "scheduled") {
    scheduled = parseEpoch(v);
  } else if (name == "start") {
    start = parseEpoch(v);
  } else if (name == "end") {
    end = parseEpoch(v);
  } else if (name.compare(0, 4, "tag_") == 0) {
    setTag(name.substr(4), value != nullptr);
  } else {
    return false;
  }
  return true;
}

std::string Task::serialize() const {
  std::string out = uuid.str();
  char buf[160];
  snprintf(buf, sizeof(buf), "\t%d\t%c\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t", static_cast<int>(status),
           priority ? priority : '-', static_cast<long long>(entry), static_cast<long long>(modified),
           static_cast<long long>(due), static_cast<long long>(wait), static_cast<long long>(scheduled),
           static_cast<long long>(start), static_cast<long long>(end));
  out += buf;
  appendEscaped(out, project);
  out += '\t';
  appendEscaped(out, tags);
  out += '\t';
  appendEscaped(out, description);
  return out;
}

bool Task::deserialize(const std::string& line, Task& out) {
  // uuid, status, priority, 7 dates, project, tags, description
  constexpr int FIELDS = 13;
  const char* fieldStart[FIELDS];
  size_t fieldLen[FIELDS];
  int n = 0;
  size_t pos = 0;
  while (n < FIELDS) {
    size_t end = n == FIELDS - 1 ? line.size() : line.find('\t', pos);
    if (end == std::string::npos) return false;
    fieldStart[n] = line.data() + pos;
    fieldLen[n] = end - pos;
    ++n;
    pos = end + 1;
  }
  if (!Uuid::parse(fieldStart[0], fieldLen[0], out.uuid)) return false;
  out.status = static_cast<Status>(atoi(fieldStart[1]));
  out.priority = fieldStart[2][0] == '-' ? 0 : fieldStart[2][0];
  int64_t* dates[] = {&out.entry, &out.modified, &out.due, &out.wait, &out.scheduled, &out.start, &out.end};
  for (int i = 0; i < 7; ++i) *dates[i] = strtoll(fieldStart[3 + i], nullptr, 10);
  out.project = unescape(fieldStart[10], fieldLen[10]);
  out.tags = unescape(fieldStart[11], fieldLen[11]);
  out.description = unescape(fieldStart[12], fieldLen[12]);
  return true;
}

// --- TaskFilter -------------------------------------------------------------

namespace {

bool projectMatches(const std::string& project, const std::string& want) {
  if (project.compare(0, want.size(), want) != 0) return false;
  return project.size() == want.size() || project[want.size()] == '.';
}

std::string unquote(std::string s) {
  if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
  return s;
}

}  // namespace

TaskFilter TaskFilter::parse(const std::string& text) {
  TaskFilter f;
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;
    size_t end = pos;
    // Quoted values may contain spaces: project:'My Project'
    char quote = 0;
    while (end < text.size() && (quote || text[end] != ' ')) {
      const char c = text[end];
      if (!quote && (c == '\'' || c == '"')) {
        quote = c;
      } else if (c == quote) {
        quote = 0;
      }
      ++end;
    }
    std::string term = text.substr(pos, end - pos);
    pos = end;
    if (term == "and") continue;
    if (term == "or" || term == "xor" || term.find('(') != std::string::npos || term.find(')') != std::string::npos) {
      f.ok = false;
      continue;
    }
    const size_t colon = term.find(':');
    if (term.size() > 1 && term[0] == '+') {
      f.terms.push_back({Kind::Tag, term.substr(1)});
    } else if (term.size() > 1 && term[0] == '-' && colon == std::string::npos) {
      f.terms.push_back({Kind::NotTag, term.substr(1)});
    } else if (colon != std::string::npos) {
      const std::string name = term.substr(0, colon);
      const std::string value = unquote(term.substr(colon + 1));
      if (name == "project" || name == "pro" || name == "proj") {
        f.terms.push_back({Kind::Project, value});
      } else if (name == "project.not" || name == "pro.not" || name == "proj.not") {
        f.terms.push_back({Kind::NotProject, value});
      }
      // Other attributes (status:, due:, ...) are not supported and ignored.
    } else {
      f.terms.push_back({Kind::Word, unquote(term)});
    }
  }
  return f;
}

bool TaskFilter::matches(const Task& task) const {
  if (!ok) return true;
  for (const auto& t : terms) {
    bool hit = false;
    switch (t.kind) {
      case Kind::Project:
        hit = t.value.empty() ? task.project.empty() : projectMatches(task.project, t.value);
        break;
      case Kind::NotProject:
        hit = t.value.empty() ? !task.project.empty() : !projectMatches(task.project, t.value);
        break;
      case Kind::Tag:
        hit = task.hasTag(t.value);
        break;
      case Kind::NotTag:
        hit = !task.hasTag(t.value);
        break;
      case Kind::Word:
        hit = task.description.find(t.value) != std::string::npos;
        break;
    }
    if (!hit) return false;
  }
  return true;
}

std::string TaskFilter::shortProject(const std::string& project) const {
  size_t cut = 0;
  if (ok) {
    for (const auto& t : terms) {
      if (t.kind != Kind::Project || t.value.empty() || !projectMatches(project, t.value)) continue;
      cut = std::max(cut, project.size() == t.value.size() ? project.size() : t.value.size() + 1);
    }
  }
  return project.substr(cut);
}

bool TaskFilter::requiresTag(const std::string& tag) const {
  if (!ok) return false;
  for (const auto& t : terms) {
    if (t.kind == Kind::Tag && t.value == tag) return true;
  }
  return false;
}

// --- Time -------------------------------------------------------------------

bool parseRfc3339(const std::string& s, OpTime& out) {
  int y, mo, d, h, mi, sec;
  if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6) return false;
  out.seconds = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
  out.nanos = 0;
  size_t i = 19;
  if (i < s.size() && s[i] == '.') {
    uint32_t scale = 100000000;
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      out.nanos += (s[i] - '0') * scale;
      scale /= 10;
    }
  }
  // Offsets other than Z are not produced by TaskChampion; accept +hh:mm anyway.
  if (i + 6 <= s.size() && (s[i] == '+' || s[i] == '-')) {
    const int sign = s[i] == '+' ? -1 : 1;
    const int oh = atoi(s.c_str() + i + 1), om = atoi(s.c_str() + i + 4);
    out.seconds += sign * (oh * 3600 + om * 60);
  }
  return true;
}

std::string formatRfc3339(const int64_t seconds) {
  int64_t days = seconds / 86400;
  int64_t rem = seconds % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  // civil_from_days
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  char buf[48];
  snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%02dZ", static_cast<long long>(y + (m <= 2)), m, d,
           static_cast<int>(rem / 3600), static_cast<int>(rem / 60 % 60), static_cast<int>(rem % 60));
  return buf;
}

// --- SyncOp -----------------------------------------------------------------

std::string SyncOp::toJson() const {
  std::string out;
  switch (kind) {
    case Kind::Create:
      out = "{\"Create\":{\"uuid\":\"" + uuid.str() + "\"}}";
      break;
    case Kind::Delete:
      out = "{\"Delete\":{\"uuid\":\"" + uuid.str() + "\"}}";
      break;
    case Kind::Update:
      out = "{\"Update\":{\"uuid\":\"" + uuid.str() + "\",\"property\":";
      appendJsonString(out, property);
      out += ",\"value\":";
      if (hasValue) {
        appendJsonString(out, value);
      } else {
        out += "null";
      }
      out += ",\"timestamp\":";
      appendJsonString(out, timestamp);
      out += "}}";
      break;
  }
  return out;
}

void transform(const SyncOp& a, const SyncOp& b, SyncOp& aOut, bool& keepA, SyncOp& bOut, bool& keepB) {
  using K = SyncOp::Kind;
  aOut = a;
  bOut = b;
  keepA = keepB = true;
  if (a.uuid != b.uuid) return;
  if (a.kind == b.kind && a.kind != K::Update) {
    keepA = keepB = false;  // both created / both deleted
  } else if (a.kind == K::Create && b.kind == K::Delete) {
    keepB = false;
  } else if (a.kind == K::Delete && b.kind == K::Create) {
    keepA = false;
  } else if (a.kind == K::Update && b.kind == K::Create) {
    keepB = false;
  } else if (a.kind == K::Create && b.kind == K::Update) {
    keepA = false;
  } else if (a.kind == K::Update && b.kind == K::Delete) {
    keepA = false;
  } else if (a.kind == K::Delete && b.kind == K::Update) {
    keepB = false;
  } else if (a.kind == K::Update && b.kind == K::Update && a.property == b.property) {
    if (a.hasValue == b.hasValue && a.value == b.value) {
      keepA = keepB = false;
      return;
    }
    OpTime ta, tb;
    parseRfc3339(a.timestamp, ta);
    parseRfc3339(b.timestamp, tb);
    if (ta < tb) {
      keepA = false;
    } else {
      keepB = false;
    }
  }
}

// --- Parsers ----------------------------------------------------------------

// Depth 1: {"operations":[...]}, 2: {"Update": ...}, 3: the operation fields.
struct HistoryParser::Impl {
  explicit Impl(OpFn fn) : fn(std::move(fn)), parser(callbacks()) {}
  JsonCallbacks callbacks() {
    JsonCallbacks cb{};
    cb.ctx = this;
    cb.onKey = [](void* c, const char* k, size_t n) { static_cast<Impl*>(c)->onKey(k, n); };
    cb.onString = [](void* c, const char* v, size_t n) { static_cast<Impl*>(c)->onValue(v, n); };
    cb.onNull = [](void* c) { static_cast<Impl*>(c)->onNull(); };
    cb.onObjectStart = [](void* c) { static_cast<Impl*>(c)->depth++; };
    cb.onObjectEnd = [](void* c) { static_cast<Impl*>(c)->onObjectEnd(); };
    return cb;
  }
  void onKey(const char* k, size_t n) {
    key.assign(k, n);
    if (depth == 2) {
      op = SyncOp();
      valueSeen = false;
      kindOk = true;
      if (key == "Create") {
        op.kind = SyncOp::Kind::Create;
      } else if (key == "Delete") {
        op.kind = SyncOp::Kind::Delete;
      } else if (key == "Update") {
        op.kind = SyncOp::Kind::Update;
      } else {
        kindOk = false;
      }
    }
  }
  void onValue(const char* v, size_t n) {
    if (depth != 3) return;
    if (key == "uuid") {
      uuidOk = Uuid::parse(v, n, op.uuid);
    } else if (key == "property") {
      op.property.assign(v, n);
    } else if (key == "value") {
      op.value.assign(v, n);
      op.hasValue = true;
      valueSeen = true;
    } else if (key == "timestamp") {
      op.timestamp.assign(v, n);
    }
  }
  void onNull() {
    if (depth == 3 && key == "value") valueSeen = true;
  }
  void onObjectEnd() {
    if (depth == 3) {
      // An Update whose value overflowed the token buffer arrives without one;
      // skip it rather than clearing the property.
      const bool complete = op.kind != SyncOp::Kind::Update || valueSeen;
      if (kindOk && uuidOk && complete) fn(op);
      uuidOk = false;
    }
    depth--;
  }

  OpFn fn;
  StreamingJsonParser parser;
  std::string key;
  SyncOp op;
  int depth = 0;
  bool valueSeen = false;
  bool kindOk = false;
  bool uuidOk = false;
};

HistoryParser::HistoryParser(OpFn fn) : impl(new Impl(std::move(fn))) {}
HistoryParser::~HistoryParser() { delete impl; }
void HistoryParser::feed(const char* data, size_t len) { impl->parser.feed(data, len); }
bool HistoryParser::ok() const { return !impl->parser.hasError() && impl->depth == 0; }

struct SnapshotParser::Impl {
  explicit Impl(TaskFn fn) : fn(std::move(fn)), parser(callbacks()) {}
  JsonCallbacks callbacks() {
    JsonCallbacks cb{};
    cb.ctx = this;
    cb.onKey = [](void* c, const char* k, size_t n) { static_cast<Impl*>(c)->onKey(k, n); };
    cb.onString = [](void* c, const char* v, size_t n) { static_cast<Impl*>(c)->onValue(v, n); };
    cb.onObjectStart = [](void* c) { static_cast<Impl*>(c)->depth++; };
    cb.onObjectEnd = [](void* c) { static_cast<Impl*>(c)->onObjectEnd(); };
    return cb;
  }
  void onKey(const char* k, size_t n) {
    if (depth == 1) {
      task = Task();
      uuidOk = Uuid::parse(k, n, task.uuid);
    } else if (depth == 2) {
      key.assign(k, n);
    }
  }
  void onValue(const char* v, size_t n) {
    if (depth != 2) return;
    value.assign(v, n);
    task.setProperty(key, &value);
  }
  void onObjectEnd() {
    if (depth == 2 && uuidOk) fn(task);
    depth--;
  }

  TaskFn fn;
  StreamingJsonParser parser;
  std::string key, value;
  Task task;
  int depth = 0;
  bool uuidOk = false;
};

SnapshotParser::SnapshotParser(TaskFn fn) : impl(new Impl(std::move(fn))) {}
SnapshotParser::~SnapshotParser() { delete impl; }
void SnapshotParser::feed(const char* data, size_t len) { impl->parser.feed(data, len); }
bool SnapshotParser::ok() const { return !impl->parser.hasError() && impl->depth == 0; }

// --- TaskStore --------------------------------------------------------------

Task* TaskStore::find(const Uuid& uuid) {
  for (auto& t : open) {
    if (t.uuid == uuid) return &t;
  }
  return nullptr;
}

void TaskStore::apply(const SyncOp& op, Archive& archive) {
  Task* task = find(op.uuid);
  switch (op.kind) {
    case SyncOp::Kind::Create:
      // Archived uuids are not looked up: a create for an existing task is a
      // no-op, and an empty task with no status is never listed.
      if (!task) {
        Task t;
        t.uuid = op.uuid;
        open.push_back(std::move(t));
      }
      return;
    case SyncOp::Kind::Delete:
      if (task) open.erase(open.begin() + (task - open.data()));
      return;
    case SyncOp::Kind::Update:
      break;
  }
  if (!task) {
    // Not open here: archived (closed) or unknown. Only a reopen matters.
    if (op.property != "status" || !op.hasValue) return;
    const Status s = parseStatus(op.value);
    if (s != Status::Pending && s != Status::Recurring) return;
    Task t;
    if (!archive.take(op.uuid, t)) return;
    open.push_back(std::move(t));
    task = &open.back();
  }
  task->setProperty(op.property, op.hasValue ? &op.value : nullptr);
}

bool TaskStore::compact(Archive& archive) {
  bool ok = true;
  auto keep = open.begin();
  for (auto it = open.begin(); it != open.end(); ++it) {
    if (it->isOpen()) {
      if (keep != it) *keep = std::move(*it);
      ++keep;
    } else {
      ok = archive.put(*it) && ok;
    }
  }
  open.erase(keep, open.end());
  return ok;
}

std::vector<SyncOp> makeUpdate(const Uuid& uuid, const int64_t now,
                               std::initializer_list<std::pair<const char*, const std::string*>> props) {
  std::vector<SyncOp> ops;
  const std::string ts = formatRfc3339(now);
  for (const auto& [name, value] : props) {
    SyncOp op;
    op.kind = SyncOp::Kind::Update;
    op.uuid = uuid;
    op.property = name;
    op.hasValue = value != nullptr;
    if (value) op.value = *value;
    op.timestamp = ts;
    ops.push_back(std::move(op));
  }
  return ops;
}

}  // namespace tw
