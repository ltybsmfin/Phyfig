"""Open the editor and inspect/control it through an actual MCP client."""
import asyncio
import json
from pathlib import Path
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]


async def main():
    server = StdioServerParameters(command=sys.executable, args=[
        str(ROOT / "mcp/server.py"), "--workspace", str(ROOT), "--start-editor",
        str(ROOT / "build/vs2022/bin/editor/Release/physics_editor.exe")])
    async with stdio_client(server) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            result = await session.call_tool("get_scene", {})
            if result.isError:
                raise RuntimeError(result.content)
            scene = json.loads(next(c.text for c in result.content if c.type == "text"))
            result = await session.call_tool("set_playback", {"time": min(0.8, scene["document"]["duration"]), "playing": False})
            if result.isError:
                raise RuntimeError(result.content)
            if "--save-example" in sys.argv:
                saved = await session.call_tool("save_scene", {"path": "scenes/vertical-throw.json", "expected_revision": scene["revision"]})
                if saved.isError:
                    raise RuntimeError(saved.content)
            print(f"Connected through MCP: revision {scene['revision']}, {len(scene['document']['objects'])} objects.")
            print("Host may reclaim auto-started windows when this client exits. For independent editing, start run-editor.ps1 first.")


if __name__ == "__main__":
    asyncio.run(main())
