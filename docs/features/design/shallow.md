# Shallow Compilation

In order to provide real time diagnostics and symbols, the server makes use of the concept of a shallow compilation, where directly referenced top level symbols in the current document are loaded for use in a slang compilation.

In software languages this is typically standard- you can compile just one cpp file for example, and forward declare other symbols that you use.
This isn't really a thing for HDLs, where a full design is almost always assumed. Parts of Slang can be tweaked to essentially get this functionality.

### Limitations

Hierarchical references can go down or up more than one layer, in which case some symbols may not load. It would be nice to continue adding the relevant syntax trees to get all symbols in the current document, rather than just loading directly referenced symbols. Upward references will always be a blind spot for the language server when a design isn't set, and are generally not considered a good practice.

## Untaken Generate Branches

Shallow compilations use slang's `CheckUninstantiated` flag to provide symbols and diagnostics in untaken generate branches. Many diagnostics are still useful in branches that are not elaborated for the current parameter values.

A related issue is the case of ifdef branches. In the future the server can attempt to parse out most untaken ifdef branches to at least provide goto/hover support, but likely never diagnostics.

## Interface Port Parameter Asserts

Without a full design, an interface port may have no connected instance to supply its parameters. Slang can infer value and type parameters for these ports from equality checks in `$static_assert` calls in the module body.

```systemverilog
package types_pkg;
    typedef logic [7:0] word_t;
endpackage

interface channel_if #(
    parameter int WIDTH = 1,
    parameter type data_t = logic
);
    logic [WIDTH-1:0] mask;
    data_t data;
endinterface

module consumer(channel_if channel, output types_pkg::word_t data);
    $static_assert(channel.WIDTH == 8);
    $static_assert(type(channel.data_t) == type(types_pkg::word_t));
    assign data = channel.data;
endmodule
```

During shallow analysis of `consumer`, these assertions set `channel.WIDTH` to `8` and `channel.data_t` to `types_pkg::word_t`, overriding the interface defaults. Hovers, completions, and diagnostics can then use the intended widths and member types. Without a matching assertion, the interface defaults apply.

This inference applies to interface ports without a connected instance. In a full design, the connected interface supplies the parameters and the assertions check them. `$static_assert` is a slang extension; ordinary runtime assertions do not provide these constraints.

## Document lifetime and saves

Saving a document updates the workspace index and publishes full-design diagnostics. If all
source buffers in the selected build are still current, including included headers, the server
reuses the existing full compilation. This applies to both ordinary compilation units and
`--single-unit`. Changes to another source or an included header still invalidate the build,
even when the saved document itself has not changed.

When a document closes, the server releases cached shallow analyses of documents that are no
longer open, including dependency documents analyzed for navigation. Open documents and requests
can keep shared analyses alive until they finish using them. Syntax trees remain available for
the build and dependency discovery; subsequent navigation can recreate a shallow analysis.
This bounds retention from browsing and closing documents, but does not impose a global memory
limit or require the allocator to return freed memory to the operating system immediately.

Run the optional ordinary-mode lifecycle benchmark with:

```sh
build/bin/server_unittests '[document-lifecycle-performance]'
```

It reports changed and unchanged save times for 128-file and 1500-file builds, plus the number
and AST allocation size of shallow compilations retained after opening and closing 30 files.
Normal lifecycle tests check compilation reuse, dependency invalidation, preserved diagnostics,
reopening documents, and shared-analysis lifetimes without timing thresholds.

Measurements against v0.3.3 on macOS arm64, Apple Clang 21, Release with mimalloc enabled:

| Ordinary-mode workload | Before | After |
| --- | ---: | ---: |
| OpenTitan `uart_core.sv`, unchanged save | 411.3 ms | 28.3 ms |
| OpenTitan `aes_core.sv`, unchanged save | 404.9 ms | 28.4 ms |
| OpenTitan `adc_ctrl_reg_top.sv`, unchanged save | 402.1 ms | 28.2 ms |
| OpenTitan changed save, range across these files | 406.3–407.6 ms | 403.2–406.1 ms |
| 1500 small synthetic sources, unchanged save | 38.6 ms | 19.9 ms |
| 1500 small synthetic sources, changed save | 38.5 ms | 39.7 ms |
| OpenTitan cached shallow AST after closing 30 files | 67.6 MiB | 0 MiB |
| OpenTitan open/close cycle, 30 files | 256.0 ms | 239.5 ms |
| Synthetic 1500-source build, open/close cycle, 30 files | 3.0 ms | 4.4 ms |

Save timings are medians of three fresh processes, each taking the median of three save cycles
for OpenTitan or five for the synthetic fixture. Before/after process order was alternated.
The ordinary OpenTitan fixture uses RTLMeter commit
`69bbe91e8a2e911ef0562da2b0abab95dd4bbfc2`: 642 syntax trees, 39,379 instances, and 18,138
diagnostics, whose fingerprints stayed unchanged during comment edits and repeated saves.
Memory figures count live shallow AST allocations, not total process RSS. The median RSS
increase during the OpenTitan open/close cycle fell from 166.1 MiB to 60.9 MiB; allocator
retention and other caches still contribute to RSS.

Closing a document scans the document cache, so cleanup costs grow with the number of known
documents. Reopening can require another shallow analysis. Unchanged saves still update the
workspace index and format/publish diagnostics; changed saves still refresh full semantics.

## Single Unit

Slang's `--single-unit` parses the files in a build as one compilation unit, allowing later files to inherit preprocessor macros from earlier files.

When a selected build uses `--single-unit`, full-design diagnostics use that compilation context. Opening an unchanged file from the build preserves its design diagnostics, including for files after the first file in the compilation unit.

Each source in a selected single-unit build has a local syntax tree for editor features. The server restores its inherited macros from the most recent build snapshot, including definitions subsequently removed by `undef` or replaced by `define`. These file-boundary contexts are cached until the snapshot changes.

Editing any source, including the last source in the unit, reparses that file and its includes. Shallow analysis loads referenced dependencies as local trees, so a reference to a declaration in the last source does not trigger a whole-unit parse. The shared build tree stays unchanged during edits.

Saving updates the workspace index, reparses an affected unit once, and refreshes full-design semantics and diagnostics. A repeated save without changed build buffers reuses the existing compilation. External changes to disk-backed unit sources or included headers also refresh the build snapshot; editor-owned buffers remain under editor control.

Full-design refreshes use a [compact instance index](instance-index.md) to preserve slang's
instance caching while retaining complete hierarchy paths and exact instance counts.

Until a build refresh, edits to earlier files' macros are not reflected in later files' inherited contexts. Local analysis also does not recreate the shared compilation-unit scope for declarations such as `$unit` typedefs, or all directive state such as `default_nettype` and `timescale`. A later `undefineall` cannot currently be rewound from slang's macro history. Full-design diagnostics use the complete compilation unit and are authoritative for these cases.

Files outside the selected build keep standalone analysis. Explicit includes and configured defines remain useful when opening a file without its build selected.

### Performance checks

The normal `server_unittests` suite checks that local edits do not visit the shared unit's source buffers, including edits to its last source and edits that load a dependency from that source. These checks use work counts rather than machine-dependent timing limits.

Run the optional synthetic benchmark separately:

```sh
build/macos-release/bin/server_unittests '[single-unit-benchmark]'
build/macos-release/bin/server_unittests '[single-unit-save-benchmark]'
```

It measures eight edits per case on 128-file and 1500-file builds, reporting median and maximum latency and the number of shared-unit source visits. Setup, initial compilation, and file opening are excluded from the timed edits. The generated files are small synthetic modules, so these measurements do not predict elaboration time for a production design.

The save benchmark measures five cycles of eight edits followed by a save, plus a repeated save without changes. It reports save latency, the complete edit-and-save cycle, and recorded parser time from slang's time trace. The trace omits individual parse events shorter than 0.5 ms. Source-visit checks require exactly one unit parse on the first save and none on the repeated save. The first save includes full semantic analysis; the repeated save reuses it.

### Real-project benchmark

The optional real-project test uses an existing RTLMeter checkout. It reuses slang's descriptor reader and compatibility flags from `external/slang/tests/benchmarks`, retaining the source order and top module. The preparer writes a separate workspace and leaves the checkout's RTL unchanged.

For the tested OpenTitan/Earlgrey snapshot:

```sh
git clone --depth 1 --filter=blob:none --sparse https://github.com/MikePopoloski/rtlmeter.git /tmp/slang-rtlmeter
git -C /tmp/slang-rtlmeter fetch --depth 1 origin 69bbe91e8a2e911ef0562da2b0abab95dd4bbfc2
git -C /tmp/slang-rtlmeter checkout --detach 69bbe91e8a2e911ef0562da2b0abab95dd4bbfc2
git -C /tmp/slang-rtlmeter sparse-checkout set designs/OpenTitan rtl

uv run --no-project --python 3.12 scripts/prepare-real-project-benchmark.py \
  /tmp/slang-rtlmeter /tmp/slang-opentitan-benchmark \
  --edit src/uart_core.sv --edit src/aes_core.sv --edit src/chip_sim_tb.sv

SLANG_REAL_PROJECT_BENCHMARK=/tmp/slang-opentitan-benchmark/benchmark.json \
  build/macos-release/bin/server_unittests '[single-unit-real-project]'
```

Use a fresh benchmark workspace when rerunning the preparer; it refuses to overwrite an existing server configuration. The test writes `results.json` next to `benchmark.json` and reports indexing/build time, file opening, edit/save latency, instance counts, diagnostics, and process peak RSS. RSS is the process high-water mark, not a measurement of retained memory after each operation.

The test requires at least 100 sources and 1000 instantiated modules/interfaces, checks that every descriptor source belongs to the unit, and requires zero initial errors. It verifies that edits do not reparse the shared unit, that each changed save reparses it once, and that repeated saves reuse the snapshot. Global diagnostics and hierarchy size must remain identical after comment-only saves. Both an unknown macro and an undeclared identifier are then injected into each edited source; the test requires diagnostics to appear and to return exactly to the baseline after restoring the text.

Measurements exercise the server handlers in process with inactive-region support enabled. They exclude editor disk I/O and JSON-RPC transport costs. Large projects are not downloaded or run by the default unit-test invocation.

See the [recorded OpenTitan run](../../development/single-unit-opentitan-benchmark.md) for its pinned revision, workload, measurements, and limits. The optional test skips when `SLANG_REAL_PROJECT_BENCHMARK` is unset.
