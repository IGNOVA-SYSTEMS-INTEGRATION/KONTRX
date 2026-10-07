# KONTRX Agent Guidelines

## Development Workflow
- Follow `.agents/rules/git-workflow.md` strictly:
  1. Default base branch is `dev`.
  2. Always branch off `dev` for any task: `git checkout dev && git pull origin dev && git checkout -b <type>/<name>`.
  3. Implement, test (`cmake --build build`), and commit atomically with Conventional Commits.
  4. Push the branch to `origin`: `git push -u origin <branch>`.
  5. Checkout `dev`, merge with `--no-ff`, and push `dev` to `origin`.
  6. ALWAYS leave the repository on `dev` (`git checkout dev`).

## Build & Test Instructions
- ARM Toolchain: GCC ARM Embedded
- Build command: `cmake --build build` or `ninja -C build`
- Always verify build compiles with zero errors before committing.
