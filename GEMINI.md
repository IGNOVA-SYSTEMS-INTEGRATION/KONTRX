# KONTRX Project Guidelines

- **Mandatory Task Protocol (المنهجية المعتمدة رسمياً)**: Always follow the 5-stage protocol:
  `مراجعة وتشخيص ⟶ إعداد خطة موثقة ⟶ نقاش وحسم القرارات معك ⟶ تنفيذ دقيق ⟶ اختبار شامل`
- **Branching & PR Workflow**: Always branch from `dev`. Commit atomic Conventional Commits, push to remote, open official GitHub PR with detailed description (`gh pr create`), conduct diff review (`gh pr diff`), merge via `gh pr merge`, and always keep active branch on `dev`.
- **Release PR Workflow**: Merge `dev` into `main` via official GitHub PR with full release documentation (`gh pr create --base main --head dev && gh pr diff && gh pr merge`).
- **Build Verification**: `cmake --build build` or `ninja -C build` before committing.
