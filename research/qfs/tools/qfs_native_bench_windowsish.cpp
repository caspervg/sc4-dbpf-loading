// Windows-shaped companion build.
//
// This intentionally reuses the validated benchmark implementation while the
// build uses conservative x86 settings resembling the older SC4 code shape:
// size-oriented optimization, no inlining, and frame pointers retained.
#include "qfs_native_bench.cpp"
