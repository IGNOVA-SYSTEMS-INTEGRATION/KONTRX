# KONTRX Project Guidelines

- **Branching Workflow**: Always branch from `dev`. Commit atomic Conventional Commits, push, merge into `dev` with `--no-ff`, and always keep active branch on `dev`.
- **Build Verification**: `cmake --build build` or `ninja -C build` before committing.
