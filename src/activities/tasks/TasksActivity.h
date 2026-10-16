#pragma once

#include <TaskSyncEngine.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "tasks/TaskwarriorPlatform.h"

// Taskwarrior task list synced with a TaskChampion server (taskwarriorsync.com).
// A slim header shows the sync state and a strip under it the Taskwarrior
// contexts; the front Left/Right buttons switch context and the side buttons
// move through the tasks. Selecting a task offers Done / Start / Wait /
// Delete. Holding Select opens a menu to pick a context, show waiting tasks or
// sync. Sync runs on open and a few seconds after an edit, over the last
// saved Wi-Fi network.
class TasksActivity final : public UiListActivity {
 public:
  TasksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~TasksActivity() override;

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Next, Waiting };
  enum class NetState : uint8_t { Idle, Connecting, Syncing };

  // A list row. Rows own their text so the list can be drawn while a sync
  // rewrites the task store.
  struct Row {
    tw::Uuid uuid;
    std::string label;
    std::string subtitle;
  };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  freeink::ui::ListNav& activeNav() override;
  void onBackButton() override;
  void drawChrome() override;
  void drawFooter() override;

  int headerHeight() const;
  int stripHeight() const;
  void drawContextStrip(int y, int height) const;
  std::string contextName(int index) const;

  void rebuildRows();
  void updateHeaderStatus();
  void loadContexts();
  void selectContext(int index);
  void showMenu();
  void closeMenu();
  void menuSelect(int index);
  void showTaskMenu(int row);
  void applyEdit(int option, const tw::Uuid& uuid, bool waiting);
  std::string describe(const tw::Task& task, int64_t now) const;
  std::string syncSummary() const;

  // Sync: requestSync() connects to the saved network without blocking input,
  // pollNetwork() runs the sync once connected.
  void requestSync(bool manual);
  bool startConnect();
  void pollNetwork();
  void runSync();
  void wifiOff();
  bool reportProgress(const char* phase, int percent);

  taskwarrior::SdFs fs;
  std::unique_ptr<taskwarrior::HttpsTransport> net;
  std::unique_ptr<tw::TaskSyncEngine> engine;
  bool configured = false;

  View view = View::Next;
  std::vector<Row> rows;
  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<tw::SyncConfig::Context> contexts;
  int contextIndex = -1;  // -1: all tasks
  tw::TaskFilter contextFilter;
  int waitingCount = 0;
  std::string lastError;
  bool syncFailed = false;

  // Drawn by the render task; written under the render lock.
  std::string headerStatus;
  std::string prevContextHint;
  std::string nextContextHint;

  // The hold-Select menu, shown in place of the task list.
  bool menuOpen = false;
  std::vector<Row> menuRows;
  std::vector<freeink::ui::ListItem> menuItems;
  freeink::ui::ListNav menuNav;

  OptionPopup popup;
  NetState netState = NetState::Idle;
  bool offline = false;
  bool wifiActivated = false;
  unsigned long connectStarted = 0;
  unsigned long syncDueAt = 0;  // 0: no upload scheduled
  int syncPercent = -1;
  unsigned long lastProgressRender = 0;
};
