# How gamecache was made

gamecache was written almost entirely by an AI coding agent, directed and tested by the repository
owner. This page documents the setup and process so the result can be judged and, if needed,
reproduced.

## Setup

| | |
|---|---|
| Tool | Claude Code (desktop app, Windows 11) |
| Model | Claude Opus 5.5 (`claude-opus-5-5`) |
| Date | 21 November 2026, one session |
| Agent configuration | Claude Code's default built-in system prompt and tools |
| Repository instructions | none: no `AGENTS.md`, `CLAUDE.md` or similar existed while it was built |
| Custom skills, plugins, sub-agents | none used for this work |
| Global user instructions | one unrelated line (about a different cloud CLI); nothing about this project |
| Agent memory | the agent kept short notes about this project in its local memory (hardware, measurement results, decisions). These notes are not needed to reproduce the work, and their content is covered on this page |

The conversation was in German. The prompts below are translated and shortened. Private details
are left out, such as file paths on the owner's machine.

## Process

1. **Diagnosis.** The owner described stutter in Control Resonant that PrimoCache with a 10 GB RAM
   cache fixed, and asked how to build something similar. The agent inspected the hardware: 32 GB RAM
   and a Kingston NV2 (a DRAM-less NVMe SSD). It suggested measuring before building, because a
   general block cache needs a signed kernel driver.
2. **Measurement.** The owner recorded 11 minutes of gameplay with Windows Performance Recorder
   (`wpr -start FileIO -start DiskIO`) in an admin shell. The agent converted the trace with
   `tracerpt` and wrote `tools/analyze_trace.py`. Result: 99.5 % of the game's reads bypass the
   Windows cache, 90 GB were read but only 13.2 GB were distinct, and data was read 6.8× on average.
3. **Design decision.** A file-level RAM disk was ruled out: the data sits in 4–7 GB packages that
   are only partly read. A prefetcher was also ruled out, because unbuffered reads ignore the
   Windows cache. A kernel driver was ruled out because of driver signing and anti-cheat. The
   remaining option: a proxy DLL that removes `FILE_FLAG_NO_BUFFERING` at open time, so Windows'
   own cache works again.
4. **Implementation.** The agent wrote the DLL, the export forwarding and a self-test. It built them
   with the MSVC Build Tools that were already installed and ran the self-test in a throwaway process.
5. **Real-world test.** The owner played the game with PrimoCache paused and confirmed that the
   stutter was gone. The log showed that all 30 data packages were opened buffered.
6. **Release preparation.** The agent made the following changes:
   - forwarding is resolved at runtime instead of through a hard-coded `C:\Windows` path
   - added a version resource
   - translated everything to English
   - added the install and package scripts and the CI workflow with build attestation
   - checked the binary and repository for personal data
   The owner tested the release build in the game again.

## Prompts (translated, in order)

1. "I have strange performance problems with Control Resonant. PrimoCache is actually the solution
   for me when I enable a 10 GB cache. Unfortunately PrimoCache costs money and I'm thinking about
   building something like it. How could we approach this?"
2. *(game location)* "PrimoCache is still running. Should I stop it? Or can we read this out of
   PrimoCache?"
3. "That sounds like a lot of work for a single game. I was hoping I could use the tool for other
   games too, the way PrimoCache works."
4. "Measure Control first."
5. "Measurement is done, the files are in the folder."
6. "Yes, build the DLL."
7. "Tested, and it actually helped! Since a lot of people have the same problem, I'm thinking of
   posting this on Reddit etc. What do you think? How could others use it the same way?"
8. "Make it release-ready, preferably in English, including the gamecache.log text."
9. "The game ran quite well, but there's nothing new in gamecache.log." *(expected: the game opens
   all files at startup and keeps them open; the docs now say so)*
10. *(GitHub account name)*, then "Yes, release v0.1.0."

## What a human verified

- that the stutter was gone in the actual game, with PrimoCache paused, for both the first build
  and the release build
- the measurement itself: the owner recorded it, and the agent analyzed it
- every outward-facing step: creating the repository, pushing, tagging the release

Everything else, including the code, the tests and the docs, was written by the agent and checked
by the build and the self-test, not by line-by-line human review. Reviews and issues are welcome.
