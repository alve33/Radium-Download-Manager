# Radium Download Manager — Apple macOS UI/UX Vision & Win32 Engineering Specification for Sub-Windows

**Document Role:** Senior Apple HIG UI/UX Design System & Native Win32 / C++20 Sub-Window Overhaul Blueprint  
**Scope:** Exclusive Specification for All Application Sub-Windows (`New Download`, `Add Torrent`, `Download Already Exists`, and `Alert / Settings Modals`)  
**Reference Baseline:** Matches the dark macOS-inspired Main Window aesthetic (`#141417` / `#1C1C1F` obsidian surfaces, `#E11D48` crimson-rose accent, pill badges, and hairline specular borders)

## Part 1: Visual Audit — Why the Current Sub-Windows Fail the Main UI Vision

An inspection of the current `New Download` (`image_11423a.png`) and `Add Torrent` (`image_1141dd.png`) dialogs reveals severe visual and technical regressions where raw Win32 controls leak through the dark theme:

| Visual Defect Category | Observed in `New Download` (`image_11423a.png`) | Observed in `Add Torrent` (`image_1141dd.png`) | Apple UI Target Fix |
| :--- | :--- | :--- | :--- |
| **Redundant & Clunky Window Chrome** | Standard Win32 title bar (`New Download — Radium`) sits directly above a duplicate internal heading (`New Download`) and a harsh full-width gray line. | Standard Win32 title bar with truncated torrent name and raw border frame. | Single unified custom borderless header (`44px` height) with `DWMWCP_ROUND` (`12px` corner radius), subtle subtitle, and a single circular close button (`24×24px`). |
| **Broken / Invisible Static Labels & Stray Lines** | Labels use legacy colons (`Download URL:`, `File Name:`) and tight vertical stacking. | **Critical Bug:** A red horizontal line slices straight through `Torrent Management Mode:`. Labels for Save Path, Category (`Torrents`), Tags, Stop Condition (`None`), and Content Layout (`Original`) are completely missing or painted black-on-black. | Eliminate stray accent lines; fix `WM_CTLCOLORSTATIC` / owner-draw text rendering; use uppercase `11px` Apple inspector section headers (`#8E8E93`) without colons. |
| **Unstyled Native Win32 ComboBoxes** | `Quality / Format` dropdown shows a jarring classic Windows light-gray button with a black arrow on the right edge. | All 4 dropdowns (`Manual`, `Torrents`, `None`, `Original`) expose bright white/gray Windows 95-style dropdown arrow buttons inside dark boxes. | 100% owner-drawn / Direct2D rounded pill-rect ComboBox (`#26262B` fill, `6px` radius, `1px` border `#38383F`, custom vector chevron `⌄` in `#98989F`). |
| **Mismatched Checkbox Strips & Stark White Boxes** | `Start download immediately` and `Schedule at:` use raw square white Win32 checkboxes (`BS_AUTOCHECKBOX`). | Every checkbox sits inside a pitch-black rectangular strip (`#0E0E10`) that clashes with the `#1C1C1F` sidebar background, paired with glaring white square checkboxes. | Paint checkbox backgrounds transparently over parent surface (`HOLLOW_BRUSH`); custom-draw `16×16px` rounded (`4px` radius) checkboxes that fill with `#E11D48` and a white vector checkmark when checked. |
| **Harsh Focus Rectangles & Floating Text** | Focused `Download URL` input has a sharp, boxy `1px` red outline; `Size: Unknown` floats in raw cyan (`#00BCD4`) beside the dropdown. | Top-right filter box has a harsh red rectangle outline with no icon or placeholder text; a stray short red line sits above `Name`. | Replace sharp red outlines with a soft `2px` Apple focus ring (`rgba(225, 29, 72, 0.45)`) + `#E11D48` border; turn `Size: Unknown` into a sleek metadata pill badge (`#26262B` fill, `#A1A1AA` text). |
| **Misaligned Footer Actions** | `Download` and `Cancel` buttons float near the center-right without a grounding footer bar. | `Save .torrent...` is trapped inside the left box while `Selected 7.50 GiB...` floats awkwardly above `Add Torrent` and `Cancel`. | Dedicated `56px` bottom footer bar (`#161619`) separated by a `1px` hairline (`rgba(255,255,255,0.07)`), with utility info/actions anchored left and `Cancel` + `Primary Action` anchored right. |

## Part 2: Unified Apple Dark Design System for All Sub-Windows

Every sub-window in Radium must share an identical visual language derived from macOS Sequoia/Sonoma dark mode and the Radium Main Window:

### 1. Surface & Color Tokens

| Token Name | Hex / RGBA Value | Usage Across Sub-Windows |
| :--- | :--- | :--- |
| `Surface.WindowBg` | `#18181C` | Primary sub-window background canvas |
| `Surface.SidebarBg` | `#141417` | Left configuration inspector pane in split sub-windows (`Add Torrent`) |
| `Surface.CardGroup` | `#1F1F24` | Grouped inset settings cards (macOS `NSBox` style, `8px` radius) |
| `Surface.ControlFill` | `#26262C` | Text inputs, custom ComboBoxes, and secondary buttons (default state) |
| `Surface.ControlHover` | `#2E2E36` | Hover state for inputs, dropdowns, and secondary buttons |
| `Surface.FooterBg` | `#141417` | Bottom action bar background (`56px` height) |
| `Border.WindowOuter` | `#2E2E36` | `1px` DWM window border (`DWMWA_BORDER_COLOR`) |
| `Border.Hairline` | `rgba(255, 255, 255, 0.07)` | `1px` section dividers, card borders, and table row separators |
| `Border.Control` | `#363640` | `1px` subtle border on text inputs and ComboBoxes |
| `Accent.Primary` | `#E11D48` | Primary CTA buttons (`Download`, `Add Torrent`), checked checkboxes, active focus border |
| `Accent.PrimaryHover` | `#F43F5E` | Hover state for primary CTA buttons |
| `Accent.FocusRing` | `rgba(225, 29, 72, 0.30)` | `3px` outer glow ring around focused input or combobox |
| `Text.Primary` | `#F4F4F6` | Window titles, input values, file names, active checkbox labels |
| `Text.Secondary` | `#A1A1AA` | Field labels, table metadata (`Size`, `Priority`), subtitles |
| `Text.Muted` | `#71717A` | Section headers, placeholders, free disk space caption |
| `Semantic.Success` | `#30D158` | Sufficient disk space indicator, valid probe status |
| `Semantic.Warning` | `#FF9F0A` | Duplicate file conflict warning badge, low disk space |

### 2. Typography & Spatial Grid (`Segoe UI Variable Display` / `Inter`)

* **Window Title:** `15px` SemiBold (`600`), `#F4F4F6`, left-aligned in the unified header bar (or centered in compact alert sheets).
* **Window Subtitle:** `12px` Regular (`400`), `#8E8E93`, positioned `2px` below the title when needed.
* **Section Group Header:** `11px` SemiBold (`600`), uppercase, letter-spacing `+0.6px`, `#71717A` (e.g., `SOURCE & DESTINATION`, `DELIVERY OPTIONS`). **Never put colons (`:`) after labels.**
* **Field Label:** `12.5px` Medium (`500`), `#A1A1AA`, positioned `6px` above inputs or inline on a 2-column form grid.
* **Input & Control Text:** `13px` Regular (`400`), `#F4F4F6`.
* **Secondary Metadata / Captions:** `11.5px` Regular (`400`), `#71717A` (tabular numerals enabled for file sizes and speeds).
* **Spacing System:** Strict `8px` base unit (`16px` outer window padding, `12px` vertical gap between form groups, `8px` gap between sibling controls, `6px` control corner radius, `8px` group card corner radius).

### 3. Custom-Drawn Win32 Control Primitives (Zero Native OS Chrome)

1. **Custom Text Input (`RadiumEditBox`):**
   * Height: `34px` (`28px` for compact search/time fields).
   * Background: `#26262C`, `6px` rounded corners, `1px` border `#363640`.
   * Focused State: Border transitions to `#E11D48` with an outer `2px` soft halo in `rgba(225, 29, 72, 0.28)`.
   * Inner Padding: `10px` left/right text margin (`32px` left margin when a leading icon like a link or search magnifier is present).
2. **Custom Dropdown / ComboBox (`RadiumComboBox`):**
   * **Must NEVER use native Win32 `CBS_DROPDOWNLIST` non-client button rendering** (which paints the ugly white/gray Windows box).
   * Implement as a full owner-drawn button/control (`34px` height, `#26262C` fill, `6px` radius, `1px` `#363640` border) that renders the selected text on the left (`10px` padding) and a crisp vector dual-chevron (`⇅` or `⌄` in `#A1A1AA`, `12px` wide) on the right (`10px` padding).
   * Clicking opens a dark custom-styled popup list (`#222228` background, `#2E2E36` border, `8px` rounded corners, `#E11D48` highlight pill on the hovered/selected item).
3. **Custom Checkbox (`RadiumCheckBox`):**
   * **Must NEVER use native `BS_AUTOCHECKBOX` default background or square box.**
   * Handle painting on a transparent background matching the parent container (`#18181C` or `#1F1F24`)—eliminating the dark black horizontal bars seen in `image_1141dd.png`.
   * Box geometry: `16×16px`, `4px` corner radius.
   * Unchecked: `#26262C` fill with `1px` `#484854` border.
   * Checked: `#E11D48` solid fill with a `1.8px` stroke white vector checkmark (`✓`).
   * Label: `12.5px` Regular `#E4E4E7`, `8px` to the right of the box, vertically centered.
4. **Buttons (`RadiumButton`):**
   * Height: `32px`, `6px` corner radius, `14px` horizontal padding, `12.5px` Medium (`500`).
   * **Primary CTA (`Download`, `Add Torrent`, `Overwrite`):** Solid `#E11D48` fill, subtle top inner highlight `rgba(255,255,255,0.15)`, white `#FFFFFF` text.
   * **Secondary Button (`Cancel`, `Browse...`, `Save .torrent...`):** `#26262C` fill, `1px` `#383842` border, `#E4E4E7` text (`#2E2E36` on hover).

## Part 3: Detailed Vision & Layout Specification for Each Sub-Window

### Sub-Window 1: "New Download" Sheet (`NewDownloadDialog`)

**Purpose:** Triggered when clicking `+ Add` in the main toolbar or when the browser extension intercepts a non-duplicate HTTP/HTTPS download (`alreadyExists == false`).

#### Window Geometry & Chrome
* **Dimensions:** `500px` width × `410px` height (fixed size, centered over the Main Window with a subtle dark backdrop dim if modal).
* **Unified Header (`52px` height):**
  * Left: Small circular accent icon (`28×28px`, `rgba(225, 29, 72, 0.14)` fill with `#E11D48` download arrow icon) + Title **`New Download`** (`15px` SemiBold `#F4F4F6`) + Subtitle **`Verify link details and destination before starting`** (`11.5px` `#8E8E93`).
  * Right: Circular `26×26px` close button (`✕` in `#8E8E93`, `#26262C` hover pill).
  * Bottom of Header: `1px` hairline divider `rgba(255, 255, 255, 0.07)`. *(Delete the duplicate native title bar so "New Download" is never written twice).*

#### Body Content (`20px` horizontal padding, `16px` vertical padding)
1. **Source URL Group:**
   * Label row: `Download URL` (`12px` Medium `#A1A1AA`) on the left; inline status pill on the right (shows `Probing...` with a subtle spinner while resolving headers, or `HTTP 200 • Resumable` in `#30D158` once resolved).
   * Input (`36px` height): Leading link icon (`🔗` vector in `#71717A`), URL text in `#F4F4F6`, and a trailing compact `Paste` icon button (`📋`) inside the right edge of the input when empty.
2. **Resolved File Card (Replaces the plain "File Name" box + floating cyan "Size: Unknown"):**
   * A sleek `12px`-padded inset card (`#1F1F24` fill, `8px` radius, `1px` `rgba(255,255,255,0.06)` border) containing:
     * **Left (`36×36px`):** Dynamic macOS-style document/archive/video vector icon badge based on the file extension (`.pdf`, `.zip`, `.mp4`, `.exe`).
     * **Center (Editable File Name Input):** Clean borderless or subtle-bordered input (`32px` height) showing the resolved filename (e.g., `weaver_gere_4_6_3.pdf`), allowing instant inline renaming before download starts.
     * **Right (File Size & Type Pill Badge):** A rounded pill badge (`#2A2A32` fill, `6px` radius, `6px 10px` padding) displaying `24.8 MB` (or `Unknown Size` in muted `#8E8E93`—never raw cyan text).
3. **Save Location & Disk Capacity Group:**
   * Label row: `Save Location` on the left; `145.4 GB available` with a tiny `6px` green status dot (`#30D158`) on the right.
   * Input row: Path input (`C:\Users\sayed\Downloads\Flux`, `34px` height, `#26262C`) taking `78%` width + `Browse...` secondary button (`22%` width, `8px` gap) with a folder icon.
4. **Format / Stream Quality & Delivery Group (2-Column Row):**
   * **Left Column (`60%` width) — `Quality / Format`:** Custom-drawn `RadiumComboBox` (`Original / Direct File`, or parsed video/audio streams when downloading media) with a clean vector chevron (`⌄`).
   * **Right Column (`40%` width) — `Connections`:** Compact segmented or dropdown selector (`8 Threads (Default)`, `16 Threads (Max)`, `1 Thread`).
5. **Start Behavior Bar (Inset Row):**
   * Single horizontal bar (`40px` height, `#1F1F24` fill, `8px` radius, `0 12px` padding) containing:
     * Left: Custom rounded checkbox `[✓] Start immediately`.
     * Right: Custom rounded checkbox `[ ] Schedule at` + compact `68px × 26px` rounded time picker (`23:00` in tabular numerals, disabled/dimmed at `40%` opacity until `Schedule at` is checked).

#### Footer Bar (`56px` height, `#141417` fill, `1px` top border `rgba(255,255,255,0.07)`)
* Left: Subtle caption `Category: Documents (Auto)` in `#71717A`.
* Right: `Cancel` button (`84px × 32px`, `#26262C`) + `Download` primary button (`110px × 32px`, `#E11D48` with download arrow icon).

### Sub-Window 2: "Add Torrent" Inspector Window (`AddTorrentDialog`)

**Purpose:** Triggered when opening a `.torrent` file or `magnet:` URI. Replaces the broken layout in `image_1141dd.png` with a macOS Transmission / Finder-inspired split inspector.

#### Window Geometry & Chrome
* **Dimensions:** `880px` width × `580px` height (resizable with minimum `780×500px`).
* **Unified Header (`48px` height, `#141417`):**
  * Left: Accent torrent magnet/cube icon + **`Add Torrent`** (`14px` SemiBold `#F4F4F6`) + separator dot `•` + torrent title (`Cyberpunk - Edgerunners - S01 - DUAL 1080p WEB H.264 -NanDesuKa (NF)`) in `#A1A1AA` with middle-ellipsis truncation if long.
  * Right: Window minimize/close controls (`26×26px` circular hover pills).
  * **Bug Fix:** Completely remove the stray red horizontal line that currently cuts across the top-left label and the short red bar above the file table header.

#### Split Body Layout (Left Inspector Sidebar `300px` | `1px` Hairline Divider | Right File Tree Pane `580px`)

##### Left Pane: Configuration Inspector (`300px` width, `#141417` background, `14px` padding, scrollable if needed)
Organize the controls into **3 clean macOS Inset Cards (`#1C1C21` fill, `8px` radius, `12px` inner padding, `10px` vertical gap between cards)** with explicit, visible labels:

1. **Card 1 — `DESTINATION & STORAGE`** (Section Header in `10.5px` uppercase SemiBold `#71717A`):
   * **Field 1:** Label `Management Mode` (`12px` `#A1A1AA`) above custom `RadiumComboBox` (`Manual` / `Automatic`).
   * **Field 2:** Label `Save Path` (`12px` `#A1A1AA`) above path input (`C:\Users\sayed\Downloads\Flux`) + compact `34×34px` folder icon button (`📁`) on the right.
   * **Checkboxes (`6px` vertical gap, transparent background):**
     * `[ ] Use temporary path for incomplete files`
     * `[✓] Remember last used save path`
2. **Card 2 — `ORGANIZATION`** (Section Header in `10.5px` uppercase SemiBold `#71717A`):
   * **Field 1:** Label `Category` (`12px` `#A1A1AA`) above custom `RadiumComboBox` (`Torrents`) + `[ ] Set as default category` checkbox below it.
   * **Field 2:** Label `Tags (comma-separated)` (`12px` `#A1A1AA`) above `RadiumEditBox` with placeholder `e.g., anime, 1080p, season-1`.
3. **Card 3 — `TRANSFER & QUEUE OPTIONS`** (Section Header in `10.5px` uppercase SemiBold `#71717A`):
   * **Row 1 (2-Column Inline Grid):**
     * Left (`50%`): Label `Stop Condition` above custom `RadiumComboBox` (`None` / `Metadata Received` / `Files Checked`).
     * Right (`50%`): Label `Content Layout` above custom `RadiumComboBox` (`Original` / `Create Subfolder` / `Don't Create Subfolder`).
   * **Checkboxes (`6px` vertical gap, transparent background — NO black background strips):**
     * `[✓] Start torrent immediately`
     * `[ ] Add to top of queue`
     * `[ ] Skip hash check`
     * `[ ] Download in sequential order`
     * `[ ] Download first and last pieces first`

##### Right Pane: Torrent Contents & File Tree (`580px` width, `#18181C` background)
1. **Top Filter & Selection Toolbar (`48px` height, `12px 16px` padding):**
   * **Left:** Segmented pill button group (`Select All` | `Select None`, `#26262C` fill, `28px` height, `12px` text, `1px` `#363640` border).
   * **Right (`240px` width):** Apple Spotlight-style filter input (`28px` height, `#26262C` fill, `14px` pill radius, leading magnifying glass icon `🔍` in `#71717A`, placeholder `Filter files in torrent...`, subtle focus ring instead of a harsh red box).
2. **Modern macOS Finder-Style TreeListView:**
   * **Column Header Bar (`30px` height, `#141417` fill, `1px` top and bottom hairline borders):**
     * `Name` (flex width, min `340px`, left-aligned)
     * `Size` (`90px` width, right-aligned)
     * `Priority` (`110px` width, center-aligned)
   * **Root Folder Row (`34px` height, `#1F1F25` subtle highlight):**
     * Disclosure chevron (`▾` in `#A1A1AA`) + Custom rounded `16×16px` checkbox (`#E11D48` when checked, `#E11D48` dash `—` when partially selected) + macOS Amber/Coral Folder Icon (`📁`) + bold folder name (`Cyberpunk - Edgerunners - S01...`) + total size (`7.50 GiB` in `#A1A1AA`).
   * **Child File Rows (`32px` height, subtle alternating tint `#18181C` / `#1B1B20`, hover highlight `#24242C`):**
     * `24px` left tree indentation (no ugly dotted Win32 tree lines) + Custom rounded `16×16px` checkbox + crisp vector media icon (`🎬` in `#8E8E93`) + episode filename in `#E4E4E7` (with middle-ellipsis so `S01E01` and `.mkv`/`.mp4` remain visible!).
     * `Size` cell: Right-aligned tabular numerals (`703.36 MiB` in `#A1A1AA`).
     * `Priority` cell: Compact clickable pill badge (`Normal` in `#26262C` text `#A1A1AA`; `High` in `rgba(225,29,72,0.18)` text `#FB7185`; `Do Not Download` in `#1F1F24` text `#52525B`).

#### Unified Bottom Footer Bar (`56px` height, spans full `880px` width, `#141417` fill, `1px` top border)
* **Left Section (Aligned with Left Sidebar):** `[ ] Don't show this dialog again` custom checkbox + `Save .torrent...` secondary button (`30px` height).
* **Center-Left Summary Badge (Aligned with Right Pane start):** `Selected 7.50 GiB of 7.50 GiB` in `#F4F4F6` Medium + `(10 of 10 files)` in `#71717A` + disk space check `(145.4 GB free • OK)` in `#30D158`.
* **Right Section:** `Cancel` button (`88px × 32px`, `#26262C`) + `Add Torrent` primary accent button (`120px × 32px`, `#E11D48`).

### Sub-Window 3: "Download Already Exists" Conflict Resolution Sheet (`DuplicateDownloadDialog`)

**Purpose:** Required by Stage 2 & Stage 3 of the Radium Browser Extension & IPC Routing Pipeline whenever `alreadyExists == true` (queue match or `std::filesystem::exists(targetFilePath)`). Never auto-starts or silently pauses with an error; always presents this clean macOS conflict sheet.

#### Window Geometry & Chrome
* **Dimensions:** `480px` width × `340px` height (modal sheet centered over Main Window).
* **Header (`56px` height):**
  * Warning Amber/Coral Badge (`32×32px` rounded circle in `rgba(255, 159, 10, 0.15)` with `#FF9F0A` duplicate document icon) + Title **`File Already Exists`** (`15px` SemiBold `#F4F4F6`) + Subtitle **`A file with this name or URL is already in your library or destination folder.`** (`12px` `#8E8E93`).

#### Body Content (`20px` padding)
1. **Comparison Card (`#1F1F24` fill, `8px` radius, `12px` padding, `1px` border):**
   * **File Name:** `weaver_gere_4_6_3.pdf` (`13.5px` SemiBold `#F4F4F6`)
   * **Folder Path:** `C:\Users\sayed\Downloads\Flux` (`11.5px` `#71717A`)
   * **2-Column Metadata Comparison Row (`8px` top margin, separated by a `1px` hairline):**
     * **Existing File on Disk / Queue:** Status badge (`Completed` or `Paused at 42%`) • Size `18.4 MB` • Modified `Today, 21:14`
     * **Incoming Download Request:** Source `overleaf.com` • Size `18.4 MB`
2. **Action Selection List (3 Interactive Radio/Action Cards or Direct Buttons):**
   * **Option 1 — `Keep Both (Auto-Rename)` (Default Recommended):** Saves as `weaver_gere_4_6_3 (1).pdf` without touching the existing file.
   * **Option 2 — `Overwrite Existing File`:** Replaces the file on disk and restarts from `0%`.
   * **Option 3 — `Resume / Use Existing`:** If the existing task is incomplete/paused, resumes it; if already `100%` completed, opens/highlights the existing file in the library.

#### Footer Bar (`56px` height, `#141417` fill)
* Left: `Cancel` button (`84px × 32px`, `#26262C`).
* Right: `Overwrite` secondary destructive button (`#2A181E` fill, `#FB7185` border/text) + `Keep Both (Rename)` primary CTA button (`#E11D48` fill, `#FFFFFF` text).

### Sub-Window 4: "Settings / Preferences" & Destructive Alert Modals

* **Settings Sub-Window (`640px × 480px`):**
  * Follow macOS App Settings layout: a unified top toolbar (`64px` height, `#141417`) with centered icon-and-label tabs (`General`, `Downloads`, `Torrents`, `Browser Extension`, `Network`), each rendered as a `64×48px` rounded pill (`#26262C` background and `#E11D48` icon tint when active).
  * Body uses centered `560px`-wide grouped inset cards (`#1F1F24` fill, `8px` radius) with label-left / control-right rows separated by `1px` `rgba(255,255,255,0.06)` hairlines (macOS Sonoma System Settings style).
* **Delete / Remove Task Confirmation Sheet (`400px × 210px`):**
  * Compact borderless rounded modal (`12px` radius) with title `Remove "filename.ext"?`, subtitle `Choose whether to remove the item from your Radium list only or also move the downloaded file to the Recycle Bin.`, a custom checkbox `[ ] Also delete file from disk`, and footer buttons `Cancel` + `Remove`.

## Part 4: Copy-Pasteable Master Prompt for Your Coding AI

Copy and paste the prompt below directly into your coding AI to overhaul all C++20 / Win32 sub-windows to match the Main Window Apple UI vision:

```text
Redesign and refactor all Sub-Windows in the Radium Download Manager C++20 / Win32 application (specifically NewDownloadDialog, AddTorrentDialog, DuplicateDownloadDialog, and Settings/Alert dialogs) so they strictly match the dark Apple macOS Sequoia/Sonoma UI aesthetic of the Main Window and eliminate all legacy Win32 visual glitches:

1. Eliminate Legacy Win32 Chrome & Fix Window Framing:
- Enable rounded corners on all sub-window HWNDs via DwmSetWindowAttribute(..., DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND) and set a 1px dark border (#2E2E36) via DWMWA_BORDER_COLOR.
- Remove the duplicate title bar problem: either use a custom borderless title bar (WM_NCCALCSIZE / WM_NCHITTEST with a 48px-52px header bar in #141417 and a custom 26x26px circular close button) OR hide the redundant internal big header text so the dialog title is never shown twice.
- Every sub-window must have a dedicated 56px bottom footer bar (#141417 background with a 1px top hairline divider rgba(255,255,255,0.07)) housing the right-aligned Cancel (#26262C) and Primary Action (#E11D48) buttons.

2. Replace Broken Native Win32 Controls with 100% Owner-Drawn / Custom-Painted Apple Controls:
- ComboBoxes (Dropdowns): NEVER allow Windows to paint the default light-gray/white dropdown arrow button on ComboBoxes. Subclass or owner-draw all ComboBoxes as 34px-tall rounded rectangles (#26262C fill, 6px radius, 1px #363640 border, #F4F4F6 text) with a custom-drawn vector chevron on the right and a dark popup list (#222228).
- Checkboxes: Fix WM_CTLCOLORSTATIC / WM_CTLCOLORBTN so checkboxes NEVER render with pitch-black rectangular background strips or classic white square boxes. Paint checkbox backgrounds transparently over the parent surface (#18181C or card #1F1F24) and custom-draw a 16x16px rounded box (4px radius, #26262C fill / #484854 border when unchecked; #E11D48 fill with a white vector checkmark when checked).
- Text Inputs: Custom-draw 34px-tall rounded input fields (#26262C fill, 6px radius, 1px #363640 border). On focus, replace the harsh 1px red rectangle with a 1px #E11D48 border plus a soft 2px outer glow ring (rgba(225, 29, 72, 0.28)).
- Labels: Remove trailing colons (":") from all field labels. Fix missing/invisible static labels in AddTorrentDialog by ensuring all static labels render in #A1A1AA (12px Medium) and section headers render in uppercase #71717A (11px SemiBold) with SetBkMode(hdc, TRANSPARENT).

3. Specific Overhaul for "New Download" Sub-Window (500x410px):
- Header: Download icon badge + "New Download" title + subtitle "Verify link details and destination before starting".
- URL Input: 36px height with leading link icon, paste button, and soft focus glow.
- Resolved File Card: Group File Name and File Size into an inset card (#1F1F24, 8px radius) with a dynamic file-type vector icon on the left, editable File Name input in the center, and a sleek pill badge on the right for file size (e.g., "24.8 MB" or "Unknown Size" in #2A2A32 pill with #A1A1AA text — remove the raw cyan "Size: Unknown" text).
- Save Location Row: Path input + "Browse..." button, with "145.4 GB available" and a green status dot (#30D158) aligned cleanly in the label row above.
- Delivery Row: Custom dark Quality/Format dropdown + compact inset bar for "[✓] Start immediately" and "[ ] Schedule at [23:00]" (dimming the time input when Schedule is unchecked).
- Footer Bar: Right-aligned "Cancel" (#26262C) and "Download" (#E11D48) pill buttons.

4. Specific Overhaul for "Add Torrent" Sub-Window (880x580px Split Inspector):
- Remove the stray red horizontal lines that currently slice through "Torrent Management Mode" and sit above the file table header.
- Left Sidebar Inspector (300px width, #141417 background): Organize all settings into 3 macOS inset cards (#1C1C21 fill, 8px radius, 12px padding) with visible labels:
  * Card 1 (DESTINATION & STORAGE): "Management Mode" dropdown, "Save Path" input + folder button, "[ ] Use temporary path for incomplete files", "[✓] Remember last used save path".
  * Card 2 (ORGANIZATION): "Category" dropdown, "[ ] Set as default category", and "Tags" input with placeholder "e.g., anime, 1080p".
  * Card 3 (TRANSFER & QUEUE OPTIONS): 2-column row for "Stop Condition" and "Content Layout" dropdowns, followed by cleanly spaced custom checkboxes ("Start torrent immediately", "Add to top of queue", "Skip hash check", "Download in sequential order", "Download first and last pieces first").
- Right File Tree Pane (580px width, #18181C background):
  * Top Toolbar: Compact "Select All" / "Select None" segmented pill buttons on the left; 240px rounded filter input with magnifying glass icon and placeholder "Filter files in torrent..." on the right.
  * Finder-Style TreeListView: Clean "Name | Size | Priority" header (#141417), custom rounded checkboxes, vector folder/media icons, middle-ellipsis filename truncation so episode numbers and extensions stay visible, right-aligned tabular file sizes, and pill badges for Priority ("Normal", "High", "Skip").
- Unified Footer Bar (full 880px width): Left side has "[ ] Don't show again" + "Save .torrent..."; center-left shows "Selected 7.50 GiB of 7.50 GiB (10/10 files)"; right side has "Cancel" and "Add Torrent" (#E11D48).

5. Specific Overhaul for "Download Already Exists" Sub-Window (480x340px):
- Present a clean macOS conflict sheet showing an inset comparison card (Filename, Save Path, Existing File Status/Size/Date vs. Incoming URL/Size) and clear action buttons in the footer: "Cancel", "Overwrite Existing", and primary CTA "Keep Both (Auto-Rename)" (or "Resume" if incomplete).
```
