# Plan: Installer stability hardening (DBI-parity)

> **STATUS: Draft — awaiting review.**
>
> Investigation covered Sphaira's install pipeline (`sphaira/source/yati/`,
> `source/usb/`, `install_stream_menu_base.cpp`) vs DBI's observable behavior.
> DBI's Switch-side source is closed (`rashevskyv/dbi` ships only README,
> config, and the host-side `dbibackend` script); its behavior below is
> inferred from those public artifacts plus community reports. Everything on
> the Sphaira side was verified directly in this repo.

## Goal

Close the stability gap between DBI and Sphaira when installing
NSP/NSZ/XCI/XCZ files, without changing wire protocols or verification
defaults:

1. **Transport resilience** — transient read failures retry instead of aborting.
2. **Error isolation** — one bad file fails alone; the rest of the batch installs.
3. **Space handling** — pre-flight checks instead of mid-write ENOSPC failures.

Non-goals: NCM/ES registration-order changes, resume support, hash/RSA
verification defaults.

---

## Investigation findings

Sphaira's pipeline ordering is already correct (NCM placeholders → SHA-256
verify → ticket import → register → push record; new content validated
*before* old content removed). The stability gap is almost entirely
**failure-handling philosophy**: DBI assumes transports hiccup (retry /
reconnect / isolate); Sphaira assumes perfection (fail-fast everywhere).

| # | Behavior | DBI | Sphaira today |
|---|---|---|---|
| 1 | Transport errors | Host tool polls non-blocking, auto-reconnects on `USBError` ("Switch connection lost" → reconnect loop); stateless range requests make any chunk re-requestable | Hard 3 s timeouts everywhere (`usb_menu.cpp:16-18`); CRC32C mismatch returns a bare literal (`usb_installer.cpp:97`); zero retries anywhere in the pipeline |
| 2 | Blast radius | Per-file colored log (green/orange/red); "RED - errors. File was not installed" while remaining files continue | First failing NCA aborts the entire multi-file batch via `R_TRY` chains (`yati.cpp:1007-1012`, `1415-1417`, `1467-1490`); all placeholders deleted, nothing registered |
| 3 | Hash mismatch | Per-file warning/error, continues | Fatal `Result_YatiInvalidNcaSha256` (`yati.cpp:1021`) — tens of GB discarded after successful transfer |
| 4 | Interrupted installs | Ships a "Cleanup orphaned files" tool sweeping leftovers of interrupted installs | Scope-exit placeholder deletion only (`yati.cpp:1395-1400`, `1441-1449`); a hard crash leaks NCM placeholders forever, no sweeper exists |
| 5 | Space handling | AUTO install target falls back SD→NAND when out of space; removes old updates before installing new ones | Storage chosen once upfront (`yati.cpp:903`); ENOSPC mid-write = failed install |

Other relevant observations:

- USB **stream mode silently disables all verification** (`usb_menu.cpp:110-116`)
  — trading crash-stability for landing unverified data.
- A transient zero-byte read from MTP/FTP throws `Result_TransferCancelled`
  (`yati/source/stream.cpp:29,35`).
- The MTP/FTP stream buffer is a 1 MiB vector with O(n) front-erase per read
  (`install_stream_menu_base.cpp:19,56`), historically freeze-prone (commits
  `0a2c16d`, `97dc396` fixed related hangs).
- curl paths set no timeouts at all (`download.cpp`, `SetCommonCurlOptions`);
  only devoptab browse mounts configure low-speed guards
  (`devoptab_common.cpp:1392-1395`).
- Both NCM storages' `cs`/`db` are already opened upfront (`yati.cpp:910-913`),
  making a target fallback cheap to implement.
- Sphaira's own history shows this path freezing under pressure: `0a2c16d`
  ("fixes freezing if write blocks for too long"), `97dc396` (condvar fix),
  `1c72350` ("fix yati not returning the read fail result").
- Upstream issue #274 ("USB Install for XCI files always fail") exemplifies the
  fragile transport path.

### Why DBI can do some things Sphaira can't (cheaply)

DBI's "Cleanup orphaned files" needs placeholder enumeration, which has no
public NCM API. DBI achieves it (and fast partition access) by bundling its own
raw-partition FAT driver. Sphaira bundles ff16 and already mounts BIS
partitions (`devoptab_fatfs.cpp`), so replicating this is feasible but is a
separate, riskier project — proposed out of scope here (see Open questions).

### Key reference locations

| What | File | Lines |
|---|---|---|
| Source interface (offset reads) | `include/yati/source/base.hpp` | whole |
| Thread failure → whole-install abort | `source/yati/yati.cpp` | 1007–1012 |
| Fatal SHA-256 verify | `source/yati/yati.cpp` | 1018–1027 |
| Per-NCA `R_TRY` loop (file path) | `source/yati/yati.cpp` | 1402–1421 |
| Stream-path install `R_TRY`s | `source/yati/yati.cpp` | 1461–1490 |
| Placeholder cleanup on unwind only (file) | `source/yati/yati.cpp` | 1395–1400 |
| Placeholder cleanup on unwind only (stream) | `source/yati/yati.cpp` | 1441–1449 |
| Storage chosen once upfront; both cs/db opened | `source/yati/yati.cpp` | 903, 910–913 |
| `skip_if_already_installed` disabled for streams | `source/yati/yati.cpp` | 1433 |
| Read chunk 4 MiB (512 KiB emummc) | `source/yati/yati.cpp` | 159–165 |
| Pipeline buffers (~32 MiB worst case) | `source/yati/yati.cpp` | 86–95, 310–311 |
| Install threads: cores 1/2/0, 64 KiB stacks | `source/yati/yati.cpp` | 943–959 |
| Emummc fixed 2 ms write sleep (TODO) | `source/yati/yati.cpp` | 801–806 |
| USB timeouts (all 3 s) | `source/ui/menus/usb_menu.cpp` | 16–18 |
| Stream mode force-disables verify | `source/ui/menus/usb_menu.cpp` | 110–116 |
| USB error → SignalCancel + fail, no retry | `source/ui/menus/usb_menu.cpp` | 120–139 |
| `Usb::Read` request/response + CRC check | `source/usb/usb_installer.cpp` | 87–101 |
| `SendAndVerify` timeout plumbing | `source/usb/usb_installer.cpp` | 104–117 |
| 16 MiB aligned staging buffer, TransferAll loop | `source/usb/base.cpp` | 29–31, 72–98 |
| Endpoint wait race; cancel-on-failure | `source/usb/usbds.cpp` | 206–251, 286–315 |
| Bare error codes TODO | `include/usb/usb_api.hpp` | 47, 78 |
| Zero-byte stream read → `Result_TransferCancelled` | `source/yati/source/stream.cpp` | 29, 35 |
| MTP/FTP stream push/pull condvar ring | `ui/menus/install_stream_menu_base.cpp` | 35–115 |
| curl common options (no timeouts) | `source/download.cpp` | ~613–685 |
| Browse-mount low-speed guards (reference values) | `source/utils/devoptab_common.cpp` | 1392–1395 |
| Batch install loop | `source/ui/menus/filebrowser.cpp` | ~830–854 |
| Host-side USB tool + tests | `tools/usb_install.py`, `tools/tests/test_usb_install.py` | whole |

---

## Implementation steps

### Phase 1 — Transport resilience (highest impact)

1. **Read-retry wrapper in the installer.**
   - Wrap the `source->Read(...)` call site(s) in `Yati::readFuncInternal`
     (and ticket/cert reads in `InstallInternal*`) with a retry loop:
     N retries (default 3), exponential backoff 250 ms → cap 2 s.
   - Only meaningful for seekable sources (`!source->IsStream()`); stream
     sources keep fail-fast semantics.
   - Check cancellation between attempts (`pbox->GetCancelEvent()`); never
     retry after user cancel.
   - Log every attempt (`log_write`) so failures stay diagnosable.
   - The USB source benefits automatically: each `Read()` re-sends the
     stateless `{off,size}` range packet (`usb_installer.cpp:87-101`), so a
     retry is protocol-clean.
   - Split-file installs (`SplitFile`) inherit retries transparently since
     they sit below the container layer.

2. **USB transport hardening.**
   - Raise timeouts in `usb_menu.cpp:16-18`: `TRANSFER_TIMEOUT` and
     `FINISHED_TIMEOUT` 3 s → 15 s default; keep `CONNECTION_TIMEOUT` 3 s
     (menu-level idle polling already handles reconnection UX).
   - Add a per-request retry loop inside `Usb::Read` / `SendAndVerify`: on
     CRC32C mismatch, magic mismatch, or transfer timeout → re-send the range
     packet up to N times before failing.
   - Replace bare literal error codes (`1`/`3`; see TODOs
     `usb_api.hpp:47,78`) with named libnx-style Results so logs and UI show
     actionable reasons.

3. **Host tool parity (`tools/usb_install.py`).**
   - Mirror dbibackend's resilience: non-blocking poll loop, auto-reconnect on
     `USBError`, stateless range serving.
   - Extend `tools/tests/test_usb_install.py` with disconnect/reconnect and
     corruption-injection cases.

4. **curl robustness (`download.cpp`).**
   - Add sane defaults to `SetCommonCurlOptions`:
     `CURLOPT_CONNECTTIMEOUT_MS` (~10 s) and
     `CURLOPT_LOW_SPEED_LIMIT` / `CURLOPT_LOW_SPEED_TIME` stall detection,
     aligning with the values already used for browse mounts
     (`devoptab_common.cpp:1392-1395`).

5. **Stream buffer efficiency.**
   - Replace `std::vector` front-erase O(n) memmove with a deque/ring buffer
     (`install_stream_menu_base.cpp:56`); narrows the stall window that caused
     past freezes.
   - Ensure writer-side failure promptly calls `Stream::Disable()` so readers
     exit fast instead of waiting on the condvar (`install_stream_menu_base.cpp:83-111`).

### Phase 2 — Error isolation (DBI-style per-file)

Design note: isolation granularity is the **container file** (one NSP/XCI),
matching DBI's "RED — File was not installed". Within a single title all NCAs
are required for registration, so intra-container failures must still abort
that container — nothing partial ever registers. Transport retries from
Phase 1 make intra-container failures rare.

6. Batch loops catch per-container errors instead of propagating them:
   - `filebrowser.cpp` `InstallFiles()` (~830–854),
   - multi-select paths in `usb_menu.cpp`, `mtp_menu.cpp`, `ftp_menu.cpp`.
7. Collect results as `{path, Result, bytes_done}`; final summary screen lists
   each file OK / SKIPPED / FAILED with reason string, DBI-style green/orange/
   red coloring where the UI supports it.
8. Still fatal immediately: user cancel, NCM service-level errors, repeated
   ENOSPC. New user-facing strings go through `_i18n`.

### Phase 3 — Space handling

9. **Pre-flight check.** Before creating the first placeholder, sum required
   installed size vs target free space (NSZ/XCZ decompress on install — use
   real NCA sizes already computed by the container parsers, not source size).
   Fail early with a message naming target + needed/free bytes instead of
   dying mid-write on ENOSPC.
10. **AUTO install target.** Both storages' `cs`/`db` are opened upfront
    (`yati.cpp:910-913`): add DBI-style AUTO — pick SD if it fits, else NAND,
    else fail pre-flight. Decided only before the first placeholder is
    created; never migrates mid-install.
11. *(Optional — needs sign-off)* Remove old update NCAs *before* writing new
    ones (DBI parity). Behavior change: the old version is gone even if the
    new install then fails. Defer unless explicitly wanted.

---

## Edge cases & risks

- Retries must never mask real media errors or swallow cancellation: attempts
  capped, every attempt logged, cancel checked between attempts.
- USB wire protocol unchanged → backward compatible with existing host tools;
  only timing and error-reporting change.
- Longer timeouts must not look frozen → ProgressBox shows a retry counter
  during stalls.
- Continue-on-error can never register a partial title — guaranteed because
  intra-container failure always aborts before ticket import / old-content
  removal / registration (`yati.cpp:1419-1421`).
- Hash mismatch stays fatal per-container (verify is off by default upstream;
  this mainly affects users who opt in).
- Stream sources cannot retry by design; their failure mode stays fail-fast
  but with prompt `Disable()` propagation so readers exit quickly rather than
  hanging on a condvar.
- Emummc 2 ms/write throttle (`yati.cpp:804-806`) untouched; retry backoff
  costs latency only on error paths.
- If pursued later, a usbDs endpoint reset between retries can wedge the USB
  stack on some firmware revisions — keep behind a config flag, default off.

## Verification

- Build both CMake targets via existing presets (`build_release.sh`;
  devkitPro toolchain). Watch for stale FetchContent deps in
  `build/Release/_deps` (known issue, see PLAN_SPLIT_NSP_INSTALL.md).
- Host-side tests: extend and run `tools/tests/test_usb_install.py`
  (disconnect/reconnect, CRC corruption, slow-host stall > 15 s).
- On-device manual matrix:
  - [ ] Pause/kill PC host mid-USB-transfer → recovers or fails cleanly with
        named result (no more bare `3`)
  - [ ] Injected CRC corruption → range re-requested, install completes
  - [ ] Multi-select 5 files incl. 1 corrupt NSP → 4 install, summary screen
        lists the failed one with reason
  - [ ] Unplug cable mid-install → clean unwind, placeholders deleted
  - [ ] File larger than free space → pre-flight error names needed/free
  - [ ] AUTO mode with full SD → falls back to NAND
  - [ ] Regression: single-file and split-file installs unchanged; MTP + FTP
        stream installs unchanged; writer abort propagates quickly

## Open questions (for review)

1. Defaults OK? (3 read retries, 250 ms→2 s backoff, 15 s USB transfer timeout)
2. Item 11 (delete old update before writing new) — include now behind an
   explicit toggle, or defer?
3. DBI-style "Cleanup orphaned files" — proposed **out of scope** (needs raw-
   partition access; separate riskier project). Confirm exclusion.
