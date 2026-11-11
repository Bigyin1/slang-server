// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

// --single-unit builds: one syntax tree spans every file of the build, so a file sees the macros
// that earlier files define. Edits and saves must keep that view.

#include "lsp/LspTypes.h"
#include "utils/ServerHarness.h"
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "slang/syntax/AllSyntax.h"
#include "slang/util/ScopeGuard.h"

namespace {

namespace fs = std::filesystem;

/// defs.sv defines W and V; child.sv uses both; after.sv, compiled after child.sv, undefines W and
/// redefines V with an argument. Standalone, child.sv would see neither; with the final macro
/// table it would see no W and the wrong V.
struct SingleUnitRepo {
    fs::path dir;

    explicit SingleUnitRepo(const std::string& name) :
        dir(fs::weakly_canonical(fs::temp_directory_path() / ("slang_test_single_unit_" + name))) {
        fs::remove_all(dir);
        fs::create_directories(dir / ".slang");
        fs::create_directories(dir / "wrap");
        write("defs.sv", "`define W 4\n`define V 2\n");
        write("top.sv", "module top (input logic [`W-1:0] a, output logic [`W-1:0] y0);\n"
                        "  child u0 (.a(a), .y(y0));\nendmodule\n");
        write("child.sv", "module child (input logic [`W-1:0] a, output logic [`W-1:0] y);\n"
                          "  typedef enum logic [1:0] {A, B, C} st_e;\n"
                          "  st_e s;\n"
                          "  localparam int v = `V;\n"
                          "  always_comb begin\n"
                          "    s = a[0];\n" // the one real error: no implicit conversion to enum
                          "    y = a;\n"
                          "  end\nendmodule\n");
        write("after.sv", "`undef W\n`define V(x) x\nmodule after_m; endmodule\n");
        std::ofstream build(dir / "build.f");
        for (auto file : {"defs.sv", "top.sv", "child.sv", "after.sv"})
            build << dir / file << '\n';
        write(".slang/server.json", R"({"flags": "--single-unit --top top -Wno-redef-macro"})");
    }
    ~SingleUnitRepo() { fs::remove_all(dir); }

    void write(const std::string& file, const std::string& text) const {
        std::ofstream(dir / file) << text;
    }
    URI uri(const std::string& file) const { return URI::fromFile(dir / file); }
};

std::vector<std::string> errorMessages(ServerHarness& server, const URI& uri) {
    std::vector<std::string> result;
    for (auto& diag : server.client.getDiagnostics(uri)) {
        if (diag.severity == lsp::DiagnosticSeverity::Error)
            result.push_back(diag.message);
    }
    return result;
}

int errorCount(ServerHarness& server, const SingleUnitRepo& repo) {
    int count = 0;
    for (auto file : {"defs.sv", "top.sv", "child.sv", "after.sv"})
        count += int(errorMessages(server, repo.uri(file)).size());
    return count;
}

class SingleUnitHarness final : public ServerHarness {
public:
    using ServerHarness::ServerHarness;
};

std::unique_ptr<SingleUnitHarness> startServer(const SingleUnitRepo& repo) {
    auto server = std::make_unique<SingleUnitHarness>(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(repo.dir), .name = "test"}}}});
    server->setBuildFile((repo.dir / "build.f").string());
    REQUIRE(server->m_driver->comp);
    return server;
}

} // namespace

TEST_CASE("SingleUnit_BuildPathsWithSpacesKeepMacroContext") {
    SingleUnitRepo repo("paths with spaces");
    auto server = startServer(repo);
    CHECK(errorCount(*server, repo) == 1);

    auto child = server->openFile("child.sv");
    child.append("// edited\n");
    child.publishChanges();
    CHECK(errorMessages(*server, repo.uri("child.sv")).size() == 1);
    child.save();
    CHECK(errorCount(*server, repo) == 1);
}

TEST_CASE("SingleUnit_EditSeesMacrosActiveAtFileStart") {
    SingleUnitRepo repo("edit");
    auto server = startServer(repo);
    CHECK(errorCount(*server, repo) == 1);

    auto child = server->openFile("child.sv");
    child.append("// edited\n");
    child.publishChanges();
    // W from defs.sv despite the later `undef; V without arguments despite the later redefinition.
    CHECK(errorMessages(*server, repo.uri("child.sv")).size() == 1);

    child.save();
    CHECK(errorCount(*server, repo) == 1);
}

TEST_CASE("SingleUnit_EditDoesNotReparseUnit") {
    SingleUnitRepo repo("reparse");
    auto server = startServer(repo);
    auto unitDoc = server->getDoc(repo.uri("after.sv")); // the unit is keyed by its last file
    REQUIRE(unitDoc);
    auto unitTree = unitDoc->getUnitTree();
    REQUIRE(unitTree);

    auto child = server->openFile("child.sv");
    for (int i = 0; i < 3; i++) {
        child.append("// edited\n");
        child.publishChanges();
    }
    CHECK(unitDoc->getUnitTree() == unitTree);

    child.save();
    CHECK(unitDoc->getUnitTree() != unitTree);
    CHECK(errorCount(*server, repo) == 1);
}

TEST_CASE("SingleUnit_IncluderOutsideBuildKeepsUnit") {
    SingleUnitRepo repo("includer");
    // Not in the build, but indexed: it includes the unit's last file.
    repo.write("wrap/wrapper.sv", "module wrapper;\n`include \"../after.sv\"\nendmodule\n");
    auto server = startServer(repo);
    CHECK(errorCount(*server, repo) == 1);

    auto child = server->openFile("child.sv");
    child.append("// edited\n");
    child.save();
    CHECK(errorMessages(*server, repo.uri("child.sv")).size() == 1);
    CHECK(errorCount(*server, repo) == 1);
}

TEST_CASE("SingleUnit_EditDependencyInLastSourceKeepsUnitSnapshot") {
    SingleUnitRepo repo("owner_dependency");
    std::ifstream file(repo.dir / "child.sv");
    REQUIRE(file);
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto declaration = text.find("  typedef");
    REQUIRE(declaration != std::string::npos);
    text.insert(declaration, "  after_m u_dependency();\n");
    repo.write("child.sv", text);

    auto server = startServer(repo);
    auto child = server->openFile("child.sv");
    DocumentHandle unit(*server, repo.uri("after.sv"), "");
    REQUIRE(unit.doc);
    auto previousTree = unit.doc->getUnitTree();
    REQUIRE(previousTree);

    std::vector<std::string> processedSources;
    auto onBufferChange = [&](slang::BufferID buffer, bool isBack, bool isSkip) {
        if (!isBack && !isSkip)
            processedSources.push_back(
                server->sourceManager().getFullPath(buffer).filename().string());
    };
    unit.setBufferChangeCallback(onBufferChange);
    auto resetCallback = slang::ScopeGuard([&] { unit.setBufferChangeCallback({}); });

    child.append("// comment-only edit\n");
    child.publishChanges();

    CHECK(processedSources.empty());
    CHECK(unit.doc->getUnitTree() == previousTree);
    REQUIRE(unit.getCachedSyntaxTree());
    CHECK(unit.getCachedSyntaxTree()->getSourceBufferIds().size() == 1);
    CHECK(errorMessages(*server, repo.uri("child.sv")).size() == 1);
}

TEST_CASE("SingleUnit_SaveUpdatesCachedContextsAndRepeatedSaveReusesCompilation") {
    SingleUnitRepo repo("context_refresh");
    auto server = startServer(repo);
    auto child = server->openFile("child.sv");
    auto definitions = server->openFile("defs.sv", "`define W 4\n`define V 2\n");
    auto snapshot = definitions.doc->getUnitTree();
    auto width = [&] {
        const auto macros = child.doc->getSyntaxTree()->getDefinedMacros();
        const auto it = std::ranges::find_if(macros, [](const auto* macro) {
            return macro->name.valueText() == "W";
        });
        REQUIRE(it != macros.end());
        return std::string((*it)->body.front().valueText());
    };
    REQUIRE(width() == "4");
    definitions.replaceAll("`define W 8\n`define V 2\n");
    definitions.publishChanges();
    child.append("// edit while earlier macros are unsaved\n");
    child.publishChanges();
    CHECK(width() == "4");
    CHECK(definitions.doc->getUnitTree() == snapshot);
    definitions.save();
    CHECK(definitions.doc->getUnitTree() != snapshot);
    CHECK(width() == "8");
    CHECK(errorCount(*server, repo) == 1);

    const std::string_view names[]{"top"};
    auto retainedAnalysis = server->m_driver->comp->createActiveDesignContext(names);
    auto* instance = server->m_driver->comp->getActiveInstanceSymbol("top");
    REQUIRE(instance);
    snapshot = definitions.doc->getUnitTree();
    definitions.save();
    CHECK(server->m_driver->comp->getActiveInstanceSymbol("top") == instance);
    CHECK(definitions.doc->getUnitTree() == snapshot);
}

TEST_CASE("SingleUnit_WatchedSourceAndHeaderRefreshInheritedMacros") {
    const bool header = GENERATE(false, true);
    CAPTURE(header);
    SingleUnitRepo repo(header ? "watched_header" : "watched_source");
    if (header) {
        repo.write("defs.svh", "`define W 4\n`define V 2\n");
        repo.write("defs.sv", "`include \"defs.svh\"\n");
    }
    auto server = startServer(repo);
    auto child = server->openFile("child.sv");
    auto snapshot = child.doc->getUnitTree();
    const auto path = header ? "defs.svh" : "defs.sv";
    repo.write(path, "`define W 8\n`define V 2\n");
    server->onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = repo.uri(path), .type = lsp::FileChangeType::Changed}}});
    CHECK(child.doc->getUnitTree() != snapshot);
    const auto macros = child.doc->getSyntaxTree()->getDefinedMacros();
    const auto width = std::ranges::find_if(macros, [](const auto* macro) {
        return macro->name.valueText() == "W";
    });
    REQUIRE(width != macros.end());
    CHECK((*width)->body.front().valueText() == "8");
    CHECK(errorCount(*server, repo) == 1);
}

TEST_CASE("SingleUnit_HierarchyLocationsUseTheEditedSourceWithoutRefreshingUnit") {
    SingleUnitRepo repo("hierarchy_locations");
    auto server = startServer(repo);
    auto child = server->openFile("child.sv");
    const auto before = server->m_driver->comp->getHierDocParams("top.u0.s");
    REQUIRE(before);
    REQUIRE(before->selection);
    REQUIRE(before->uri == repo.uri("child.sv"));
    const auto snapshot = child.doc->getUnitTree();
    child.insert(0, "// inserted line\n");
    child.publishChanges();
    const auto after = server->m_driver->comp->getHierDocParams("top.u0.s");
    REQUIRE(after);
    REQUIRE(after->selection);
    CHECK(after->uri == repo.uri("child.sv"));
    CHECK(after->selection->start.line == before->selection->start.line + 1);
    CHECK(child.doc->getUnitTree() == snapshot);
}
