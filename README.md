# 2pack

A tiny, portable **Windows executable compressor** built with Visual C++ 2019.
It uses the **LZMA** algorithm (7‑zip LZMA SDK) to shrink a `.exe` into a
self‑extracting, single‑file executable.

```
   original.exe  ── 2pack.exe ──▶  original.exe.packed.exe
                                        (stub + random-named LZMA section)
```

There is **no `-d` / decompress-to-file mode by design.** Unpacking happens only
inside the packed program, at run time, in memory — the packed file *is* the
program.

## Features

* **LZMA level 9** (64 MiB dictionary) for a high compression ratio, with an
  optional **x86 BCJ** branch pre‑filter that improves it further.
* **Self‑extracting single file** — the packed `.exe` is a complete program;
  there is no separate decompress step and nothing is written to disk at run
  time.
* **Automatic stub selection** — picks `stub32.exe` or `stub64.exe` from the
  target's machine type; one tool compresses both 32‑ and 64‑bit binaries.
* **Randomized payload section name** on every pack (8 chars, lower‑case +
  digits) — no fixed, greppable marker; the stub locates the payload by magic
  dword, not by name.
* **Tiny no‑CRT stub** — the unpacker imports only `kernel32`, links with
  `/NODEFAULTLIB`, and maps the original in memory exactly like the Windows
  loader: sections, page protections, base relocations, imports, and **TLS**
  (including Go‑style absolute TLS addresses and high
  `TlsExpansion` indices via `TlsSetValue`).
* **Preserves the original** subsystem (console/GUI), bitness, image base, and
  entry point.
* **Zero dependencies** — statically linked, no VC++ redistributable, no
  install; copy `bin\` anywhere on Windows and it works.
* **No debug info** in any binary (`/NODEBUG`).
* **Auto‑incrementing version** — each `build.bat` run bumps the minor version
  (state in `version.txt`, `version.rc` regenerated from it).

## Supported executables

2pack compresses native Windows PE executables:

| Property        | Supported                                                        |
|-----------------|------------------------------------------------------------------|
| Format          | PE32 (x86) and PE32+ (x64)                                       |
| Subsystem       | Console (`CUI`) and GUI (`WINDOWS`)                               |
| Toolchains      | MSVC / MinGW C, C++, Rust, **Go** (validated)                     |
| Base address    | Must map at preferred base **or** carry a `.reloc` section        |
| TLS             | Yes — MSVC (RVA) and Go (absolute) layouts, incl. `TlsExpansion`  |
| Delay imports   | No — targets with a delay‑import table are not supported          |

Not supported: drivers (`.sys`), DLLs, managed/CLR images, ARM/ARM64, and
anything requiring loader features the stub doesn't replicate (see
Limitations).

## What it does

`2pack` reads a PE executable and produces a new executable that is a copy of
a small **stub** with **one extra PE section appended**. That section:

* has a **randomized name** (lower‑case letters + digits, 8 chars) — no two
  packed files share a recognizable marker, and the stub does **not** look the
  section up by name;
* contains a small header followed by the **LZMA‑compressed original** (with an
  optional **x86 BCJ** branch pre‑filter to improve the ratio).

At launch the stub finds its own appended section (by a magic dword), decompresses
and un‑filters the original in memory, **manually maps** it exactly like the
Windows loader (sections, page protections, base relocations, imports, TLS), and
jumps to the original entry point.

The packed file keeps the original's **subsystem** (console vs GUI) and
**bitness** (32/64), so a packed console app prints to a console and a packed GUI
app opens no console window.

## Files

| Path                    | Role                                                                 |
|-------------------------|----------------------------------------------------------------------|
| `bin/2pack.exe`       | The compressor (x64). This is the tool you run.                       |
| `bin/stub32.exe`        | 32‑bit runtime unpacker (template for x86 targets).                   |
| `bin/stub64.exe`        | 64‑bit runtime unpacker (template for x64 targets).                   |
| `compressor/`           | `2pack` sources (PE parsing, LZMA front‑end, section builder).      |
| `stub/stub_main.cpp`    | The no‑CRT runtime loader (shared by stub32 and stub64).              |
| `common/payload.h`      | The on‑disk header contract shared by compressor and stub.            |
| `third_party/lzma/`     | 7‑zip LZMA SDK (public domain).                                       |
| `build.bat`             | One‑shot build script (VS2019 `cl`/`link`/`rc`).                      |

## Build

Requirements:

* **Visual Studio 2019** (Community is fine) with the
  **“Desktop development with C++”** workload, and
* the **Windows 10/11 SDK** (comes with the VS C++ workload).

From the repo root:

```bat
build.bat
```

The script locates `vcvarsall.bat`, builds `2pack.exe` (x64), `stub64.exe`
(x64) and `stub32.exe` (x86), and drops them in `bin\`.

Build flags that keep the output clean and portable:

* `/O1 /DNDEBUG` — optimized, asserts off.
* `/NODEBUG` — **no PDB / no debug information** in the binaries.
* `2pack` links **static** (`/MT`) so it needs no VC++ redistributable.
* The stubs link with **`/NODEFAULTLIB`** (no C runtime at all) and import only
  `kernel32`, so they are tiny and fully self‑contained.
* `Z7_ST` — single‑threaded LZMA encoder (portable, no extra SDK files).

> To rebuild a single target, open a **x64 Native Tools Command Prompt for
> VS2019** and re‑run the relevant `cl`/`link` lines from `build.bat`.

## Usage

```
2pack <input.exe> [-o <output.exe>] [--no-bcj] [--stub-dir <dir>]
```

| Option              | Meaning                                                        |
|---------------------|----------------------------------------------------------------|
| `<input.exe>`       | The executable to compress (PE32 or PE32+).                    |
| `-o <output.exe>`   | Output path (default: `<input>.packed.exe`).                   |
| `--no-bcj`          | Skip the x86 BCJ branch pre‑filter (slightly worse ratio).     |
| `--stub-dir <dir>`  | Where to find `stub32.exe`/`stub64.exe` (default: `2pack`'s dir). |
| `-h`, `--help`      | Show help.                                                     |

Example:

```bat
bin\2pack.exe myapp.exe
bin\2pack.exe myapp.exe -o myapp.small.exe
```

`2pack` picks the matching stub automatically based on the input's machine
type (x86 → `stub32.exe`, x64 → `stub64.exe`).

## Portability

* **One folder, no dependencies.** Copy `bin\` (the three `.exe`s) anywhere on a
  Windows machine — 32‑ or 64‑bit — and it works. `2pack` is statically linked
  and the stubs have no CRT, so there is nothing to install and no redistributable
  to ship.
* **No debug info.** All binaries are built with `/NODEBUG`; none carry a PDB or
  a debug directory entry.
* **Randomized section name** on every pack, so the payload section is not a
  fixed, greppable marker.

## How the packed file is laid out

```
+---------------------------------------------------------------+
|  MZ / PE headers + the stub's own sections                    |
|  (code, data, .reloc, imports)                                |
+---------------------------------------------------------------+
|  [ appended payload section, name = random 8 chars ]          |
|  +---------------------------------------------------------+  |
|  | Pack2Header (magic, version, method, flags, sizes,   |  |
|  | lzma props)                                            |  |
|  +---------------------------------------------------------+  |
|  | LZMA( [BCJ( original.exe bytes )] )                     |  |
|  +---------------------------------------------------------+  |
+---------------------------------------------------------------+
```

`Pack2Header` (packed, 32 bytes) is defined in `common/payload.h`:

| Field           | Type     | Notes                                   |
|-----------------|----------|-----------------------------------------|
| `magic`         | `u32`    | `0x4B514150` — also the discovery marker |
| `version`       | `u32`    | `1`                                      |
| `method`        | `u32`    | `0`=STORE, `1`=LZMA                      |
| `flags`         | `u32`    | bit 0 = x86 BCJ applied                  |
| `original_size` | `u64`    | size of the original file                |
| `packed_size`   | `u64`    | size of the compressed body              |
| `lzma_props[5]` | `u8[5]`  | LZMA property blob                       |
| `reserved[3]`   | `u8[3]`  | pad to 8‑byte boundary                   |

## Limitations

The stub is a deliberately minimal loader. It will not run every conceivable
PE, but it covers the common cases:

* The original must be mappable at its **preferred base** **or** carry a
  **`.reloc`** section (for base relocations).
* Only **console** (`CUI`) and **GUI** subsystems are supported (no `.sys`
  drivers, no DLLs, no C++/CLR/managed images, no ARM).
* Runs inside the stub's own process (shared address space / CRT state).
* Standard import & relocation types only — no delay imports, no
  forwarded exports, no CET/CFG shadow‑stack fixups.
* LZMA level 9 with a 64 MiB dictionary is used for the best ratio; on very
  large inputs this is memory‑hungry (the encoder can use several hundred MB).

## Credits

* **LZMA** — Igor Pavlov, 7‑zip. The SDK in `third_party/lzma` is public domain.
* The PE mapping approach mirrors what the Windows loader does
  (see the PE/COFF format documentation).
