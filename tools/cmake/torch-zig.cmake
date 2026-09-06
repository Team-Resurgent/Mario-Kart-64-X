# Injected into torch's configure via -DCMAKE_PROJECT_torch_INCLUDE=<this file>
# by setup.py when torch is built with zig (a Windows machine with no Visual
# Studio, or any host without a native C++ toolchain). Runs right after
# torch's project() call and before the rest of its CMakeLists.
#
# 1. torch pins spdlog 7e635fc (2022), whose bundled fmt does not compile
#    under the Clang 21 that zig 0.16 carries: its consteval format-string
#    check is rejected, and on Windows-with-libc++ it includes <__std_stream>,
#    a private header libc++ removed years ago. FetchContent keeps the FIRST
#    declaration of a name, so declaring spdlog here, before torch does, swaps
#    in a release whose bundled fmt handles both. spdlog's 1.x API is stable;
#    torch uses nothing that changed.
#
#    v1.14.1 (fmt 10.2.1) specifically, not the newest: from fmt 11 on,
#    ostream.h pulls in chrono.h, which has a local variable named `tab` --
#    and torch's BaseFactory.h does `#define tab "	"`, so any TU that
#    includes both fails to parse. fmt 10.2.1 is new enough for Clang 21 and
#    old enough not to include chrono.h there.
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
# 2. Even fmt 10.2.1's compile-time format-string check is rejected by
#    Clang 21 ("call to consteval function ... is not a constant expression").
#    fmt provides FMT_CONSTEVAL as the override: empty, the check moves to run
#    time, which is how fmt behaves on compilers without consteval anyway.
add_compile_definitions(FMT_CONSTEVAL=)
#    And fmt 10 no longer formats an `enum class` implicitly; torch logs one.
#    torch-zig-compat.h supplies the format_as() hook fmt 10 looks for.
add_compile_options(-include "${CMAKE_CURRENT_LIST_DIR}/torch-zig-compat.h")
# 3. src/audio/AudioManager.cpp has `a <= x <= b` chained comparisons that
#    Clang 21 diagnoses as an error by default. Third-party code; keep it
#    building.
add_compile_options(-Wno-parentheses)
# 4. Static libraries. CMake looks for a standalone `ar`/`ranlib`, which a
#    machine with no native toolchain does not have (CMAKE_AR-NOTFOUND at link
#    time). zig ships both as subcommands. CMAKE_AR must be one executable, so
#    point it at zig itself and put the subcommand into the rule strings.
#    CMAKE_C_COMPILER is the resolved zig path here; the "cc" argument lives in
#    CMAKE_C_COMPILER_ARG1.
set(CMAKE_AR "${CMAKE_C_COMPILER}" CACHE FILEPATH "zig, used as ar/ranlib" FORCE)
foreach(lang C CXX)
    set(CMAKE_${lang}_ARCHIVE_CREATE "<CMAKE_AR> ar qc <TARGET> <OBJECTS>")
    set(CMAKE_${lang}_ARCHIVE_APPEND "<CMAKE_AR> ar q <TARGET> <OBJECTS>")
    set(CMAKE_${lang}_ARCHIVE_FINISH "<CMAKE_AR> ranlib <TARGET>")
endforeach()
