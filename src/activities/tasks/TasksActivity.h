#pragma once

#include <TaskSyncEngine.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "tasks/TaskwarriorPlatform.h"

// Taskwarrior task list synced with a TaskChampion server (taskwarriorsync.com).
// Row 0 syncs, row 1 picks a Taskwarrior context (when the config defines
// any), then the tasks of the current view, then a row that switches between
// the Next and Waiting views. Selecting a task offers Done / Start /
// Wait / Delete; edits are kept on the SD card until the next sync.
class TasksActivity final : public UiListActivity {
 public:
  TasksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~TasksActivity() override;

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Next, Waiting };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  void rebuildRows();
  void loadContexts();
  void selectContext(int index);
  int firstTaskRow() const { return contexts.empty() ? 1 : 2; }
  void startSync();
  void runSync();
  void showTaskMenu(int taskIndex);
  void applyEdit(int option, const tw::Uuid& uuid, bool waiting);
  bool reportProgress(const char* phase, int percent);
  std::string describe(const tw::Task& task, int64_t now) const;
  std::string syncSubtitle() const;

  taskwarrior::SdFs fs;
  std::unique_ptr<taskwarrior::HttpsTransport> net;
  std::unique_ptr<tw::TaskSyncEngine> engine;

  View view = View::Next;
  // Tasks of the current view, sorted; rows point into these.
  std::vector<const tw::Task*> visible;
  std::vector<std::string> subtitles;
  std::vector<freeink::ui::ListItem> rowItems;
  std::string syncRowSubtitle;
  std::string toggleRowLabel;
  std::string contextRowLabel;
  std::string contextRowSubtitle;
  std::vector<tw::SyncConfig::Context> contexts;
  int contextIndex = -1;  // -1: all tasks
  tw::TaskFilter contextFilter;
  std::string lastError;
  int waitingCount = 0;

  OptionPopup popup;
  bool wifiActivated = false;
  bool syncing = false;
  std::string syncStatus;
  const char* lastPhase = nullptr;
  unsigned long lastProgressRender = 0;
};
