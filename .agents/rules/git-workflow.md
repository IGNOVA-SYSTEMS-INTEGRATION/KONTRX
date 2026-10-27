# Git Dev & PR Workflow Standard

Whenever performing any code modifications, updates, features, or bug fixes in this repository, ALWAYS follow this strict workflow without exception:

1. **Active Branch is `dev`**:
   - The default development and integration branch is `dev`.
   - Never commit directly to `main` or directly to `dev`.
   - Before writing or editing code for any task/update, checkout `dev`, pull latest, and create a new dedicated branch:
     ```bash
     git checkout dev
     git pull origin dev
     git checkout -b <type>/<descriptive-kebab-name>
     # Examples: feat/pto-control, fix/flash-bus-safety, chore/update-deps
     ```

2. **Atomic & Clean Commits**:
   - Keep changes scoped strictly to the task.
   - Use clear Conventional Commits messages (`feat: ...`, `fix: ...`, `refactor: ...`, `docs: ...`, `chore: ...`).

3. **Verify Build & Tests**:
   - Ensure the project builds cleanly before committing:
     ```bash
     cmake --build build  # or ninja -C build
     ```

4. **Push Branch & Open Official GitHub Pull Request**:
   - Push the branch to the remote repository:
     ```bash
     git push -u origin <type>/<descriptive-kebab-name>
     ```
   - Create an official Pull Request using GitHub CLI (`gh`):
     ```bash
     gh pr create --base dev --head <type>/<descriptive-kebab-name> --title "<type>(<scope>): <summary>" --body "<description>"
     ```
   - Merge the PR on GitHub:
     ```bash
     gh pr merge --merge
     ```
   - Switch back to `dev` and pull latest:
     ```bash
     git checkout dev
     git pull origin dev
     ```

5. **Release Workflow (dev -> main)**:
   - When releasing to production:
     ```bash
     gh pr create --base main --head dev --title "release: vX.Y.Z (merge dev into main)" --body "Changelog..."
     gh pr merge --merge
     git checkout main && git pull origin main
     git checkout dev  # ALWAYS return immediately to dev!
     ```

6. **Always Remain on `dev`**:
   - After completing and merging any task, ALWAYS ensure the active workspace is on `dev`.
   - Never leave the workspace checked out on a feature branch or on `main`.
