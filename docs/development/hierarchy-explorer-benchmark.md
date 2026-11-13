# Hierarchy Explorer checks

Measured locally on November 13, 2026, on macOS arm64 with the Release server.
Browser checks use a fresh headless Chrome profile and the actual browser bundle.
Numbers are observations, not CI timing thresholds.

| Scenario | Available structural rows | Rendered DOM rows | JavaScript heap |
| --- | ---: | ---: | ---: |
| Synthetic top, 500 children | 501 | 32 initially, 41 after scrolling | 1.23 MiB |
| OpenTitan `chip_sim_tb.u_dut.top_earlgrey` | 46 | 32 | 1.11 MiB |

Heap measurements describe the standalone page's JavaScript heap, not total
Chrome or VS Code process memory. No baseline speedup is claimed.

The existing OpenTitan demo build (`design.f`, single unit) loaded in about
0.58 seconds. Sequential queries for the testbench, its DUT wrapper, and
`top_earlgrey` took about 3.3 ms combined. The final scope contains 752 members;
only its 45 structural children go into the main table. Ports, parameters, and
signals are used for the details panel. No collapsed child module was queried.

A first global structural search for `uart` took 1.8–2.3 seconds. It builds the
existing hierarchy-search index and can cover many elaborated occurrences.
Search remains an explicit action, with at most 100 transferred results. It is
not part of opening or expanding the Explorer.

To repeat the synthetic browser check after building the extension:

```sh
cd clients/vscode
node scripts/build.mjs
node integration/hierarchyBrowser.mjs
```

Set `CHROME_PATH` when Chrome is not at the platform default location. The check
asserts that DOM rows stay bounded and that clicking a row selects without
activating an instance. An optional first argument supplies JSON `Row[]` from
a real hierarchy. Adapter unit tests separately check request deduplication,
embedded array paths, and stale-generation rejection.
