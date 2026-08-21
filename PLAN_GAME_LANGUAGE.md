# Per-game Launch Language Override — Implementation Plan

## Goal
Add a per-game **"Launch language"** option to the Games menu (X → Game Options).
Default is "System default"; selecting a language forces that specific game to
launch in it, matching DBI's "Force language" feature.

## Mechanism (researched & verified)
Atmosphere per-title config override:

- **File**: `/atmosphere/contents/<TID lowercase 16-hex>/config.ini`

```ini
[override_config]
override_language = ja
```

- Parsed by Atmosphere loader at every process launch
  (`cfg_override.board.nintendo_nx.inc:281-302,370`, `GetContentOverrideConfig()`);
  served to games/NS via ams_mitm's `set` service hook (`set_mitm_service.cpp`).
  No reboot needed; applies regardless of launch source (sphaira or HOME).
- Valid codes (18):
  `ja, en-US, fr, de, it, es, zh-CN, ko, nl, pt, ru, zh-TW, en-GB, fr-CA,
   es-419, zh-Hans, zh-Hant, pt-BR`
- Requires Atmosphere CFW. Forcing an unsupported language may misbehave
  (game-side fallback/crash).
- Precedent: nx-locale-switcher writes exactly this format; DBI uses the same
  feature.
- Key deletion: sphaira's minIni fork supports `ini_puts(section, key, NULL,
  file)` → deletes key; preserves unrelated keys (HBL `override_key` etc.).

## Decisions (confirmed with user)
| Decision | Choice |
|---|---|
| Scope | Language only (no region override) |
| Unsupported languages | Marked "(not supported)" using NACP `supported_language_flag` |
| Placement | Top-level Game Options sidebar, directly **after "Launch random game"** |
| Persistence | Write immediately on change; system-wide effect; no separate sphaira-side storage |

## Implementation Steps

### 1. New files: `sphaira/include/game_lang.hpp` + `sphaira/source/game_lang.cpp`
- Language table, index = SetLanguage/NACP-bitmask order:

  ```
  0 ja / 1 en-US / 2 fr / 3 de / 4 it / 5 es / 6 zh-CN / 7 ko / 8 nl /
  9 pt / 10 ru / 11 zh-TW / 12 en-GB / 13 fr-CA / 14 es-419 /
  15 zh-Hans / 16 zh-Hant / 17 pt-BR
  ```

  Display names disambiguate Chinese pairs (zh-CN vs zh-Hans, zh-TW vs zh-Hant).
- API:

  ```cpp
  // returns 0 = system default, else 1..18; <0 on error
  s64 GameLangGet(u64 app_id);
  // index 0 deletes the key; creates /atmosphere/contents/<TID>/ recursively
  Result GameLangSet(u64 app_id, s64 index);
  ```

- Internals: path `snprintf("/atmosphere/contents/%016lX/config.ini", app_id)`
  (lowercase); `ini_gets`/`ini_puts("override_config", "override_language", ...)`;
  `fs::CreateDirectoryRecursively(path_dir)` before write; use `fs::FsNativeSd`.

### 2. `sphaira/include/title_info.hpp` + `source/title_info.cpp`
- Add `u32 supported_language_flag{}` to `ThreadResultData` (title_info.hpp:31-37).
- Populate from `nacp.supported_language_flag` wherever a NACP is successfully
  loaded in the worker (~line 392 area, incl. `LoadControlManual` fallback
  path). Leave 0 (= unknown → show all plainly).

### 3. `sphaira/source/ui/menus/game_menu.cpp` — X handler
- Insert after the "Launch random game" block (ends line 628), before
  `Export NSP` (line 630):

  ```cpp
  auto lang_e = options->Add<SidebarEntryArray>("Launch language"_i18n,
      lang_items, [this](s64& index_out){
          const auto rc = game::GameLangSet(m_entries[m_index].app_id, index_out);
          if (R_FAILED(rc)) { App::PushErrorBox(rc, "Failed to set language!"_i18n); }
      }, GameLangGet(m_entries[m_index].app_id),
      "Forces this game to launch in the selected language.\nRequires Atmosphere."_i18n);
  ```

- Build `lang_items`: item 0 `"System default"_i18n`, then 18 names; append
  `" (not supported)"_i18n` when `supported_language_flag` is known and bit not
  set.
- Skip entry entirely for forwarders:
  `(app_id & 0x0500000000000000) == 0x0500000000000000` (same check as line 930).

### 4. `sphaira/CMakeLists.txt`
- Add `source/game_lang.cpp` to `add_executable(sphaira ...)` list (~line 103 block).

### 5. i18n
- Use `_i18n` literals only (project convention); translations land later via PRs.

## Edge cases
- Write takes effect on next launch; no effect on running game.
- Non-Atmosphere SD: option inert (info string documents requirement). Don't
  create dirs when getting (read only).
- Empty leftover section/file after key deletion is harmless to Atmosphere's
  parser.
- `GameLangGet` failure → default to index 0.

## Verification
1. Build: `./build_release.sh` (cmake preset Release).
2. Lint/typecheck: none configured beyond build warnings.
3. On-device checklist:
   - Set Japanese on a multi-lang game → launch from sphaira AND from HOME →
     boots in Japanese.
   - Verify `/atmosphere/contents/<TID>/config.ini` contents; pre-existing keys
     preserved.
   - Back to System default → key removed → launches with system language.
   - Unsupported-language marking matches game's NACP.
   - Forwarders don't show the entry; applet mode still allows changing (write
     works, launch gated elsewhere).
