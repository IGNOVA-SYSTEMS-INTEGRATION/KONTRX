import sys
from pathlib import Path

# Add memory_system to path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcp_server import search_antigravity_memory, list_memory_files

def test_mcp_functions():
    print("=" * 60)
    print("[*] TEST 1: Listing all memory files in ChromaDB:")
    print("=" * 60)
    file_list = list_memory_files()
    print(file_list)
    
    print("\n" + "=" * 60)
    print("[*] TEST 2: Semantic Search Query: 'battery voltage sensor'")
    print("=" * 60)
    search_res = search_antigravity_memory("battery voltage sensor", top_k=2)
    print(search_res)
    
    print("\n" + "=" * 60)
    print("[*] TEST 3: Semantic Search Query: 'relay MQTT control'")
    print("=" * 60)
    search_res2 = search_antigravity_memory("relay MQTT control", top_k=1)
    print(search_res2)

if __name__ == "__main__":
    test_mcp_functions()
