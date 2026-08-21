# Plan: Install support for split NSP/NSZ/XCI files (FAT32 4GB limit)

> **STATUS: Implemented (build passes, Release preset).** Remaining: on-device
> testing (see checklist at the bottom).
>
> What landed:
> - `sphaira/include/yati/source/split_file.hpp` + `source/yati/source/split_file.cpp`
>   (`SplitFile` source, `IsSplitPath`, `IsSplitPartName`, `SPLIT_CONTAINER_EXTENSIONS`).
> - Routing in `yati::InstallFromFile` (split detection + logical-path rewrite).
> - UI in `filebrowser.cpp`: dir prompt (Yes/Open folder), flat-part click installs,
>   sidebar Install via `check_all_installable`.
> - CMakeLists.txt: added `source/yati/source/split_file.cpp`.
> - Note: build host had stale FetchContent deps; fixed by wiping
>   `build/Release/_deps`. LSP errors in editor are pre-existing clangd/devkitPro
>   sysroot noise.

## Goal

Support installing split NSP/NSZ/XCI files stored for FAT32, in **both** layouts:

1. **Folder variant**: a directory named `game.nsp/` containing parts `00`, `01`, `02`, ...
   (ns-usbloader style).
2. **Flat variant**: sibling files `game.nsp.00`, `game.nsp.01`, `game.nsp.02`, ...

Agreed UX: pressing A on a folder named like an installable container prompts
**"Install" / "Open folder"** instead of silently navigating into it.
Pressing A on a first-part flat file (`*.nsp.00`) prompts install of the whole set.

Out of scope: USB PC-tool install (`usb_menu.cpp`), FTP/MTP streaming,
browse-inside-a-split-file via devoptab mount (`MountNsp` etc.).

---

## Investigation findings (as of planning)

Split install is **not supported today**:

- Installer reads everything through an offset-based abstraction:
  `source::Base::Read(buf, off, size, *bytes_read)` —
  `sphaira/include/yati/source/base.hpp:8`.
- Entry point `yati::InstallFromFile` (`sphaira/source/yati/yati.cpp:1525-1534`)
  opens a single file via `source::File` → `fsFsOpenFile`; pointing it at a
  directory fails immediately.
- Container dispatch is by filename extension using `strrchr(path.s, '.')`
  (`yati.cpp:1537-1548`): `.msp` → msp, `.nsp/.nsz` → Nsp, `.xci/.xcz` → Xci.
  NOTE: for flat variant the path must be normalized to the logical name
  (`game.nsp`), otherwise `strrchr` finds `.00` and dispatch fails.
- UI quirk: `FileEntry::GetExtension()` (`include/ui/menus/filebrowser.hpp:154-161`)
  works on any name, so a folder named `game.nsp` already passes
  `check_all_ext(INSTALL_EXTENSIONS)` and shows a sidebar "Install" option
  (`filebrowser.cpp:1750-1758`) — but it fails at open time.
  Pressing A on a dir always navigates into it (`OnClick`, `filebrowser.cpp:690-692`).
- Existing split-concat logic exists ONLY for internal NAND BIS partitions via
  bundled FatFs: `sphaira/source/utils/devoptab_fatfs.cpp:113-190` and
  `243-348` (part naming `"%s/%02u"`, offset→part mapping at lines 126-150,
  cross-part read loop at 306-332). Not usable for SD/USB/network installs;
  use as the pattern reference only.
- Everything downstream (PFS0/HFS0 parsing, ticket import, NCA copy) uses only
  absolute-offset reads on `source::Base*` (e.g. `yati.cpp:394`,
  `container/nsp.cpp:83-145`, `container/xci.cpp:35-71`), so a concatenating
  source makes split install work transparently for the whole pipeline.
- SD card uses native nx FS (`fsFsOpenFile`/`fsFileRead`), network mounts /
  USB HDD / devoptab mounts use stdio — both behind `fs::Fs` abstraction
  (`source/fs.cpp:470-537`). A split source built on `fs::Fs` therefore works
  on all mount types automatically.

### Key reference locations

| What | File | Lines |
|---|---|---|
| Source interface | `include/yati/source/base.hpp` | 8-32 |
| Single-file source to mirror | `include/yati/source/file.hpp`, `source/yati/source/file.cpp` | whole |
| `InstallFromFile` (open + size) | `source/yati/yati.cpp` | 1525-1534 |
| Container ext dispatch | `source/yati/yati.cpp` | 1536-1553 |
| Install extensions list | `source/ui/menus/filebrowser.cpp` | 83-91 (`INSTALL_EXTENSIONS`, `NSP_EXTENSIONS`, `XCI_EXTENSIONS`) |
| Dir click = navigate | `source/ui/menus/filebrowser.cpp` | 690-692 |
| Install trigger from click | `source/ui/menus/filebrowser.cpp` | 743-744 |
| `InstallFiles()` loop | `source/ui/menus/filebrowser.cpp` | 830-854 |
| Sidebar install visibility | `source/ui/menus/filebrowser.cpp` | 1739-1758 (`check_all_ext`) |
| OptionBox prompt example | `source/ui/menus/filebrowser.cpp` | 677-685 (DayBreak) |
| Split-part concat pattern | `source/utils/devoptab_fatfs.cpp` | 113-190, 243-348 |
| `GetEntryType` / `DirExists` helpers | `include/fs.hpp` | 339, 343-344 |

---

## Implementation steps

### 1. New source class `yati/source/split_file`

New files:
- `sphaira/include/yati/source/split_file.hpp`
- `sphaira/source/yati/source/split_file.cpp`

Mirror `file.hpp/cpp`. Sketch:

```cpp
struct File final : Base {
    // path: directory containing "00","01",... OR first part "X.nsp.00"
    File(fs::Fs* fs, const fs::FsPath& path);
    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;
    Result GetSize(s64* out);

private:
    fs::Fs* m_fs{};
    std::vector<fs::File> m_files;
    std::vector<s64> m_sizes;      // per-part sizes
    std::vector<s64> m_offsets;    // cumulative start offsets
};
```

Behaviour:
- Constructor detects mode: if `DirExists(path)` → open `<path>/00`, `/01`, ...
  else treat as first part: open given file, then siblings with incremented
  2-digit suffix. Stop when next part fails to open. Require at least one part
  (else set failing `m_open_result`).
- Part naming: two digits, `%02u` style ("00".."99"), matching devoptab_fatfs
  and ns-usbloader conventions. For flat variant increment numerically.
- `Read`: locate part via cumulative offsets (scan or binary search), then loop
  while `size > 0`, splitting reads at part boundaries; advance global offset.
  Guard `off >= GetSize()` → return 0 bytes read or error, consistent with
  how `fs::File::Read` behaves past EOF.
- `GetSize`: sum of part sizes.
- No `IsStream()` override (random access, non-stream).

### 2. Detection & routing in `yati::InstallFromFile`

`sphaira/source/yati/yati.cpp:1525-1534`:

1. `GetEntryType(path)`:
   - `FsDirEntryType_Dir` and name ends with an install ext → construct
     `source::SplitFile(fs, path)`.
   - file whose basename matches `*.<nsp|xci ext>.<2-digit>` → strip the
     trailing `.NN` and construct split source over siblings; pass the
     **stripped logical path** to `InstallFromSource` so ext dispatch works.
2. Otherwise existing single-file behaviour unchanged.

Keep `GetSize` usage as-is (split source returns real sum; -1 fallback kept).

### 3. UI changes in `filebrowser.cpp`

1. `FsView::OnClick()` (~line 690): if entry is a dir AND its extension matches
   `INSTALL_EXTENSIONS`, push OptionBox: title e.g. `"Open folder?"`/
   `"Install <name>?"` with options **Install** / **Open** (fit existing
   OptionBox API; see DayBreak example at 677-685). Default safe action =
   Open? (decide during implementation; keep B = cancel).
2. `FsView::OnClick()`: file matching `*.<install-ext>.NN` (first part or any
   part) → prompt install of the resolved set (call `InstallFiles()` after
   selecting it, or directly resolve base path).
3. Sidebar visibility (`check_all_ext`, ~1739): extend predicate so selection
   containing split-set entries also shows "Install". Simplest: add helper
   `bool IsEntryInstallable(e)` = matches INSTALL_EXTENSIONS OR split-set
   pattern; require all selected entries pass it. `InstallFiles()` itself needs
   no change because `InstallFromFile` resolves dirs/parts.

### 4. i18n

Any new user-facing strings go through `_i18n` like neighbouring code; check
`romfs`/lang files convention used by the project before adding keys.

---

## Edge cases

- Missing `00` / empty folder → constructor fails with clear Result; existing
  error box surfaces it (`filebrowser.cpp:850`).
- Read spanning part boundaries (must loop, not single read).
- Parts > 99 not supported (matches upstream conventions; ~400 GB cap).
- NSZ/XCZ splits work identically (container-level split, NCZ payloads inside).
- Do NOT break normal files: routing changes are strictly additive.
- MSP (`*.msp`) untouched.

---

## Verification

- Build: repo has `CMakePresets.json` + `build_release.sh` (devkitPro aarch64
  toolchain required). Ensure clean build of both CMake projects
  (`hbl`, `sphaira`) — app target is `sphaira`.
- On-device manual test matrix (needs user's Switch):
  - [ ] folder variant install (nsp + nsz + xci if available)
  - [ ] flat variant install
  - [ ] A on `*.nsp` folder → prompt offers Open (browse still possible)
  - [ ] multi-select mix of regular + split entries via sidebar Install
  - [ ] regression: normal single-file nsp/xci installs unchanged
- Log check via the app's logging (`log_write`) for open/read failures.
