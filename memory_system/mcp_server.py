import sys
import json
from pathlib import Path
from typing import Optional, List, Dict, Any

# Ensure local imports work regardless of working directory
sys.path.insert(0, str(Path(__file__).resolve().parent))
from db import query_documents, list_all_documents, get_memory_collection
from doc_generator import process_file, SUPPORTED_EXTENSIONS

# Compatibility with both mcp 2.x (MCPServer) and mcp 1.x (FastMCP)
try:
    from mcp.server import MCPServer
    mcp_app = MCPServer(
        name="AntigravityMemoryServer",
        instructions="MCP Server providing semantic search and memory retrieval over Kontrx / Antigravity codebase and documentation."
    )
except (ImportError, AttributeError):
    try:
        from mcp.server.fastmcp import FastMCP
        mcp_app = FastMCP(
            name="AntigravityMemoryServer",
            instructions="MCP Server providing semantic search and memory retrieval over Kontrx / Antigravity codebase and documentation."
        )
    except Exception as e:
        raise ImportError(f"Failed to load MCP server from mcp library: {e}")

@mcp_app.tool()
def search_antigravity_memory(query: str, top_k: int = 4) -> str:
    """
    Search the Antigravity / Kontrx memory database for matching code, documentation, functions, and architecture.
    
    Args:
        query: The natural language search query, question, or code topic (e.g. 'How do relays get switched on MQTT commands?', 'FreeRTOS task priorities').
        top_k: Number of most relevant documentation/code entries to retrieve (default: 4).
    
    Returns:
        Structured string containing matched files, explanations, and code snippets.
    """
    results = query_documents(query_text=query, n_results=top_k)
    
    if not results:
        return f"No relevant documentation found in ChromaDB memory for query: '{query}'."
    
    output_parts = [f"### Antigravity Memory Search Results for: '{query}'\n"]
    for i, res in enumerate(results, 1):
        meta = res.get("metadata", {})
        doc = res.get("document", "")
        file_name = meta.get("file_name", "Unknown")
        file_path = meta.get("file_path", "Unknown")
        lang = meta.get("language", "text")
        distance = res.get("distance", "N/A")
        
        output_parts.append(f"#### Match #{i}: `{file_name}` ({lang})")
        output_parts.append(f"- **Path**: `{file_path}`")
        if distance != "N/A" and distance is not None:
            output_parts.append(f"- **Similarity Distance**: `{distance:.4f}`")
        output_parts.append(f"\n{doc}\n")
        output_parts.append("-" * 40)
        
    return "\n".join(output_parts)

@mcp_app.tool()
def list_memory_files() -> str:
    """
    Lists all files currently indexed in the Antigravity memory database.
    """
    items = list_all_documents(limit=100)
    if not items:
        return "The memory database is currently empty. Run doc_generator.py or watcher.py to index project files."
    
    summary = [f"### Indexed Files in Memory ({len(items)} files total):\n"]
    for item in items:
        meta = item.get("metadata", {})
        summary.append(f"- **{meta.get('file_name', 'Unknown')}** (`{meta.get('language', 'unknown')}`) | Path: `{meta.get('file_path', '')}` | Updated: `{meta.get('updated_at', '')}`")
        
    return "\n".join(summary)

@mcp_app.tool()
def reindex_single_file(file_path: str) -> str:
    """
    Manually triggers documentation generation and reindexing for a specific source file.
    
    Args:
        file_path: The relative or absolute path of the file to index.
    """
    res = process_file(file_path)
    if res:
        return f"Successfully generated documentation and updated ChromaDB for: {file_path}"
    else:
        return f"Failed to index {file_path}. Check if file exists and has a supported extension ({', '.join(SUPPORTED_EXTENSIONS.keys())})."

if __name__ == "__main__":
    print("[*] Starting Antigravity MCP Memory Server via stdio transport...", file=sys.stderr)
    mcp_app.run(transport="stdio")
