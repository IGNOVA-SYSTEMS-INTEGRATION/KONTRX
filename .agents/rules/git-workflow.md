# Git Branch & PR Workflow Standard

Whenever performing any code modifications, updates, features, or bug fixes in this repository, ALWAYS follow this strict workflow without exception:

1. **Branch per Task**:
   - Never commit directly to `main` or `master`.
   - Before writing or editing code for any task/update, create and checkout a new dedicated feature branch:
     ```bash
     git checkout -b <type>/<descriptive-kebab-name>
     # Examples: feat/pto-control, fix/flash-bus-safety, chore/update-deps
     ```

2. **Atomic & Clean Commits**:
   - Keep changes scoped strictly to the task.
   - Use clear Conventional Commits messages (`feat: ...`, `fix: ...`, `refactor: ...`).

3. **Verify Build & Tests**:
   - Ensure the project builds cleanly before committing:
     ```bash
     cmake --build build
     ```

4. **Push Branch & Merge via PR Workflow**:
   - Push the feature branch to the remote repository:
     ```bash
     git push origin <branch-name>
     ```
   - Switch back to `main`, pull latest, and perform a non-fast-forward merge commit (`--no-ff`) representing the Pull Request integration:
     ```bash
     git checkout main
     git merge --no-ff <branch-name> -m "merge: <branch-name> into main"
     git push origin main
     ```

5. **Start Next Task Only After Merge**:
   - Only start a new task once the previous branch has been cleanly merged and pushed to `main`.
