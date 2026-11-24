# Changelog

All notable changes to **Radium Download Manager** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- Cleaned up repository hygiene: removed committed release binaries, obsolete AI-generated audit dumps, and duplicate root files; relocated localized READMEs to `docs/i18n/` and helper scripts to `scripts/`.
- Replaced hardcoded local filesystem paths in helper and benchmark scripts with repository-relative paths.
- Added `SECURITY.md`, `CODE_OF_CONDUCT.md`, `.editorconfig`, GitHub issue/PR templates, Dependabot configuration, and CI repository hygiene checks.

## [1.0.0] - 2026-11-10

### Added
- Native C++20 / Win32 desktop download manager (`radium.exe`) with adaptive segmented HTTP/HTTPS engine (`WinHTTP` + IOCP).
- Out-of-process HLS/DASH/MSS stream pipeline via `N_m3u8DL-RE` and `FFmpeg` in isolated Windows Job Objects.
- Built-in BitTorrent v1 engine with metadata exchange (BEP 9), tracker announces, piece verification, and selective file downloads.
- Manifest V3 browser extension for Chromium and Firefox communicating via `radium-nmh.exe` and per-user ACL-restricted Windows Named Pipes.
- Windows x64 and ARM64 installers and portable packages.
