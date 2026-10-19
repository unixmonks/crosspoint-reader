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
#include <optional>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/WifiPowerSaveGuard.h"

namespace fui = freeink::ui;

namespace {

constexpr int64_t DAY = 86400;
constexpr char CONTEXT_FILE[] = "context";
// E-ink refreshes are slow; redraw key derivation progress at most this often.
constexpr unsigned long PROGRESS_RENDER_MS = 5000;
// Edits made within this time are uploaded together.
constexpr unsigned long UPLOAD_DELAY_MS = 5000;
// Same as WifiSelectionActivity's saved-network attempt.
constexpr unsigned long CONNECT_TIMEOUT_MS = 7000;
constexpr unsigned long MENU_HOLD_MS = 600;
// Menu rows: Sync Now, Show Waiting, then All and the contexts.
constexpr int MENU_FIRST_CONTEXT = 2;
constexpr int STRIP_FONT = UI_10_FONT_ID;
constexpr int STRIP_PAD = 8;

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

// Time only for today, otherwise the date too.
std::string formatSyncTime(const int64_t epoch, const int64_t now) {
  const time_t t = static_cast<time_t>(epoch);
  const time_t n = static_cast<time_t>(now);
  struct tm a, b;
  localtime_r(&t, &a);
  localtime_r(&n, &b);
  char buf[24];
  const bool today = a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
  strftime(buf, sizeof(buf), today ? "%H:%M" : "%b %d %H:%M", &a);
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
  tw::SyncConfig config;
  configured = taskwarrior::loadConfig(config);
  loadContexts();
  rebuildRows();
  updateHeaderStatus();
  requestUpdate();
  if (configured) requestSync(false);
}

void TasksActivity::onExit() {
  Activity::onExit();
  engine.reset();
  net.reset();
  if (wifiActivated) {
    // Reboot to home to undo the heap fragmentation of the Wi-Fi and TLS session.
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

int TasksActivity::listCount() const { return static_cast<int>(menuOpen ? menuItems.size() : rowItems.size()); }

fui::ListNav& TasksActivity::activeNav() { return menuOpen ? menuNav : nav; }

std::string TasksActivity::contextName(const int index) const {
  return index < 0 ? std::string(tr(STR_TASKS_CONTEXT_ALL)) : contexts[index].name;
}

std::string TasksActivity::describe(const tw::Task& task, const int64_t now) const {
  std::string s;
  auto add = [&s](const std::string& part) {
    if (part.empty()) return;
    if (!s.empty()) s += "  ";
    s += part;
  };
  if (task.start) add("* started");
  if (task.priority) add(std::string("!") + task.priority);
  // What the context already guarantees isn't repeated on every row.
  add(contextFilter.shortProject(task.project));
  if (!task.tags.empty()) {
    std::string tags;
    size_t pos = 0;
    while (pos < task.tags.size()) {
      size_t end = task.tags.find(' ', pos);
      if (end == std::string::npos) end = task.tags.size();
      const std::string tag = task.tags.substr(pos, end - pos);
      pos = end + 1;
      if (contextFilter.requiresTag(tag) || tag == tagFilter) continue;
      if (!tags.empty()) tags += ' ';
      tags += '+' + tag;
    }
    add(tags);
  }
  if (task.due) add((task.due < now ? "overdue " : "due ") + formatDate(task.due, now));
  if (view == View::Waiting && task.wait) add("until " + formatDate(task.wait, now));
  return s;
}

// The long form for the menu: errors in full, last sync, unsent edits.
std::string TasksActivity::syncSummary() const {
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
  if (contextIndex >= 0 && !contextFilter.supported()) {
    s += std::string("  ") + contexts[contextIndex].name + " " + tr(STR_TASKS_CONTEXT_UNSUPPORTED);
  }
  return s;
}

void TasksActivity::updateHeaderStatus() {
  std::string s;
  if (netState == NetState::Connecting) {
    s = tr(STR_TASKS_CONNECTING);
  } else if (netState == NetState::Syncing) {
    s = tr(STR_TASKS_SYNCING);
    if (syncPercent >= 0) s += " " + std::to_string(syncPercent) + "%";
  } else if (!engine) {
    s = lastError;
  } else if (syncFailed) {
    s = tr(STR_TASKS_SYNC_FAILED);
  } else if (!lastError.empty()) {
    s = lastError;
  } else if (engine->pendingCount()) {
    s = std::to_string(engine->pendingCount()) + " " + tr(STR_TASKS_CHANGES_TO_UPLOAD);
  } else if (offline) {
    s = tr(STR_TASKS_OFFLINE);
  } else if (engine->hasSynced() && engine->lastSync()) {
    s = std::string(tr(STR_TASKS_SYNCED)) + " " + formatSyncTime(engine->lastSync(), nowOrZero());
  } else if (configured) {
    s = tr(STR_TASKS_NEVER_SYNCED);
  }
  RenderLock lock(*this);
  headerStatus = std::move(s);
}

void TasksActivity::rebuildRows() {
  std::vector<const tw::Task*> visible;
  int waiting = 0;
  const int64_t now = nowOrZero();
  // Tags of the tasks this context and view show, with how many use each.
  std::vector<std::pair<std::string, int>> tagCounts;
  if (engine) {
    for (const auto& task : engine->store().tasks()) {
      if (task.status != tw::Status::Pending || !contextFilter.matches(task)) continue;
      const bool isWaiting = task.isWaiting(now);
      if (isWaiting) ++waiting;
      if (isWaiting != (view == View::Waiting)) continue;
      size_t pos = 0;
      while (pos < task.tags.size()) {
        size_t end = task.tags.find(' ', pos);
        if (end == std::string::npos) end = task.tags.size();
        const std::string tag = task.tags.substr(pos, end - pos);
        pos = end + 1;
        if (tag.empty() || contextFilter.requiresTag(tag)) continue;
        auto it = std::find_if(tagCounts.begin(), tagCounts.end(), [&tag](const auto& tc) { return tc.first == tag; });
        if (it == tagCounts.end()) {
          tagCounts.emplace_back(tag, 1);
        } else {
          ++it->second;
        }
      }
      visible.push_back(&task);
    }
  }
  std::stable_sort(tagCounts.begin(), tagCounts.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  // A tag no task here uses any more (done, or edited elsewhere) stops filtering.
  int activeTag = 0;
  for (size_t i = 0; i < tagCounts.size(); ++i) {
    if (tagCounts[i].first == tagFilter) activeTag = static_cast<int>(i) + 1;
  }
  if (activeTag == 0) tagFilter.clear();
  if (!tagFilter.empty()) {
    visible.erase(std::remove_if(visible.begin(), visible.end(),
                                 [this](const tw::Task* task) { return !task->hasTag(tagFilter); }),
                  visible.end());
  }
  if (view == View::Next) {
    std::stable_sort(visible.begin(), visible.end(),
                     [now](const tw::Task* a, const tw::Task* b) { return a->urgency(now) > b->urgency(now); });
  } else {
    std::stable_sort(visible.begin(), visible.end(),
                     [](const tw::Task* a, const tw::Task* b) { return a->wait < b->wait; });
  }

  // Rows, items and strip labels are read by the render task.
  RenderLock lock(*this);
  waitingCount = waiting;
  tagLabels.clear();
  tagLabels.reserve(tagCounts.size() + 1);
  tagLabels.push_back(tr(STR_TASKS_TAG_ANY));
  for (const auto& tc : tagCounts) tagLabels.push_back("+" + tc.first);
  tagActive = activeTag;
  if (focus == Focus::Tags && tagLabels.size() < 2) focus = contexts.empty() ? Focus::List : Focus::Contexts;
  rows.clear();
  rows.reserve(visible.size() ? visible.size() : 1);
  for (const auto* task : visible) {
    rows.push_back(
        Row{task->uuid, task->description.empty() ? "(no description)" : task->description, describe(*task, now)});
  }
  if (rows.empty()) {
    // Placeholder; selecting it opens the menu (rows[0].uuid stays nil).
    rows.push_back(Row{tw::Uuid{}, tr(STR_TASKS_EMPTY), tr(STR_TASKS_MENU_HINT)});
  }
  rowItems.clear();
  rowItems.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    fui::ListItem item;
    item.label = rows[i].label.c_str();
    item.subtitle = rows[i].subtitle.empty() ? nullptr : rows[i].subtitle.c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
  if (nav.selected >= listCount()) nav.selected = listCount() - 1;
  if (nav.selected < 0) nav.selected = 0;
  nav.follow(listCount());
}

int TasksActivity::headerHeight() const {
  // The short band the home screen uses: title and status share one line.
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.homeTopPadding - metrics.topPadding;
}

int TasksActivity::stripLineHeight() const { return renderer.getLineHeight(STRIP_FONT) + 8; }

int TasksActivity::contextStripHeight() const { return contexts.empty() || menuOpen ? 0 : stripLineHeight(); }

int TasksActivity::tagStripHeight() const { return tagLabels.size() < 2 || menuOpen ? 0 : stripLineHeight(); }

void TasksActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* title = view == View::Next ? tr(STR_TASKS) : tr(STR_TASKS_WAITING);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), headerHeight()}, title,
                 headerStatus.empty() ? nullptr : headerStatus.c_str());
  const int y = metrics.topPadding + headerHeight();
  drawStrip(y, contextStripHeight(), contextLabels, contextIndex + 1, focus == Focus::Contexts);
  drawStrip(y + contextStripHeight(), tagStripHeight(), tagLabels, tagActive, focus == Focus::Tags);
}

// One line of names; the active one is inverted and the line scrolls so it
// stays visible. "<" and ">" mark names cut off at either side. A frame marks
// the strip Left/Right currently change.
void TasksActivity::drawStrip(const int y, const int height, const std::vector<std::string>& labels, const int active,
                              const bool focused) const {
  if (height <= 0) return;  // hidden, or the menu is open
  const int width = renderer.getScreenWidth();
  const int margin = 6;
  const int arrowWidth = renderer.getTextWidth(STRIP_FONT, "<") + 6;
  const int count = static_cast<int>(labels.size());
  auto itemWidth = [this, &labels](const int i) {
    return renderer.getTextWidth(STRIP_FONT, labels[i].c_str()) + 2 * STRIP_PAD;
  };
  // Scroll right until the active name fits, leaving room for both markers.
  int first = 0;
  for (;;) {
    int extent = first > 0 ? arrowWidth : 0;
    for (int i = first; i <= active; ++i) extent += itemWidth(i);
    if (extent <= width - 2 * margin - arrowWidth || first >= active) break;
    ++first;
  }
  const int textY = y + (height - renderer.getLineHeight(STRIP_FONT)) / 2;
  int x = margin;
  if (first > 0) {
    renderer.drawText(STRIP_FONT, x, textY, "<");
    x += arrowWidth;
  }
  for (int i = first; i < count; ++i) {
    const int w = itemWidth(i);
    const bool last = i == count - 1;
    if (x + w > width - margin - (last ? 0 : arrowWidth)) {
      renderer.drawText(STRIP_FONT, width - margin - arrowWidth + 6, textY, ">");
      break;
    }
    if (i == active) {
      renderer.fillRect(x, y + 3, w, height - 6, true);
      renderer.drawText(STRIP_FONT, x + STRIP_PAD, textY, labels[i].c_str(), false);
    } else {
      renderer.drawText(STRIP_FONT, x + STRIP_PAD, textY, labels[i].c_str());
    }
    x += w;
  }
  if (focused) {
    renderer.drawRect(1, y, width - 2, height, 2, true);
  } else {
    renderer.drawLine(0, y + height - 1, width - 1, y + height - 1);
  }
}

void TasksActivity::drawFooter() {
  const bool hasTags = tagLabels.size() > 1;
  if ((contexts.empty() && !hasTags) || menuOpen) {
    UiListActivity::drawFooter();
    return;
  }
  // Left/Right change the focused strip; from the list, the context strip.
  const bool tags = focus == Focus::Tags || (focus == Focus::List && contexts.empty());
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), (tags ? prevTagHint : prevContextHint).c_str(),
                            (tags ? nextTagHint : nextContextHint).c_str());
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TasksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + headerHeight() + contextStripHeight() + tagStripHeight()),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  // Long task descriptions wrap instead of being cut off.
  if (!menuOpen) props.labelText.maxLines = 3;
  syncListViewport(screen, props);
  // No row is highlighted while a strip has focus.
  if (!menuOpen && focus != Focus::List) props.selectedIndex = -1;
  const auto& items = menuOpen ? menuItems : rowItems;
  props.items = items.data();
  props.itemsWindowFirst = 0;
  props.itemsWindowCount = static_cast<uint16_t>(items.size());
  screen.list(props);
}

void TasksActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (menuOpen) {
    menuSelect(index);
    return;
  }
  nav.selected = index;
  if (rows[index].uuid.isNil()) {
    showMenu();
  } else {
    showTaskMenu(index);
  }
}

// The menu replaces the task list (it can be longer than a popup fits):
// Sync Now, Show Waiting, then the contexts with their next-task counts.
void TasksActivity::showMenu() {
  if (popup.isActive() || menuOpen) return;
  const int64_t now = nowOrZero();
  const int n = static_cast<int>(contexts.size());
  std::vector<int> counts(n + 1, 0);
  std::vector<tw::TaskFilter> filters;
  filters.reserve(n);
  for (const auto& context : contexts) filters.push_back(tw::TaskFilter::parse(context.filter));
  if (engine) {
    for (const auto& task : engine->store().tasks()) {
      if (task.status != tw::Status::Pending || task.isWaiting(now)) continue;
      ++counts[0];
      for (int i = 0; i < n; ++i) {
        if (filters[i].matches(task)) ++counts[i + 1];
      }
    }
  }

  RenderLock lock(*this);
  menuRows.clear();
  menuRows.reserve(MENU_FIRST_CONTEXT + n + 1);
  menuRows.push_back(Row{tw::Uuid{}, tr(STR_TASKS_SYNC_NOW), syncSummary()});
  menuRows.push_back(Row{tw::Uuid{}, view == View::Next ? tr(STR_TASKS_SHOW_WAITING) : tr(STR_TASKS_SHOW_NEXT),
                         view == View::Next ? std::to_string(waitingCount) : std::string()});
  if (n > 0) {
    for (int i = -1; i < n; ++i) {
      menuRows.push_back(Row{tw::Uuid{}, contextName(i), std::to_string(counts[i + 1])});
    }
  }
  menuItems.clear();
  menuItems.reserve(menuRows.size());
  for (size_t i = 0; i < menuRows.size(); ++i) {
    fui::ListItem item;
    item.label = menuRows[i].label.c_str();
    // Row 0 shows the sync summary underneath; the others show a count on the right.
    if (i == 0) {
      item.subtitle = menuRows[i].subtitle.empty() ? nullptr : menuRows[i].subtitle.c_str();
    } else {
      item.value = menuRows[i].subtitle.empty() ? nullptr : menuRows[i].subtitle.c_str();
    }
    if (static_cast<int>(i) == MENU_FIRST_CONTEXT) item.sectionHeading = tr(STR_TASKS_CONTEXT);
    item.actionValue = static_cast<int16_t>(i);
    menuItems.push_back(item);
  }
  menuOpen = true;
  menuNav.reset();
  menuNav.selected = n > 0 ? MENU_FIRST_CONTEXT + contextIndex + 1 : 0;
  menuNav.follow(static_cast<int>(menuItems.size()));
  requestUpdate();
}

void TasksActivity::closeMenu() {
  {
    RenderLock lock(*this);
    menuOpen = false;
  }
  requestUpdate();
}

void TasksActivity::menuSelect(const int index) {
  closeMenu();
  if (index == 0) {
    requestSync(true);
  } else if (index == 1) {
    {
      RenderLock lock(*this);
      view = view == View::Next ? View::Waiting : View::Next;
      nav.selected = 0;
      nav.top = 0;
    }
    rebuildRows();
    requestUpdate();
  } else {
    selectContext(index - MENU_FIRST_CONTEXT - 1);
  }
}

void TasksActivity::showTaskMenu(const int row) {
  tw::Task* task = engine ? engine->store().find(rows[row].uuid) : nullptr;
  if (!task) return;
  const tw::Uuid uuid = task->uuid;
  const bool waiting = view == View::Waiting;
  // Order matches applyEdit().
  const char* options[] = {tr(STR_TASKS_DONE),
                           task->start ? tr(STR_TASKS_STOP) : tr(STR_TASKS_START),
                           waiting ? tr(STR_TASKS_UNWAIT) : tr(STR_TASKS_WAIT_DAY),
                           tr(STR_TASKS_WAIT_WEEK),
                           tr(STR_DELETE),
                           tr(STR_CANCEL)};
  popup.show(tr(STR_TASKS), task->description.c_str(), options, 6, 0,
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
    updateHeaderStatus();
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
  if (ok && configured) syncDueAt = millis() + UPLOAD_DELAY_MS;
  rebuildRows();
  updateHeaderStatus();
  requestUpdate();
}

void TasksActivity::loadContexts() {
  tw::SyncConfig config;
  taskwarrior::loadConfig(config);  // contexts are read even without sync settings
  int index = -1;
  if (auto f = fs.open(CONTEXT_FILE, tw::Fs::Mode::Read)) {
    char buf[64];
    const long n = f->read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    const std::string saved(buf, n > 0 ? static_cast<size_t>(n) : 0);
    for (size_t i = 0; i < config.contexts.size(); ++i) {
      if (config.contexts[i].name == saved) index = static_cast<int>(i);
    }
  }
  RenderLock lock(*this);
  contexts = std::move(config.contexts);
  contextIndex = index;
  contextFilter = index < 0 ? tw::TaskFilter() : tw::TaskFilter::parse(contexts[index].filter);
  contextLabels.clear();
  for (int i = -1; i < static_cast<int>(contexts.size()); ++i) contextLabels.push_back(contextName(i));
  prevContextHint = std::string("< ") + tr(STR_TASKS_CONTEXT);
  nextContextHint = std::string(tr(STR_TASKS_CONTEXT)) + " >";
  prevTagHint = std::string("< ") + tr(STR_TASKS_TAG);
  nextTagHint = std::string(tr(STR_TASKS_TAG)) + " >";
}

void TasksActivity::selectContext(const int index) {
  {
    RenderLock lock(*this);
    contextIndex = index;
    contextFilter = index < 0 ? tw::TaskFilter() : tw::TaskFilter::parse(contexts[index].filter);
    // The tag may not exist in the new context.
    tagFilter.clear();
    nav.selected = 0;
    nav.top = 0;
  }
  if (index < 0) {
    fs.remove(CONTEXT_FILE);
  } else if (auto f = fs.open(CONTEXT_FILE, tw::Fs::Mode::Write)) {
    f->write(contexts[index].name);
  }
  rebuildRows();
  requestUpdate();
}

bool TasksActivity::handleCustomInput() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  pollNetwork();
  return !menuOpen && handleStripInput();
}

void TasksActivity::setFocus(const Focus next) {
  {
    RenderLock lock(*this);
    focus = next;
  }
  requestUpdate();
}

void TasksActivity::selectTag(const int index) {
  {
    RenderLock lock(*this);
    tagFilter = index > 0 && index < static_cast<int>(tagLabels.size()) ? tagLabels[index].substr(1) : "";
    nav.selected = 0;
    nav.top = 0;
  }
  rebuildRows();
  requestUpdate();
}

// Focus moves list -> tag strip -> context strip with the side Up button
// (from the first task) and back with Down; Left/Right change the focused
// strip, or the context strip while the list has focus.
bool TasksActivity::handleStripInput() {
  using Button = MappedInputManager::Button;
  const bool hasContexts = !contexts.empty();
  const bool hasTags = tagLabels.size() > 1;
  if (!hasContexts && !hasTags) return false;

  const int left = mappedInput.wasPressed(Button::Left) ? -1 : mappedInput.wasPressed(Button::Right) ? 1 : 0;
  if (left != 0) {
    const bool tags = focus == Focus::Tags || (focus == Focus::List && !hasContexts);
    if (tags) {
      const int count = static_cast<int>(tagLabels.size());
      selectTag((tagActive + left + count) % count);
    } else {
      const int count = static_cast<int>(contexts.size()) + 1;  // + All
      selectContext((contextIndex + 1 + left + count) % count - 1);
    }
    return true;
  }
  if (mappedInput.wasPressed(Button::Up)) {
    if (focus == Focus::Tags && hasContexts) {
      setFocus(Focus::Contexts);
    } else if (focus == Focus::List && nav.selected == 0) {
      setFocus(hasTags ? Focus::Tags : Focus::Contexts);
    } else if (focus == Focus::List) {
      return false;
    }
    return true;
  }
  if (focus == Focus::List) return false;
  if (mappedInput.wasPressed(Button::Down)) {
    setFocus(focus == Focus::Contexts && hasTags ? Focus::Tags : Focus::List);
    return true;
  }
  // Select or Back on a strip returns to the list.
  if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Back)) {
    setFocus(Focus::List);
    return true;
  }
  return false;
}

bool TasksActivity::handleButtons() {
  if (!menuOpen && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, MENU_HOLD_MS)) {
    app.clearTapFlash();
    showMenu();
    return true;
  }
  return UiListActivity::handleButtons();
}

void TasksActivity::navigateButtons() {
  if (menuOpen || (contexts.empty() && tagLabels.size() < 2)) {
    UiListActivity::navigateButtons();
    return;
  }
  if (focus != Focus::List) return;
  // Left/Right switch context, so only the side buttons move the selection.
  using Button = MappedInputManager::Button;
  ButtonNavigator::onPress({Button::Down},
                           [this] { moveSelectionTo(ButtonNavigator::nextIndex(nav.selected, listCount())); });
  ButtonNavigator::onPress({Button::Up},
                           [this] { moveSelectionTo(ButtonNavigator::previousIndex(nav.selected, listCount())); });
  buttonNavigator.onContinuous({Button::Down}, [this] {
    moveSelectionTo(ButtonNavigator::nextPageIndex(nav.selected, listCount(), nav.inputPageRows()));
  });
  buttonNavigator.onContinuous({Button::Up}, [this] {
    moveSelectionTo(ButtonNavigator::previousPageIndex(nav.selected, listCount(), nav.inputPageRows()));
  });
}

void TasksActivity::onBackButton() {
  if (menuOpen) {
    closeMenu();
    return;
  }
  // Last try for unsent edits, if Wi-Fi is already up.
  if (engine && configured && engine->pendingCount() && WiFi.status() == WL_CONNECTED) runSync();
  finish();
}

void TasksActivity::requestSync(const bool manual) {
  if (!engine || netState != NetState::Idle) return;
  syncDueAt = 0;
  tw::SyncConfig config;
  configured = taskwarrior::loadConfig(config);
  if (!configured) {
    if (manual) {
      // The popup draws over the last frame; put the list back under it first
      // (a manual sync comes from the menu, which replaces the list).
      requestUpdateAndWait();
      const char* options[] = {tr(STR_OK_BUTTON)};
      popup.showMessage(tr(STR_TASKS_SETUP_TITLE), tr(STR_TASKS_SETUP_MESSAGE), options, 1, 0,
                        [this](int) { requestUpdate(); });
      requestUpdate();
    }
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    runSync();
    return;
  }
  if (startConnect()) return;
  if (manual) {
    // No saved network: let the user pick one.
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) runSync();
                           });
    return;
  }
  offline = true;
  updateHeaderStatus();
  requestUpdate();
}

// Joins the last network WifiSelectionActivity connected to. Returns false
// when there is none.
bool TasksActivity::startConnect() {
  std::optional<WifiCredential> cred;
  {
    RenderLock lock(*this);  // the SD card shares the display's SPI bus
    WIFI_STORE.loadFromFile();
  }
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (!ssid.empty()) cred = WIFI_STORE.findCredential(ssid);
  if (!cred) return false;
  LOG_DBG("TASKS", "Connecting to %s", cred->ssid.c_str());
  wifiActivated = true;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  netState = NetState::Connecting;
  connectStarted = millis();
  updateHeaderStatus();
  requestUpdate();
  return true;
}

void TasksActivity::pollNetwork() {
  if (netState == NetState::Connecting) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      netState = NetState::Idle;
      runSync();
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
               millis() - connectStarted > CONNECT_TIMEOUT_MS) {
      LOG_INF("TASKS", "Wi-Fi connect failed (%d)", static_cast<int>(status));
      netState = NetState::Idle;
      offline = true;
      wifiOff();
      updateHeaderStatus();
      requestUpdate();
    }
    return;
  }
  if (syncDueAt && static_cast<long>(millis() - syncDueAt) >= 0 && !popup.isActive()) requestSync(false);
}

void TasksActivity::wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool TasksActivity::reportProgress(const char* /*phase*/, const int percent) {
  // Only the one-time key derivation is slow enough to need progress.
  if (percent < 0 || millis() - lastProgressRender < PROGRESS_RENDER_MS) {
    delay(1);
    return true;
  }
  syncPercent = percent;
  updateHeaderStatus();
  requestUpdateAndWait();
  lastProgressRender = millis();
  // Long CPU work (key derivation) runs on the loop task; let idle tasks in.
  delay(1);
  return true;
}

// Runs on the loop task: input waits until the sync is done. The list stays
// drawn from the rows' own copies of the task text.
void TasksActivity::runSync() {
  tw::SyncConfig config;
  if (!engine || !taskwarrior::loadConfig(config)) return;
  syncDueAt = 0;
  wifiActivated = true;
  WifiPowerSaveGuard psGuard;
  WiFi.setSleep(false);
  netState = NetState::Syncing;
  syncPercent = -1;
  updateHeaderStatus();
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
  wifiOff();
  netState = NetState::Idle;
  offline = false;
  syncFailed = result != tw::SyncResult::Ok;
  lastError = syncFailed ? std::string(tr(STR_TASKS_SYNC_FAILED)) + ": " + tw::syncResultName(result) : std::string();
  rebuildRows();
  updateHeaderStatus();
  requestUpdate();
}

// The popup draws over the last list frame; redrawing the list under it would
// push a second, popup-less frame to the panel on every highlight move.
void TasksActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
