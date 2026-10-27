# Linux System Call Forensics and Execution Analysis

## Files

| File | Content |
|---|---|
| `backup.c` | Backup simulation source |
| `trace_success.txt` | Experiment A: full trace, successful backup |
| `trace_failed.txt` | Experiment B: full trace, destination directory not writable |
| `trace_failed_missing.txt` | Experiment B (variant): source file missing |
| `trace_failed_child.txt` | Experiment B (variant): source unreadable, failure inside the child |
| `trace_success_filtered.txt` | Experiment A: only file, descriptor and process calls |
| `trace_summary.txt` | Experiment A: per-syscall counts (`strace -c`) |

## Task 1: Program

`backup <source_file> <backup_dir>` performs these steps:

1. `stat()` checks that the source is a regular file.
2. `mkdir(dir, 0755)` creates the backup directory. `EEXIST` is treated as success.
3. `open(dir/backup.log, O_WRONLY|O_CREAT|O_APPEND)` opens the log. Each entry is `P#nnn`/`C#nnn` (parent or child, sequence number) with a `time()` timestamp and the PID.
4. `fork()` creates a child, which performs the copy with `open`/`read`/`write`/`close`. The child loops on `read` and on partial `write`, retries on `EINTR`, and calls `unlink`s on a partial destination after a read or write error. It ends with `_exit(code)` (10 = cannot open the source, 11 = cannot create the destination, 12 = read error, 13 = write error, 14 = close error).
5. The parent calls `waitpid()` and decodes the status with `WIFEXITED`/`WEXITSTATUS`, then verifies the copy by comparing `stat()` sizes.
6. Every step prints `[ OK ]` or `[FAIL]` with `strerror(errno)`. All descriptors are closed and the buffer is freed.

## Task 2: Tracing Commands

```bash
gcc -Wall -Wextra -o backup backup.c
strace -f -y -o trace_success.txt ./backup source.txt backup_dir
strace -f -y -o trace_failed.txt ./backup source.txt readonly_dir
strace -f -y -e trace=%file,%desc,%process -o trace_success_filtered.txt ./backup source.txt backup_dir
strace -f -c -o trace_summary.txt ./backup source.txt backup_dir
```

| Option | Effect |
|---|---|
| `-f` | Follow the forked child. Every line is prefixed with its PID. |
| `-y` | Print the path behind each file descriptor |
| `-o` | Write the trace to a file |
| `-e trace=%file,%desc,%process` | Keep only file, descriptor and process calls |
| `-c` | Count calls and errors per syscall |

## Task 3: File-Related System Calls

Excerpts come from `trace_success.txt`. Paths are shortened to `.`. The parent is PID 24752 and the child is PID 24753.

**Calls made by the program.** These match `backup.c`:

```
24752 newfstatat(AT_FDCWD<.>, "source.txt", {st_mode=S_IFREG|0664, st_size=37, ...}, 0) = 0
24752 mkdir("backup_dir", 0755)         = 0
24752 openat(AT_FDCWD<.>, "backup_dir/backup.log", O_WRONLY|O_CREAT|O_APPEND, 0644) = 3
24753 openat(AT_FDCWD<.>, "source.txt", O_RDONLY) = 4
24753 openat(AT_FDCWD<.>, "backup_dir/source.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 5
24753 read(4<./source.txt>, "Quarterly report\nline two\nline t"..., 4096) = 37
24753 write(5<./backup_dir/source.txt>, "Quarterly report\nline two\nline t"..., 37) = 37
24753 read(4<./source.txt>, "", 4096) = 0
24753 close(5<./backup_dir/source.txt>) = 0
24753 close(4<./source.txt>) = 0
```

- **`newfstatat`**: `stat()` in glibc becomes `newfstatat(AT_FDCWD, path, buf, 0)`. It returned 0, `S_IFREG` and 37 bytes. A second call after the child exits verifies that the destination size is also 37.
- **`mkdir`**: path `backup_dir`, mode `0755`. It returns 0 when the directory is created, or `-1 EEXIST` when it already exists, as in Experiment B.
- **`openat`**: `open()` becomes `openat(AT_FDCWD, ...)`.
  - The log uses `O_APPEND`, so parent and child writes never overwrite each other.
  - The source uses `O_RDONLY`.
  - The destination uses `O_CREAT|O_TRUNC` with mode `0644`.
  - The calls returned the lowest free descriptors: 3, 4 and 5.
- **`read`**: requests 4096 bytes (`BUF_SIZE`). The first call returns 37 (the whole file). The second returns `0` (EOF), which ends the copy loop.
- **`write`**: writes exactly the 37 bytes read to fd 5. The log writes (fd 3) and console writes (fd 1 `/dev/pts`, fd 2 for errors) are also visible.
- **`close`**: each returns 0. The child closes 5, 4 and its inherited copy of 3. The parent closes 3 at the end.

**Calls from runtime initialisation (the loader and libc), not from `backup.c`:**
- `execve`
- `access("/etc/ld.so.preload", R_OK) = -1 ENOENT`: the loader checking for forced preloads. This is the only `access` call.
- `openat("/etc/ld.so.cache")`, `openat(".../libc.so.6")`, `read(...,832)` (ELF header), `pread64`
- `fstat`, `mmap`/`mprotect`/`munmap`
- `brk`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `rseq`, `prlimit64`, `getrandom`
- `fstat(1</dev/pts/2>)`: stdio checking whether stdout is a terminal on the first `printf`

## Task 4: Process-Related System Calls

```
24752 rt_sigprocmask(SIG_BLOCK, ~[], [], 8) = 0
24752 clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, ...) = 24753
24752 wait4(24753 <unfinished ...>
24753 openat(AT_FDCWD<.>, "source.txt", O_RDONLY) = 4
...
24753 exit_group(0)                     = ?
24753 +++ exited with 0 +++
24752 <... wait4 resumed>, [{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 24753
24752 --- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=24753, si_status=0, ...} ---
24752 exit_group(0)                     = ?
```

- **`execve`**: appears once, when the shell or strace starts `./backup`. The program itself never calls `exec`.
- **`clone`**: glibc implements `fork()` as `clone()` with `SIGCHLD`, with no shared memory and no `CLONE_VM`. It returns the child PID 24753 in the parent. The child has no `execve`, so it runs the same program image. It inherits fd 3 (the log), which is why its log write appears on `3<./backup_dir/backup.log>`.
- **`rt_sigprocmask`**: glibc blocks every signal around `clone` so the child starts in a consistent state. This is library behaviour, not program code.
- **`wait4`**: `waitpid(pid, &status, 0)` becomes `wait4(24753, ..., 0, NULL)`. The parent blocks (`<unfinished ...>`) while the child's calls run. It resumes when the child exits, and the kernel delivers `SIGCHLD`.
- **`exit_group`**: `_exit(rc)` in the child and `return` from `main` in the parent both end with `exit_group(status)`. The child's status (0 or 10) is exactly what `wait4` reports to the parent.

## Task 5: Successful vs Failed Execution

| | A: success | B: read-only destination | B variant: missing source | B variant: unreadable source |
|---|---|---|---|---|
| Command | `./backup source.txt backup_dir` | `./backup source.txt readonly_dir` (mode 555) | `./backup missing.txt backup_dir` | `./backup locked.txt backup_dir` (mode 000) |
| First failing call | none | `openat("readonly_dir/backup.log", O_WRONLY\|O_CREAT\|O_APPEND, 0644) = -1 EACCES` | `newfstatat(..., "missing.txt", ...) = -1 ENOENT` | child: `openat(..., "locked.txt", O_RDONLY) = -1 EACCES` |
| `clone` / `wait4` | yes / status 0 | not reached | not reached | yes / `WEXITSTATUS(s) == 10` |
| Data copied | `read` 37 → `write` 37 | none | none | none |
| Final call | `exit_group(0)` | `exit_group(1)` | `exit_group(1)` | child `exit_group(10)`, parent `exit_group(1)` |

- **Experiment B.** `mkdir("readonly_dir", 0755) = -1 EEXIST` shows that the directory exists. The kernel then refuses to create a file in it, returning `EACCES` because the directory lacks write permission for UID 1000. The program stops before `clone`, so no destination is created and nothing is left half-written. It exits with status 1, as the trace's `exit_group(1)` shows.
- **Missing source.** The trace shows `ENOENT` from `newfstatat`, then `exit_group(1)`, with no directory or file operations. The check up front prevents any further side effects.
- **Unreadable source.** `stat` succeeds even with mode 000, because stat needs only directory search permission. The `EACCES` error therefore appears in the child's `openat`. The child logs it (`write(3, "C#001 ...")`), prints to fd 2, closes fd 3 and exits with code 10. The parent's `wait4` decodes code 10 and logs the failure.

In all three cases the error is handled correctly. The trace shows a single failing syscall, followed directly by a clean exit with no leaked or partially written files.

## Task 6: System Call Summary

| System Call | Purpose | Relevant Arguments | Return value or error | Role in the program |
|---|---|---|---|---|
| `newfstatat()` | Get file metadata | `AT_FDCWD`, `"source.txt"`, flags 0 | `0` (`S_IFREG`, 37 B). `-1 ENOENT` for `missing.txt` | Validate the source; verify the copy size |
| `mkdir()` | Create a directory | `"backup_dir"`, `0755` | `0`. `-1 EEXIST` for `readonly_dir` | Create the backup directory |
| `openat()` | Open or create a file | `O_WRONLY\|O_CREAT\|O_APPEND` (log), `O_RDONLY` (src), `O_WRONLY\|O_CREAT\|O_TRUNC, 0644` (dst) | fds `3`, `4`, `5`. `-1 EACCES` for the read-only dir and for `locked.txt` | Open the log, source and destination |
| `read()` | Read from an fd | fd 4, buffer, `4096` | `37`, then `0` (EOF) | Read the source content |
| `write()` | Write to an fd | fd 5 with 37 B; fd 3 log lines; fd 1/2 console | `37`, `75`, `50`, `73` | Copy data, append the log, report status |
| `close()` | Release an fd | fds 5, 4, 3 | `0` | Free descriptors in the child and parent |
| `clone()` | Create a process (`fork`) | `flags=...\|SIGCHLD`, `child_stack=NULL` | child PID `24753` | Start the copy worker |
| `wait4()` | Wait for a child | pid `24753`, options 0 | `24753`, status `0` or `10` | Synchronise and collect the result |
| `exit_group()` | End the process | status | child `0`/`10`, parent `0`/`1` | Report the outcome to the parent and the shell |
| `getpid()` | Get the process ID | none | `24752` / `24753` | PID field in each log entry |
| `access()` | Check permissions | `"/etc/ld.so.preload"`, `R_OK` | `-1 ENOENT` | Loader only; not called by the program |

## Task 7: Analysis

**File interaction.**
- Every file operation is a kernel request on a numbered descriptor. `openat` turns a path into an fd (3, 4, 5).
- The kernel checks permissions at open time: `EACCES` appeared at `openat`, never at `read` or `write`.
- `read`/`write` move bytes between the user buffer and the kernel. The copy needed one 37-byte `read`/`write` pair and an EOF `read` returning 0.
- `close` releases each fd, and every opened fd has a matching `close`.
- `O_APPEND` on the log lets two processes share fd 3 safely. Their entries `P#001`, `C#001`, `P#002` appear in order in `backup_dir/backup.log`.

**Process creation and synchronisation.**
- `fork()` appears as `clone(...SIGCHLD) = 24753`. From then on, `strace -f` interleaves PIDs 24752 and 24753.
- The parent's `wait4(24753 <unfinished ...>` stays blocked through all of the child's file calls. It resumes only after `24753 +++ exited with 0 +++`.
- This proves the copy finished before the parent's verification `newfstatat` on `backup_dir/source.txt`.

**Success vs failure.**
- The successful trace reaches `clone`, `read`/`write` and `exit_group(0)`.
- Each failed trace diverges at one call returning `-1` with an errno: `EACCES` on the log `openat`, `ENOENT` on `newfstatat`, or `EACCES` on the child's source `openat`.
- In each failed trace, the next program-level calls are only the error report `write` and `exit_group` with a non-zero code.

**Unexpected or unnecessary calls.**
- About 37 of the 62 counted calls (`strace -c`) come from the loader, libc start-up and glibc internals. Only about 25 come directly from `backup.c`.
- `getpid()` is issued for every log line (3 calls), although the PID could be cached once per process.
- `rt_sigprocmask` around `clone` and `fstat(1)` come from glibc internals.
- `mkdir` is attempted even when the directory exists, which produces `EEXIST`. This is harmless and avoids a check-then-create race.
- `stat` passes for a mode-000 file, so the permission problem is only detected later, in the child.

**Improvement.**
- The child writes directly to the final path and never calls `fsync`. A crash mid-copy could therefore leave a truncated backup that a later run cannot detect.
- Writing to `backup_dir/.source.txt.tmp`, then calling `fsync()` and `rename()` onto the final name, would make each backup atomic and durable.
- Checking `access(dir, W_OK)` or opening the source in the parent before `fork` would report permission errors earlier.
