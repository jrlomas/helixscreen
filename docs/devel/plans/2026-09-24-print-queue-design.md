# Queue a Print From the Detail View - Design

Adding a file to Moonraker's job queue from the print file detail view while
another print runs, carrying that job's pre-print options, and making the queue
reachable in every printer state.

## Why

While a print is active the detail view's Print button is disabled with the
reason "Printing: start after this job", and nothing lets the user do that.
`MoonrakerQueueAPI::add_job()` (`server.job_queue.post_job`) exists and has no UI
caller. The queue is only reachable from the Print Status widget's library card,
which is idle-only, or from the Job Queue home widget, which is off by default.

## Moonraker facts this rests on

- `automatic_transition` defaults to **false**. A finished print never starts the
  next job; the queue waits for someone to start it.
- `post_job` accepts `filenames`, `reset` and the implicit user. No per-job data.
- The queue is in memory. A Moonraker restart empties it and `job_id`s are
  regenerated.
- With `automatic_transition: True`, Moonraker calls `start_print(filename)`
  itself; HelixScreen is not involved.

Pre-print options are applied by rewriting the gcode
(`src/ui/ui_print_preparation_manager.cpp#modify_and_print_streaming`), and the
AMS remap, filament gates and printer-stop check all read from the detail view
(`src/ui/ui_print_start_controller.cpp#gather_print_start_context`). A queued job
therefore cannot be started with its options without the detail view.

## Design

**HelixScreen starts queued jobs, through the detail view.** Starting a queued
job opens the detail view for that file with its saved option states applied;
the user taps Print and the normal pipeline runs. The remap is computed against
the lanes loaded at that moment and every gate runs. The extra tap is where the
user confirms the bed is clear, which `automatic_transition: false` already
requires.

### 1. Add to Queue (detail view)

- When `can_start_new_print()` is false because a print is PRINTING/PAUSED and
  the `job_queue` component is present, the Print button becomes "Add to Queue"
  and stays enabled. Macro analysis still disables it. No `job_queue` component:
  today's disabled button and reason, unchanged.
- Label is subject-bound (`print_file_detail.xml` `print_button` is static text
  today). Mode is decided in `src/ui/ui_panel_print_select.cpp#update_print_button_state`.
- Tap: read the option row states, `post_job` the **original** filename, find
  the new `job_id` in the response's `queued_jobs` (the id absent before the
  call), store the options, toast "Added to queue (position N)", plus "queue
  paused" when it is.
- Filament mapping card is hidden in queue mode: the mapping is not saved, it is
  recomputed when the job starts.
- `automatic_transition: True`: the options card is hidden too, because
  Moonraker will start the job without us. No other behaviour for that mode.

### 2. Per-job option store

- Moonraker database, namespace `helix-screen` (already used by
  `src/printer/tool_state.cpp`), one key `queued_job_options`:
  `{ "<job_id>": { "filename": "...", "options": { "<option_id>": bool } } }`.
- Pruned on every `JobQueueState` refresh: drop entries whose `job_id` is no
  longer queued. That also covers Moonraker restarts.
- Pure helpers for encode/decode/prune so they test without LVGL.

### 3. Starting a queued job (one path, three callers)

`start_queued_job(entry)`:

1. Navigate to PrintSelect, `PrintSelectPanel::select_file_by_name(filename)`.
   Not found: toast "file not found", job stays queued.
2. Seed the option rows from the saved states instead of `default_enabled`.
   Options the file no longer offers are ignored; new ones take their default.
3. Remember the pending `job_id`. When the print start **succeeds**, call
   `remove_jobs({job_id})` and delete its option entry. Backing out of the
   detail view changes nothing.

Callers:

- Job queue modal row tap (`src/ui/ui_job_queue_modal.cpp#start_job`). Replaces
  the current remove-then-`start_print` path, which skips prep, options and the
  gates. The modal's own printer-stop check goes away; the pipeline runs it.
- Completion modal "Start next: X".
- "Up next" line when the printer is idle.

### 4. Completion modal

`ui_xml/print_completion_modal.xml` moves from a single OK button to
`modal_button_row`: OK stays primary, "Start next: X" is the secondary, shown
via `secondary_show_subject` only when the queue has jobs.

No modal appears when the user is on the print status panel at completion
(`src/print/print_completion.cpp#should_notify_print_ended`); the "Up next" line
covers that case.

### 5. "Up next" line

- `JobQueueState` gains `job_queue_next_filename` (display name of the first
  job) next to `job_queue_count`.
- Shown when the queue is non-empty on the print status panel
  (`print_status_extras` in `ui_xml/print_status_panel.xml`) and the home Print
  Status widget's printing view. Text: "Up next: benchy (+2)".
- Tap while printing: open the job queue modal. Tap when a print can start:
  `start_queued_job(first)`.

### 6. automatic_transition

Read once at connect with `server.config` (`config.job_queue.automatic_transition`),
exposed as a bool on `JobQueueState`. Default false when absent or unreadable.

## Mock

`src/api/moonraker_client_mock_queue.cpp` already implements post/delete. Needs:
`post_job` returning `queued_jobs`, a `server.config` job_queue section, and
database get/post for `helix-screen` if the mock lacks it.

## Tests (beside the existing ones)

- Option store: encode/decode round trip, prune drops unqueued ids, unknown
  option ids ignored on seed.
- Button mode: printing + job_queue present -> queue mode; macro analysis ->
  disabled; no job_queue component -> disabled with today's reason.
- `start_queued_job`: job removed only after a successful start; failed start
  and back-out leave it queued; missing file leaves it queued.
  (`tests/unit/test_job_queue_start_guard.cpp` changes with the modal path.)
- Completion modal: "Start next" hidden with an empty queue.
- `job_queue_next_filename` follows the first entry.

## Out of scope

- Baking options into the file for `automatic_transition: True` setups. Hidden
  options are the whole answer there until someone with an eject setup asks.
- Reordering the queue.
