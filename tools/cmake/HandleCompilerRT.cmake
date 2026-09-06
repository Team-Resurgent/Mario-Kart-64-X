# torch's CMakeLists does `include(../cmake/HandleCompilerRT.cmake)` whenever the
# compiler is Clang on Windows, expecting a file from the Ship-of-Harkinian
# superproject that a standalone torch checkout does not have. That path
# resolves to THIS file. It exists so the include succeeds when torch is built
# with zig (which identifies as Clang); the function it is expected to define
# only locates compiler-rt builtins to link into StormLib, and zig links its
# own compiler-rt automatically, so a no-op is the correct answer here.
function(find_compiler_rt_library name outvar)
    set(${outvar} "" PARENT_SCOPE)
endfunction()
