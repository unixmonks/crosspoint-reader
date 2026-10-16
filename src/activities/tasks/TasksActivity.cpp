#include "TasksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/WifiPowerSaveGuard.h"

namespace fui = freeink::ui;

namespace {

constexpr int64_t DAY = 86400;
// E-ink refreshes are slow; redraw sync progress at most this often.
constexpr unsigned long PROGRESS_RENDER_MS = 2500;

std::string formatDate(const int64_t epoch, const int64_t now) {
  if (!epoch) return "";
  const time_t t = static_cast<time_t>(epoch);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[24];
  // Show the time only when it is not midnight (Taskwarrior's date-only form).
  const bool withTime = tmv.tm_hour != 0 || tmv.tm_min != 0;
  const bool thisYear = epoch > now - 180 * DAY && epoch < now + 180 * DAY;
  strftime(buf, sizeof(buf),
           withTime ? (thisYear ? "%b %d %H:%M" : "%b %d %Y %H:%M") : (thisYear ? "%b %d" : "%b %d %Y"), &tmv);
  return buf;
}

int64_t nowOrZero() { return trustedtime::trustedNow(); }

}  // namespace

TasksActivity::TasksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Tasks", renderer, mappedInput) {}

TasksActivity::~TasksActivity() = default;

void TasksActivity::onEnter() {
  UiListActivity::onEnter();
  net = makeUniqueNoThrow<taskwarrior::HttpsTransport>();
  if (net) engine = makeUniqueNoThrow<tw::TaskSyncEngine>(fs, *net, taskwarrior::fillRandom);
  if (!engine) {
    LOG_ERR("TASKS", "OOM: task engine");
    lastError = "Out of memory";
  } else if (!engine->load()) {
    lastError = "Could not read saved tasks";
  }
  rebuildRows();
  requestUpdate();
}

void TasksActivity::onExit() {
  Activity::onExit();
  engine.reset();
  net.reset();
  if (wifiActivated && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

const char* TasksActivity::headerTitle() const { return view == View::Next ? tr(STR_TASKS) : tr(STR_TASKS_WAITING); }

int TasksActivity::listCount() const { return static_cast<int>(rowItems.size()); }

std::string TasksActivity::describe(const tw::Task& task, const int64_t now) const {
  std::string s;
  auto add = [&s](const std::string& part) {
    if (part.empty()) return;
    if (!s.empty()) s += "  ";
    s += part;
  };
  if (task.start) add("* started");
  if (task.priority) add(std::string("!") + task.priority);
  add(task.project);
  if (!task.tags.empty()) {
    std::string tags;
    size_t pos = 0;
    while (pos < task.tags.size()) {
      size_t end = task.tags.find(' ', pos);
      if (end == std::string::npos) end = task.tags.size();
      if (!tags.empty()) tags += ' ';
      tags += '+';
      tags.append(task.tags, pos, end - pos);
      pos = end + 1;
    }
    add(tags);
  }
  if (task.due) add((task.due < now ? "overdue " : "due ") + formatDate(task.due, now));
  if (view == View::Waiting && task.wait) add("until " + formatDate(task.wait, now));
  return s;
}

std::string TasksActivity::syncSubtitle() const {
  if (!engine) return lastError;
  std::string s;
  if (!lastError.empty()) {
    s = lastError;
  } else if (!engine->hasSynced()) {
    s = tr(STR_TASKS_NEVER_SYNCED);
  } else if (engine->lastSync()) {
    s = std::string(tr(STR_TASKS_LAST_SYNC)) + " " + formatDate(engine->lastSync(), nowOrZero());
  }
  if (engine->pendingCount()) {
    if (!s.empty()) s += "  ";
    s += std::to_string(engine->pendingCount()) + " " + tr(STR_TASKS_CHANGES_TO_UPLOAD);
  }
  return s;
}

void TasksActivity::rebuildRows() {
  // Rows point into the task store, which sync mutates: rebuild under the
  // render lock, and list no tasks while a sync runs.
  RenderLock lock(*this);
  visible.clear();
  waitingCount = 0;
  const int64_t now = nowOrZero();
  if (engine && !syncing) {
    for (const auto& task : engine->store().tasks()) {
      if (task.status != tw::Status::Pending) continue;
      const bool waiting = task.isWaiting(now);
      if (waiting) ++waitingCount;
      if (waiting == (view == View::Waiting)) visible.push_back(&task);
    }
  }
  if (view == View::Next) {
    std::stable_sort(visible.begin(), visible.end(),
                     [now](const tw::Task* a, const tw::Task* b) { return a->urgency(now) > b->urgency(now); });
  } else {
    std::stable_sort(visible.begin(), visible.end(),
                     [](const tw::Task* a, const tw::Task* b) { return a->wait < b->wait; });
  }

  subtitles.clear();
  subtitles.reserve(visible.size());
  for (const auto* task : visible) subtitles.push_back(describe(*task, now));
  syncRowSubtitle = syncSubtitle();
  toggleRowLabel = view == View::Next
                       ? std::string(tr(STR_TASKS_SHOW_WAITING)) + " (" + std::to_string(waitingCount) + ")"
                       : std::string(tr(STR_TASKS_SHOW_NEXT));

  rowItems.clear();
  rowItems.reserve(visible.size() + 2);
  fui::ListItem syncRow;
  syncRow.label = tr(STR_TASKS_SYNC_NOW);
  syncRow.subtitle = syncRowSubtitle.empty() ? nullptr : syncRowSubtitle.c_str();
  syncRow.icon = listIconFor(UIIcon::Wifi, 24);
  syncRow.actionValue = 0;
  rowItems.push_back(syncRow);
  for (size_t i = 0; i < visible.size(); ++i) {
    fui::ListItem row;
    row.label = visible[i]->description.empty() ? "(no description)" : visible[i]->description.c_str();
    row.subtitle = subtitles[i].empty() ? nullptr : subtitles[i].c_str();
    row.actionValue = static_cast<int16_t>(i + 1);
    rowItems.push_back(row);
  }
  fui::ListItem toggleRow;
  toggleRow.label = toggleRowLabel.c_str();
  toggleRow.actionValue = static_cast<int16_t>(rowItems.size());
  rowItems.push_back(toggleRow);

  if (nav.selected >= listCount()) nav.selected = listCount() - 1;
  if (nav.selected < 0) nav.selected = 0;
  nav.follow(listCount());
}

void TasksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  props.items = rowItems.data();
  props.itemsWindowFirst = 0;
  props.itemsWindowCount = static_cast<uint16_t>(rowItems.size());
  screen.list(props);
}

void TasksActivity::activateIndex(const int index) {
  if (syncing || popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  if (index == 0) {
    startSync();
  } else if (index == listCount() - 1) {
    view = view == View::Next ? View::Waiting : View::Next;
    nav.selected = 0;
    rebuildRows();
    requestUpdate();
  } else {
    showTaskMenu(index - 1);
  }
}

void TasksActivity::showTaskMenu(const int taskIndex) {
  if (taskIndex < 0 || taskIndex >= static_cast<int>(visible.size())) return;
  const tw::Task& task = *visible[taskIndex];
  const tw::Uuid uuid = task.uuid;
  const bool waiting = view == View::Waiting;
  // Order matches applyEdit().
  const char* options[] = {tr(STR_TASKS_DONE),
                           task.start ? tr(STR_TASKS_STOP) : tr(STR_TASKS_START),
                           waiting ? tr(STR_TASKS_UNWAIT) : tr(STR_TASKS_WAIT_DAY),
                           tr(STR_TASKS_WAIT_WEEK),
                           tr(STR_DELETE),
                           tr(STR_CANCEL)};
  popup.show(tr(STR_TASKS), task.description.c_str(), options, 6, 0,
             [this, uuid, waiting](const int option) { applyEdit(option, uuid, waiting); });
  requestUpdate();
}

void TasksActivity::applyEdit(const int option, const tw::Uuid& uuid, const bool waiting) {
  if (!engine || option >= 5) {
    requestUpdate();
    return;
  }
  const int64_t now = nowOrZero();
  if (now == 0) {
    // Without a trustworthy clock the edit would lose every conflict.
    lastError = tr(STR_TASKS_CLOCK_UNKNOWN);
    rebuildRows();
    requestUpdate();
    return;
  }
  tw::Task* task = engine->store().find(uuid);
  bool ok = task != nullptr;
  if (ok) {
    switch (option) {
      case 0:
        ok = engine->complete(uuid, now);
        break;
      case 1:
        ok = engine->setStarted(uuid, task->start == 0, now);
        break;
      case 2:
        ok = engine->waitUntil(uuid, waiting ? 0 : now + DAY, now);
        break;
      case 3:
        ok = engine->waitUntil(uuid, now + 7 * DAY, now);
        break;
      case 4:
        ok = engine->remove(uuid, now);
        break;
      default:
        break;
    }
  }
  lastError = ok ? "" : tr(STR_TASKS_SAVE_FAILED);
  rebuildRows();
  requestUpdate();
}

bool TasksActivity::handleCustomInput() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  return syncing;
}

void TasksActivity::startSync() {
  if (!engine) return;
  tw::SyncConfig config;
  if (!taskwarrior::loadConfig(config)) {
    const char* options[] = {tr(STR_OK_BUTTON)};
    popup.showMessage(tr(STR_TASKS_SETUP_TITLE), tr(STR_TASKS_SETUP_MESSAGE), options, 1, 0,
                      [this](int) { requestUpdate(); });
    requestUpdate();
    return;
  }
  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    runSync();
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) runSync();
                         });
}

bool TasksActivity::reportProgress(const char* phase, const int percent) {
  const bool phaseChanged = phase != lastPhase;
  lastPhase = phase;
  if (phaseChanged || millis() - lastProgressRender > PROGRESS_RENDER_MS) {
    {
      RenderLock lock(*this);
      syncStatus = phase;
      if (percent >= 0) syncStatus += " " + std::to_string(percent) + "%";
    }
    requestUpdateAndWait();
    lastProgressRender = millis();
  }
  // Long CPU work (key derivation) runs on the loop task; let idle tasks in.
  delay(1);
  return true;
}

void TasksActivity::runSync() {
  tw::SyncConfig config;
  if (!engine || !taskwarrior::loadConfig(config)) return;
  WifiPowerSaveGuard psGuard;
  WiFi.setSleep(false);
  {
    RenderLock lock(*this);
    syncing = true;
    syncStatus = tr(STR_TASKS_SYNCING);
  }
  rebuildRows();
  lastPhase = nullptr;
  requestUpdateAndWait();
  lastProgressRender = millis();

  // Edits need a real clock for their timestamps.
  trustedtime::syncNow(5000);
  const int64_t now = nowOrZero();
  const tw::SyncResult result =
      engine->sync(config, now, [this](const char* phase, int percent) { return reportProgress(phase, percent); });
  net->close();
  LOG_INF("TASKS", "Sync: %s", tw::syncResultName(result));
  if (result != tw::SyncResult::Ok) {
    // Keep whatever was saved before the failure.
    engine->load();
  }
  {
    RenderLock lock(*this);
    syncing = false;
    lastError =
        result == tw::SyncResult::Ok ? "" : std::string(tr(STR_TASKS_SYNC_FAILED)) + ": " + tw::syncResultName(result);
  }
  rebuildRows();
  requestUpdate();
}

void TasksActivity::drawFooter() {
  UiListActivity::drawFooter();
  if (syncing) GUI.drawPopup(renderer, syncStatus.c_str());
}

void TasksActivity::render(RenderLock&& lock) {
  UiListActivity::render(std::move(lock));
  popup.processRender(renderer, mappedInput);
}
