---
name: window-sight
description: "Extract structured UI information from Windows windows as JSON via UI Automation (UIA): control tree, ControlType, Name, AutomationId, Rect, IsEnabled, IsOffscreen, HelpText, Value, and interaction patterns. Auto-detects window type (browser/qt/winrt/webview2/unknown), browser CDP capability, focused element, and process info. Supports interactive-control filtering (90%+ size reduction), homogeneous list-item dedup, and PNG screenshots. Use for UI automation testing, interface debugging, control location, accessibility audits, and LLM-driven UI analysis. Trigger when the user needs window info, control-tree extraction, UI element location, interface diagnostics, or screenshot+structure input for an LLM."
---

# WindowSight

WindowSight turns any Windows window into **structured JSON** via UI Automation (UIA) — control tree, control properties, interaction patterns, focused element, and process info — instead of relying on screenshots.

**Design goal**: when a program misbehaves, feed the UI *structure* (not pixels) to an LLM so it can reason from control semantics rather than guessing from vision.

## When to use

- Inspect the foreground window or a window specified by handle
- Extract the UIA control tree of a desktop application
- Locate UI elements (by AutomationId, ControlType, Name)
- Diagnose why a program's UI is not behaving as expected
- Build a combined "screenshot + structure" input for LLM analysis

## Install as an agent skill (LLM-loaded)

This is a **generic agent skill**, not tied to any specific host. The only contract is `SKILL.md` itself: its YAML frontmatter (`name`, `description`) lets any LLM agent environment discover and auto-load the skill, and the tool it describes is called via a plain command line.

**How it works**: your agent host scans its skills directory, reads each `SKILL.md`, and loads the instructions into the LLM's context. The LLM then invokes `WindowSight.exe` as a command-line tool.

**To install** — copy the entire `window-sight` directory into your host's skills folder:

| Host / convention | Location |
|---|---|
| Claude Code / Codex (user-level) | `~/.claude/skills/window-sight/` |
| Claude Code (project-level) | `.claude/skills/window-sight/` |
| Cursor (project-level) | `.cursor/skills/window-sight/` |
| WorkBuddy (user-level) | `~/.workbuddy/skills/window-sight/` |
| Any other agent | put `SKILL.md` anywhere the host loads skills from, or reference its path explicitly in your system prompt |

**Binary placement** — the prebuilt executable ships in `bin/WindowSight.exe`. Make sure the LLM can find it, in one of these ways:

1. Add `bin/` to your `PATH` (simplest), or
2. Keep it at `bin/WindowSight.exe` relative to `SKILL.md` (hosts that resolve relative paths), or
3. Pass the full path on the command line, e.g. `C:\tools\window-sight\bin\WindowSight.exe --filter interactive`.

If you build from source instead, copy the binary to `bin/` or your `PATH` after compiling (see [README](README.md) → Build).

**Verify**:

```bash
WindowSight.exe --help
```

If it prints usage text, the tool is ready. The LLM can then be asked things like "inspect the foreground window", "extract the control tree of window 123456", or "diagnose this window and screenshot it".

## Repository layout

```
window-sight/
├── SKILL.md               # Skill descriptor (this file) — the LLM entry point
├── README.md              # Full documentation
├── CMakeLists.txt         # CMake build config
├── bin/
│   └── WindowSight.exe    # Prebuilt binary (also attached to GitHub Releases)
└── scripts/
    └── main.cpp           # C++17 single-file implementation
```

## Build

Full instructions in [README.md](README.md). TL;DR:

```bash
vcpkg install jsoncpp:x64-windows
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
# Output: build/bin/Release/WindowSight.exe
# Copy it to bin/ (or your PATH) so the LLM can find it.
```

## Quick reference

| Option | Description |
|---|---|
| *(none)* | Inspect the foreground window (auto mode) |
| `--hwnd <n>` | Target window handle (decimal or 0x hex) |
| `--mode auto\|uia\|simple` | Strategy: auto (default), force UIA, basic info only |
| `--max-depth <N>` | Max recursion depth (default 20, range 1-50) |
| `--filter all\|interactive` | All controls, or interactive-only (90%+ smaller) |
| `--no-dedup` | Disable folding of homogeneous list items |
| `--screenshot <path>` | Also save the window as a PNG |
| `--help` | Show help |

### Recommended LLM workflow

1. **Overview**: `--mode simple --screenshot shot.png` — visual + basic info
2. **Locate**: `--filter interactive --max-depth 5` — interactive controls only
3. **Deep dive**: `--mode uia --max-depth 20 --no-dedup` — full tree (browsers: use CDP for DOM)

## Output

JSON on stdout with exit code 0; error JSON on stderr with exit code 1. Top-level fields: `hwnd`, `title`, `class`, `rect`, `window_type`, `pid`, `exe_path`, `focused_element`, `structure`, plus optional `screenshot` / `cdp_recommended`.

## Limitations

- Max recursion depth defaults to 20 (configurable 1-50); at most 200 children per level (`children_truncated: true` when exceeded)
- Browser DOM requires CDP (the tool flags `cdp_capable` / `cdp_recommended`)
