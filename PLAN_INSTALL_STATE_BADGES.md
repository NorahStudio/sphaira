# Plan: Install-state badges for installable files in the file browser

> **STATUS: Implemented (build passes, Release preset, zero warnings).** Remaining:
> on-device testing (see checklist at the bottom).
>
> What landed:
> - `sphaira/include/game_file_scan.hpp` + `sphaira/source/game_file_scan.cpp`
>   (`game_file_scan` namespace: `GetCnmts`, `CompareInstalled`,
>   `ScanFileInstallState`, `InstallState`, `ScanResult`, `CnmtInfo`).
> - UI in `filebrowser.cpp`: lazy one-per-frame scanning of `.nsp`/`.nsz` files
>   while a directory is shown, right-aligned badge text next to the date/size
>   column (**New** / **Update** / **Installed** / **Older**).
> - CMakeLists.txt: added `source/game_file_scan.cpp`.
> - Note: build host had stale FetchContent deps (`libusbdvd`, `ftpsrv` patch
>   steps failed); fixed by wiping `build/Release/_deps`. LSP errors in editor
>   are pre-existing clangd/devkitPro sysroot noise.

## Goal

In the file browser, show at a glance whether an installable game file is:

| Badge       | Meaning                                              | Colour (theme ID)    |
|-------------|------------------------------------------------------|----------------------|
| `New`       | title is not installed                                | `HIGHLIGHT_1`        |
| `Update`    | file contains a newer version than what is installed  | `HIGHLIGHT_2`        |
| `Installed` | file version matches the installed version            | `TEXT_INFO`          |
| `Older`     | installed version is newer than the file              | `TEXT_INFO`          |

Unparseable files / files without a CNMT show no badge at all (cleaner than an
"Unknown" tag on every random NSP).

### Scope decisions (v1)

- **`.nsp`/`.nsz` only.** XCI/HFS0 deferred; `NSP_EXTENSIONS` gates both scanning
  and badge display.
- **Compare against installed titles only** (`nsListApplicationContentMetaStatus`
  via `title::GetMetaEntries`). The `nx_versions` online catalog comparison is a
  possible phase 2.
- Badges are **on by default** (`m_show_install_state{true}`), list view only.
- Files containing multiple metas (base+update+DLC multi-CNMT) aggregate with
  priority **Update > New > Older > Installed**.
- `.cnmt.ncz` inside NSZ archives is not supported yet → such files simply show
  no badge (the plain `.cnmt.nca` case covers virtually all real dumps).

---

## Investigation findings

- Install state source of truth lives in NS services:
  - `nsCountApplicationContentMeta` fails when an app id is not installed —
    used as the "New" signal (`title_info.cpp:596-611`).
  - `NsApplicationContentMetaStatus` carries `{meta_type, storageID, version,
    application_id}`; match entries by `meta_type`, compare `version`.
  - No dependency on `title::Init()` background thread for this call.
- App id derivation from a CNMT: `ncm::GetAppId(meta_type, title_id)`
  (`yati/nx/ncm.hpp`) maps patch/addon ids back to the application id.
- Reading a CNMT out of an arbitrary file (works over sdmc:/ums:/etc. because
  everything goes through `fs::Fs`):
  1. `yati::source::File{fs, path}` → PFS0 parse via
     `container::Nsp::GetCollections` (entries `{name, offset, size}`).
  2. Find entry named `*.cnmt.nca`.
  3. Read first `0xC00` bytes → `nca::DecryptHeader(in, keys, out)` (XTS with
     `keys.header_key`).
  4. Meta NCA = single PFS0 section (`fs_header[0].fs_type ==
     FileSystemType_PFS0`); decrypt key area in place via `nca::DecryptKeak`.
  5. Wrap the file source in an offset-limiting sub-source so NCA-relative
     offsets work; construct `nca::NcaReader(header, key_area[0], size, src)`.
  6. Parse inner PFS0 at `fs_table[0].GetOffset()` (header + table +
     string table), find name ending in `.cnmt`, read first `0x20` bytes as
     `ncm::PackagedContentMeta{title_id u64, title_version u32, meta_type u8}`
     (static_assert'd size).
- Keys loading precedent: `keys::parse_keys(keys, true)` (as in
  `devoptab_nca.cpp`, `gc_menu.cpp`). Parsed once per file, not per entry.
- Title-ID heuristics from filenames were rejected early: Switch ids start with
  `01` (the `0004…` prefix idea was 3DS-era), and real NSP entries are named by
  content id hex — hence proper CNMT parsing instead.
- Blocking work in `OnFocusGained` froze the UI while scanning every entry;
  replaced by the existing lazy pattern used for `done_stat` timestamp lookups.

---

## Implementation

### New scanner (`game_file_scan`)

- `SubSource` (anon ns): clamps reads to `[base, base+size)` of the wrapped
  source; holds a non-owning `shared_ptr` alias to keep `NcaReader`'s
  shared-ptr signature happy without double-freeing the stack object.
- `ReadExact` helper: loops through `source->Read`, errors with
  `Result_NspInvalidHeader` on short read.
- Sanity caps on inner PFS0: ≤ 0x10000 files, string table ≤ 16 MiB (mirrors
  `container/nsp.cpp` limits).
- `CompareInstalled` keeps the highest file version seen (for future display)
  and prefers the Application meta's app id when aggregating.
- Errors reuse existing codes from `defines.hpp` (`Result_NspBadMagic`,
  `Result_NspInvalidHeader`, `Result_YatiInvalidNcaMagic`) — no new enum churn.

### File browser integration

- `FsView` members: `std::map<std::string, game_file_scan::ScanResult>
  m_file_install_state` keyed by full path string (`fs::FsPath` has no
  `std::hash`), plus `std::string m_install_scan_next` queue slot. Both cleared
  in `Scan()` when changing directories.
- Draw loop (list view): for each visible `.nsp`/`.nsz` file, if not yet in the
  map, claim `m_install_scan_next`; after `m_list->Draw` returns, exactly one
  scan runs per frame (failed scans are stored as `Unknown` so they aren't
  retried forever).
- Badge drawn with `gfx::drawText(vg, x + w - text_xoffset - 130, y + h/2, 14f,
  colour, text, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE)` — sits left of the
  date/size column. Text uses `_i18n` keys `New` / `Update` / `Installed` /
  `Older` (missing translations fall back to the English literal).
- Header includes `game_file_scan.hpp` (moved to `include/` to satisfy the
  project's include layout).

---

## Phase 2 ideas (not implemented)

- XCI/HFS0 containers (`xci::GetCollections` already exists upstream).
- Compare against the `nx_versions` catalog to flag "Update" for files that are
  newer/older than latest known, independent of local installs.
- Show versions in the badge or sidebar (e.g. `Update v65536 (v1.0.1 → v1.1.0)`);
  `file_version` / `installed_version` are already tracked in `ScanResult`.
- Grid-view badges; option toggle in settings menu.
- Background thread scanning if per-frame cost proves noticeable on slow USB
  drives.

## On-device testing checklist

- [ ] SD card dir with mixed files: uninstalled NSP shows `New`; up-to-date dump
      shows `Installed`; older dump shows `Older`; newer dump shows `Update`.
- [ ] Multi-meta NSP (base+update) against base-only install → `Update`.
- [ ] Non-game/corrupt NSP → no badge, no crash, no repeated rescans.
- [ ] USB (ums:) browsing — badges appear, UI stays responsive (one scan/frame).
- [ ] Directory navigation back/forth re-scans correctly (cache clears).
- [ ] Dark/light/custom themes: badge colours readable.
