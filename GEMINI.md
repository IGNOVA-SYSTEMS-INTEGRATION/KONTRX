# KONTRX Project Guidelines

- **Mandatory Task Protocol (المنهجية المعتمدة رسمياً)**: Always follow the 5-stage protocol:
  `مراجعة وتشخيص ⟶ إعداد خطة موثقة ⟶ نقاش وحسم القرارات معك ⟶ تنفيذ دقيق ⟶ اختبار شامل`
- **Branching Workflow**: Always branch from `dev`. Commit atomic Conventional Commits, push, merge into `dev` with `--no-ff`, and always keep active branch on `dev`.
- **Build Verification**: `cmake --build build` or `ninja -C build` before committing.
