# Multi-Platform Repository Management Guide

This document specifies the multi-platform mirror topology, credentials, remotes, and push workflows for **Radium Download Manager**.

---

## 1. Hosting Architecture Overview

Radium Download Manager uses a resilient multi-platform mirror configuration. Local Git remains the single source of truth, with **GitLab** as the primary active upstream and three fully verified community mirrors.

| Platform | Role | Remote Name | Repository URL | Status | Protocol |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **GitLab** | **Primary Upstream** | `gitlab` | `https://gitlab.com/sayedalve/Radium-Download-Manager` | **OPERATIONAL** | HTTPS (GCM / PAT) |
| **Codeberg** | Community Mirror | `codeberg` | `https://codeberg.org/sayedalve/Radium-Download-Manager` | **OPERATIONAL** | HTTPS / tea |
| **SourceForge** | Open Source Portal | `sourceforge` | `ssh://sayedalve@git.code.sf.net/p/radium-download-manager/code` | **OPERATIONAL** | Git over SSH |
| **Bitbucket** | Secondary Mirror | `bitbucket` | `git@bitbucket.org:sayedalve/radium-download-manager.git` | **OPERATIONAL** | Git over SSH |
| **Forgejo** | Self-Hosted / Alt | `forgejo` | Configurable per instance | Optional | HTTPS / SSH |

> **IMPORTANT:** GitHub is banned and strictly excluded. No commands or scripts contact `github.com`.

---

## 2. Remote Configuration & Commands

### 2.1 Daily Development Commands (GitLab)
By default, standard Git operations target **GitLab** without touching other mirrors:
```bash
git add .
git commit -m "feat(ui): update layout"
git push      # Pushes to gitlab
git pull      # Pulls from gitlab
```

### 2.2 Publishing to All Configured Mirrors
To publish all local branches and tags across all configured platforms in one operation:

```powershell
# Using the configured git alias:
git pushall

# Or invoking the tool directly:
.\tools\push-all-remotes.ps1
```

### 2.3 Selectively Skipping Mirrors
If one mirror is undergoing maintenance:
```powershell
.\tools\push-all-remotes.ps1 -Skip bitbucket
```

---

## 3. Platform Verification Summary

### 3.1 GitLab (https://gitlab.com) — PRIMARY
- URL: [https://gitlab.com/sayedalve/Radium-Download-Manager](https://gitlab.com/sayedalve/Radium-Download-Manager)
- Remote: `gitlab`
- Default branch: `main`
- All 5 branches & 5 tags verified.

### 3.2 Codeberg (https://codeberg.org) — MIRROR
- URL: [https://codeberg.org/sayedalve/Radium-Download-Manager](https://codeberg.org/sayedalve/Radium-Download-Manager)
- Remote: `codeberg`
- All 5 branches & 5 tags verified.

### 3.3 SourceForge (https://sourceforge.net) — MIRROR
- Project Page: [https://sourceforge.net/projects/radium-download-manager/](https://sourceforge.net/projects/radium-download-manager/)
- Git URL: `ssh://sayedalve@git.code.sf.net/p/radium-download-manager/code`
- Remote: `sourceforge`
- All 5 branches & 5 tags verified.

### 3.4 Bitbucket (https://bitbucket.org) — MIRROR
- Repository URL: [https://bitbucket.org/sayedalve/radium-download-manager/](https://bitbucket.org/sayedalve/radium-download-manager/)
- Git URL: `git@bitbucket.org:sayedalve/radium-download-manager.git`
- Remote: `bitbucket`
- All 5 branches & 5 tags verified.

---

## 4. Verification Procedures

To check connectivity to any remote without modifying files:
```bash
git ls-remote <remote-name>
```

To verify all tracking branches:
```bash
git branch -vv
```
