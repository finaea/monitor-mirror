# Monitor Mirror

Show a live, low-lag copy of part of one monitor on another monitor.

It was made for drawing on a pen display / tablet: you draw on your **main
screen** (where the drawing app is), and an exact copy appears on a **second
screen** — so someone watching, a stream, or a reference monitor sees exactly
what you draw, with almost no delay.

> **Windows 10 / 11 only** (64-bit). Not available for macOS or Linux.

---

## Just want to use it?

👉 **Open the [`cpp`](cpp) folder and follow its README.**

That is the app to run. It's a single small `mirror.exe`, and the first time you
start it, a setup window walks you through picking your screens and the area to
mirror. No installation, no technical steps.

---

## What's in this project

| Folder | What it is |
|---|---|
| [`cpp/`](cpp) | **The app you should use.** Tiny, fastest, set up with a visual window. |
| [`python/`](python) | An **older version kept for reference/archival**. Same idea, but the settings are edited in code instead of a window. |

A detailed side-by-side of the two versions lives in
[`python/build_compare.md`](python/build_compare.md).
