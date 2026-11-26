# ADR 0002: C++20, CMake, plain Win32 UI

- Status: accepted
- Date: 2026-11-26

## Context

The APO must be a native in-process COM DLL loaded into `audiodg.exe`, with
hard real-time constraints, and it must share an exact memory layout with
the app.

## Decision

- **C++20 with MSVC**, one language for app and APO, so the shared-memory
  layout lives in one header compiled into both.
- **CMake + Ninja presets**, GoogleTest via `FetchContent` (pinned, hashed).
- **Static CRT** everywhere: no VC++ redistributable dependency, which matters
  in particular inside `audiodg.exe`.
- **Plain Win32 UI** for v0.1: zero UI dependencies, a small single `.exe`,
  native look, per-monitor DPI. The UI is a thin view over a UI-free
  `AppController`, so it can later be replaced (e.g. WinUI 3 or a tray-only
  front-end) without touching the engine.

## Alternatives considered

- Rust (`windows-rs`): viable, but APO samples and headers are C++; COM APO
  implementation in Rust is uncharted.
- C#/.NET for the app + C++ for the APO: duplicated protocol definitions
  across languages, interop for process loopback, larger distribution.
- Qt/ImGui UI: heavy or non-native for a small control panel.
