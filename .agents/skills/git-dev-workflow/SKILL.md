---
name: git-dev-workflow
description: >-
  Standard Gitflow branching and GitHub PR workflow for KONTRX ecosystem projects (Kontrx, Rules Desktop App, AXIRA Mobile, AXIRA).
  Use whenever creating branches, committing changes, opening PRs with detailed descriptions via gh pr create,
  conducting diff reviews, merging tasks into dev, or preparing releases between dev and main. Enforces that active workspace remains on dev.
---

# Git Dev Workflow Standard

Strict branching and pull-request integration workflow for all KONTRX ecosystem repositories.

## Core Branch Architecture

- **`main`**: Production & release branch. Contains tagged stable releases. Only receives merges from `dev` via GitHub PR.
- **`dev`**: Active integration & development branch. All task branches originate from `dev` and merge back into `dev` via GitHub PR.
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

### Step 4: Create GitHub Pull Request, Review & Merge into `dev`
Every task integration MUST have an official Pull Request opened, documented, and reviewed on GitHub:

1. **Mandatory PR Description Structure**:
   Every PR must contain a clear, professional description including:
   - **Summary**: Concise overview of what was added, modified, or fixed.
   - **Root Cause & Technical Details**: Why the change is needed and how it was implemented.
   - **Key Changes by File**: Bulleted breakdown of affected files and modules.
   - **Testing & Verification Proof**: Exact commands run (`ninja -C build`, `pytest`) and outcomes.
   - **Review Checklist**: Verification of safety, quality, and zero regressions.

2. **Open PR using GitHub CLI**:
   ```bash
   gh pr create --base dev --head <type>/<descriptive-kebab-name> \
     --title "<type>(<scope>): <clear concise description>" \
     --body "<Structured PR Description with Summary, Details, Changes, and Test Proof>"
   ```

3. **Diff Review**:
   Inspect and verify the diff to guarantee minimal blast radius:
   ```bash
   gh pr diff
   ```

4. **Merge the Pull Request on GitHub**:
   ```bash
   gh pr merge --merge
   ```
   *(Fallback if offline: perform local `--no-ff` merge `git checkout dev && git merge --no-ff <branch> -m "merge: ..."` and then close/merge PR).*

5. **Pull the merged `dev` branch locally**:
   ```bash
   git checkout dev
   git pull origin dev
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
1. **Create Detailed Release Pull Request from `dev` to `main`**:
   ```bash
   gh pr create --base main --head dev \
     --title "release: vX.Y.Z (merge dev into main)" \
     --body "<Comprehensive release summary, features, bug fixes, and test proofs>"
   ```
2. **Review Diff & Merge the Release PR on GitHub**:
   ```bash
   gh pr diff
   gh pr merge --merge
   ```
3. **Sync local `main` and tag**:
   ```bash
   git checkout main
   git pull origin main
   git tag -a vX.Y.Z -m "Release vX.Y.Z"
   git push origin vX.Y.Z
   ```
4. **Immediately switch back to `dev`**:
   ```bash
   git checkout dev
   ```
