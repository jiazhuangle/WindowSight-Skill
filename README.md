# WindowSight

> Understand Windows windows with structured data — turn a UI control tree into JSON, ready for code, scripts, or LLMs.

WindowSight is a Windows window structured-information extractor built on the UI Automation (UIA) API, implemented as a single C++17 file. It walks the control tree of a target window and outputs **structured JSON** — control types, names, AutomationIds, positions, interaction patterns — instead of a screenshot.

**Motivation**: when a program misbehaves, feed the UI's *structure* (not pixels) to an LLM, so it can reason from control semantics instead of guessing from vision.

## Features

- **Window info**: hwnd, title, class name, rect, PID, executable path
- **Window type detection**: auto-classifies `browser` / `qt` / `winrt` / `webview2` / `unknown`
- **UIA control tree traversal**: recursive extraction with configurable depth (1–50)
- **Control properties**: ControlType, Name, AutomationId, Rect, IsEnabled, IsOffscreen, HelpText, Value, and 17 interaction patterns (Invoke/Toggle/Scroll/ExpandCollapse/...)
- **Focused element detection**: reports the currently focused control to diagnose interaction issues
- **Browser detection**: recognizes Chrome_RenderWidgetHostHWND and flags when DOM can be retrieved via CDP
- **Output size control**: interactive-only filtering (`--filter interactive`, size can shrink 90%+) and folding of homogeneous list items
- **Screenshot**: `--screenshot` saves the window as PNG in one shot — a "visual + structure" combined input

## Install as an Agent Skill (LLM-loaded)

WindowSight is designed as a **generic agent skill**: a `SKILL.md` descriptor plus a prebuilt `bin/WindowSight.exe`, loadable by any LLM agent environment that supports skills. The YAML frontmatter (`name`, `description`) in `SKILL.md` is what lets the agent auto-discover and load the skill; the LLM then calls the tool over the command line — no API, no SDK, no host-specific integration.

### The contract

```
skill directory
├── SKILL.md              ← entry point; the agent loads this into the LLM's context
└── bin/WindowSight.exe   ← the tool itself (also attached to GitHub Releases)
```

### Placement per host

Copy the **entire `window-sight` directory** into your agent's skills folder:

| Host / convention | Location |
|---|---|
| Claude Code / Codex (user-level) | `~/.claude/skills/window-sight/` |
| Claude Code (project-level) | `.claude/skills/window-sight/` |
| Cursor (project-level) | `.cursor/skills/window-sight/` |
| WorkBuddy (user-level) | `~/.workbuddy/skills/window-sight/` |
| Any other agent | point the host at `SKILL.md`, or reference its path in your system prompt |

No host-specific code: the only requirement is that the LLM can execute `WindowSight.exe`. Three ways to make that happen:

1. Add `bin/` to your `PATH`;
2. Leave it at `bin/WindowSight.exe` next to `SKILL.md` (for hosts that resolve skill-relative paths);
3. Call it with an absolute path, e.g. `C:\tools\window-sight\bin\WindowSight.exe`.

### Prebuilt binary

`bin/WindowSight.exe` (x64, ~580 KB, no dependencies beyond the standard Windows runtime) is included in this repository and attached to every GitHub Release — prefer the release asset for a smaller download. Build from source only if you need to modify the tool.

### Verify

```bash
WindowSight.exe --help
```

If it prints usage text, the skill is ready. In conversation you can then say things like "inspect the foreground window", "extract the control tree of window 123456", or "diagnose this window and screenshot it" — the agent invokes the tool automatically.

## Build

### Prerequisites

| Dependency | Notes |
|------|------|
| MSVC | Visual Studio 2019/2022 or Build Tools, C++17 |
| Windows SDK | with UIAutomation (uiautomation.h) |
| jsoncpp | JSON library |
| CMake | ≥ 3.16 |

### Option 1: vcpkg (recommended)

```bash
vcpkg install jsoncpp:x64-windows

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg-path>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Option 2: FetchContent (auto-downloads jsoncpp, no manual dependency install)

```bash
cmake -B build -S . -DUSE_FETCHCONTENT=ON
cmake --build build --config Release
```

### Option 3: Direct cl invocation (requires jsoncpp installed)

```bash
cl /EHsc /std:c++17 /utf-8 scripts\main.cpp /I<jsoncpp-include> /link uiautomationcore.lib ole32.lib oleaut32.lib user32.lib gdi32.lib windowscodecs.lib jsoncpp.lib
```

Output binary: `build/bin/Release/WindowSight.exe` — copy it to `bin/` (or your `PATH`) to match the skill layout above.

## Usage

### Command-line options

| Option | Description |
|------|------|
| *(none)* | Inspect the current foreground window (auto mode) |
| `--hwnd <number>` | Target window handle (decimal or 0x-prefixed hex) |
| `--mode auto` | Auto-select the best extraction strategy (default) |
| `--mode uia` | Force UI Automation tree traversal |
| `--mode simple` | Basic window info only, no tree walk |
| `--max-depth <N>` | Max tree recursion depth (default 20, range 1-50) |
| `--filter all` | Output every control (default) |
| `--filter interactive` | Keep interactive controls + necessary containers; drop decorative nodes |
| `--no-dedup` | Disable folding of homogeneous list items (enabled by default) |
| `--screenshot <path>` | Also save the window as PNG; JSON gets a `screenshot` field |
| `--help` | Show help |

### Typical usage

```bash
# Full structure of the foreground window
WindowSight.exe

# Specific window (decimal / hex)
WindowSight.exe --hwnd 123456
WindowSight.exe --hwnd 0x1E240

# Basic info only (fastest, no tree walk)
WindowSight.exe --mode simple

# Limit traversal depth
WindowSight.exe --max-depth 5

# Interactive controls only (size can shrink 90%+, LLM-friendly)
WindowSight.exe --filter interactive --max-depth 5

# Screenshot + structure JSON in one shot
WindowSight.exe --filter interactive --max-depth 5 --screenshot detail.png

# Deep dive (no filter, no dedup)
WindowSight.exe --mode uia --max-depth 20 --no-dedup
```

### Output

On success, JSON goes to stdout with exit code 0; on failure, an error JSON goes to stderr with exit code 1.

```json
{
  "hwnd": 123456,
  "title": "Calculator",
  "class": "CalcFrame",
  "rect": [100, 200, 500, 600],
  "window_type": "qt",
  "pid": 1234,
  "exe_path": "C:\\Windows\\System32\\calc.exe",
  "focused_element": {
    "control_type": "Button",
    "name": "7",
    "automation_id": "btn7",
    "is_enabled": true,
    "patterns": ["Invoke"]
  },
  "structure": {
    "control_type": "Window",
    "name": "Calculator",
    "is_enabled": true,
    "children": [
      {
        "control_type": "Button",
        "name": "7",
        "automation_id": "btn7",
        "rect": [120, 250, 160, 290],
        "is_enabled": true,
        "patterns": ["Invoke"],
        "value": "7"
      }
    ]
  }
}
```

### Three-step strategy for LLM analysis

| Step | Command | Purpose |
|------|------|------|
| ① Overview | `--mode simple --screenshot shot.png` | screenshot for the big picture + basic info |
| ② Locate | `--filter interactive --max-depth 5` | interactive controls only, minimal size |
| ③ Deep dive | `--mode uia --max-depth 20 --no-dedup` | full control tree; for browsers, use CDP for DOM |

### Mode selection

- **auto** (default): tries a full UIA traversal, falls back to root-node info on failure; flags `cdp_recommended` for browser windows
- **uia**: forces a full UIA control tree traversal
- **simple**: basic window info only, fastest response

## Limitations

- Max recursion depth defaults to 20 (configurable 1-50)
- At most 200 children per level; exceeding this sets `children_truncated: true`
- Browser DOM requires CDP (the tool flags `cdp_capable` / `cdp_recommended`)

## Tech stack

C++17 · Win32 / COM · UI Automation (UIA) · WIC (PNG capture) · jsoncpp · CMake
