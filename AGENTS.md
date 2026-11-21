# AGENTS.md

Instructions for AI coding agents (and humans) working on this repository.
How the project was originally created is described in [docs/AI-DEVELOPMENT.md](docs/AI-DEVELOPMENT.md).

## What this is

A Windows x64 proxy DLL. It is placed in a game folder under the name of a system DLL the game
imports (currently `xinput1_4.dll`). It forwards every export of the real DLL from the system
directory, and it removes `FILE_FLAG_NO_BUFFERING` when the game opens matching data files
read-only. Windows then caches those files in RAM.

## Layout

| Path | Purpose |
|---|---|
| `src/gamecache.c` | the whole DLL: export forwarding, import-table hooks, config, log |
| `src/version.h`, `src/version.rc` | version number and Windows version resource |
| `proxies/<name>.exports` | export list of the real system DLL (ordinal + name, `-` = ordinal-only) |
| `tools/gen_proxy.py` | `dump` creates an `.exports` file, `gen` creates `proxy_exports.h` + `proxy_stubs.asm` |
| `tools/test_hook.py` | self-test in a throwaway Python process |
| `tools/analyze_trace.py`, `tools/pe_imports.py` | measurement and analysis helpers |
| `profiles/<game>.ini` | per-game `gamecache.ini`; first lines are comments, `; proxy = <name>` is required |
| `packaging/INSTALL.txt` | template for the text file in release ZIPs |
| `build.cmd`, `package.ps1`, `install.ps1` | build, release ZIP, local install/uninstall |
| `.github/workflows/build.yml` | CI: build, self-test, package, attestation, release on `v*` tags |

## Commands

Requires Windows, Visual Studio (Build Tools) with the C++ x64 workload, and Python 3.

```bat
build.cmd xinput1_4
python -I tools\test_hook.py build\xinput1_4.dll %TEMP%\gctest
powershell -File package.ps1 -GameProfile control-resonant -Game "Control Resonant" -Exe CONTROLResonant.exe
```

A change is only done when `build.cmd` succeeds (it compiles with `/W4 /WX`) and every line of
`test_hook.py` prints `PASS`.

## Rules that must not be broken

- **Only read-only opens are changed.** Never remove `FILE_FLAG_NO_BUFFERING` from a handle opened
  with any write, append or delete access. Save games depend on this.
- **Never touch device or volume paths** (`\\.\...`, `\\?\GLOBALROOT...`).
- **No hard-coded system paths in the binary.** The real DLL is located at runtime with
  `GetSystemDirectoryW`.
- **Keep `DllMain` small.** It may load the real system DLL, read the ini, patch import tables and
  write the log. No threads, no waiting, no network, no other libraries.
- **The hooks must not change behaviour for anything else**: same arguments, same return values,
  same last-error.
- **Never overwrite a DLL that is not gamecache.** `install.ps1` checks the version resource
  (`ProductName == gamecache`); keep that check.
- **Do not commit measurement data.** `.etl`, `.csv`, logs and anything under `private/` can contain
  user names, paths and details about someone's system. They are ignored in `.gitignore`.

## Conventions

- C17 compiled with MSVC, Win32 API only, no third-party libraries.
- All code, comments, log messages and docs in English.
- Log lines are user-facing: short, plain, and stable enough that people can paste them into issues.
- Bump `src/version.h` for every release; releases are created by pushing a tag `vX.Y.Z`.

## Adding a game

1. Measure it (see the README section "Adding a game"). Only add it if most reads bypass the cache
   and data is read several times.
2. Choose a proxy DLL the game imports that is not a KnownDLL and does not exist in the game folder.
   If it is new, run `python -I tools\gen_proxy.py dump <name>` and commit `proxies/<name>.exports`.
3. Add `profiles/<game>.ini` and a row to the table in the README.
4. Extend the CI workflow if the new profile needs a different proxy build.
