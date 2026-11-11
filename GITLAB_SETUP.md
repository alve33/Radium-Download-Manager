# GitLab Repository & Workflow Setup for Radium Download Manager

This document records the primary remote configuration, authentication, and standard Git workflows for **Radium Download Manager** following the migration from GitHub to GitLab.

---

## 1. GitLab Repository Details

- **Repository Web URL:** [https://gitlab.com/sayedalve/Radium-Download-Manager](https://gitlab.com/sayedalve/Radium-Download-Manager)
- **HTTPS Clone / Remote URL:** `https://gitlab.com/sayedalve/Radium-Download-Manager.git`
- **SSH Clone / Remote URL:** `git@gitlab.com:sayedalve/Radium-Download-Manager.git`
- **Default Branch:** `main`
- **Visibility:** Public

---

## 2. Local Git Remote Configuration

GitLab is set as the active primary upstream and default push destination (`remote.pushDefault = gitlab`). The legacy GitHub remote `origin` is preserved solely for historical lineage and must not be pushed to.

To inspect remotes:
```bash
git remote -v
```
Expected output:
```text
gitlab  https://gitlab.com/sayedalve/Radium-Download-Manager.git (fetch)
gitlab  https://gitlab.com/sayedalve/Radium-Download-Manager.git (push)
origin  https://github.com/sayedalve/Radium-Download-Manager.git (fetch)
origin  https://github.com/sayedalve/Radium-Download-Manager.git (push)
```

To verify branch upstream tracking:
```bash
git branch -vv
```
All branches (`main`, `codex/settings-path-notifications-updater-overlay`, `maintenance/*`) track `gitlab/*`.

---

## 3. GitLab CLI & Credential Authentication

The official GitLab CLI (`glab`) is installed and authenticated:
- **CLI Tool:** `glab` (v1.128.0 or later)
- **Status Check:**
  ```powershell
  glab auth status
  ```
- **Git Credential Manager (GCM):**
  Git on Windows uses Git Credential Manager to securely cache GitLab credentials in the Windows Credential Locker. When pulling or pushing over HTTPS, authentication will seamlessly use your stored credentials or Personal Access Token (PAT).

If you need to re-authenticate or rotate your token in the future:
```powershell
glab auth login
# Choose: gitlab.com -> HTTPS -> Paste your Personal Access Token
```

---

## 4. Daily Development Workflow

With GitLab configured as the primary remote and default push destination, standard Git commands operate directly with GitLab without needing any arguments:

### 4.1 Making Changes & Committing
```bash
git add .
git commit -m "feat(module): describe your changes"
```

### 4.2 Pushing Commits
```bash
git push
```
*(Automatically pushes the active branch to its tracking branch on `gitlab`)*

### 4.3 Pulling Changes
```bash
git pull
```
*(Automatically fetches and merges from the corresponding branch on `gitlab`)*

### 4.4 Creating and Pushing a New Branch
When creating a new branch, push and set upstream to GitLab:
```bash
git checkout -b feature/my-new-feature
git push -u gitlab feature/my-new-feature
```

### 4.5 Creating and Pushing Tags
```bash
git tag -a v1.0.1 -m "Release v1.0.1"
git push gitlab v1.0.1
# Or to push all local tags:
git push gitlab --tags
```

---

## 5. CI/CD Considerations

- **Current Status:** Radium Download Manager relies on native Windows 11 build scripts (`scripts/build-windows-msvc.ps1`, `scripts/build-installer.ps1`, `installer/msix/build-msix.ps1`) targeting MSVC, Windows SDK, Inno Setup, and MSIX.
- **GitLab CI/CD:** GitLab CI/CD requires Windows Shared Runners (which require a paid/credit-card verified SaaS tier or a self-hosted Windows GitLab Runner). Because local PowerShell scripts already provide deterministic local builds and tests without external dependencies, no `.gitlab-ci.yml` is introduced automatically to prevent failing pipeline notifications. If desired, a self-hosted runner can be registered using `gitlab-runner register`.

---

## 6. Historical Data & Audit Notes

1. **Commit History:** Complete 100% locally preserved history (104 commits from initial commit through `c95bb86`).
2. **Branches Transferred to GitLab:**
   - `main`
   - `codex/settings-path-notifications-updater-overlay`
   - `maintenance/consolidate-benchmarks-and-runners`
   - `maintenance/final-engineering-cleanup`
   - `maintenance/license-and-docs-validation`
3. **Tags Transferred to GitLab:**
   - `stream-tools-v1`
   - `v0.3.0`
   - `v0.3.1`
   - `v0.3.2`
   - `v1.0.0`
4. **Git LFS:** No Git LFS pointers or binary objects were tracked by Git LFS in the repository; all repository assets are standard Git objects and were completely migrated.
5. **Unavailable GitHub Data:**
   - GitHub Issue comments, Discussions, and Pull Request conversation threads hosted exclusively on `github.com` cannot be accessed due to account ban.
   - All Git commit data, PR merge commits, author identities, and commit timestamps were present in the local repository and have been successfully pushed to GitLab.
