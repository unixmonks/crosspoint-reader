#include <TaskModel.h>
#include <TaskSyncEngine.h>
#include <TwCrypto.h>
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace tw;

namespace {

std::vector<uint8_t> hex(const char* s) {
  std::vector<uint8_t> out;
  for (; s[0] && s[1]; s += 2) {
    while (*s == ' ') ++s;
    out.push_back(static_cast<uint8_t>(std::stoi(std::string(s, 2), nullptr, 16)));
  }
  return out;
}

Uuid uuid(const char* s) {
  Uuid u;
  EXPECT_TRUE(Uuid::parse(s, strlen(s), u));
  return u;
}

class MemArchive final : public Archive {
 public:
  bool put(const Task& task) override {
    lines[task.uuid.str()] = task.serialize();
    return true;
  }
  bool take(const Uuid& id, Task& out) override {
    const auto it = lines.find(id.str());
    return it != lines.end() && Task::deserialize(it->second, out);
  }
  std::map<std::string, std::string> lines;
};

SyncOp update(const char* id, const char* prop, const char* value, const char* ts) {
  SyncOp op;
  op.uuid = uuid(id);
  op.property = prop;
  op.hasValue = value != nullptr;
  if (value) op.value = value;
  op.timestamp = ts;
  return op;
}

constexpr char ID1[] = "a3a4990e-0c75-41cb-91aa-f99b11e43106";

}  // namespace

TEST(TwCrypto, Sha256) {
  uint8_t out[32];
  twcrypto::sha256(reinterpret_cast<const uint8_t*>("abc"), 3, out);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 32),
            hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(TwCrypto, Pbkdf2Rfc7914) {
  uint8_t out[64];
  ASSERT_TRUE(twcrypto::pbkdf2Sha256(reinterpret_cast<const uint8_t*>("passwd"), 6,
                                     reinterpret_cast<const uint8_t*>("salt"), 4, 1, out, sizeof(out)));
  EXPECT_EQ(std::vector<uint8_t>(out, out + 64),
            hex("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
}

// RFC 8439 section 2.8.2.
TEST(TwCrypto, AeadRfc8439) {
  const auto key = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
  const auto nonce = hex("070000004041424344454647");
  const auto aad = hex("50515253c0c1c2c3c4c5c6c7");
  const std::string plain =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would "
      "be it.";
  std::vector<uint8_t> cipher(plain.size());
  twcrypto::AeadStream aead;
  aead.begin(key.data(), nonce.data(), aad.data(), aad.size());
  // Odd chunk sizes exercise the streaming paths.
  size_t pos = 0;
  for (const size_t chunk : {1u, 15u, 17u, 64u, 200u}) {
    const size_t n = std::min(chunk, plain.size() - pos);
    aead.encrypt(reinterpret_cast<const uint8_t*>(plain.data()) + pos, cipher.data() + pos, n);
    pos += n;
  }
  uint8_t tag[16];
  aead.finish(tag);
  EXPECT_EQ(std::vector<uint8_t>(cipher.begin(), cipher.begin() + 16), hex("d31a8d34648e60db7b86afbc53ef7ec2"));
  EXPECT_EQ(std::vector<uint8_t>(tag, tag + 16), hex("1ae10b594f09e26a7e902ecbd0600691"));
}

// test-good.data from taskchampion 3.1.0 (MIT), made by its generate-test-data.py.
TEST(TwCrypto, OpensTaskChampionEnvelope) {
  const auto envelope = hex("010b0874fe60261292c7d15f1e29d389e7dbfac718fbde080767ff727d882df85ab8c073");
  const Uuid client = uuid("0666d464-418a-4a08-ad53-6f15c78270cd");
  const Uuid version = uuid("b0517957-f912-4d49-8330-f612e73030c4");
  const char secret[] = "b4a4e6b7b811eda1dc1a2693ded";
  uint8_t key[32];
  ASSERT_TRUE(twcrypto::pbkdf2Sha256(reinterpret_cast<const uint8_t*>(secret), strlen(secret), client.b, 16,
                                     twcrypto::PBKDF2_ITERATIONS, key, sizeof(key)));
  uint8_t out[64];
  const long n = twcrypto::openEnvelope(key, version.b, envelope.data(), envelope.size(), out);
  ASSERT_EQ(n, 7);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(out), 7), "SUCCESS");

  const Uuid otherVersion = uuid("b0517957-f912-4d49-8330-f612e73030c5");
  EXPECT_EQ(twcrypto::openEnvelope(key, otherVersion.b, envelope.data(), envelope.size(), out), -1);
}

TEST(TwCrypto, SealRoundTrip) {
  uint8_t key[32] = {1, 2, 3};
  const Uuid version = uuid(ID1);
  const uint8_t nonce[12] = {9};
  const std::string msg = "{\"operations\":[]}";
  std::vector<uint8_t> sealed(msg.size() + 29);
  const size_t len = twcrypto::sealEnvelope(key, version.b, nonce, reinterpret_cast<const uint8_t*>(msg.data()),
                                            msg.size(), sealed.data());
  std::vector<uint8_t> out(msg.size());
  ASSERT_EQ(twcrypto::openEnvelope(key, version.b, sealed.data(), len, out.data()), static_cast<long>(msg.size()));
  EXPECT_EQ(std::string(out.begin(), out.end()), msg);
  sealed[20] ^= 1;
  EXPECT_EQ(twcrypto::openEnvelope(key, version.b, sealed.data(), len, out.data()), -1);
}

TEST(TaskModel, UuidAndTime) {
  EXPECT_EQ(uuid(ID1).str(), ID1);
  OpTime t;
  ASSERT_TRUE(parseRfc3339("2026-10-16T00:52:12.123456789Z", t));
  EXPECT_EQ(t.seconds, 1792111932);
  EXPECT_EQ(t.nanos, 123456789u);
  EXPECT_EQ(formatRfc3339(1792111932), "2026-10-16T00:52:12Z");
}

TEST(TaskModel, SerializeRoundTrip) {
  Task t;
  t.uuid = uuid(ID1);
  t.status = Status::Pending;
  t.priority = 'H';
  t.due = 1792111932;
  t.project = "home";
  t.tags = "next phone";
  t.description = "tab\there \\ newline\nend";
  Task back;
  ASSERT_TRUE(Task::deserialize(t.serialize(), back));
  EXPECT_EQ(back.description, t.description);
  EXPECT_EQ(back.tags, t.tags);
  EXPECT_EQ(back.priority, 'H');
  EXPECT_EQ(back.due, t.due);
}

TEST(TaskModel, UrgencyMatchesTaskwarriorDefaults) {
  Task t;
  t.status = Status::Pending;
  t.tags = "next";
  EXPECT_NEAR(t.urgency(1000), 15.8f, 0.01f);
  t.tags.clear();
  t.priority = 'H';
  t.project = "work";
  EXPECT_NEAR(t.urgency(1000), 7.0f, 0.01f);
}

TEST(TaskModel, HistoryParserAndJson) {
  std::vector<SyncOp> ops;
  HistoryParser parser([&](const SyncOp& op) { ops.push_back(op); });
  SyncOp u = update(ID1, "description", "say \"hi\"", "2026-10-16T00:52:12Z");
  SyncOp c;
  c.kind = SyncOp::Kind::Create;
  c.uuid = uuid(ID1);
  SyncOp n = update(ID1, "start", nullptr, "2026-10-16T00:52:13Z");
  const std::string json = "{\"operations\":[" + c.toJson() + "," + u.toJson() + "," + n.toJson() + "]}";
  for (const char ch : json) parser.feed(&ch, 1);
  ASSERT_TRUE(parser.ok());
  ASSERT_EQ(ops.size(), 3u);
  EXPECT_EQ(ops[0].kind, SyncOp::Kind::Create);
  EXPECT_EQ(ops[1].value, "say \"hi\"");
  EXPECT_FALSE(ops[2].hasValue);
}

TEST(TaskModel, SnapshotParser) {
  std::vector<Task> tasks;
  SnapshotParser parser([&](Task& t) { tasks.push_back(t); });
  const std::string json =
      std::string("{\"") + ID1 +
      "\":{\"status\":\"pending\",\"description\":\"x\",\"tag_next\":\"\",\"annotation_1\":\"a\"}}";
  parser.feed(json.data(), json.size());
  ASSERT_TRUE(parser.ok());
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0].status, Status::Pending);
  EXPECT_EQ(tasks[0].tags, "next");
}

TEST(TaskModel, TransformLaterUpdateWins) {
  const SyncOp a = update(ID1, "status", "completed", "2026-10-16T00:00:01Z");
  const SyncOp b = update(ID1, "status", "deleted", "2026-10-16T00:00:02Z");
  SyncOp aOut, bOut;
  bool keepA, keepB;
  transform(a, b, aOut, keepA, bOut, keepB);
  EXPECT_FALSE(keepA);
  EXPECT_TRUE(keepB);
  SyncOp del;
  del.kind = SyncOp::Kind::Delete;
  del.uuid = uuid(ID1);
  transform(a, del, aOut, keepA, bOut, keepB);
  EXPECT_FALSE(keepA);
  EXPECT_TRUE(keepB);
}

TEST(TaskModel, ClosedTasksArchiveAndReopen) {
  TaskStore store;
  MemArchive archive;
  SyncOp create;
  create.kind = SyncOp::Kind::Create;
  create.uuid = uuid(ID1);
  store.apply(create, archive);
  store.apply(update(ID1, "status", "pending", "2026-01-01T00:00:00Z"), archive);
  store.apply(update(ID1, "description", "Buy milk", "2026-01-01T00:00:00Z"), archive);
  store.apply(update(ID1, "status", "completed", "2026-01-01T00:00:01Z"), archive);
  ASSERT_TRUE(store.compact(archive));
  EXPECT_TRUE(store.tasks().empty());
  EXPECT_EQ(archive.lines.size(), 1u);

  // Updates to closed tasks are ignored until one reopens it.
  store.apply(update(ID1, "priority", "H", "2026-01-01T00:00:02Z"), archive);
  EXPECT_TRUE(store.tasks().empty());
  store.apply(update(ID1, "status", "pending", "2026-01-01T00:00:03Z"), archive);
  ASSERT_EQ(store.tasks().size(), 1u);
  EXPECT_EQ(store.tasks()[0].description, "Buy milk");
}

TEST(TaskModel, SyncConfigFromTaskrc) {
  SyncConfig c;
  ASSERT_TRUE(
      SyncConfig::parse("# comment\nsync.server.url=https:\\/\\/taskwarriorsync.com\n"
                        "sync.server.client_id=0666d464-418a-4a08-ad53-6f15c78270cd\n"
                        "sync.encryption_secret=two words\n",
                        c));
  EXPECT_EQ(c.url, "https://taskwarriorsync.com");
  EXPECT_EQ(c.secret, "two words");
  EXPECT_FALSE(SyncConfig::parse("sync.server.url=x\n", c));
}
