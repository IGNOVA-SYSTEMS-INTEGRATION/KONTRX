import os
import sys
import time
from pathlib import Path
from typing import Optional, Dict, Any
from dotenv import load_dotenv
import openai

import functools
# Force unbuffered prints
print = functools.partial(print, flush=True)

# Load environment variables (.env file if present)
load_dotenv()

# Import ChromaDB helper from local db module
try:
    from db import upsert_document, query_documents, get_memory_collection
except ImportError:
    from memory_system.db import upsert_document, query_documents, get_memory_collection

# Supported source file extensions
SUPPORTED_EXTENSIONS = {
    ".py": "python",
    ".c": "c",
    ".h": "c_header",
    ".cpp": "cpp",
    ".hpp": "cpp_header",
    ".js": "javascript",
    ".ts": "typescript",
    ".json": "json",
    ".md": "markdown",
    ".ld": "linker_script",
    ".cmake": "cmake",
    ".txt": "text",
    ".yaml": "yaml",
    ".yml": "yaml",
}

# Max file size to document (250 KB)
MAX_FILE_SIZE = 250 * 1024

def get_openai_client() -> Optional[openai.OpenAI]:
    """Returns an OpenAI client if OPENAI_API_KEY is available."""
    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key or api_key == "your_openai_api_key_here":
        return None
    try:
        return openai.OpenAI(api_key=api_key)
    except Exception as e:
        print(f"[!] Warning initializing OpenAI client: {e}")
        return None

def fallback_document_generator(file_path: Path, code_content: str, language: str) -> str:
    """
    Generates a structured static-analysis summary when LLM API key is not configured.
    Ensures the system remains fully functional in offline/local environments.
    """
    lines = code_content.splitlines()
    total_lines = len(lines)
    non_empty_lines = len([l for l in lines if l.strip()])
    
    # Simple extraction of functions/classes/imports
    definitions = []
    for line in lines:
        stripped = line.strip()
        if language in ["python"] and (stripped.startswith("def ") or stripped.startswith("class ")):
            definitions.append(stripped.split("(")[0].replace("def ", "Function: ").replace("class ", "Class: "))
        elif language in ["c", "c_header", "cpp"] and ("(" in stripped and ")" in stripped and ";" in stripped or "{" in stripped):
            if not stripped.startswith("//") and not stripped.startswith("/*") and not stripped.startswith("#"):
                if any(kw in stripped for kw in ["void ", "int ", "uint", "bool ", "Status", "Task", "BaseType"]):
                    definitions.append(f"Interface/Function: {stripped.split('{')[0].strip()}")
                    
    summary = f"""# Code Documentation: {file_path.name}

- **File Path**: `{file_path.as_posix()}`
- **Language**: `{language}`
- **Total Lines**: `{total_lines}` (Executable lines: `{non_empty_lines}`)

## Overview
This file is part of the Kontrx/Antigravity codebase. It provides implementation/definitions for `{file_path.stem}` components.

## Detected Interfaces & Declarations
"""
    if definitions:
        for d in definitions[:20]:
            summary += f"- `{d}`\n"
        if len(definitions) > 20:
            summary += f"- *...and {len(definitions) - 20} more symbols.*\n"
    else:
        summary += "- General source/configuration module.\n"

    summary += f"\n## Source Snippet Preview\n```{language}\n" + "\n".join(lines[:30]) + "\n```\n"
    return summary

def generate_ai_documentation(file_path: Path, code_content: str, language: str) -> str:
    """Generates detailed code documentation using OpenAI LLM or fallback."""
    client = get_openai_client()
    
    if client is None:
        return fallback_document_generator(file_path, code_content, language)
    
    prompt = f"""You are an expert AI software architect analyzing a source file from the Kontrx / Antigravity embedded & IoT project.

File: {file_path.name}
Language: {language}
Path: {file_path.as_posix()}

Code Content:
```{language}
{code_content[:15000]}
```

Please generate a concise, high-value technical documentation summary covering:
1. **Module Purpose & Role**: What does this module do in the system architecture?
2. **Key Functions, Classes & Data Structures**: Detail the main APIs and their parameters/returns.
3. **Hardware / Protocol / System Interactions**: (e.g. FreeRTOS, HAL, MQTT, Sparkplug, Relays, STM32, Sensors).
4. **Usage Notes / Constraints**: Any threading, memory, or protocol requirements.

Format your output in clean Markdown.
"""

    try:
        model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
        response = client.chat.completions.create(
            model=model,
            messages=[
                {"role": "system", "content": "You are a professional software engineer specialized in embedded systems, IoT architectures, and clean code documentation."},
                {"role": "user", "content": prompt}
            ],
            temperature=0.2,
            max_tokens=1500
        )
        return response.choices[0].message.content or fallback_document_generator(file_path, code_content, language)
    except Exception as e:
        print(f"[!] OpenAI API call failed ({e}), falling back to structural doc generator.")
        return fallback_document_generator(file_path, code_content, language)

def process_file(file_path_str: str) -> Optional[Dict[str, Any]]:
    """
    Reads a file, generates documentation, and saves/updates it in ChromaDB.
    Returns metadata dict or None if skipped.
    """
    path = Path(file_path_str).resolve()
    if not path.is_file():
        print(f"[!] Error: File '{file_path_str}' does not exist.")
        return None

    # Check extension
    ext = path.suffix.lower()
    if ext not in SUPPORTED_EXTENSIONS:
        return None

    # Check size
    try:
        size = path.stat().st_size
        if size > MAX_FILE_SIZE or size == 0:
            return None
    except Exception:
        return None

    language = SUPPORTED_EXTENSIONS[ext]
    
    # Read file content safely
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            code_content = f.read()
    except Exception as e:
        print(f"[!] Failed to read file {path}: {e}")
        return None

    print(f"[*] Generating documentation for: {path.name} ({language})...")
    doc_text = generate_ai_documentation(path, code_content, language)
    
    # Combine doc and snippet for semantic embedding
    composite_document = f"""# DOCUMENTATION FOR {path.name}
Path: {path.as_posix()}
Language: {language}

{doc_text}

---
# CODE CONTENT:
```{language}
{code_content[:8000]}
```
"""
    doc_id = str(path.as_posix())
    metadata = {
        "file_name": path.name,
        "file_path": str(path.as_posix()),
        "language": language,
        "file_size": size,
        "timestamp": time.time(),
        "updated_at": time.strftime("%Y-%m-%d %H:%M:%S")
    }

    upsert_document(doc_id=doc_id, document=composite_document, metadata=metadata)
    print(f"[OK] Documented and stored in ChromaDB: {path.name}")
    
    return {
        "id": doc_id,
        "doc_text": doc_text,
        "metadata": metadata
    }

if __name__ == "__main__":
    if len(sys.argv) > 1:
        target_path = sys.argv[1]
    else:
        # Default test file in Kontrx project
        target_path = os.path.join(os.path.dirname(__file__), "..", "control_relays.py")
    
    print(f"[*] Running Doc Generator on target: {target_path}")
    result = process_file(target_path)
    
    if result:
        print("\n" + "="*50)
        print(f"GENERATED DOCUMENTATION PREVIEW ({result['metadata']['file_name']}):")
        print("="*50)
        print(result["doc_text"][:600] + "\n...")
        print("="*50)
        
        print("\n[*] Testing retrieval from ChromaDB:")
        query_res = query_documents("relay control MQTT commands", n_results=1)
        for q in query_res:
            print(f"[+] Retrieved from DB: {q['metadata'].get('file_name')} (distance: {q.get('distance')})")
    else:
        print("[!] No document was generated.")
