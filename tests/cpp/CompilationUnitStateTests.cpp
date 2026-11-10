// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "ast/CompilationUnitState.h"
#include "utils/ServerHarness.h"

#include "slang/syntax/AllSyntax.h"

TEST_CASE("CompilationUnitState caches and refreshes file-boundary macros", "[single-unit]") {
    slang::SourceManager sm;
    const auto root = fs::current_path();
    auto definitions = sm.assignText((root / "defs.sv").string(), "`define W 8\n");
    auto use = sm.assignText((root / "use.sv").string(),
                             "module use; logic [`W-1:0] value; endmodule\n");
    auto after = sm.assignText((root / "after.sv").string(), "`undef W\n`define W(x) x\n");
    const slang::SourceBuffer buffers[]{definitions, use, after};
    auto tree = slang::syntax::SyntaxTree::fromBuffers(buffers, sm);
    server::CompilationUnitState unit(sm, tree, tree->options());
    REQUIRE(unit.sources().size() == 3);
    const auto& macros = unit.macrosBefore(root / "use.sv");
    REQUIRE(macros.size() == 1);
    CHECK(macros.front()->body.front().valueText() == "8");
    CHECK_FALSE(macros.front()->formalArguments);
    CHECK(&unit.macrosBefore(root / "use.sv") == &macros);

    const std::string replacement = "`define W 16\n";
    slang::SmallVector<char> text;
    text.append(replacement.begin(), replacement.end());
    text.push_back('\0');
    sm.replaceBuffer(definitions.id, std::move(text));

    // Unsaved buffers do not implicitly replace the build snapshot or its cached contexts.
    CHECK(unit.tree() == tree);
    CHECK(unit.macrosBefore(root / "use.sv").front()->body.front().valueText() == "8");
    CHECK(unit.refresh() != tree);
    const auto& refreshed = unit.macrosBefore(root / "use.sv");
    REQUIRE(refreshed.size() == 1);
    CHECK(refreshed.front()->body.front().valueText() == "16");
    CHECK_FALSE(refreshed.front()->formalArguments);
    CHECK(unit.refresh() == unit.tree());
}

TEST_CASE("CompilationUnitState snapshots retain and release their source buffers",
          "[single-unit]") {
    slang::SourceManager sm;
    const auto root = fs::current_path();
    const auto usePath = root / "snapshot_use.sv";
    auto definitions = sm.assignText((root / "snapshot_defs.sv").string(), "`define W 8\n");
    auto use = sm.assignText(usePath.string(), "module use; logic [`W-1:0] value; endmodule\n");
    const slang::SourceBuffer buffers[]{definitions, use};
    auto tree = slang::syntax::SyntaxTree::fromBuffers(buffers, sm);
    auto options = tree->options();
    auto unit = std::make_unique<server::CompilationUnitState>(sm, std::move(tree), options);
    auto snapshot = unit->tree();
    const auto& macros = unit->macrosBefore(usePath);
    REQUIRE(macros.size() == 1);
    const auto* macro = macros.front();

    const slang::BufferID ids[]{definitions.id};
    auto handles = sm.retainBuffers(ids);
    REQUIRE(handles.size() == 1);
    std::weak_ptr<void> oldBuffer = handles.front();
    handles.clear();

    const std::string replacement = "`define W 16\n";
    slang::SmallVector<char> text;
    text.append(replacement.begin(), replacement.end());
    text.push_back('\0');
    sm.replaceBuffer(definitions.id, std::move(text));
    CHECK_FALSE(oldBuffer.expired());
    CHECK(macro->body.front().valueText() == "8");

    CHECK(unit->refresh() != snapshot);
    REQUIRE(unit->macrosBefore(usePath).size() == 1);
    CHECK(unit->macrosBefore(usePath).front()->body.front().valueText() == "16");
    CHECK_FALSE(oldBuffer.expired());
    CHECK(macro->body.front().valueText() == "8");

    unit.reset();
    CHECK_FALSE(oldBuffer.expired());
    CHECK(macro->body.front().valueText() == "8");
    snapshot.reset();
    CHECK(oldBuffer.expired());
}
