// Hybrid QFS benchmark entry point.
// The implementation is defined in qfs_native_bench.cpp; this selects the
// corpus-profiled hybrid decoder for the standalone benchmark harness.
#define QFS_SIMD 1
#define QFS_HYBRID 1
#include "qfs_native_bench.cpp"
