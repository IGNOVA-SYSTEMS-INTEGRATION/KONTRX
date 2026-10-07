import os
from pathlib import Path
from typing import List, Dict, Any, Optional
import chromadb
from chromadb.config import Settings

# Directory paths
BASE_DIR = Path(__file__).resolve().parent
DB_DIR = BASE_DIR / "chroma_db"
COLLECTION_NAME = "antigravity_memory"

def get_chroma_client():
    """Initializes and returns a persistent ChromaDB client."""
    DB_DIR.mkdir(parents=True, exist_ok=True)
    return chromadb.PersistentClient(path=str(DB_DIR))

def get_memory_collection(client=None):
    """Retrieves or creates the antigravity_memory collection."""
    if client is None:
        client = get_chroma_client()
    return client.get_or_create_collection(
        name=COLLECTION_NAME,
        metadata={"description": "Kontrx / Antigravity Code & Documentation Memory Collection"}
    )

def upsert_document(
    doc_id: str,
    document: str,
    metadata: Dict[str, Any]
) -> None:
    """Inserts or updates a document with its metadata in ChromaDB."""
    collection = get_memory_collection()
    # Ensure metadata values are str, int, float, or bool
    clean_metadata = {k: str(v) if not isinstance(v, (int, float, bool)) else v for k, v in metadata.items()}
    collection.upsert(
        ids=[doc_id],
        documents=[document],
        metadatas=[clean_metadata]
    )

def query_documents(
    query_text: str,
    n_results: int = 5,
    where: Optional[Dict[str, Any]] = None
) -> List[Dict[str, Any]]:
    """Performs semantic search on the memory collection."""
    collection = get_memory_collection()
    count = collection.count()
    if count == 0:
        return []
    
    n_results = min(n_results, count)
    results = collection.query(
        query_texts=[query_text],
        n_results=n_results,
        where=where
    )
    
    formatted_results = []
    if results and "ids" in results and results["ids"]:
        ids = results["ids"][0]
        documents = results["documents"][0] if "documents" in results else []
        metadatas = results["metadatas"][0] if "metadatas" in results else []
        distances = results.get("distances", [[]])[0] if "distances" in results else []
        
        for i in range(len(ids)):
            formatted_results.append({
                "id": ids[i],
                "document": documents[i] if i < len(documents) else "",
                "metadata": metadatas[i] if i < len(metadatas) else {},
                "distance": distances[i] if i < len(distances) else None
            })
            
    return formatted_results

def list_all_documents(limit: int = 100) -> List[Dict[str, Any]]:
    """Lists indexed documents from the collection."""
    collection = get_memory_collection()
    results = collection.get(limit=limit)
    items = []
    if results and "ids" in results:
        for i in range(len(results["ids"])):
            items.append({
                "id": results["ids"][i],
                "metadata": results["metadatas"][i] if "metadatas" in results and results["metadatas"] else {},
                "document_preview": results["documents"][i][:200] + "..." if results.get("documents") else ""
            })
    return items

def delete_document(doc_id: str) -> None:
    """Deletes a document by ID."""
    collection = get_memory_collection()
    collection.delete(ids=[doc_id])

if __name__ == "__main__":
    print("[*] Initializing ChromaDB test...")
    client = get_chroma_client()
    collection = get_memory_collection(client)
    print(f"[+] Connected to ChromaDB at: {DB_DIR}")
    print(f"[+] Collection '{COLLECTION_NAME}' active. Current items count: {collection.count()}")
    
    # Test Upsert
    test_id = "test_item_sample"
    test_content = "This is a test code explanation for Kontrx relay control system."
    test_meta = {"file_path": "test_sample.py", "language": "python", "type": "test"}
    upsert_document(test_id, test_content, test_meta)
    print(f"[+] Successfully inserted test document: '{test_id}'")
    
    # Test Query
    print("[*] Testing semantic search...")
    res = query_documents("How to control relays in Kontrx?", n_results=2)
    for r in res:
        print(f"  - Found match [ID: {r['id']} | Distance: {r['distance']}]:\n    {r['document']}")
    
    print("[OK] ChromaDB initialization and test completed successfully!")
