# Signal-Driven Linux Service Controller

## Files

| File | Content |
|---|---|
| `server_controller.c` | Service source |
| `demo_terminal.txt` | Terminal demonstration covering all four signals |
| `server_controller.log` | Timestamped log written by the service (one line per event) |

## Compile and Test

```bash
gcc -Wall -Wextra -o server_controller server_controller.c
./server_controller
```

In a second terminal:

```bash
pidof server_controller
kill -SIGUSR1 <pid>
kill -SIGUSR2 <pid>
kill -SIGUSR1 <pid>
kill -SIGINT <pid>
```

Test SIGTERM on a fresh instance: start `./server_controller`, then run `kill -SIGTERM <pid>` and `echo $?` in terminal 1.

## Demonstration (excerpt from `demo_terminal.txt`)

```
[Server Controller] Started. PID=35289, state=NORMAL, monitoring every 5 seconds.
[Server Controller] Service running normally...
17:57:38 [terminal 2] $ kill -SIGUSR1 35289
[Server Controller] STATUS REPORT: state=NORMAL, PID=35289, uptime=7s
17:57:38 [terminal 2] $ kill -SIGUSR2 35289
[Server Controller] Mode change (SIGUSR2): NORMAL -> MAINTENANCE
[Server Controller] Service in MAINTENANCE mode - non-essential tasks suspended...
17:57:44 [terminal 2] $ kill -SIGUSR1 35289
[Server Controller] STATUS REPORT: state=MAINTENANCE, PID=35289, uptime=13s
17:57:44 [terminal 2] $ kill -SIGUSR2 35289
[Server Controller] Mode change (SIGUSR2): MAINTENANCE -> NORMAL
17:57:50 [terminal 2] $ kill -SIGINT 35289
[Server Controller] Graceful shutdown requested (SIGINT). Stopping monitoring...
[Server Controller] Summary: uptime=19s, monitoring ticks=3, status reports=3, mode changes=2, final state=NORMAL
[Server Controller] Cleanup complete. Closing log file. Goodbye.
$ echo $?
0
17:57:57 [terminal 2] $ kill -SIGTERM 35408
[Server Controller] EMERGENCY TERMINATION requested (SIGTERM). Exiting immediately, cleanup skipped.
$ echo $?
143
```

| Signal | Observed response | State check |
|---|---|---|
| SIGUSR1 | Status report showing state, PID and uptime | Reports NORMAL → MAINTENANCE → NORMAL across the three requests |
| SIGUSR2 | `NORMAL -> MAINTENANCE`, then `MAINTENANCE -> NORMAL` | The periodic message changes to the MAINTENANCE text |
| SIGINT | Shutdown message, summary, log closed | Exit status 0 |
| SIGTERM | Emergency message, immediate exit | Exit status 143 (128 + 15) |

## Technical Explanation

**Signal registration and handling.**
- `install_handler()` calls `sigaction()` for SIGUSR1, SIGUSR2, SIGINT and SIGTERM, with an empty `sa_mask`.
- Each handler performs a single async-signal-safe action: it sets its own `volatile sig_atomic_t` flag (`flag_status`, `flag_toggle`, `flag_shutdown`, `flag_terminate`).
- No `printf`, `malloc` or file I/O runs inside a handler.
- strace confirms the registration:
  ```
  rt_sigaction(SIGUSR1, {sa_handler=0x...409, ...}) = 0
  rt_sigprocmask(SIG_BLOCK, [INT USR1 USR2 TERM], [], 8) = 0
  ```

**Safe processing through flags.**
- The four signals are blocked with `sigprocmask()` everywhere except inside `pselect()`, which atomically installs the original, empty mask only while it sleeps until the next 5-second tick.
- A signal can therefore interrupt only that wait (`pselect6(...) = ? ERESTARTNOHAND`, followed by `--- SIGUSR1 ---`), never the middle of `printf` or a state update.
- No signal can be lost between checking the flags and going to sleep.
- After each wake-up, the main loop checks the flags in priority order (terminate, shutdown, status, toggle), clears each one, and does the actual work with ordinary functions.
- The tick deadline uses `CLOCK_MONOTONIC`, so signals do not shift the 5-second schedule.

**Operational state.**
- The state is an enum, `service_state_t { STATE_NORMAL, STATE_MAINTENANCE }`, changed only by the main loop.
- On `flag_toggle` the state flips, `mode_changes` is incremented, and the transition is printed.
- The periodic message and the status report read the current state, so a change is visible from the next output line onward.
- Every event is also written to `server_controller.log` with a timestamp, PID and state.

**SIGINT vs SIGTERM.**
- *SIGINT* means graceful shutdown. The loop stops monitoring, prints an uptime and activity summary, flushes and closes the log with `fclose`, and returns `EXIT_SUCCESS` (0), so normal `exit` processing runs.
- *SIGTERM* means emergency termination. The service prints a single message and calls `_exit(143)`. This skips the summary, `fclose` and `atexit` processing, and uses the conventional 128 + 15 status for "terminated by SIGTERM".
- Because both signals are handled, neither kills the process mid-operation. SIGKILL cannot be caught and would bypass both paths.

**Limitation.** Standard signals are not queued. Two SIGUSR2 signals sent before the loop runs are merged into one pending signal, so they toggle the state only once.
