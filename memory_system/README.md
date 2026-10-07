# Antigravity & Kontrx Memory & Semantic Documentation System

نظام متكامل لإدارة الذاكرة الدلالية والتوثيق الكودي التلقائي وربطه بمساعدي الذكاء الاصطناعي عبر بروتوكول MCP (Model Context Protocol).

---

## 🏗️ مكونات النظام (Architecture Components)

```
memory_system/
├── db.py                 # إدارة قاعدة بيانات المتجهات المحلية ChromaDB
├── doc_generator.py      # توليد التوثيق الكودي وتحليله وتخزينه
├── watcher.py            # مراقب حي للملفات (Watchdog) لتحديث التوثيق فور الحفظ
├── mcp_server.py         # مخدم بروتوكول سياق النموذج (MCP Server)
├── test_mcp_client.py    # اختبارات التحقق المباشر
├── mcp_config.json       # إعدادات ربط الخادم بمحررات وبيئات العمل
├── .env.example          # قالب مفاتيح وبيئات التشغيل (OpenAI / Local LLM)
└── chroma_db/            # قاعدة بيانات المتجهات المحلية الدائمة
```

---

## 🚀 كيفية الاستخدام والتشغيل

### 1. تثبيت المتطلبات (Dependencies)
```bash
pip install chromadb openai watchdog mcp python-dotenv
```

### 2. تكوين مفاتيح الذكاء الاصطناعي (اختياري)
قم بنسخ `.env.example` إلى `.env` وإضافة مفتاحك (في حال عدم وجود مفتاح، يعمل النظام تلقائياً بنمط التحليل الهيكلي المحلي):
```env
OPENAI_API_KEY=sk-...
OPENAI_MODEL=gpt-4o-mini
```

### 3. تشغيل مراقب الملفات الحي (Real-time Watcher)
يقوم بمراقبة أي حفظ أو تعديل على ملفات الكود (`.py`, `.c`, `.h`, `.js`, `.ts`, `.md`, ...) وتحديث قاعدة البيانات مباشرة:
```bash
python memory_system/watcher.py
```

### 4. تشغيل خادم MCP (Model Context Protocol)
```bash
python memory_system/mcp_server.py
```

---

## 🛠️ أدوات خادم MCP المتاحة

1. **`search_antigravity_memory(query, top_k)`**:
   - البحث الدلالي باللغة الطبيعية عن أي كود، وظيفة، هيكلية، أو بروتوكول في المشروع.
2. **`list_memory_files()`**:
   - عرض قائمة بجميع الملفات المفهرسة حالياً في الذاكرة وتاريخ آخر تحديث.
3. **`reindex_single_file(file_path)`**:
   - إعادة توليد التوثيق وفهرسة ملف محدد فورياً.

---

## ⚙️ دمج الخادم مع Antigravity / Claude Desktop / VS Code

أضف المقطع التالي إلى ملف إعدادات الـ MCP الخاص بك:

```json
{
  "mcpServers": {
    "antigravity-memory": {
      "command": "python",
      "args": [
        "d:\\WORKSPACE\\IGNOVA\\DEVELOPMENT\\Kontrx\\memory_system\\mcp_server.py"
      ],
      "env": {
        "PYTHONIOENCODING": "utf-8"
      }
    }
  }
}
```
