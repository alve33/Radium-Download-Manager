# Radium Download Manager Engineering Documentation Index

**Document Status:** Active Navigation Map
**Last Reviewed:** November 24, 2026
**Scope:** Guide to the Radium Download Manager engineering documentation suite, architectural decision records, and subsystem specifications.

---

## 1. Quick Start & Mandatory Governance

1. **[../README.md](../README.md)** — Project overview, architecture summary, live benchmarks, and build instructions.
2. **[../AGENTS.md](../AGENTS.md)** — Mandatory coding instructions, RAII rules, security invariants, and documentation sync requirements.
3. **[../CONTRIBUTING.md](../CONTRIBUTING.md)** — Development workflow, commit guidelines, and testing requirements.
4. **[../SECURITY.md](../SECURITY.md)** — Vulnerability reporting policy and security boundaries.
5. **[PROJECT_BRIEF.md](PROJECT_BRIEF.md)** — High-level product scope, goals, and target audience.
6. **[DEVELOPMENT_SETUP.md](DEVELOPMENT_SETUP.md)** — Step-by-step developer environment setup for Windows 10 / 11 MSVC and CMake.
7. **[SYSTEM_ARCHITECTURE.md](SYSTEM_ARCHITECTURE.md)** — Process boundaries, IPC topologies, component design, and storage architecture.
8. **[TECHNOLOGY_STACK.md](TECHNOLOGY_STACK.md)** — Approved compilers, Windows 11 OS APIs, third-party dependencies, and tooling choices.

---

## 2. Core Architecture & Subsystem Specifications

### 2.1 Engine & Protocols
- **[DOWNLOAD_ENGINE_DESIGN.md](DOWNLOAD_ENGINE_DESIGN.md)** — Segmented HTTP/HTTPS engine, adaptive range splitting, work-stealing, and rollback.
- **[STREAM_DOWNLOAD_PIPELINE.md](STREAM_DOWNLOAD_PIPELINE.md)** — Out-of-process HLS/DASH/MSS pipeline, N_m3u8DL-RE worker isolation, and FFmpeg muxing.
- **[MEDIA_DETECTION.md](MEDIA_DETECTION.md)** — Network sniffing, DOM observation, manifest deduplication, and confidence scoring.
- **[UPSTREAM_PROJECT_ANALYSIS.md](UPSTREAM_PROJECT_ANALYSIS.md)** — Architectural analysis of Brisk, Cat Catch, and N_m3u8DL-RE.
- **[UPSTREAM_INTEGRATION_MAP.md](UPSTREAM_INTEGRATION_MAP.md)** — Integration mapping between Radium subsystems and upstream reference projects.
- **[SOURCE_MAPPING.md](SOURCE_MAPPING.md)** — Upstream source file and license attribution mapping.

### 2.2 Client UI & Browser Extension
- **[UI_DESIGN_SYSTEM.md](UI_DESIGN_SYSTEM.md)** — Deep Charcoal design system, GDI/Direct2D rendering, Per-Monitor DPI v2, and dialog specifications.
- **[RADIUM_BRANDING_SPECIFICATION.md](RADIUM_BRANDING_SPECIFICATION.md)** — Visual identity, Coral Red accent styling, typography, and iconography.
- **[BROWSER_EXTENSION.md](BROWSER_EXTENSION.md)** — Manifest V3 browser extension architecture for Chromium and Firefox with closed Shadow DOM.
- **[NATIVE_MESSAGING_AND_IPC.md](NATIVE_MESSAGING_AND_IPC.md)** — 32-bit length-prefixed stdio JSON wire protocol and ACL-restricted Windows Named Pipe.

### 2.3 Storage, Reliability & Security
- **[DATA_MODEL_AND_STORAGE.md](DATA_MODEL_AND_STORAGE.md)** — SQLite WAL schema, state machine, crash recovery, and atomic commit semantics.
- **[ERROR_HANDLING_AND_RECOVERY.md](ERROR_HANDLING_AND_RECOVERY.md)** — Error taxonomy, exponential retry backoff, and recovery flows.
- **[SECURITY_THREAT_MODEL.md](SECURITY_THREAT_MODEL.md)** — STRIDE threat model, credential protections (DPAPI), and path sanitization.
- **[PRIVACY_AND_DATA_HANDLING.md](PRIVACY_AND_DATA_HANDLING.md)** — Sensitive data classification, redaction policies, and telemetry boundaries.
- **[KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md)** — Explicit non-goals (DRM non-circumvention, browser-internal blobs) and platform constraints.
- **[TECHNICAL_RISK_REGISTER.md](TECHNICAL_RISK_REGISTER.md)** — Active engineering risk register and mitigation tracking.

### 2.4 Product Requirements, Build & Quality Assurance
- **[PRODUCT_REQUIREMENTS.md](PRODUCT_REQUIREMENTS.md)** — Functional and non-functional requirements specification.
- **[FEATURE_MATRIX.md](FEATURE_MATRIX.md)** — Requirement-to-module traceability and implementation verification status.
- **[TEST_STRATEGY.md](TEST_STRATEGY.md)** — Automated unit, loopback integration, and reliability test strategy.
- **[TEST_MATRIX.md](TEST_MATRIX.md)** — Test suite mappings, verification classifications, and test commands.
- **[WINDOWS_BUILD_AND_PACKAGING.md](WINDOWS_BUILD_AND_PACKAGING.md)** — MSVC/CMake compilation pipeline, binary size targets, and installer packaging.
- **[WINDOWS_COMPATIBILITY_AND_CLOUD_TESTING.md](WINDOWS_COMPATIBILITY_AND_CLOUD_TESTING.md)** — Windows 10/11 compatibility matrix and CI verification.
- **[RELEASE_AND_UPDATE_PROCESS.md](RELEASE_AND_UPDATE_PROCESS.md)** — Release workflow, checksum generation, and update channel policies.
- **[PERFORMANCE_AND_SIZE_BUDGET.md](PERFORMANCE_AND_SIZE_BUDGET.md)** — Resource budgets and binary footprint benchmarks.
- **[ROADMAP.md](ROADMAP.md)** — Milestone definitions and future platform goals.
- **[GLOSSARY.md](GLOSSARY.md)** — Technical domain terminology and definitions.
- **[LICENSE_REVIEW.md](LICENSE_REVIEW.md)** — Third-party license audits and GPL-3.0 compatibility verification.

---

## 3. Architecture Decision Records (`docs/adr/`)

- **[adr/0001-desktop-framework.md](adr/0001-desktop-framework.md)** — Selection of ISO C++20 with Native Win32 API.
- **[adr/0002-process-and-module-boundaries.md](adr/0002-process-and-module-boundaries.md)** — Process separation between desktop engine, Native Host, and worker processes.
- **[adr/0003-browser-communication.md](adr/0003-browser-communication.md)** — Native Messaging + ACL-protected Windows Named Pipe.
- **[adr/0004-media-detection.md](adr/0004-media-detection.md)** — In-browser detection decoupled from desktop downloading.
- **[adr/0005-download-engine-integration.md](adr/0005-download-engine-integration.md)** — In-process HTTP segmentation + out-of-process stream worker isolation.
- **[adr/0006-licensing-and-dependency-policy.md](adr/0006-licensing-and-dependency-policy.md)** — GPL-3.0-or-later licensing and minimal zero-unapproved dependency policy.
- **[adr/0007-configuration-and-persistence.md](adr/0007-configuration-and-persistence.md)** — Embedded SQLite 3 with WAL journaling and DPAPI encryption.
- **[adr/0008-packaging-and-updates.md](adr/0008-packaging-and-updates.md)** — Portable packages, per-user installers, and Native Host registry deployment.

---

## 4. Benchmark Documentation (`benchmarks/`)

- **[../benchmarks/RESULTS.md](../benchmarks/RESULTS.md)** — Master benchmark registry, 500 MB comparative results against IDM, historical benchmarks, and raw artifact download instructions.
- **[../benchmarks/LATEST_COMPARISON.md](../benchmarks/LATEST_COMPARISON.md)** — Latest comparative benchmark summary.

---

## 5. Localized READMEs & Hosting Mirrors

- **[i18n/](i18n/)** — Localized README translations (`README.ar-SA.md`, `README.de-DE.md`, `README.es.md`, `README.fr.md`, `README.it-IT.md`, `README.ja.md`, `README.ko.md`, `README.pt-BR.md`, `README.ru-RU.md`, `README.zh-CN.md`, `README.zh-TW.md`).
- **[hosting/MULTI_PLATFORM_REPOSITORY.md](hosting/MULTI_PLATFORM_REPOSITORY.md)** — Multi-platform Git mirror topology.
- **[hosting/MULTI_PLATFORM_STATUS.md](hosting/MULTI_PLATFORM_STATUS.md)** — Mirror synchronization status.
- **[hosting/GITLAB_SETUP.md](hosting/GITLAB_SETUP.md)** — GitLab mirror configuration notes.
