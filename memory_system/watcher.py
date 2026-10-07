import os
import sys
import time
from typing import Optional, Dict, Any
from pathlib import Path
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler

import functools
# Force unbuffered prints
print = functools.partial(print, flush=True)

# Ensure local module imports work
sys.path.insert(0, str(Path(__file__).resolve().parent))
from doc_generator import process_file, SUPPORTED_EXTENSIONS

# Directories to ignore
IGNORED_DIRS = {
    ".git",
    "build",
    "node_modules",
    "__pycache__",
    "chroma_db",
    ".gemini",
    "brain",
    "scratch",
    "ultrasonic_build",
    ".vscode",
    ".idea"
}

class CodeChangeHandler(FileSystemEventHandler):
    """Watches for file modifications/creations and updates ChromaDB documentation."""

    def __init__(self, debounce_interval: float = 1.5):
        super().__init__()
        self.debounce_interval = debounce_interval
        self._last_processed = {}

    def _should_ignore(self, path_str: str) -> bool:
        path = Path(path_str)
        # Ignore directories
        if path.is_dir():
            return True
            
        # Ignore if any parent component is in IGNORED_DIRS
        for part in path.parts:
            if part in IGNORED_DIRS or part.startswith("."):
                return True

        # Ignore if not a supported code/text extension
        if path.suffix.lower() not in SUPPORTED_EXTENSIONS:
            return True

        return False

    def _handle_event(self, event_type: str, file_path_str: str):
        if self._should_ignore(file_path_str):
            return

        now = time.time()
        last_time = self._last_processed.get(file_path_str, 0)
        
        # Debounce rapid consecutive file-save events
        if now - last_time < self.debounce_interval:
            return

        self._last_processed[file_path_str] = now
        file_path = Path(file_path_str)
        
        print(f"\n[WATCHER] Event [{event_type}]: Detected change in '{file_path.name}'")
        print(f"[WATCHER] Triggering documentation generator for: {file_path_str}")
        
        try:
            res = process_file(file_path_str)
            if res:
                print(f"[WATCHER] [OK] Successfully updated ChromaDB memory for: {file_path.name}")
            else:
                print(f"[WATCHER] Skipped {file_path.name} (no changes or unsupported)")
        except Exception as e:
            print(f"[WATCHER] [ERR] Error processing {file_path.name}: {e}")

    def on_modified(self, event):
        if not event.is_directory:
            self._handle_event("MODIFIED", event.src_path)

    def on_created(self, event):
        if not event.is_directory:
            self._handle_event("CREATED", event.src_path)

def start_watching(watch_dir: Optional[str] = None):
    """Starts the directory observer."""
    if watch_dir is None:
        # Default to workspace root (parent of memory_system)
        watch_dir = str(Path(__file__).resolve().parent.parent)
        
    print("=" * 60)
    print("KONTRX / ANTIGRAVITY REAL-TIME CODE WATCHER")
    print(f"Monitored Directory: {watch_dir}")
    print(f"Target Extensions: {', '.join(SUPPORTED_EXTENSIONS.keys())}")
    print(f"Ignored Folders: {', '.join(IGNORED_DIRS)}")
    print("=" * 60)
    print("[*] Watching for file changes (Save / Create)... Press Ctrl+C to stop.")

    event_handler = CodeChangeHandler()
    observer = Observer()
    observer.schedule(event_handler, path=watch_dir, recursive=True)
    observer.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[!] Stopping file watcher...")
        observer.stop()
    observer.join()
    print("[OK] Watcher stopped cleanly.")

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else None
    start_watching(target)
