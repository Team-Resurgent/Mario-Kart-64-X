/* Force-included (-include) into every torch TU by torch-zig.cmake.
 *
 * torch's pinned spdlog carries fmt 9, which formatted an `enum class` by
 * silently converting it to its underlying integer. The fmt 10 that the zig
 * build substitutes (see torch-zig.cmake) dropped that and instead looks for
 * a `format_as(E)` function found by ADL. GeoLayoutFactory.cpp logs a
 * GeoOpcode, so supply the one-liner fmt 9 used to do implicitly.
 *
 * Declaring the enum opaquely here (a scoped enum may be declared before it
 * is defined, and its underlying type is int either way) keeps this header
 * independent of torch's include order. */
#pragma once
#ifdef __cplusplus   /* -include applies to StormLib's C sources too */
enum class GeoOpcode;
constexpr int format_as(GeoOpcode o) { return static_cast<int>(o); }
#endif
