# Hierarchy Explorer

Run **slang: Open Hierarchy Explorer**, or use the list-tree button in the
Hierarchy sidebar, to inspect the selected build in an editor tab. The sidebar's
**Open Here in Hierarchy Explorer** action opens a subtree directly.

The table separates instance names, module definitions, kinds, and definition
files. Expand individual branches, filter the displayed levels by instance or
module name, and sort siblings by name or module. Double-click a block to focus
its subtree; use the breadcrumbs, Parent, or Root to move back out.

Selecting a row shows ports, parameters, and signals for that hierarchy path.
It does not change the active instance. Use **Make active** explicitly to change
the editor's analysis context. **Instantiation** and **Definition** navigate to
source, including definitions outside the workspace. **Show in sidebar** selects
the item in the original hierarchy view. The solid dot identifies the most
recently activated instance.

The global search includes module names and structural hierarchy paths. It
returns up to 100 matches and reports the total; choosing a match reveals its
ancestors without activating the instance.

Arrow keys move through visible rows and expand or collapse branches. Enter
focuses the selected subtree. Drag header separators to resize columns and the
separator next to the details panel to resize it. The details separator also
supports Left and Right arrow keys.

The view loads levels on demand and renders only rows near the viewport.
It preserves expansion, selection, and subtree focus across refreshes of the
same build, and returns to the roots when selecting another build. It uses the
latest global compilation, which can differ from unsaved local analysis (and,
with single-unit incremental analysis, from ordinary edits). Refresh reloads the
view; it does not force a compilation of the entire design.
