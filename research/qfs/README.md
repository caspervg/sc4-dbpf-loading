# QFS/RefPack decoder research

This directory preserves the reusable tools used to profile and optimize the
DLL's QFS decoder. They were imported from the sibling `dbpf-analysis`
workspace after testing against a large SimCity 4 Plugins corpus. Generated
executables, object files, assembly listings, measurements, CSV samples, and
the corpus itself are intentionally excluded.

## Contents

- `tools/qfs_experiment.py` is the original readable Python DBPF/QFS scanner
  and decoder used for early inventory and latency measurements.
- `tools/qfs_native_bench.cpp` contains the Windows-shaped scalar decoder, the
  first AVX2 decoder, the final hybrid candidate, and the shared DBPF scanner.
- `tools/qfs_native_bench_windowsish.cpp` and
  `tools/qfs_native_bench_simd.cpp` are thin companion build entry points.
- `tools/qfs_native_profile.cpp` validates all three native decoders byte for
  byte, profiles FSH command distributions, reports malformed streams, and
  measures the latency tails that motivated the hybrid copy strategy.

## Building the native tools

Use an x86 Visual Studio Developer PowerShell. The DLL and SimCity 4 process
are 32-bit, so benchmark the same target architecture.

```powershell
cd research\qfs\tools

cl /nologo /std:c++20 /EHsc /Os /Ob0 /Oy- `
  qfs_native_bench_windowsish.cpp /Fe:qfs_native_bench_windowsish.exe

cl /nologo /std:c++20 /EHsc /O2 /arch:AVX2 `
  qfs_native_bench_simd.cpp /Fe:qfs_native_bench_simd.exe

cl /nologo /std:c++20 /EHsc /O2 /arch:AVX2 `
  qfs_native_profile.cpp /Fe:qfs_native_profile.exe
```

The profiler requires an AVX2-capable PC. The production DLL differs by
placing AVX2 code in its own translation unit and selecting it only after
CPUID, OSXSAVE, and XCR0 checks.

## Running a corpus profile

The profiler accepts individual `.dat` files or directories, which it scans
recursively. Results vary with CPU state and storage/cache conditions, so use
the same corpus and repeat count when comparing revisions.

```powershell
.\qfs_native_profile.exe --repeats=3 'D:\path\to\SimCity 4\Plugins'
```

To validate streams and print detailed warnings without timing every valid
entry:

```powershell
.\qfs_native_profile.exe --scan-errors 'D:\path\to\SimCity 4\Plugins'
```

The Python inventory can be run with its declared dependency via `uv`:

```powershell
uv run .\qfs_experiment.py 'D:\path\to\SimCity 4\Plugins' --repeats=3
```

These tools are research utilities, not hardened parsers for untrusted input.
They should not be linked into the production DLL.
