# Instance indexing

Loading a build or refreshing its semantics first runs slang's semantic diagnostics pass.
This allows slang to identify equivalent instance bodies before the server indexes the
hierarchy. Walking all concrete bodies first forces slang to elaborate duplicate symbols,
which can dominate both loading time and memory use in a replicated design.

The server stores a graph of canonical bodies. An edge records an instance and its path
relative to the containing body, including generate scopes and array indices. Propagating
the number of occurrences through this graph gives exact module instance counts without
expanding every concrete subtree. The same traversal selects the default active instance
using depth, source location, and path, as before.

`slang.getScopesByModule` returns these exact counts. `slang.getInstancesOfModule` expands
and caches the requested module's complete path list in the original traversal order.
The response format, instantiation locations, and CodeLens choices are unchanged. The
representative symbol supplies source locations; it is not used as the semantic context
of another occurrence. Active-instance queries resolve the selected concrete path through
slang and cache that symbol until the next build refresh. Refreshing also checks whether
the previous selection still exists in the compact graph, without parsing and resolving
every saved path through slang again.

This applies to both separate compilation units and `--single-unit` builds. It reduces the
cost of full-design refreshes; it does not eliminate the refresh after saving changed build
sources. Local editing and syntax-tree ownership follow the existing shallow-analysis rules.

Enumerating a module with many instances still requires time and response memory proportional
to the number of returned paths. Opening a concrete scope can elaborate that scope. Other
queries that traverse the full semantic hierarchy, such as hierarchy search or cone tracing,
can still expand many concrete bodies. This index does not replace those traversals.

## Verification

The normal `server_unittests` suite compares counts, ordered paths, source locations, and
default selections against a concrete AST traversal. Cases include replicated subtrees,
parameter variations, instance arrays, generate loops, escaped identifiers, interfaces,
`defparam`, and `bind`. It also checks active-instance selection and CodeLens after a refresh.
Allocation checks require indexing and path enumeration to leave slang's AST allocation
unchanged for a replicated design.
Another check refreshes a design with 128 distinct child modules and requires the selected
paths to survive without allocating a separate name parse for every automatic selection.

The optional benchmark creates 111,111 instances. Run each mode in a separate Release-build
process so process peak RSS can be compared:

```sh
build/bin/server_unittests '[instance-index-benchmark]'
SLANG_INSTANCE_BENCHMARK_MODE=legacy build/bin/server_unittests '[instance-index-benchmark]'
SLANG_INSTANCE_BENCHMARK_MODE=dedupe build/bin/server_unittests '[instance-index-benchmark]'
```

The legacy mode uses the original concrete-body traversal before semantic diagnostics.
The canonical mode runs diagnostics first and uses the compact index. Both check the same
total instance count; the canonical mode additionally enumerates all 100,000 leaf paths without
allocating duplicate AST bodies. The `dedupe` mode measures the literal proposal from
[issue #586](https://github.com/hudson-trading/slang-server/issues/586), which indexes only
51 representatives in this design and omits the other concrete occurrences. Timings are
reported rather than used as test thresholds.

The [OpenTitan benchmark](../../development/single-unit-opentitan-benchmark.md) exercises
complete server build, edit, save, and diagnostic publication paths on a real design.

See the [before/after measurements](../../development/instance-index-benchmark.md) for
synthetic and OpenTitan timing and memory results, including a comparison with the literal
patch from issue #586.
