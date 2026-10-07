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
     cmake --build build
     ```

4. **Push Branch & Merge via PR Workflow**:
   - Push the feature branch to the remote repository:
     ```bash
     git push -u origin <branch-name>
     ```
   - Switch back to `dev`, pull latest, and perform a non-fast-forward merge commit (`--no-ff`) representing the Pull Request integration:
     ```bash
     git checkout dev
     git pull origin dev
     git merge --no-ff <branch-name> -m "merge: <branch-name> into dev"
     git push origin dev
     ```

5. **Always Remain on `dev`**:
   - After completing and merging any task, ALWAYS ensure you are on `dev`.
   - Never leave the workspace checked out on a feature branch.
