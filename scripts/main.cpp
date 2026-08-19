/**
 * WindowSight - Windows window structured info extractor
 *
 * Purpose: extract structured info from the foreground window (or a window
 * specified by handle) and output it as JSON on stdout.
 * Supports UI Automation (UIA) control-tree traversal, automatic window-type
 * detection, and multiple working modes.
 *
 * Build requirements: C++17, MSVC, Windows SDK (UIAutomation), jsoncpp
 *
 * Usage:
 *   WindowSight.exe                            foreground window info (auto mode)
 *   WindowSight.exe --hwnd 123456              specific window
 *   WindowSight.exe --mode uia                 force UIA control-tree traversal
 *   WindowSight.exe --mode simple              basic info only
 *   WindowSight.exe --max-depth 10             limit UIA traversal depth
 *   WindowSight.exe --filter interactive       interactive controls only (much smaller)
 *   WindowSight.exe --no-dedup                 disable folding of homogeneous list items
 *   WindowSight.exe --screenshot shot.png      also save a window screenshot
 *   WindowSight.exe --help                     show help
 *
 * Output-size control strategy (recommended combos for LLM analysis):
 *   1. Overview: screenshot (--screenshot) + basic info (--mode simple)
 *   2. Locate:   --filter interactive --max-depth 3~5, interactive controls only
 *   3. Deep dive: drop --filter for the full tree, or use CDP to get browser DOM
 */

// ============================================================
// Headers
// ============================================================
// Disable the min/max macros in windows.h to avoid conflicts with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uiautomation.h>
#include <objbase.h>
#include <wincodec.h>     // WIC: encodes window screenshots as PNG

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <cstdint>
#include <memory>
#include <optional>
#include <algorithm>

// jsoncpp headers (requires the jsoncpp dev library)
#include <json/json.h>

// ============================================================
// Constants
// ============================================================

// Default max recursion depth of the UIA control tree (overridable via --max-depth)
static constexpr int DEFAULT_MAX_UIA_DEPTH = 20;

// Max children traversed per level, to prevent oversized output on dense UIs
static constexpr int MAX_CHILDREN_PER_LEVEL = 200;

// Max buffer size for window title / class name
static constexpr int TITLE_BUFFER_SIZE = 512;

// ============================================================
// Enums
// ============================================================

// Control-tree output filter mode
enum class FilterMode {
    All,          // output every control (default)
    Interactive   // keep interactive controls + necessary containers; drop decorative leaves
};

// ============================================================
// Utility functions
// ============================================================

/**
 * Convert a wide string to a UTF-8 string
 * Uses the Win32 API WideCharToMultiByte
 */
std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                  static_cast<int>(wstr.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                        static_cast<int>(wstr.size()),
                        result.data(), len, nullptr, nullptr);
    return result;
}

/**
 * Convert a UTF-8 string to a wide string
 * Used to turn command-line file paths into the wide format Win32 APIs expect
 */
std::wstring UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                                  static_cast<int>(str.size()),
                                  nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                        static_cast<int>(str.size()),
                        result.data(), len);
    return result;
}

/**
 * Convert a BSTR to a UTF-8 string
 */
std::string BSTRToUTF8(BSTR bstr) {
    if (!bstr) return "";
    return WStringToUTF8(std::wstring(bstr, SysStringLen(bstr)));
}

/**
 * Convert a CONTROLTYPEID to a readable string
 * See the definitions in UIAutomationClient.h
 */
std::string ControlTypeToString(CONTROLTYPEID typeId) {
    switch (typeId) {
        case UIA_ButtonControlTypeId:           return "Button";
        case UIA_CalendarControlTypeId:         return "Calendar";
        case UIA_CheckBoxControlTypeId:         return "CheckBox";
        case UIA_ComboBoxControlTypeId:         return "ComboBox";
        case UIA_EditControlTypeId:             return "Edit";
        case UIA_HyperlinkControlTypeId:        return "Hyperlink";
        case UIA_ImageControlTypeId:            return "Image";
        case UIA_ListItemControlTypeId:         return "ListItem";
        case UIA_ListControlTypeId:             return "List";
        case UIA_MenuControlTypeId:             return "Menu";
        case UIA_MenuBarControlTypeId:          return "MenuBar";
        case UIA_MenuItemControlTypeId:         return "MenuItem";
        case UIA_ProgressBarControlTypeId:      return "ProgressBar";
        case UIA_RadioButtonControlTypeId:      return "RadioButton";
        case UIA_ScrollBarControlTypeId:        return "ScrollBar";
        case UIA_SliderControlTypeId:           return "Slider";
        case UIA_SpinnerControlTypeId:          return "Spinner";
        case UIA_StatusBarControlTypeId:        return "StatusBar";
        case UIA_TabControlTypeId:              return "Tab";
        case UIA_TabItemControlTypeId:          return "TabItem";
        case UIA_TextControlTypeId:             return "Text";
        case UIA_ToolBarControlTypeId:          return "ToolBar";
        case UIA_ToolTipControlTypeId:          return "ToolTip";
        case UIA_TreeControlTypeId:             return "Tree";
        case UIA_TreeItemControlTypeId:         return "TreeItem";
        case UIA_CustomControlTypeId:           return "Custom";
        case UIA_GroupControlTypeId:            return "Group";
        case UIA_ThumbControlTypeId:            return "Thumb";
        case UIA_DataGridControlTypeId:         return "DataGrid";
        case UIA_DataItemControlTypeId:         return "DataItem";
        case UIA_DocumentControlTypeId:         return "Document";
        case UIA_SplitButtonControlTypeId:      return "SplitButton";
        case UIA_WindowControlTypeId:           return "Window";
        case UIA_PaneControlTypeId:             return "Pane";
        case UIA_HeaderControlTypeId:           return "Header";
        case UIA_HeaderItemControlTypeId:       return "HeaderItem";
        case UIA_TableControlTypeId:            return "Table";
        case UIA_TitleBarControlTypeId:         return "TitleBar";
        case UIA_SeparatorControlTypeId:        return "Separator";
        case UIA_SemanticZoomControlTypeId:     return "SemanticZoom";
        case UIA_AppBarControlTypeId:           return "AppBar";
        default:
            return "Unknown(" + std::to_string(typeId) + ")";
    }
}

/**
 * Check whether a control type is "interactive" (directly operable by the user)
 * Used by --filter interactive: these are the core objects for LLM debugging
 */
bool IsInteractiveControlType(CONTROLTYPEID typeId) {
    switch (typeId) {
        case UIA_ButtonControlTypeId:       // button
        case UIA_CheckBoxControlTypeId:     // checkbox
        case UIA_ComboBoxControlTypeId:     // combo box
        case UIA_EditControlTypeId:         // edit field
        case UIA_HyperlinkControlTypeId:    // hyperlink
        case UIA_ListItemControlTypeId:     // list item
        case UIA_MenuItemControlTypeId:     // menu item
        case UIA_RadioButtonControlTypeId:  // radio button
        case UIA_SliderControlTypeId:       // slider
        case UIA_SpinnerControlTypeId:      // numeric spin box
        case UIA_TabItemControlTypeId:      // tab item
        case UIA_TreeItemControlTypeId:     // tree node
        case UIA_DataItemControlTypeId:     // data item
        case UIA_HeaderItemControlTypeId:   // header item
        case UIA_SplitButtonControlTypeId:  // split button
        case UIA_ThumbControlTypeId:        // scrollbar thumb
        case UIA_ProgressBarControlTypeId:  // progress bar
        case UIA_ScrollBarControlTypeId:    // scrollbar
            return true;
        default:
            return false;
    }
}

/**
 * Check whether a control type is a "container" (hosts/organizes other controls)
 * Kept in interactive mode to preserve the hierarchy; empty containers left
 * after filtering are pruned
 */
bool IsContainerControlType(CONTROLTYPEID typeId) {
    switch (typeId) {
        case UIA_WindowControlTypeId:       // window
        case UIA_PaneControlTypeId:         // pane
        case UIA_GroupControlTypeId:        // group
        case UIA_ListControlTypeId:         // list
        case UIA_TreeControlTypeId:         // tree
        case UIA_TabControlTypeId:          // tab container
        case UIA_MenuControlTypeId:         // menu
        case UIA_MenuBarControlTypeId:      // menu bar
        case UIA_ToolBarControlTypeId:      // toolbar
        case UIA_StatusBarControlTypeId:    // status bar
        case UIA_DocumentControlTypeId:     // document
        case UIA_CalendarControlTypeId:     // calendar
        case UIA_DataGridControlTypeId:     // data grid
        case UIA_TableControlTypeId:        // table
        case UIA_TitleBarControlTypeId:     // title bar
        case UIA_HeaderControlTypeId:       // header container
        case UIA_CustomControlTypeId:       // custom control (kept conservatively)
        case UIA_SemanticZoomControlTypeId: // semantic zoom
        case UIA_AppBarControlTypeId:       // app bar
            return true;
        default:
            return false;
    }
}

/**
 * Detect the UI Automation interaction patterns supported by an element
 * The pattern list tells the LLM which actions a control supports (click/check/scroll/type etc.)
 */
Json::Value DetectSupportedPatterns(IUIAutomationElement* element) {
    Json::Value patterns(Json::arrayValue);

    // Mapping table of common pattern IDs to names
    struct PatternEntry { int id; const char* name; };
    static const PatternEntry kPatterns[] = {
        { UIA_InvokePatternId,          "Invoke"          },  // activatable/clickable (button, menu item)
        { UIA_SelectionItemPatternId,   "SelectionItem"   },  // selectable (list item, tab)
        { UIA_ValuePatternId,           "Value"           },  // readable/writable value (edit field)
        { UIA_RangeValuePatternId,      "RangeValue"      },  // range value (slider, progress bar)
        { UIA_ScrollPatternId,          "Scroll"          },  // scrollable (container)
        { UIA_ScrollItemPatternId,      "ScrollItem"      },  // can be scrolled into view (child)
        { UIA_ExpandCollapsePatternId,  "ExpandCollapse"  },  // expandable/collapsible (tree node, menu)
        { UIA_TogglePatternId,          "Toggle"          },  // toggleable state (checkbox, switch)
        { UIA_TextPatternId,            "Text"            },  // readable text content
        { UIA_WindowPatternId,          "Window"          },  // window ops (move/close/maximize)
        { UIA_GridPatternId,            "Grid"            },  // grid layout (table container)
        { UIA_GridItemPatternId,        "GridItem"        },  // grid cell
        { UIA_TablePatternId,           "Table"           },  // table
        { UIA_TableItemPatternId,       "TableItem"       },  // table row/cell
        { UIA_DockPatternId,            "Dock"            },  // dock layout
        { UIA_TransformPatternId,       "Transform"       },  // movable/scalable/rotatable
        { UIA_SelectionPatternId,       "Selection"       },  // manages a selection set (list box)
    };

    for (const auto& p : kPatterns) {
        IUnknown* pattern = nullptr;
        if (SUCCEEDED(element->GetCurrentPattern(p.id, &pattern)) && pattern) {
            patterns.append(p.name);
            pattern->Release();
        }
    }

    return patterns;
}

/**
 * Write a JSON error message to stderr and return the error code
 */
int OutputError(const std::string& message) {
    Json::Value error;
    error["error"] = message;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::cerr << Json::writeString(builder, error) << std::endl;
    return 1;
}

/**
 * Write JSON to stdout
 */
void OutputJSON(const Json::Value& root) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::cout << Json::writeString(builder, root) << std::endl;
}

// ============================================================
// Window type detection
// ============================================================

/**
 * Check whether a window contains a WebView2 rendering child window
 * Checks common WebView2 / Chromium rendering window class names
 */
bool IsWebView2Window(HWND hwnd) {
    // Chrome_RenderWidgetHostHWND — Chromium-family rendering window
    if (FindWindowExW(hwnd, NULL, L"Chrome_RenderWidgetHostHWND", NULL))
        return true;
    // WebView2_View — WebView2 internal class
    if (FindWindowExW(hwnd, NULL, L"WebView2_View", NULL))
        return true;
    return false;
}

/**
 * Check whether a window has a Chrome_RenderWidgetHostHWND child
 * Used to mark whether a browser window can provide DOM via CDP
 */
bool HasChromeRenderWidget(HWND hwnd) {
    return FindWindowExW(hwnd, NULL, L"Chrome_RenderWidgetHostHWND", NULL) != nullptr;
}

/**
 * Detect window type from the window class name
 * Returns: browser / qt / winrt / webview2 / unknown
 */
std::string DetectWindowType(HWND hwnd, const std::string& className) {
    // Browser windows: Chrome / Firefox / Edge
    if (className.find("Chrome_WidgetWin") != std::string::npos ||
        className.find("MozillaWindowClass") != std::string::npos ||
        className.find("Edge_View") != std::string::npos) {
        return "browser";
    }

    // Qt application windows
    if (className.find("Qt") != std::string::npos ||
        className.find("QWidget") != std::string::npos ||
        className.find("QMainWindow") != std::string::npos) {
        return "qt";
    }

    // UWP / WinUI / WinRT windows
    if (className.find("HwndWrapper") != std::string::npos ||
        className.find("Windows.UI.Core") != std::string::npos ||
        className.find("ApplicationFrame") != std::string::npos ||
        className.find("WinUIDesktopWin32WindowClass") != std::string::npos) {
        return "winrt";
    }

    // WebView2 embedded windows (Tauri / Electron etc.)
    if (IsWebView2Window(hwnd)) {
        return "webview2";
    }

    return "unknown";
}

// ============================================================
// Basic window info
// ============================================================

/**
 * Fetch basic window info (title, class, rect) and fill the JSON object
 */
void PopulateBasicInfo(Json::Value& root, HWND hwnd) {
    // Window handle
    root["hwnd"] = Json::UInt64(reinterpret_cast<uintptr_t>(hwnd));

    // Window title
    wchar_t title[TITLE_BUFFER_SIZE] = {0};
    GetWindowTextW(hwnd, title, TITLE_BUFFER_SIZE);
    root["title"] = WStringToUTF8(title);

    // Window class name
    wchar_t className[TITLE_BUFFER_SIZE] = {0};
    GetClassNameW(hwnd, className, TITLE_BUFFER_SIZE);
    std::string classNameStr = WStringToUTF8(className);
    root["class"] = classNameStr;

    // Window rect [left, top, right, bottom]
    RECT rect;
    GetWindowRect(hwnd, &rect);
    Json::Value rectArray(Json::arrayValue);
    rectArray.append(rect.left);
    rectArray.append(rect.top);
    rectArray.append(rect.right);
    rectArray.append(rect.bottom);
    root["rect"] = rectArray;

    // Window type
    root["window_type"] = DetectWindowType(hwnd, classNameStr);

    // Process info: PID and executable path
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        root["pid"] = Json::UInt64(pid);

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess) {
            wchar_t exePath[MAX_PATH] = {0};
            DWORD pathLen = MAX_PATH;
            if (QueryFullProcessImageNameW(hProcess, 0, exePath, &pathLen)) {
                root["exe_path"] = WStringToUTF8(exePath);
            }
            CloseHandle(hProcess);
        }
    }

    // For browsers with a Chrome_RenderWidgetHostHWND, flag that DOM can be fetched via CDP
    std::string wtype = root["window_type"].asString();
    if (wtype == "browser" && HasChromeRenderWidget(hwnd)) {
        root["cdp_capable"] = true;
    }
}

// Forward declaration: ExtractElementProperties is defined in the UIA section below,
// but GetFocusedElement needs to call it first
void ExtractElementProperties(Json::Value& node, IUIAutomationElement* element);

/**
 * Get info on the currently focused UIA element
 * Helps debug "clicked X but focus is on Y", "Tab focus order is wrong", etc.
 */
Json::Value GetFocusedElement() {
    Json::Value result(Json::objectValue);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    if (FAILED(hr)) return result;
    bool shouldCoUninit = (hr == S_OK);

    IUIAutomation* pAutomation = nullptr;
    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IUIAutomation, (void**)&pAutomation);
    if (FAILED(hr) || !pAutomation) {
        if (shouldCoUninit) CoUninitialize();
        return result;
    }

    IUIAutomationElement* pFocused = nullptr;
    hr = pAutomation->GetFocusedElement(&pFocused);
    if (SUCCEEDED(hr) && pFocused) {
        ExtractElementProperties(result, pFocused);
        pFocused->Release();
    }

    pAutomation->Release();
    if (shouldCoUninit) CoUninitialize();

    return result;
}

// ============================================================
// UI Automation control-tree traversal
// ============================================================

/**
 * RAII wrapper: automatically releases COM objects
 */
template <typename T>
struct ComPtr {
    T* ptr = nullptr;
    explicit ComPtr(T* p = nullptr) : ptr(p) {}
    ~ComPtr() { if (ptr) ptr->Release(); }
    T* operator->() { return ptr; }
    T** operator&() { return &ptr; }
    operator bool() const { return ptr != nullptr; }
    T* Detach() { T* p = ptr; ptr = nullptr; return p; }
};

/**
 * Extract a UIA element's properties into the JSON node
 * Extracts: ControlType, Name, AutomationId, BoundingRectangle, IsEnabled,
 *           IsOffscreen, HelpText, Value, supported interaction patterns
 */
void ExtractElementProperties(Json::Value& node, IUIAutomationElement* element) {
    // ControlType
    CONTROLTYPEID typeId = 0;
    if (SUCCEEDED(element->get_CurrentControlType(&typeId))) {
        node["control_type"] = ControlTypeToString(typeId);
    } else {
        node["control_type"] = "Unknown";
    }

    // Name
    BSTR name = nullptr;
    if (SUCCEEDED(element->get_CurrentName(&name)) && name) {
        node["name"] = BSTRToUTF8(name);
        SysFreeString(name);
    } else {
        node["name"] = "";
    }

    // AutomationId — the unique control id set by the developer; a key field for debugging/locating
    BSTR automationId = nullptr;
    if (SUCCEEDED(element->get_CurrentAutomationId(&automationId)) && automationId &&
        SysStringLen(automationId) > 0) {
        node["automation_id"] = BSTRToUTF8(automationId);
        SysFreeString(automationId);
    }

    // HelpText — developer-provided tooltip / accessibility description
    BSTR helpText = nullptr;
    if (SUCCEEDED(element->get_CurrentHelpText(&helpText)) && helpText &&
        SysStringLen(helpText) > 0) {
        node["help_text"] = BSTRToUTF8(helpText);
        SysFreeString(helpText);
    }

    // BoundingRectangle → [left, top, right, bottom]
    RECT rect;
    if (SUCCEEDED(element->get_CurrentBoundingRectangle(&rect))) {
        Json::Value rectArray(Json::arrayValue);
        rectArray.append(rect.left);
        rectArray.append(rect.top);
        rectArray.append(rect.right);
        rectArray.append(rect.bottom);
        node["rect"] = rectArray;
    }

    // IsEnabled
    BOOL isEnabled = FALSE;
    if (SUCCEEDED(element->get_CurrentIsEnabled(&isEnabled))) {
        node["is_enabled"] = isEnabled ? true : false;
    }

    // IsOffscreen — whether the control is outside the visible area (relevant to "why can't I see it")
    BOOL isOffscreen = FALSE;
    if (SUCCEEDED(element->get_CurrentIsOffscreen(&isOffscreen))) {
        node["is_offscreen"] = isOffscreen ? true : false;
    }

    // Supported interaction patterns (only output non-empty sets to reduce noise)
    Json::Value patterns = DetectSupportedPatterns(element);
    if (patterns.size() > 0) {
        node["patterns"] = patterns;
    }

    // Value (if the control supports the Value Pattern)
    IUIAutomationValuePattern* valuePattern = nullptr;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId,
                                              (IUnknown**)&valuePattern)) && valuePattern) {
        BSTR value = nullptr;
        if (SUCCEEDED(valuePattern->get_CurrentValue(&value)) && value) {
            node["value"] = BSTRToUTF8(value);
            SysFreeString(value);
        }
        valuePattern->Release();
    }
}

/**
 * Build a dedup key for a node
 * Controls with an automation_id are treated as unique (explicitly identified in
 * code, never merged); controls without one are grouped by "type + name" so that
 * homogeneous list items can be folded.
 */
std::string BuildDedupKey(const Json::Value& node) {
    if (node.isMember("automation_id")) {
        return "id:" + node["automation_id"].asString();
    }
    return "t:" + node.get("control_type", "").asString() +
           "|n:" + node.get("name", "").asString();
}

/**
 * Recursively walk the UIA control tree and build the JSON structure
 *
 * @param element           current UIA element
 * @param walker            tree walker
 * @param depth             current recursion depth
 * @param maxDepth          max recursion depth (runtime argument)
 * @param filter            filter mode (All = output everything, Interactive = interactive controls only)
 * @param dedup             whether to fold consecutive homogeneous nodes (e.g. 100 identical list items)
 * @return                  JSON node; filtered decorative nodes return null
 */
Json::Value WalkUIATree(IUIAutomationElement* element,
                        IUIAutomationTreeWalker* walker,
                        int depth,
                        int maxDepth,
                        FilterMode filter,
                        bool dedup) {
    // Fetch the control type first, used by interactive filtering (avoid building nodes needlessly)
    CONTROLTYPEID typeId = 0;
    bool typeKnown = SUCCEEDED(element->get_CurrentControlType(&typeId));
    bool isInteractive = typeKnown && IsInteractiveControlType(typeId);

    // --filter interactive: on non-root nodes, drop decorative controls (non-interactive, non-container)
    if (filter == FilterMode::Interactive && depth > 0 &&
        typeKnown && !isInteractive && !IsContainerControlType(typeId)) {
        return Json::Value(Json::nullValue);
    }

    // Extract the current element's properties
    Json::Value node(Json::objectValue);
    ExtractElementProperties(node, element);

    // Stop recursing beyond the max depth
    if (depth >= maxDepth) {
        node["children_truncated"] = true;
        return node;
    }

    // Walk the children
    Json::Value children(Json::arrayValue);
    IUIAutomationElement* child = nullptr;

    // Get the first child element
    if (FAILED(walker->GetFirstChildElement(element, &child)) || !child) {
        // In interactive mode an empty container has no output value; prune it
        if (filter == FilterMode::Interactive && depth > 0 && !isInteractive) {
            return Json::Value(Json::nullValue);
        }
        return node;
    }

    // ---- Dedup state ----
    // pendingKey: dedup key of the most recently emitted node
    // pendingNode: most recently emitted node (only one sample kept per homogeneous group)
    // repeatCount: number of duplicate nodes folded so far in the current group
    std::string pendingKey;
    Json::Value pendingNode(Json::nullValue);
    int repeatCount = 0;

    // Flush the pending dedup group into children (append the repeat count to the sample node)
    auto flushPending = [&]() {
        if (!pendingNode.isNull()) {
            if (repeatCount > 0) {
                pendingNode["repeated_count"] = repeatCount;
            }
            children.append(pendingNode);
        }
        pendingNode = Json::Value(Json::nullValue);
        pendingKey.clear();
        repeatCount = 0;
    };

    int count = 0;
    while (child && count < MAX_CHILDREN_PER_LEVEL) {
        // Recurse into the child (may return null in filter mode)
        Json::Value childNode = WalkUIATree(child, walker, depth + 1,
                                            maxDepth, filter, dedup);

        if (!childNode.isNull()) {
            if (dedup) {
                std::string key = BuildDedupKey(childNode);
                if (key == pendingKey) {
                    // Homogeneous with the previous node: fold, count only
                    repeatCount++;
                } else {
                    // New group: flush the previous one, then hold the current node
                    flushPending();
                    pendingKey = key;
                    pendingNode = childNode;
                }
            } else {
                children.append(childNode);
            }
        }

        // Get the next sibling element
        IUIAutomationElement* next = nullptr;
        HRESULT hr = walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;

        if (FAILED(hr) || !next) break;
        count++;
    }

    // Flush the last group at the end
    if (dedup) {
        flushPending();
    }

    // Mark if children were truncated
    if (child) {
        Json::Value truncNode(Json::objectValue);
        truncNode["control_type"] = "Truncated";
        truncNode["name"] = "Child node limit reached (" + std::to_string(MAX_CHILDREN_PER_LEVEL) + ")";
        children.append(truncNode);
        child->Release();
    }

    if (children.size() > 0) {
        node["children"] = children;
    } else if (filter == FilterMode::Interactive && depth > 0 && !isInteractive) {
        // Interactive mode: a container with no surviving children is pruned entirely
        return Json::Value(Json::nullValue);
    }

    return node;
}

/**
 * Extract a window's control-tree structure via UIA
 *
 * @param hwnd      target window handle
 * @param maxDepth  max recursion depth
 * @param filter    filter mode (All / Interactive)
 * @param dedup     whether to fold homogeneous list items
 * @return          JSON object containing the control tree; empty JSON on failure
 */
Json::Value ExtractUIATree(HWND hwnd, int maxDepth = DEFAULT_MAX_UIA_DEPTH,
                           FilterMode filter = FilterMode::All,
                           bool dedup = true) {
    // Initialize COM (multi-threaded mode)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        // Thread was already initialized as apartment; fall back to a compatible mode
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    if (FAILED(hr)) {
        return Json::Value(Json::objectValue);
    }

    // CoUninitialize is needed only when this call actually initialized COM (S_OK);
    // S_FALSE means it was already initialized — do not (and must not) uninitialize
    bool shouldCoUninit = (hr == S_OK);

    // Create the IUIAutomation instance
    IUIAutomation* pAutomation = nullptr;
    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IUIAutomation, (void**)&pAutomation);
    if (FAILED(hr) || !pAutomation) {
        if (shouldCoUninit) CoUninitialize();
        return Json::Value(Json::objectValue);
    }

    // Get the root UIA element from the window handle
    IUIAutomationElement* pRoot = nullptr;
    hr = pAutomation->ElementFromHandle(hwnd, &pRoot);
    if (FAILED(hr) || !pRoot) {
        pAutomation->Release();
        if (shouldCoUninit) CoUninitialize();
        return Json::Value(Json::objectValue);
    }

    // Get the ControlView TreeWalker (skips decorative elements in the Raw view)
    IUIAutomationTreeWalker* pWalker = nullptr;
    hr = pAutomation->get_ControlViewWalker(&pWalker);
    if (FAILED(hr) || !pWalker) {
        pRoot->Release();
        pAutomation->Release();
        if (shouldCoUninit) CoUninitialize();
        return Json::Value(Json::objectValue);
    }

    // Recursively walk the control tree
    Json::Value tree = WalkUIATree(pRoot, pWalker, 0, maxDepth, filter, dedup);

    // Clean up resources
    pWalker->Release();
    pRoot->Release();
    pAutomation->Release();
    if (shouldCoUninit) CoUninitialize();

    return tree;
}

/**
 * Extract simple root-node info only (no recursive child traversal)
 */
Json::Value ExtractSimpleInfo(HWND hwnd) {
    Json::Value node(Json::objectValue);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    if (FAILED(hr)) return node;
    bool shouldCoUninit = (hr == S_OK);

    IUIAutomation* pAutomation = nullptr;
    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IUIAutomation, (void**)&pAutomation);
    if (FAILED(hr) || !pAutomation) {
        if (shouldCoUninit) CoUninitialize();
        return node;
    }

    IUIAutomationElement* pRoot = nullptr;
    hr = pAutomation->ElementFromHandle(hwnd, &pRoot);
    if (FAILED(hr) || !pRoot) {
        pAutomation->Release();
        if (shouldCoUninit) CoUninitialize();
        return node;
    }

    ExtractElementProperties(node, pRoot);

    pRoot->Release();
    pAutomation->Release();
    if (shouldCoUninit) CoUninitialize();

    return node;
}

// ============================================================
// Window screenshot (--screenshot argument)
// ============================================================

/**
 * Encode an HBITMAP as a PNG file (via WIC)
 *
 * @param hbm       bitmap handle
 * @param width     bitmap width
 * @param height    bitmap height
 * @param pathUtf8  output PNG file path (UTF-8)
 * @param errMsg    error description on failure
 * @return          whether the operation succeeded
 */
bool SaveBitmapToPNG(HBITMAP hbm, int width, int height,
                     const std::string& pathUtf8, std::string& errMsg) {
    // 1. Extract pixel data with GetDIBits (32bpp BGRA, top-down)
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // negative = top-down row order
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    HDC hdc = GetDC(nullptr);
    int lines = GetDIBits(hdc, hbm, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (lines == 0) {
        errMsg = "Failed to read window pixel data";
        return false;
    }

    // Force the alpha channel to opaque (PrintWindow does not guarantee alpha values)
    for (size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 0xFF;
    }

    // 2. Initialize WIC
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        errMsg = "WIC imaging factory initialization failed";
        return false;
    }

    // 3. Create the output stream
    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        factory->Release();
        errMsg = "Failed to create image output stream";
        return false;
    }
    std::wstring wpath = UTF8ToWString(pathUtf8);
    hr = stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        stream->Release();
        factory->Release();
        errMsg = "Cannot open output file: " + pathUtf8;
        return false;
    }

    // 4. Create the PNG encoder
    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        stream->Release();
        factory->Release();
        errMsg = "Failed to create PNG encoder";
        return false;
    }
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        encoder->Release();
        stream->Release();
        factory->Release();
        errMsg = "PNG encoder initialization failed";
        return false;
    }

    // 5. Write the frame data
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr) && frame) {
        frame->Initialize(props);
        frame->SetSize(width, height);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        hr = frame->WritePixels(height, static_cast<UINT>(width) * 4,
                                static_cast<UINT>(width * height * 4),
                                pixels.data());
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        frame->Release();
    }
    if (props) props->Release();
    encoder->Release();
    stream->Release();
    factory->Release();

    if (FAILED(hr)) {
        errMsg = "PNG image encoding failed";
        return false;
    }
    return true;
}

/**
 * Capture a window's content and save it as a PNG screenshot
 * Prefers PrintWindow (captures DWM-composited windows such as Chrome/Electron),
 * falls back to BitBlt grabbing the corresponding screen region.
 *
 * @param hwnd      target window handle
 * @param pathUtf8  output PNG file path (UTF-8)
 * @param errMsg    error description on failure
 * @return          whether the operation succeeded
 */
bool CaptureWindowToPNG(HWND hwnd, const std::string& pathUtf8, std::string& errMsg) {
    // WIC encoding needs a COM environment; initialize independently and balance the release
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hrCom == RPC_E_CHANGED_MODE) {
        hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    if (FAILED(hrCom)) {
        errMsg = "COM initialization failed";
        return false;
    }
    bool shouldCoUninit = (hrCom == S_OK);

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) {
        errMsg = "Failed to get window rect";
        if (shouldCoUninit) CoUninitialize();
        return false;
    }
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        errMsg = "Invalid window size";
        if (shouldCoUninit) CoUninitialize();
        return false;
    }

    // 1. Capture the window content into an in-memory bitmap
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    if (!hbm) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        errMsg = "Failed to create bitmap";
        if (shouldCoUninit) CoUninitialize();
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbm);

    // PrintWindow + PW_RENDERFULLCONTENT: force full content (incl. GPU-accelerated windows)
    BOOL captured = PrintWindow(hwnd, hdcMem, PW_RENDERFULLCONTENT);
    if (!captured) {
        // Fallback: grab the corresponding screen region directly (works for some custom-drawn / fullscreen apps)
        captured = BitBlt(hdcMem, 0, 0, width, height,
                          hdcScreen, rc.left, rc.top, SRCCOPY);
    }

    bool result = false;
    if (captured) {
        result = SaveBitmapToPNG(hbm, width, height, pathUtf8, errMsg);
    } else {
        errMsg = "Window content capture failed (both PrintWindow and BitBlt failed)";
    }

    // Clean up resources
    SelectObject(hdcMem, oldBmp);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    if (shouldCoUninit) CoUninitialize();
    return result;
}

// ============================================================
// Command-line argument parsing
// ============================================================

enum class Mode {
    Auto,    // auto-select the best strategy
    UIA,     // force the accessibility tree
    Simple   // basic info only
};

struct CliArgs {
    std::optional<uintptr_t> hwnd;   // target window handle
    bool hwndParseError = false;     // --hwnd argument failed to parse
    Mode mode = Mode::Auto;          // working mode
    int maxDepth = DEFAULT_MAX_UIA_DEPTH;  // max UIA recursion depth
    FilterMode filter = FilterMode::All;   // control-tree filter mode
    bool dedup = true;               // fold homogeneous list items (disabled by --no-dedup)
    std::optional<std::string> screenshotPath;  // --screenshot output path
    bool showHelp = false;           // whether to show help
};

/**
 * Show help text
 */
void PrintHelp() {
    std::cout <<
        "WindowSight - Windows window structured info extractor\n"
        "\n"
        "Usage:\n"
        "  WindowSight.exe [options]\n"
        "\n"
        "Options:\n"
        "  --hwnd <number>    Window handle (decimal or 0x-prefixed hex).\n"
        "                     Defaults to the foreground window.\n"
        "  --mode <mode>      Extraction mode:\n"
        "                       auto   - auto-select best strategy (default)\n"
        "                       uia    - force UI Automation tree traversal\n"
        "                       simple - basic window info only, no tree walk\n"
        "  --max-depth <N>    Max UIA tree recursion depth (default 20, range 1-50).\n"
        "                     Lower depth = faster and smaller output.\n"
        "  --filter <mode>    Tree filter mode:\n"
        "                       all         - output every control (default)\n"
        "                       interactive - keep interactive controls + necessary\n"
        "                                     containers; drop decorative nodes like\n"
        "                                     Text/Image; size can shrink 90%+\n"
        "  --no-dedup         Disable folding of homogeneous list items (default on:\n"
        "                     N identical items output as 1 sample + repeated_count)\n"
        "  --screenshot <path> Also save the window as PNG; JSON gets a screenshot field\n"
        "  --help             Show this help\n"
        "\n"
        "Output:\n"
        "  On success: JSON to stdout, exit code 0\n"
        "  On failure: error JSON to stderr, exit code 1\n"
        "\n"
        "Examples:\n"
        "  WindowSight.exe                       foreground window (auto mode)\n"
        "  WindowSight.exe --hwnd 123456         specific window\n"
        "  WindowSight.exe --mode uia            force UIA traversal\n"
        "  WindowSight.exe --mode simple         basic info only\n"
        "  WindowSight.exe --max-depth 10        limit depth to 10\n"
        "  WindowSight.exe --filter interactive --max-depth 5\n"
        "                                       interactive controls only (LLM-friendly)\n"
        "  WindowSight.exe --screenshot shot.png\n"
        "                                       structure + screenshot in one shot\n"
        "  WindowSight.exe --hwnd 0x1E240 --mode uia --max-depth 5\n";
}

/**
 * Parse command-line arguments
 */
CliArgs ParseArgs(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
        } else if (arg == "--hwnd" && i + 1 < argc) {
            // Parse the window handle (decimal or 0x-prefixed hexadecimal)
            std::string hwndStr(argv[++i]);
            try {
                int base = 10;
                if (hwndStr.size() > 2 && hwndStr[0] == '0' &&
                    (hwndStr[1] == 'x' || hwndStr[1] == 'X')) {
                    base = 16;
                    hwndStr = hwndStr.substr(2);
                }
                args.hwnd = std::stoull(hwndStr, nullptr, base);
            } catch (...) {
                // Argument is not a number: flag the error, main will report it
                args.hwndParseError = true;
            }
        } else if (arg == "--mode" && i + 1 < argc) {
            std::string mode(argv[++i]);
            if (mode == "auto") {
                args.mode = Mode::Auto;
            } else if (mode == "uia") {
                args.mode = Mode::UIA;
            } else if (mode == "simple") {
                args.mode = Mode::Simple;
            }
            // Invalid mode keeps the default Auto
        } else if (arg == "--max-depth" && i + 1 < argc) {
            // Parse the max recursion depth
            std::string depthStr(argv[++i]);
            try {
                int depth = std::stoi(depthStr);
                // Clamp to a sane range 1-50 to reject malformed input
                args.maxDepth = std::max(1, std::min(50, depth));
            } catch (...) {
                // Parse failure keeps the default value
            }
        } else if (arg == "--filter" && i + 1 < argc) {
            // Control-tree filter mode
            std::string filter(argv[++i]);
            if (filter == "interactive") {
                args.filter = FilterMode::Interactive;
            } else if (filter == "all") {
                args.filter = FilterMode::All;
            }
            // Invalid value keeps the default All
        } else if (arg == "--no-dedup") {
            // Disable folding of homogeneous list items
            args.dedup = false;
        } else if (arg == "--screenshot" && i + 1 < argc) {
            // Screenshot output path
            args.screenshotPath = std::string(argv[++i]);
        }
    }

    return args;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    // Parse command-line arguments
    CliArgs args = ParseArgs(argc, argv);

    // Show help
    if (args.showHelp) {
        PrintHelp();
        return 0;
    }

    // --hwnd argument failed to parse
    if (args.hwndParseError) {
        return OutputError("Invalid --hwnd argument; expected a decimal or 0x-prefixed hexadecimal number");
    }

    // Resolve the target window handle
    HWND targetHwnd;
    if (args.hwnd.has_value()) {
        targetHwnd = reinterpret_cast<HWND>(args.hwnd.value());
    } else {
        targetHwnd = GetForegroundWindow();
    }

    // Validate the window handle
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        return OutputError("Invalid window handle");
    }

    // Build the output JSON
    Json::Value root;
    PopulateBasicInfo(root, targetHwnd);

    std::string windowType = root["window_type"].asString();

    // Choose the structure-extraction strategy based on mode
    if (args.mode == Mode::Simple) {
        // simple mode: basic info only, no control-tree walk
        // no structure field is added
    } else if (args.mode == Mode::UIA) {
        // uia mode: force the UIA accessibility tree
        Json::Value tree = ExtractUIATree(targetHwnd, args.maxDepth,
                                          args.filter, args.dedup);
        if (!tree.isNull() && tree.isObject() && !tree.empty()) {
            root["structure"] = tree;
        } else {
            root["structure"] = Json::Value(Json::objectValue);
            root["structure"]["error"] = "Failed to extract UIA control tree";
        }
    } else {
        // auto mode: auto-select the best strategy
        // try a full UIA traversal for every window type; fall back to root info on failure
        Json::Value tree = ExtractUIATree(targetHwnd, args.maxDepth,
                                          args.filter, args.dedup);

        if (windowType == "browser") {
            // Browser window: additionally flag that DOM can be fetched via CDP
            root["cdp_recommended"] = true;
        }

        if (!tree.isNull() && tree.isObject() && !tree.empty()) {
            root["structure"] = tree;
        } else if (windowType != "browser") {
            // Fallback when UIA traversal fails: at least fetch root-node info
            Json::Value simpleInfo = ExtractSimpleInfo(targetHwnd);
            if (!simpleInfo.isNull() && simpleInfo.isObject() && !simpleInfo.empty()) {
                root["structure"] = simpleInfo;
            }
        }
    }

    // Fetch the focused element (output in every mode except simple; helps debug interaction issues)
    if (args.mode != Mode::Simple) {
        Json::Value focused = GetFocusedElement();
        if (!focused.isNull() && focused.isObject() && !focused.empty()) {
            root["focused_element"] = focused;
        }
    }

    // Optional: save a window screenshot (--screenshot argument)
    // The screenshot + structure JSON form an "overview + locate" combined input for the LLM
    if (args.screenshotPath.has_value()) {
        std::string errMsg;
        if (CaptureWindowToPNG(targetHwnd, args.screenshotPath.value(), errMsg)) {
            root["screenshot"] = args.screenshotPath.value();
        } else {
            root["screenshot_error"] = errMsg;
        }
    }

    // Output the result to stdout
    OutputJSON(root);

    return 0;
}
