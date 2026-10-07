# KONTRX Agent Guidelines

## Development Workflow
- Follow `.agents/rules/git-workflow.md` strictly:
  1. Always branch off `main` for any task: `git checkout -b <type>/<name>`.
  2. Implement, test, and commit atomically with conventional commit messages.
  3. Push the feature branch to `origin`.
  4. Merge into `main` with `--no-ff` (PR simulation) and push `main` to `origin`.
  5. Start the next task only after `main` is merged and pushed.

## Build & Test Instructions
- ARM Toolchain: GCC ARM Embedded
- Build command: `cmake --build build` or `ninja -C build`
- Always verify build compiles with zero errors before committing.
