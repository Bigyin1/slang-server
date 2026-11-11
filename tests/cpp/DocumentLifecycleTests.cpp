// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>
#include <iostream>

#include "slang/ast/symbols/ParameterSymbols.h"

namespace {

class LifecycleServer final : public ServerHarness {
public:
    using ServerHarness::ServerHarness;
};

struct LifecycleRepo {
    fs::path dir;

    explicit LifecycleRepo(std::string_view name, bool singleUnit = false) :
        dir(fs::weakly_canonical(fs::temp_directory_path() /
                                 ("slang_document_lifecycle_" + std::string(name)))) {
        fs::create_directories(dir / ".slang");
        write(".slang/server.json", "{}");
        write("width.svh", "parameter int W = 8;\n");
        write("leaf.sv", "module leaf;\n`include \"width.svh\"\n"
                         "logic [W-1:0] value;\nendmodule\n");
        write("top.sv", "module top; leaf child(); endmodule\n");
        std::ofstream build(dir / "design.f");
        build << "+incdir+" << dir << "\n--top top\n";
        if (singleUnit)
            build << "--single-unit\n";
        build << dir / "leaf.sv" << '\n' << dir / "top.sv" << '\n';
    }

    ~LifecycleRepo() { fs::remove_all(dir); }

    void write(std::string_view name, std::string_view text) const {
        std::ofstream(dir / name) << text;
    }

    std::unique_ptr<LifecycleServer> start(bool build = true) const {
        auto server = std::make_unique<LifecycleServer>(lsp::InitializeParams{
            .workspaceFolders = {
                {lsp::WorkspaceFolder{.uri = URI::fromFile(dir), .name = "lifecycle"}}}});
        if (build)
            server->setBuildFile((dir / "design.f").string());
        return server;
    }
};

std::string lifecycleDiagnostics(ServerHarness& server, const LifecycleRepo& repo) {
    std::string result;
    for (auto name : {"top.sv", "leaf.sv", "width.svh"})
        result += rfl::json::write(server.client.getDiagnostics(URI::fromFile(repo.dir / name)));
    return result;
}

int parameterWidth(ServerHarness& server) {
    auto* instance = server.m_driver->comp->getActiveInstanceSymbol("leaf");
    REQUIRE(instance);
    auto value =
        instance->body.find<slang::ast::ParameterSymbol>("W").getValue().integer().as<int>();
    REQUIRE(value);
    return *value;
}

} // namespace

TEST_CASE("Unchanged saves retain compilation and build diagnostics", "[document-lifecycle]") {
    const auto singleUnit = GENERATE(false, true);
    LifecycleRepo repo("unchanged", singleUnit);
    auto server = repo.start();
    auto leaf = server->openFile("leaf.sv");
    auto& compilation = *server->m_driver->comp;
    auto snapshot = compilation.createActiveDesignContext({});
    auto baseline = lifecycleDiagnostics(*server, repo);
    auto local = server->getDoc(leaf.m_uri)->getAnalysis();
    for (int i = 0; i < 3; ++i) {
        leaf.save();
        CHECK(&compilation.createActiveDesignContext({}).getAnalysis() == &snapshot.getAnalysis());
        CHECK(server->getDoc(leaf.m_uri)->getAnalysis() == local);
        CHECK(lifecycleDiagnostics(*server, repo) == baseline);
    }

    leaf.after("logic [W-1:0] value;").write("\nassign value = missing_signal;\n");
    leaf.publishChanges();
    leaf.save();
    auto changed = compilation.createActiveDesignContext({});
    CHECK(&changed.getAnalysis() != &snapshot.getAnalysis());
    REQUIRE(std::ranges::any_of(leaf.getDiagnostics(), [](const auto& diag) {
        return diag.severity == lsp::DiagnosticSeverity::Error;
    }));
    auto changedDiagnostics = lifecycleDiagnostics(*server, repo);
    leaf.save();
    CHECK(&compilation.createActiveDesignContext({}).getAnalysis() == &changed.getAnalysis());
    CHECK(lifecycleDiagnostics(*server, repo) == changedDiagnostics);
}

TEST_CASE("Saving unchanged source refreshes edited include dependencies", "[document-lifecycle]") {
    const auto singleUnit = GENERATE(false, true);
    LifecycleRepo repo("dependencies", singleUnit);
    auto server = repo.start();
    auto top = server->openFile("top.sv");
    auto header = server->openFile("width.svh", "parameter int W = 8;\n");
    auto& compilation = *server->m_driver->comp;
    auto before = compilation.createActiveDesignContext({});
    REQUIRE(parameterWidth(*server) == 8);

    header.insert(header.getText().find('8'), "1");
    header.publishChanges();
    top.save();
    CHECK(&compilation.createActiveDesignContext({}).getAnalysis() != &before.getAnalysis());
    CHECK(parameterWidth(*server) == 18);

    auto changed = compilation.createActiveDesignContext({});
    top.save();
    CHECK(&compilation.createActiveDesignContext({}).getAnalysis() == &changed.getAnalysis());
    CHECK(parameterWidth(*server) == 18);
}

TEST_CASE("Watched include changes survive subsequent unchanged saves", "[document-lifecycle]") {
    const auto singleUnit = GENERATE(false, true);
    LifecycleRepo repo("watched", singleUnit);
    auto server = repo.start();
    auto top = server->openFile("top.sv");
    auto before = server->m_driver->comp->createActiveDesignContext({});
    REQUIRE(parameterWidth(*server) == 8);
    repo.write("width.svh", "parameter int W = 24;\n");
    server->m_driver->onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = URI::fromFile(repo.dir / "width.svh"),
                      .type = lsp::FileChangeType::Changed}}});
    // Ordinary builds can defer full semantics until save; the invalidated include
    // must still prevent the unchanged top source from skipping that refresh.
    top.save();
    CHECK(parameterWidth(*server) == 24);
    auto changed = server->m_driver->comp->createActiveDesignContext({});
    CHECK(&changed.getAnalysis() != &before.getAnalysis());
    top.save();
    CHECK(&server->m_driver->comp->createActiveDesignContext({}).getAnalysis() ==
          &changed.getAnalysis());
    CHECK(parameterWidth(*server) == 24);
}

TEST_CASE("Closing documents releases cached analyses and preserves held requests",
          "[document-lifecycle]") {
    const auto withBuild = GENERATE(false, true);
    LifecycleRepo repo("close");
    auto server = repo.start(withBuild);
    auto leaf = server->openFile("leaf.sv");
    auto top = server->openFile("top.sv");
    auto doc = server->getDoc(leaf.m_uri);
    auto held = doc->getAnalysis();
    auto topAnalysis = server->getDoc(top.m_uri)->getAnalysis();
    std::weak_ptr<server::ShallowAnalysis> weak = held;
    leaf.close();
    CHECK_FALSE(doc->hasAnalysis());
    CHECK_FALSE(weak.expired());
    CHECK_FALSE(held->getDocSymbols().empty());
    CHECK(server->getDoc(top.m_uri)->getAnalysis() == topAnalysis);
    held.reset();
    CHECK(weak.expired());

    auto reopened = server->openFile("leaf.sv");
    CHECK_FALSE(reopened.getSymbolTree().empty());
    if (withBuild)
        CHECK(parameterWidth(*server) == 8);
}

TEST_CASE("Closing includes releases shared compilations when no view needs them",
          "[document-lifecycle]") {
    const auto closeSourceFirst = GENERATE(false, true);
    LifecycleRepo repo("includes");
    auto server = repo.start();
    auto leaf = server->openFile("leaf.sv");
    auto header = server->openFile("width.svh", "parameter int W = 8;\n");
    auto source = server->getDoc(leaf.m_uri);
    auto included = server->getDoc(header.m_uri);
    std::weak_ptr<server::ShallowCompilation> weak = source->getAnalysis()->getShallowCompilation();
    REQUIRE(included->getAnalysis()->getShallowCompilation() == weak.lock());
    if (closeSourceFirst) {
        leaf.close();
        CHECK_FALSE(weak.expired());
        // An open header remains navigable after the source view has closed.
        CHECK_FALSE(included->getAnalysis()->getDocSymbols().empty());
        header.close();
    }
    else {
        header.close();
        CHECK_FALSE(weak.expired());
        CHECK(source->hasAnalysis());
        leaf.close();
    }
    CHECK(weak.expired());
    CHECK_FALSE(source->hasAnalysis());
    CHECK_FALSE(included->hasAnalysis());
    auto reopened = server->openFile("leaf.sv");
    CHECK_FALSE(reopened.getSymbolTree().empty());
    CHECK(parameterWidth(*server) == 8);
}

TEST_CASE("Closing many documents does not retain their shallow compilations",
          "[document-lifecycle]") {
    ServerHarness server;
    std::vector<std::weak_ptr<server::ShallowCompilation>> analyses;
    for (int i = 0; i < 30; ++i) {
        auto doc = server.openFile("closed_" + std::to_string(i) + ".sv",
                                   "module m" + std::to_string(i) +
                                       "; logic [31:0] value; endmodule\n");
        auto source = server.getDoc(doc.m_uri);
        analyses.push_back(source->getAnalysis()->getShallowCompilation());
        doc.close();
        CHECK_FALSE(source->hasAnalysis());
    }
    CHECK(std::ranges::all_of(analyses, [](const auto& weak) { return weak.expired(); }));
}

TEST_CASE("Ordinary document lifecycle performance", "[.][document-lifecycle-performance]") {
    const auto sourceCount = GENERATE(128, 1500);
    LifecycleRepo repo("performance_" + std::to_string(sourceCount));
    {
        std::ofstream build(repo.dir / "design.f");
        std::ofstream top(repo.dir / "top.sv");
        build << "--top top\n" << repo.dir / "top.sv" << '\n';
        top << "module top;\n";
        for (int i = 0; i < sourceCount - 1; ++i) {
            auto name = "leaf_" + std::to_string(i);
            repo.write(name + ".sv", "module " + name + "; logic [31:0] value; endmodule\n");
            build << repo.dir / (name + ".sv") << '\n';
            top << name << " u" << i << "();\n";
        }
        top << "endmodule\n";
    }
    auto server = repo.start();
    auto leaf = server->openFile("leaf_0.sv");
    const auto baseline = rfl::json::write(leaf.getDiagnostics());
    std::vector<double> saves, repeatedSaves;
    auto elapsed = [](auto start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    for (int cycle = 0; cycle < 5; ++cycle) {
        leaf.append("\n// edit\n");
        leaf.publishChanges();
        auto start = std::chrono::steady_clock::now();
        leaf.save();
        saves.push_back(elapsed(start));
        start = std::chrono::steady_clock::now();
        leaf.save();
        repeatedSaves.push_back(elapsed(start));
        CHECK(rfl::json::write(leaf.getDiagnostics()) == baseline);
    }
    leaf.close();

    std::vector<std::weak_ptr<server::ShallowCompilation>> analyses;
    size_t allocatedBytes = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 1; i <= 30; ++i) {
        auto doc = server->openFile("leaf_" + std::to_string(i) + ".sv");
        auto compilation = server->getDoc(doc.m_uri)->getAnalysis()->getShallowCompilation();
        allocatedBytes += compilation->getCompilation()->getTotalBytesAllocated();
        analyses.push_back(compilation);
        doc.close();
    }
    auto openCloseMs = elapsed(start);
    size_t retainedBytes = 0, retainedCount = 0;
    for (const auto& weak : analyses) {
        if (auto compilation = weak.lock()) {
            ++retainedCount;
            retainedBytes += compilation->getCompilation()->getTotalBytesAllocated();
        }
    }
    std::ranges::sort(saves);
    std::ranges::sort(repeatedSaves);
    std::cout << "document-lifecycle sources=" << sourceCount
              << " changed_save_ms=" << saves[saves.size() / 2]
              << " unchanged_save_ms=" << repeatedSaves[repeatedSaves.size() / 2]
              << " open_close_30_ms=" << openCloseMs << " closed_analyses=" << retainedCount
              << " retained_ast_bytes=" << retainedBytes << " visited_ast_bytes=" << allocatedBytes
              << '\n';
}
