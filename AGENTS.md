# KONTRX Agent Guidelines

## Mandatory Task Protocol (المنهجية المعتمدة رسمياً)
Follow the 5-stage task methodology in every task:
```
مراجعة وتشخيص ⟶ إعداد خطة موثقة ⟶ نقاش وحسم القرارات معك ⟶ تنفيذ دقيق ⟶ اختبار شامل
```
1. **مراجعة وتشخيص (Review & Diagnosis)**: Examine codebase, logs, and root causes before touching code.
2. **إعداد خطة موثقة (Documented Plan)**: Formulate clear, structured plan and acceptance criteria.
3. **نقاش وحسم القرارات معك (Discussion & Alignment)**: Present plan to user, highlight tradeoffs/decisions, and align before execution.
4. **تنفيذ دقيق (Precise Implementation)**: Execute with minimal blast radius and strict quality.
5. **اختبار شامل (Comprehensive Testing)**: Run automated tests and verify build (`cmake --build build`).

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
