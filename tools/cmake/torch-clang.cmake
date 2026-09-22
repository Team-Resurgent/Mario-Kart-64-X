# Injected into torch's configure via -DCMAKE_PROJECT_torch_INCLUDE=<this file>
# by setup.py when torch is built with RXDK's host-capable clang (a Windows
# machine with no Visual Studio, or any host without a native C++ toolchain).
# Runs right after torch's project() call and before the rest of its CMakeLists.
#
# 1. torch pins spdlog 7e635fc (2022), whose bundled fmt does not compile
#    under the RXDK clang: its consteval format-string
#    check is rejected, and on Windows-with-libc++ it includes <__std_stream>,
#    a private header libc++ removed years ago. FetchContent keeps the FIRST
#    declaration of a name, so declaring spdlog here, before torch does, swaps
#    in a release whose bundled fmt handles both. spdlog's 1.x API is stable;
#    torch uses nothing that changed.
#
#    v1.14.1 (fmt 10.2.1) specifically, not the newest: from fmt 11 on,
#    ostream.h pulls in chrono.h, which has a local variable named `tab` --
#    and torch's BaseFactory.h does `#define tab "	"`, so any TU that
#    includes both fails to parse. fmt 10.2.1 is new enough for a recent Clang
#    and old enough not to include chrono.h there.
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
# 2. Even fmt 10.2.1's compile-time format-string check is rejected by
#    this clang ("call to consteval function ... is not a constant expression").
#    fmt provides FMT_CONSTEVAL as the override: empty, the check moves to run
#    time, which is how fmt behaves on compilers without consteval anyway.
add_compile_definitions(FMT_CONSTEVAL=)
#    And fmt 10 no longer formats an `enum class` implicitly; torch logs one.
#    torch-zig-compat.h supplies the format_as() hook fmt 10 looks for.
add_compile_options(-include "${CMAKE_CURRENT_LIST_DIR}/torch-zig-compat.h")
# 3. src/audio/AudioManager.cpp has `a <= x <= b` chained comparisons that
#    this clang diagnoses as an error by default. Third-party code; keep it
#    building.
add_compile_options(-Wno-parentheses)
# 4. Static libraries (StormLib, spdlog, …). Pin the archiver at the llvm-ar /
#    llvm-ranlib that ship beside clang in the RXDK toolchain.
#
#    The old zig path used `zig ar` as a multicall subcommand and forced
#    CMAKE_AR to the compiler with `<CMAKE_AR> ar qc` rules. RXDK's clang has NO
#    `ar` subcommand, so `clang ar qc <lib> <objs>` fails with
#    "clang: error: no such file or directory: 'ar'". CMake usually finds a
#    sibling llvm-ar on its own, but pinning it (a) guarantees it over a stray
#    ar/lib.exe on PATH and (b) lets CMake's default Clang archive rules do the
#    work, instead of the broken zig subcommand strings.
get_filename_component(_rxdk_bindir "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(RXDK_LLVM_AR     NAMES llvm-ar     HINTS "${_rxdk_bindir}" NO_DEFAULT_PATH)
find_program(RXDK_LLVM_RANLIB NAMES llvm-ranlib HINTS "${_rxdk_bindir}" NO_DEFAULT_PATH)
if(RXDK_LLVM_AR)
    set(CMAKE_AR "${RXDK_LLVM_AR}" CACHE FILEPATH "RXDK llvm-ar" FORCE)
endif()
if(RXDK_LLVM_RANLIB)
    set(CMAKE_RANLIB "${RXDK_LLVM_RANLIB}" CACHE FILEPATH "RXDK llvm-ranlib" FORCE)
endif()
