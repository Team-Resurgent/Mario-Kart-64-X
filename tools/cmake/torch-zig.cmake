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
# 2. src/audio/AudioManager.cpp has `a <= x <= b` chained comparisons that
#    Clang 21 diagnoses as an error by default. Third-party code; keep it
#    building.
add_compile_options(-Wno-parentheses)
