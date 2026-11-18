#!/usr/bin/env bash
# MP 4.0.0-mp: LuaJIT is built from source (https://github.com/ThirtyFoxMCx00E1/LuaJIT, branch v2.1) for both
# Android ABIs instead of the old prebuilt lib/lua/bin. Output: lib/lua/bin/<abi>/static_release/libluajit-5.1.a and
# the matching headers in lib/lua/. Usage: tools/build_luajit.sh <LuaJIT source dir>   (ANDROID_NDK_HOME set)
set -euo pipefail
SRC=${1:?LuaJIT source dir}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:?}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
API=24
for ABI in arm64-v8a armeabi-v7a; do
  rm -rf /tmp/lj-$ABI && cp -r "$SRC" /tmp/lj-$ABI
  if [ $ABI = arm64-v8a ]; then CC=$BIN/aarch64-linux-android$API-clang; CROSS=$BIN/aarch64-linux-android-; HOST="gcc"
  else CC=$BIN/armv7a-linux-androideabi$API-clang; CROSS=$BIN/arm-linux-androideabi-; HOST="gcc -m32"; fi
  make -C /tmp/lj-$ABI/src -j"$(nproc)" HOST_CC="$HOST" CROSS="$CROSS" STATIC_CC="$CC" DYNAMIC_CC="$CC -fPIC" \
       TARGET_LD="$CC" TARGET_AR="$BIN/llvm-ar rcus" TARGET_STRIP="$BIN/llvm-strip" \
       TARGET_CFLAGS="-fPIC -ffunction-sections -fdata-sections" BUILDMODE=static libluajit.a
  mkdir -p "$ROOT/lib/lua/bin/$ABI/static_release"
  cp /tmp/lj-$ABI/src/libluajit.a "$ROOT/lib/lua/bin/$ABI/static_release/libluajit-5.1.a"
done
for h in lua.h lualib.h lauxlib.h luaconf.h lua.hpp luajit.h; do cp /tmp/lj-arm64-v8a/src/$h "$ROOT/lib/lua/"; done
# the LuaJIT Lua side (jit.*: bcsave, dump, p, v ...) for scripts that require 'jit.xxx'
mkdir -p "$ROOT/dist/lib/jit" && cp /tmp/lj-arm64-v8a/src/jit/*.lua "$ROOT/dist/lib/jit/"
grep -m1 "LUAJIT_VERSION " "$ROOT/lib/lua/luajit.h" || true
