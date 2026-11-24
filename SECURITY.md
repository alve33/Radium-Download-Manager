# Security Policy

## Supported Versions

| Version | Supported |
| :--- | :--- |
| `1.0.x` (and `main`) | Yes |
| `< 1.0.0` | No |

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Instead, report security issues privately using **GitHub Security Advisories** on this repository (`Security` -> `Advisories` -> `Report a vulnerability`), or contact the maintainer directly via the email listed on the repository profile.

When reporting, please include:
- A clear description of the vulnerability and its potential impact.
- Steps to reproduce or a minimal proof of concept.
- Affected component (`radium.exe`, `radium-nmh.exe`, browser extension, or installer).

We aim to acknowledge reports within 72 hours and provide a remediation timeline once verified.

## Security Architecture

For details on Radium's threat model, Windows Named Pipe ACL isolation, credential handling, and DRM non-circumvention policy, see [docs/SECURITY_THREAT_MODEL.md](docs/SECURITY_THREAT_MODEL.md).
