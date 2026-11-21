# gamecache

**Fixes stutter in games that read their data unbuffered, by letting Windows cache it in RAM.**
It does what a block cache like PrimoCache does for these games, but it is free, needs no driver
and reserves no memory.

Some games open their data packages with `FILE_FLAG_NO_BUFFERING`. That bypasses the Windows file
cache. If the game then reads the same data again and again, every read goes to the drive. On
DRAM-less or QLC NVMe SSDs, SATA SSDs and HDDs this shows up as stutter, especially when entering
new areas or fast-travelling.

gamecache is a small DLL you drop into the game folder. When the game opens its data files, the
DLL removes only that one flag. Windows then keeps the data in its normal file cache (the standby
list), and repeated reads come from RAM.

> Single-player games without anti-cheat only. Anti-cheat systems treat any foreign DLL as tampering.

## Does it help me?

Probably, if:

- the game stutters, and the stutter gets better with PrimoCache or a similar RAM cache, or after
  moving the game to a faster drive
- the game is on a slower drive: a DRAM-less/QLC NVMe SSD (e.g. Kingston NV2, Crucial P3, WD Blue SN5x0),
  a SATA SSD or an HDD
- you have 16 GB of RAM or more (32 GB is ideal)

It won't do much on a fast NVMe SSD with a DRAM cache.

## Supported games

| Game | Data files | Proxy DLL | Status |
|------|-----------|-----------|--------|
| Control Resonant | `*.rmdblob` | `xinput1_4.dll` | tested |

Other games can be added; see [Adding a game](#adding-a-game).

## Install (Control Resonant)

1. Download `gamecache-<version>-control-resonant.zip` from the
   [Releases](../../releases) page.
2. Copy `xinput1_4.dll` and `gamecache.ini` into the game folder (the folder that contains
   `CONTROLResonant.exe`).
3. Start the game as usual.

**Check that it works:** the game folder now contains `gamecache.log`:

```
2026-11-21 02:39:54.054 gamecache 0.1.0 loaded into C:\...\CONTROLResonant.exe
2026-11-21 02:39:54.055 forwarding to C:\WINDOWS\system32\xinput1_4.dll (14 of 14 exports resolved)
2026-11-21 02:39:54.055 active - patched <n> import entries in <m> modules, patterns: *.rmdblob
2026-11-21 02:39:55.120 buffered open: C:\...\data_pack2\pc\base-generic-000.rmdblob
```

New lines only appear when the game opens a file. Control Resonant opens all its packages within the
first seconds and keeps them open, so the log stops growing after that. That's expected: the change
applies to every read from those files for the rest of the session.

If you use PrimoCache or a similar cache for this drive, pause it while you compare.

The first time an area loads, the data still comes from the drive. Repeated loads come from RAM.
In Task Manager (Performance → Memory), "Cached" grows while you play.

**Uninstall:** delete `xinput1_4.dll`, `gamecache.ini` and `gamecache.log` from the game folder.

## Configuration

`gamecache.ini` next to the DLL:

```ini
[gamecache]
enabled=1           ; 0 = DLL stays loaded but changes nothing
log=1               ; 0 = no gamecache.log
patterns=*.rmdblob  ; file name patterns, separated by ; (empty = all files)
```

## Safety

- Only files opened **read-only** are changed. Save games, configs and anything else the game
  writes keep their original behaviour.
- Device and volume paths are never touched.
- Nothing is installed system-wide, no admin rights, no driver, no service. Deleting the files
  undoes everything.
- All other functions of `xinput1_4.dll` (controller input) are forwarded unchanged to the real DLL
  in the Windows system folder.
- The release ZIPs are built by GitHub Actions from this source, with a
  [build provenance attestation](https://docs.github.com/en/actions/security-for-github-actions/using-artifact-attestations).
  Verify a download with `gh attestation verify <file> --repo kaveyro/gamecache`.

Some antivirus programs flag DLLs that redirect system functions. If you don't trust the binary,
build it yourself (see below). It's about 400 lines of C.

## How it works

1. The game loads `xinput1_4.dll` from its own folder before the system folder, so it loads gamecache.
2. gamecache loads the real `xinput1_4.dll` from the system folder and forwards all its exports
   through small jump stubs.
3. It redirects `CreateFileW`, `CreateFileA` and `CreateFile2` in the import tables of the loaded
   modules, including kernel32's own forwarding to kernelbase.
4. When a read-only open of a matching file has `FILE_FLAG_NO_BUFFERING`, the flag is removed.
   Nothing else changes.

### Why this helps in Control Resonant

Measured over 11 minutes of play with Windows Performance Recorder (FileIO + DiskIO):

| | |
|---|---|
| reads that bypass the Windows cache | 99.5 % |
| data read by the game | 90 GB |
| distinct data actually touched | 13.2 GB |
| average number of times the same data was read | 6.8× |
| most common read sizes | 256 KB, and 4–20 KB random reads |

Without a cache, every one of those reads hits the drive. With gamecache, the 13 GB working set
stays in RAM after the first read.

## Adding a game

You need Python 3 and Visual Studio Build Tools with the C++ x64 workload.

1. **Measure.** In an admin PowerShell:
   ```powershell
   wpr -start FileIO -start DiskIO -filemode
   # play 10-15 minutes where it stutters, then:
   wpr -stop C:\temp\game.etl
   tracerpt C:\temp\game.etl -o C:\temp\game.csv -of CSV
   python -I tools\analyze_trace.py C:\temp\game.csv .pak
   ```
   Use the extension of the game's data files instead of `.pak`. gamecache helps if
   "bypassing the Windows cache" is high and "read volume / distinct data" is well above 1.
2. **Pick a proxy DLL.** `python -I tools\pe_imports.py Game.exe` lists the DLLs the game imports.
   Pick one that is not listed under
   `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs` and doesn't already exist in
   the game folder. Good candidates are `xinput1_4`, `xinput1_3`, `winmm`, `version` and `dinput8`.
3. **Build:** `build.cmd <proxy>`. This creates `build\<proxy>.dll`.
4. **Test:** `python -I tools\test_hook.py build\<proxy>.dll %TEMP%\gctest`
5. **Install:** `.\install.ps1 -GameDir 'D:\Games\Game' -Proxy <proxy> -Patterns '*.pak'`
6. If it works, add `profiles\<game>.ini` (with a `; proxy = <name>` line) and open a pull request.

## Building

```bat
build.cmd xinput1_4
python -I tools\test_hook.py build\xinput1_4.dll %TEMP%\gctest
powershell -File package.ps1 -GameProfile control-resonant -Game "Control Resonant" -Exe CONTROLResonant.exe
```

| Path | Purpose |
|---|---|
| `src/gamecache.c` | the DLL: forwarding, import-table hooks, config, log |
| `tools/gen_proxy.py` | generates export table and jump stubs from `proxies/<name>.exports` |
| `tools/test_hook.py` | self-test in a throwaway process |
| `tools/analyze_trace.py` | analyzes a WPR trace |
| `tools/pe_imports.py` | lists a game's imported DLLs |
| `profiles/` | per-game `gamecache.ini` |

## FAQ

**Is this a mod? Does it change game files?**
No game files are changed. Two files are added next to the game and can be deleted at any time.

**Does Steam's "verify integrity" remove it?**
No. Steam only checks files that belong to the game.

**Does it use more RAM?**
It uses the Windows file cache, which only takes memory that is otherwise free and gives it back
when programs need it. Nothing is reserved.

**Why not fix it in the game?**
That would be the real fix. If you're affected, report it to the developer with the numbers above.

## Built with AI

This project was written with Claude Code (Claude Opus 5.5) and tested in-game by the repository
owner. [docs/AI-DEVELOPMENT.md](docs/AI-DEVELOPMENT.md) describes the setup, the process and the
prompts. [AGENTS.md](AGENTS.md) contains instructions for agents working on the code.

## License

MIT, see [LICENSE](LICENSE).
