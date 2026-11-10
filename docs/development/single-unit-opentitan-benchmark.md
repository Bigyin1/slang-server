# Single-unit benchmark on OpenTitan

This run exercises the OpenTitan/Earlgrey design packaged by RTLMeter, using the same ordered source list, top module, defines, and compatibility options as slang's own benchmark runner.

- RTLMeter repository: <https://github.com/MikePopoloski/rtlmeter>
- RTLMeter commit: `69bbe91e8a2e911ef0562da2b0abab95dd4bbfc2`
- OpenTitan origin revision recorded by the descriptor: `a9e85dc63159e780b3640ecbfd965578deeb39b6`
- Configuration: `OpenTitan`, top `chip_sim_tb`, `--single-unit`.
- slang-server base: upstream `0541246eb1b9e4c1c986179cd9a9f1f8b5bb8b44` with the single-unit changes.
- Slang revision: `616dbef7379bcf0a85b3ccf86f9ae34919714404`.
- Build: Release, Apple clang 21.0.0, macOS arm64.

See [the reproduction instructions](../features/design/shallow.md#real-project-benchmark) for checkout and invocation commands. No third-party RTL is added to this repository. All edits in the test are sent as document notifications; the checkout remains unchanged.

## Workload

The build contains 643 top-level sources and 662 preprocessed source buffers, including headers. The source directory contains approximately 385,000 lines of RTL. Full elaboration records 39,379 module/interface instances. Initial diagnostics contain zero errors; the complete fingerprint of 18,138 diagnostics is retained for comparison.

Each selected file undergoes three cycles of three comment-only edits, a save, and a repeated save. The selected files exercise a peripheral, a larger cryptographic block, and the last source in the compilation unit.

The test also injects an unknown macro and an undeclared identifier into each file. Both must produce diagnostics locally and after saving. Restoring the source must restore the exact global diagnostic fingerprint and hierarchy size.

## Results

Times are medians in milliseconds from one run. Opening includes the server harness's document-symbol query. Initial workspace setup took 32.1 ms and initial build/diagnostic publication took 645.7 ms.

| Edited source | Open | Edit | Save after edits | Repeated save | Three edits and save |
| --- | ---: | ---: | ---: | ---: | ---: |
| `uart_core.sv` | 5.68 | 3.36 | 599.95 | 21.87 | 610.38 |
| `aes_core.sv` | 15.14 | 8.59 | 608.54 | 22.08 | 635.01 |
| `chip_sim_tb.sv` | 4.63 | 3.10 | 604.89 | 22.15 | 614.36 |

All three cases recorded zero shared-unit source visits during edits and zero unexpected local errors. A changed save visited the shared unit's source buffers once; repeated saves made no such visits. Diagnostic fingerprints and hierarchy sizes matched the baseline after each comment-only save and each error recovery.

The process peak RSS was 1955 MiB (about 1.91 GiB), including error-injection and recovery phases. This high-water mark does not establish that retained memory is constant over long editing sessions.

These are in-process server-handler measurements, not end-to-end editor latency: JSON-RPC serialization, transport, and editor disk writes are excluded. This run does not compare the previous patch on OpenTitan, and the results should not be interpreted as a measured speedup over that version. The larger design takes substantially longer to save than the synthetic benchmark; semantic analysis and diagnostic publication remain material costs.
