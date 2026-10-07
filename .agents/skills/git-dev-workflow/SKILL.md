---
name: git-dev-workflow
description: >-
  Standard Gitflow branching and PR workflow for KONTRX ecosystem projects (Kontrx, Rules Desktop App, AXIRA Mobile, AXIRA).
  Use whenever creating branches, committing changes, opening PRs, merging tasks into dev,
  or preparing releases between dev and main. Enforces that active workspace remains on dev.
---

# Git Dev Workflow Standard

Strict branching and pull-request integration workflow for all KONTRX ecosystem repositories.

## Core Branch Architecture

- **`main`**: Production & release branch. Contains tagged stable releases. Only receives merges from `dev`.
- **`dev`**: Active integration & development branch. All task branches originate from `dev` and merge back into `dev`.
- **`<type>/<task-name>`**: Ephemeral feature, bugfix, documentation, or chore branch branched off `dev`.

> [!IMPORTANT]
> **Golden Rule**: Always keep the active working tree on `dev`. Never leave the workspace sitting on a feature branch or on `main` for ongoing development.

---

## 5-Step Task Execution Protocol

### Step 1: Sync `dev` & Create Task Branch
Before making any changes:
```bash
git checkout dev
git pull origin dev
git checkout -b <type>/<descriptive-kebab-name>
```
Branch types:
- `feat/`: New capabilities or enhancements
- `fix/`: Bug fixes and regression repairs
- `docs/`: Technical manuals, architecture documents, and specs
- `chore/`: Tooling, dependencies, repository hygiene, agent skills/rules
- `refactor/`: Code reorganization without behavior change
- `test/`: Test additions or test framework improvements

### Step 2: Atomic Implementation & Verification
1. Implement changes strictly within the task scope.
2. Run project-specific verification tests:
   - Kontrx: `cmake --build build` or `ninja -C build`
   - Rules Desktop App: `npm test`
   - AXIRA Mobile: `flutter test`
   - AXIRA Backend: `npm test` or `cargo test`
3. Commit with Conventional Commits:
   ```bash
   git add <modified-files>
   git commit -m "<type>(<scope>): <clear concise description>"
   ```

### Step 3: Push Task Branch
Push the task branch to remote to make it visible on GitHub:
```bash
git push -u origin <type>/<descriptive-kebab-name>
```

### Step 4: Pull Request Simulation & Merge into `dev`
Integrate the task into `dev` using a non-fast-forward merge commit (`--no-ff`):
```bash
git checkout dev
git pull origin dev
git merge --no-ff <type>/<descriptive-kebab-name> -m "merge: <type>/<descriptive-kebab-name> into dev"
git push origin dev
```

### Step 5: Verification of Active Branch
Always confirm the active branch is `dev`:
```bash
git status
# Must output: "On branch dev"
```

---

## Release Workflow: Merging `dev` into `main`

When a milestone, version tag, or release candidate is verified on `dev`:
```bash
git checkout main
git pull origin main
git merge --no-ff dev -m "release: vX.Y.Z (merge dev into main)"
git push origin main
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin vX.Y.Z
git checkout dev   # Immediately switch back to dev
```
