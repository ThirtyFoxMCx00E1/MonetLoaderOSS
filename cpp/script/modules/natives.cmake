# 4.0.0-mp: native Lua modules compiled into libmonetloader.so from their real upstream sources
#   lpeg 1.1.0 (Roberto Ierusalimschy)          -> require 'lpeg' (+ re.lua)
#   lrexlib (ThirtyFoxMCx00E1/lrexlib)         -> require 'rex_posix', 'rex_pcre2' (alias 'rex_pcre')
#   LuaSQL sqlite3 + SQLite 3.50.4             -> require 'luasql.sqlite3'
#   LuaLanes 4 (ThirtyFoxMCx00E1/lanes)        -> require 'lanes' (lanes.core)
#   lubyk xml (RapidXML)                       -> require 'xml' (xml.core)
set(MODDIR ${PROJECT_SOURCE_DIR}/cpp/script/modules)

include(FetchContent)
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT OFF CACHE BOOL "" FORCE)
set(PCRE2_STATIC_PIC ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pcre2
    URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz
    URL_HASH SHA256=86b9cb0aa3bcb7994faa88018292bc704cdbb708e785f7c74352ff6ea7d3175b)
FetchContent_MakeAvailable(pcre2)

add_library(mp_lua_natives STATIC
    ${MODDIR}/lpeg/lpcap.c ${MODDIR}/lpeg/lpcode.c ${MODDIR}/lpeg/lpcset.c ${MODDIR}/lpeg/lpprint.c
    ${MODDIR}/lpeg/lptree.c ${MODDIR}/lpeg/lpvm.c
    ${MODDIR}/lrexlib/common.c ${MODDIR}/lrexlib/posix/lposix.c ${MODDIR}/lrexlib/pcre2/lpcre2.c ${MODDIR}/lrexlib/pcre2/lpcre2_f.c
    ${MODDIR}/luasql/luasql.c ${MODDIR}/luasql/ls_sqlite3.c ${MODDIR}/luasql/sqlite/sqlite3.c
    ${MODDIR}/xml/Parser.cpp ${MODDIR}/xml/bind/dub/dub.cpp ${MODDIR}/xml/bind/xml_Parser.cpp ${MODDIR}/xml/bind/xml_core.cpp
    ${MODDIR}/lanes/allocator.cpp ${MODDIR}/lanes/cancel.cpp ${MODDIR}/lanes/compat.cpp ${MODDIR}/lanes/deep.cpp
    ${MODDIR}/lanes/intercopycontext.cpp ${MODDIR}/lanes/keeper.cpp ${MODDIR}/lanes/lane.cpp ${MODDIR}/lanes/lanes.cpp
    ${MODDIR}/lanes/linda.cpp ${MODDIR}/lanes/lindafactory.cpp ${MODDIR}/lanes/nameof.cpp ${MODDIR}/lanes/state.cpp
    ${MODDIR}/lanes/threading.cpp ${MODDIR}/lanes/tools.cpp ${MODDIR}/lanes/tracker.cpp ${MODDIR}/lanes/universe.cpp)
target_include_directories(mp_lua_natives PRIVATE ${PROJECT_SOURCE_DIR}/lib/lua ${MODDIR}/lrexlib ${MODDIR}/luasql
    ${MODDIR}/luasql/sqlite ${MODDIR}/xml/include ${MODDIR}/xml/vendor ${MODDIR}/xml/bind ${MODDIR}/lanes)
target_compile_options(mp_lua_natives PRIVATE -w -fPIC)
target_compile_definitions(mp_lua_natives PRIVATE
    VERSION="2.10.1" PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_MEMSTATUS=0 LUASQL_VERSION_NUMBER="2.7.0"
    "LUA_COMPAT_ALL" "LUA_COMPAT_5_1" MP_LUAJIT
    buffer_init=rex_buffer_init buffer_free=rex_buffer_free buffer_clear=rex_buffer_clear
    buffer_addbuffer=rex_buffer_addbuffer buffer_addlstring=rex_buffer_addlstring
    buffer_addvalue=rex_buffer_addvalue buffer_pushresult=rex_buffer_pushresult)
target_link_libraries(mp_lua_natives PRIVATE pcre2-8-static)
target_link_libraries(${PROJECT_NAME} PRIVATE mp_lua_natives pcre2-8-static)
