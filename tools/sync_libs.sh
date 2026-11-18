#!/usr/bin/env bash
# 4.0.0-mp: refresh dist/lib from the real library repositories (proper require layout: lib/<mod>.lua, lib/<mod>/init.lua).
# Usage: tools/sync_libs.sh <dir with the clones>   (clone names as on github.com/ThirtyFoxMCx00E1/<name>)
# MonetLoader's own files (moonloader.lua, monetloader.lua, memory.lua, mimgui/, samp/ events glue, sampfuncs.lua,
# vkeys.lua, game/, imgui.lua MoonImGui layer ...) are left alone.
set -euo pipefail
S=${1:?source dir}
L=$(cd "$(dirname "$0")/.." && pwd)/dist/lib
put() { mkdir -p "$(dirname "$L/$2")"; rm -rf "$L/$2"; cp -r "$S/$1" "$L/$2"; echo "  $2  <- $1"; }
# stale layouts from the old copy (wrong require paths)
rm -rf "$L/penlight" "$L/inspect" "$L/lume" "$L/compat"
put aeslua/src/aeslua.lua aeslua.lua
put aeslua/src/aeslua aeslua
put arizona-events/arizona-events arizona-events
put carbJsonConfigLua/carbJsonConfig.lua carbJsonConfig.lua
put copas/src/copas.lua copas.lua
put copas/src/copas copas
put extensions-lite/lua/extensions-lite extensions-lite
put lua-fa-icons-4/fa-icons.lua fa-icons.lua
put lua-htmlparser/src/htmlparser.lua htmlparser.lua
put lua-htmlparser/src/htmlparser htmlparser
put inspect.lua/inspect.lua inspect.lua
put lua-requests/src/requests.lua requests.lua
put lume/lume.lua lume.lua
put lua-path/lua/path.lua path.lua
put lua-path/lua/path path
put Penlight/lua/pl pl
put rkeys/lua/rkeys.lua rkeys.lua
put SNET/lua SNET
put xml2lua/xml2lua.lua xml2lua.lua
put xml2lua/XmlParser.lua XmlParser.lua
put xml2lua/xmlhandler xmlhandler
put lua-websockets/src/websocket.lua websocket.lua
put lua-websockets/src/websocket websocket
put lua-compat-5.3/compat53 compat53
put moonloader-bind/lua/bind.lua bind.lua
put SAMP.Lua/samp/events.lua samp/events.lua
put SAMP.Lua/samp/events samp/events
put SAMP.Lua/samp/raknet.lua samp/raknet.lua
put SAMP.Lua/samp/synchronization.lua samp/synchronization.lua
