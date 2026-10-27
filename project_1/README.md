# ELF Binary Forensics and Comparative Threat Assessment

Neither target binary was executed. All findings come from `file`, `readelf`, `nm` and `objdump`. Full command outputs are in [outputs/data_sync.txt](outputs/data_sync.txt) and [outputs/data_sync_v2.txt](outputs/data_sync_v2.txt).

## Task 1: Programs and Compilation

| File | Role |
|---|---|
| `data_sync.c` | Lists two directories with `opendir`/`readdir`, compares names with `strcmp`, prints files missing from the destination. |
| `data_sync_v2.c` | Adds logging to `sync_v2.log` (`fopen`, `time`), metadata checks (`stat`: size, mtime) and a rotating-XOR checksum (`fgetc`). |

Compiler: `gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0`

```bash
gcc -std=c11 -g -O0 -o data_sync data_sync.c
gcc -std=c11 -O2 -s -o data_sync_v2 data_sync_v2.c
```

| Option | Effect |
|---|---|
| `-std=c11` | ISO C11 language standard |
| `-g` | Embed DWARF debug information |
| `-O0` / `-O2` | No optimisation / standard optimisation (also enables `_FORTIFY_SOURCE` on Ubuntu) |
| `-s` | Strip `.symtab`/`.strtab` from the output |

Ubuntu's GCC also enables PIE, `-fstack-protector-strong` and `-fcf-protection` by default. This is confirmed by the `DW_AT_producer` string in the `data_sync` debug info. Rebuilding `data_sync_v2` with the command above gives a byte-identical file.

## Task 2: ELF Structure

`file` gives a quick identification. `readelf -h/-S/-l` reads the ELF header, the section header table and the program header table.

- **Class / byte order / machine:** ELF64, little-endian (2's complement), x86-64, for both.
- **Type:** `DYN (Position-Independent Executable)` for both. The code is linked at base 0 and relocated to a random address at load (ASLR).
- **Entry point:** `0x1140` (data_sync) and `0x14f0` (data_sync_v2). Both point to `_start`. In v2 it is later because `-O2` places `main` in `.text.startup`, ahead of `_start`.
- **Program headers (14 in each):**
  - `PHDR` locates the header table itself.
  - `INTERP` names the loader `/lib64/ld-linux-x86-64.so.2`.
  - The four `LOAD` segments are mapped into memory: R (headers/dynsym), R+X (code), R (rodata/eh_frame), RW (data/bss/got).
  - `DYNAMIC` holds the linker metadata.
  - `NOTE` holds the build-id and ABI tag.
  - `GNU_EH_FRAME` points to the unwind table.
  - `GNU_STACK` marks the stack non-executable.
  - `GNU_RELRO` makes the GOT read-only after relocation.
  - `GNU_PROPERTY` carries the CET/IBT properties.
- **Sections:**

| Section | Purpose | data_sync | data_sync_v2 |
|---|---|---|---|
| `.text` | Machine code | 0x39b bytes | 0x524 bytes |
| `.rodata` | Constant strings | 0x6c | 0xd4 |
| `.data` | Initialised writable data | 0x10 | 0x10 |
| `.bss` | Zeroed data (NOBITS, no file space) | 0x20020 | 0x20020 (the two 256×256 static arrays) |
| `.dynsym` / `.dynstr` | Symbols needed for dynamic linking | present | present |
| `.dynamic` | Dynamic linking table | present | present |
| `.symtab` / `.strtab` | Full static symbol table | present | **absent** |
| `.debug_*` | DWARF debug info | present | **absent** |

- **Debug info:** data_sync has `.debug_info`, `.debug_line` and related sections (`file`: *with debug_info, not stripped*). data_sync_v2 has none (`file`: *stripped*). Section count is 37 vs 29.
- **Sections vs segments:** Sections are a link-time view, used by the linker and by tools such as `readelf -S` and `objdump -h`. Segments (program headers) are the run-time view: the loader `mmap`s them with the given permissions. One segment usually contains several sections, as shown by the "Section to Segment mapping" in `readelf -l`. A binary can run with no section table at all, but never without segments.

## Task 3: Symbols and Functions

- `nm -n` lists symbols by address. `nm -D` and `objdump -T` show only dynamic symbols. `objdump -t` shows the static symbol table, `objdump -d` disassembles code, and `objdump -h`/`-p` show sections and the private (dynamic/version) headers.
- **Entry point and startup:** `_start` at the entry address clears `%rbp` and aligns the stack. It then loads the address of `main` into `%rdi` and calls `__libc_start_main@GLIBC_2.34`, which initialises libc, calls `main`, then `exit`.
- **main:**
  - data_sync: `T main` at `0x1398`, plus local `t list_dir` (0x1229) and `t contains` (0x1338). Static arrays `b src.1` and `b dst.0` are in `.bss`.
  - data_sync_v2: `nm` reports *no symbols*. `main` can only be inferred from `_start`, which loads `0x1250` into `%rdi` before calling `__libc_start_main`. Local helpers were inlined by `-O2` and their names were stripped.
- **Undefined / imported functions (`U` in `nm -D`):**
  - data_sync: `opendir`, `readdir`, `closedir`, `strcmp`, `strncpy`, `printf`, `fprintf`, `perror`, `__libc_start_main`.
  - data_sync_v2: all of these, plus `fopen`, `fclose`, `fgetc`, `stat`, `time`, `__snprintf_chk` and `__stack_chk_fail`. `printf`/`fprintf` become the fortified `__printf_chk`/`__fprintf_chk`.
  - Weak (`w`) symbols such as `__gmon_start__` and `_ITM_*` are optional, and they may stay unresolved.
- **Differences:** v2 adds file I/O, metadata and time imports. The `.rodata` strings `sync_v2.log`, `scan started`, `MISSING:`, `CHANGED: %s (size %lld vs %lld)` and `%s/%s` point to logging, path building and size comparison.

## Task 4: Linking and Dependencies

- Both are **dynamically linked**. The evidence:
  - an `INTERP` segment
  - a `.dynamic` section with `NEEDED libc.so.6` only (`readelf -d`, and `objdump -p` "Dynamic Section")
  - `FLAGS BIND_NOW` / `FLAGS_1 NOW PIE`
  - versioned imports such as `GLIBC_2.34`
- **Undefined symbol:** a symbol that is referenced but not defined in this object (`U`). It has to be resolved by something else.
- **Dynamically imported symbol:** an undefined symbol listed in `.dynsym`, which the run-time loader binds through the PLT/GOT, e.g. `opendir@GLIBC_2.2.5`.
- **Shared-library dependency:** a whole library the loader must find (`DT_NEEDED`). It names a library, not a function, and does not say which symbols come from it.
- **Missing `nm` symbols:**
  - `nm` reads `.symtab`. `-s` (or `strip`) removes `.symtab`, so v2 shows "no symbols" and only `nm -D` (`.dynsym`) still works. The dynamic table cannot be stripped because the loader needs it.
  - `-O2` also inlines or renames static functions (`.constprop`), so names can disappear even before stripping.
  - `-fvisibility`/`static` keeps symbols out of the dynamic table.
- `ldd` was not used because it can execute the target's loader. `readelf -d` gives the same dependency list statically.

## Task 5: Comparison

| Property | data_sync | data_sync_v2 |
|---|---|---|
| ELF class and architecture | ELF64, LSB, x86-64 | ELF64, LSB, x86-64 |
| ELF file type | DYN (PIE) | DYN (PIE) |
| Entry-point address | 0x1140 | 0x14f0 |
| Presence of main() symbol | Yes, `T main` @ 0x1398 | No (stripped). Inferred at 0x1250 from `_start` |
| Important sections | .text 0x39b, .rodata 0x6c, .data, .bss, .dynsym, .dynamic, .symtab, .strtab, .debug_* (37 sections) | .text 0x524, .rodata 0xd4, .data, .bss, .dynsym, .dynamic. No .symtab/.strtab/.debug_* (29 sections) |
| Dynamic dependencies | libc.so.6 | libc.so.6 |
| Relevant imported functions | opendir, readdir, closedir, strcmp, strncpy, printf, fprintf, perror | the same plus fopen, fclose, fgetc, stat, time, __snprintf_chk, __printf_chk, __fprintf_chk, __stack_chk_fail |
| Debugging symbols | Yes (DWARF, not stripped) | No (stripped) |
| Significant differences | Name comparison only | Logging, stat-based metadata checks, byte-reading checksum. Optimised, fortified and stripped |

**Explanation:**
- **Source changes** add the new imports and strings, and the larger `.text`/`.rodata`.
- **`-O2`** inlines the helpers, moves `main` (which shifts the entry point), and with fortification swaps in the `*_chk` functions.
- **`-s`** removes `.symtab`, `.strtab` and the debug sections, which hides function names.
- **Linking** is unchanged: both are PIE and depend only on libc.

## Task 6: Forensic Assessment Report

**Purpose and methodology.** Two unknown executables, `data_sync` and `data_sync_v2`, were examined to describe their structure and likely behaviour. No target was executed. Analysis used `file`, `readelf` (`-h -S -l -s -d -p`), `nm` (`-n -D`) and `objdump` (`-h -t -T -p -d`). `ldd` was avoided.

**Key findings.**
- Both are 64-bit little-endian x86-64 PIE executables built by GCC 15.2.0 on Ubuntu.
- Both are dynamically linked with a single dependency, `libc.so.6`, via `/lib64/ld-linux-x86-64.so.2`, with full RELRO and a non-executable stack.
- Entry points are `0x1140` and `0x14f0`, both at `_start`, which passes `main` to `__libc_start_main`.
- `data_sync` keeps `.symtab` and DWARF debug sections, exposing `main`, `list_dir` and `contains`. Its imports are limited to directory listing (`opendir`, `readdir`, `closedir`), string comparison and console output.
- `data_sync_v2` is stripped: `nm` finds no symbols and `main` (`0x1250`) can only be recovered from the `_start` disassembly.

**Comparative assessment.**
- `data_sync_v2` imports additional file and metadata functions: `fopen`, `fgetc`, `fclose`, `stat`, `time` and `__snprintf_chk`.
- Its `.rodata` holds `sync_v2.log`, `[%ld] %s`, `scan started/finished`, `%s/%s`, `MISSING:` and `CHANGED: %s (size %lld vs %lld)`.
- Together these suggest that v2 builds full paths, compares file size and timestamps, reads file contents byte by byte (consistent with a checksum), and appends timestamped entries to a local log file.
- The larger `.text`, the `*_chk` imports, and the missing symbols and debug info are consistent with `-O2` optimisation plus stripping, rather than a different toolchain.
- No network, process-creation or privilege-related imports (e.g. `socket`, `connect`, `execve`, `fork`, `setuid`) appear in either binary.
- The behaviour indicated is local, read-mostly directory comparison. The only write is to a log file.

**Conclusion and limitations.**
- *Observed* facts: headers, sections, segments, imports, strings and call sites.
- *Inferred* behaviour: checksum logic, logging and size comparison. These inferences rest on imports and strings, and an import does not prove a call happens at run time. Code paths may be unreachable, input-dependent or triggered only under specific conditions.
- Static inspection cannot confirm the actual files touched, the log contents, or behaviour introduced at run time through `LD_PRELOAD`, a modified `libc`, or data read during execution.
- Stripping in `data_sync_v2` limits attribution of internal functions.
- On the available evidence, neither binary shows indicators of malicious functionality. Dynamic analysis in an isolated sandbox would be needed to confirm this.
