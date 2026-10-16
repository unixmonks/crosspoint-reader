# Tasks (Taskwarrior sync)

**Tasks** on the home screen shows your [Taskwarrior](https://taskwarrior.org) 3 tasks, synced through any
TaskChampion sync server such as [taskwarriorsync.com](https://taskwarriorsync.com). The device is a
TaskChampion replica: it decrypts your tasks locally, and the server only ever sees ciphertext.

## Setup

Copy the three sync lines from your `~/.taskrc` into a file named `taskwarrior.conf` at the root of
the SD card (the File Transfer web page can upload it):

```
sync.server.url=https://taskwarriorsync.com
sync.server.client_id=<your client id>
sync.encryption_secret=<your secret>
```

`.taskrc`'s escaped slashes (`https:\/\/...`) are accepted as is.

To filter by [context](https://taskwarrior.org/docs/context/), add your context lines too:

```
context.work.read=project:work
context.personal.read=project.not:work project.not:fun
```

The **Context** row under Sync Now then cycles through All and each context; the choice is
remembered. Filters support `project:X` (including subprojects), `project.not:X`, `+tag`, `-tag`,
`and`, and bare words (description contains). Other attributes such as `due:` are ignored. A
filter with `or` or parentheses is marked as not supported and shows all tasks.

## Use

- **Sync Now** connects to Wi-Fi, pulls changes, uploads your edits, and shows the last sync time
  and how many edits are waiting to upload. The first sync derives the encryption key
  (PBKDF2, 600,000 rounds), which takes a while once; the key is then cached on the SD card.
- The list shows pending tasks by Taskwarrior's urgency. The last row switches to **Waiting** tasks
  (a `wait` date in the future).
- Select a task for **Done**, **Start/Stop**, **Wait 1 Day**, **Wait 1 Week** (or **Stop Waiting**),
  and **Delete**. Edits work offline and are uploaded on the next sync. They need the clock to be
  set, which happens on any sync.

## Storage

Everything lives in `/.crosspoint/tasks/` on the SD card:

| File | Contents |
| --- | --- |
| `state` | last synced version and time |
| `key` | derived encryption key (treat like the secret) |
| `open.tsv` | open tasks: the fields the list shows |
| `archive.tsv` | completed and deleted tasks, in case another device reopens one |
| `pending.jsonl` | edits not uploaded yet |
| `context` | the selected context |

Delete the folder to start over; the next sync fetches everything again. The device never uploads
snapshots (it does not keep every property), so it ignores the server's snapshot requests; your
computer's `task sync` provides them.

## Code

- `lib/TaskSync/` — portable sync engine: crypto (`TwCrypto`), task model, operation transform and
  JSON parsing (`TaskModel`), and the sync protocol (`TaskSyncEngine`). Host tests: `test/task_sync`.
- `src/tasks/TaskwarriorPlatform.*` — SD card and HTTPS bindings.
- `src/activities/tasks/TasksActivity.*` — the screen.
