# Canonical instance index benchmark

Measured locally on November 11, 2026 with Apple Clang 21.0.0, macOS arm64, Release
(`-O3 -DNDEBUG`), and mimalloc enabled. No sanitizers were enabled for timing measurements.
The baseline server has the production sources of `a97990e` (the corrected `v0.3.2` release).
Both builds use slang `616dbef7379bcf0a85b3ccf86f9ae34919714404` and the same dependencies.
The comparison uses the canonical instance index introduced with this report. These historical
integration measurements include the separate single-unit changes in both versions. The index
implementation and its synthetic benchmark below can also be built independently on upstream.

Each case ran in three fresh processes, with the order reversed in the second round.
Tables report medians. Builds and sanitizer tests had finished before timing started.
Peak RSS is the process high-water mark from `getrusage` / macOS `/usr/bin/time -l`.

## Replicated synthetic hierarchy

The optional `[instance-index-benchmark]` test generates six module definitions and
111,111 concrete instances, including 100,000 leaf instances. Each leaf contains 32 logic
variables. This deliberately emphasizes replication with very little parsing work.
It exercises `slang::ast::Compilation` and the instance index, excluding workspace indexing,
LSP handlers, and driver analysis. The OpenTitan comparison below exercises full server handlers.

| Implementation | Compile and index (ms) | Indexed occurrences | AST allocations (MiB) | Process peak RSS (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Original concrete-body traversal | 159.215 | 111,111 | 529.664 | 832.547 |
| Literal body-deduplication patch from #586 | 0.313 | 51 | 0.344 | 9.484 |
| Compact index with complete paths and counts | 0.352 | 111,111 | 0.344 | 18.469 |

The compact-index process additionally enumerates all 100,000 leaf paths. That query takes
2.395 ms and does not increase slang's AST allocations. The compile/index column excludes
this query; peak RSS includes it. Including the query, the measured work takes about 2.75 ms
versus 159.22 ms for the original compile/index step alone. The literal patch does not return
the omitted concrete paths.

The compact version uses about 45 times less peak process memory in this case. Its compile
and index step is about 453 times faster, or about 58 times faster when including the full
leaf-path query. These ratios describe this highly repetitive microbenchmark, not general
server loading performance.

Reproduce each mode in a fresh Release-build process:

```sh
SLANG_INSTANCE_BENCHMARK_MODE=legacy build/bin/server_unittests '[instance-index-benchmark]'
SLANG_INSTANCE_BENCHMARK_MODE=dedupe build/bin/server_unittests '[instance-index-benchmark]'
build/bin/server_unittests '[instance-index-benchmark]'
```

On macOS, prefix each command with `/usr/bin/time -l env` to collect peak RSS; on Linux,
use `/usr/bin/time -v env`. The test itself reports compilation time, AST allocations, and
the complete path-query time.

## OpenTitan

This uses the [pinned OpenTitan workload from the integration build](https://github.com/Bigyin1/slang-server/blob/08ce296f17646bbfe5ee7b974f92957fdd360ba2/docs/development/single-unit-opentitan-benchmark.md):
643 sources, 39,379 module/interface instances, and 18,138 published diagnostics with zero
initial errors. Both versions retained these counts in all three runs. Every run passed the
1,419 assertions covering local edits, saves, repeated saves, injected errors, and restoration
of each run's complete diagnostic fingerprint.

The initial build measurement includes parsing, semantic analysis, indexing the hierarchy,
and publishing build diagnostics. Workspace indexing is measured separately and excluded
from this column. The memory measurement covers the whole test process, including repeated
edits, saves, and error injection; it is not the retained-memory size immediately after load.

| Metric | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Initial build (ms) | 657.148 | 524.502 | 20.2% |
| Process peak RSS (MiB) | 1944.703 | 1367.859 | 29.7% |

| Edited file | Edit before / after (ms) | Changed save before / after (ms) | Repeated save before / after (ms) |
| --- | ---: | ---: | ---: |
| `uart_core.sv` | 3.499 / 2.861 | 612.690 / 490.522 | 22.372 / 22.292 |
| `aes_core.sv` | 8.596 / 6.738 | 621.543 / 500.860 | 22.590 / 22.628 |
| `chip_sim_tb.sv` | 3.082 / 3.087 | 610.379 / 493.383 | 22.536 / 23.110 |

Changed saves take 19–20% less time. Repeated saves remain around 22–23 ms and do not rebuild
the instance index. These are in-process handler timings; editor I/O and JSON-RPC transport
are excluded. Three paired runs do not establish sub-millisecond differences as regressions
or improvements.

Reproducing these integration measurements requires the separate single-unit benchmark harness;
it is not included in the standalone instance-index change. Follow the linked workload's
preparation instructions in that integration checkout, then run the same manifest with each
Release test binary in separate processes:

```sh
SLANG_REAL_PROJECT_BENCHMARK=/tmp/slang-opentitan-benchmark/benchmark.json \
  build/bin/server_unittests '[single-unit-real-project]'
```

Save the generated `results.json` after each invocation because the next run overwrites it.

## Ordinary compilation units

An additional local handler benchmark used the existing 128-file and 1500-file fixtures, with
one distinct module per file and every module instantiated. There is little body reuse in
these designs, making them useful for checking overhead. Each file underwent three cycles of
four edits, a changed save, and an unchanged save. Three fresh-process comparisons alternated
the order of versions. Instance counts and the captured diagnostic fingerprints remained
unchanged after saves.

| Sources / edited file | Initial build before / after (ms) | Edit before / after (ms) | Changed save before / after (ms) |
| --- | ---: | ---: | ---: |
| 128 / `leaf.sv` | 11.639 / 12.361 | 0.055 / 0.056 | 5.557 / 5.615 |
| 128 / `consumer.sv` | 9.996 / 10.142 | 6.659 / 6.372 | 5.781 / 5.765 |
| 1500 / `leaf.sv` | 120.497 / 120.338 | 0.149 / 0.143 | 71.321 / 70.783 |
| 1500 / `consumer.sv` | 115.515 / 114.659 | 84.510 / 83.661 | 73.160 / 72.991 |

No substantial edit/save slowdown was observed in these cases. Initial loading of the first
128-file case was 0.72 ms slower; these measurements do not establish a general improvement
for designs without repeated bodies. A normal unit test also checks that refreshing 128
distinct modules preserves automatic selections without allocating another set of parsed
hierarchical names for every selected module.

The ordinary-mode OpenTitan fixture has 642 syntax trees and the same 39,379 instances and
18,138 diagnostics. Its three-cycle edit/save workload also passed in all runs:

| Edited file | Initial build before / after (ms) | Changed save before / after (ms) | Repeated save before / after (ms) |
| --- | ---: | ---: | ---: |
| `uart_core.sv` | 789.631 / 686.363 | 540.992 / 416.887 | 533.442 / 406.849 |
| `aes_core.sv` | 748.819 / 620.464 | 570.209 / 404.866 | 545.409 / 405.261 |
| `adc_ctrl_reg_top.sv` | 758.439 / 613.038 | 550.760 / 415.789 | 556.028 / 413.398 |

Ordinary-mode saves rebuild the global compilation even without text changes, so both save
columns benefit from the optimized instance index. Its file-list configuration differs from
the single-unit fixture; compare before and after within each fixture, not across the two tables.

## Relation to issue #586

The [issue author's proprietary design](https://github.com/hudson-trading/slang-server/issues/586)
has 601,409 instances; they report a reduction from 52.6 to 4.25 GB and from 105 to 16–20 seconds.
That design is unavailable for reproducing the exact numbers. Our synthetic measurement
confirms that premature concrete-body traversal can cause very large time and memory costs.
Single-unit OpenTitan shows a smaller gain: about 1.25 times faster loading and 1.42 times less peak memory.
The result depends on how much of a design consists of interchangeable bodies and how much
time is spent in parsing and other analyses.

Correctness checks compare complete ordered paths and source locations against the original
concrete traversal, including parameter variants, arrays, generate blocks, local module
definitions, interfaces, `bind`, and `defparam`. Active selection and CodeLens are checked
after a build refresh. All 538 C++ test cases passed under ASan and UBSan. LeakSanitizer is not
available in the installed Apple Clang runtime.
