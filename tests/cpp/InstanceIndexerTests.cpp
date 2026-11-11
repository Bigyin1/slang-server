// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "ast/InstanceIndexer.h"
#include "utils/ServerHarness.h"
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/util/OS.h"
#include "slang/util/ScopeGuard.h"

namespace {

// Reference implementation used before #586. Deliberately walks concrete bodies so the
// comparison verifies paths, ordering, locations, and multiplicities independently of the DAG.
struct ConcreteInstances : slang::ast::ASTVisitor<ConcreteInstances> {
    std::map<std::string, std::vector<const slang::ast::InstanceSymbol*>> modules;
    bool deduplicate = false;
    std::unordered_set<const slang::ast::InstanceBodySymbol*> visited;
    void handle(const slang::ast::InstanceSymbol& symbol) {
        if (symbol.body.flags.has(slang::ast::InstanceFlags::Uninstantiated))
            return;
        modules[std::string(symbol.getDefinition().name)].push_back(&symbol);
        if (deduplicate) {
            // The literal proposal from #586, measured separately from the complete index.
            auto* body = symbol.getCanonicalBody() ? symbol.getCanonicalBody() : &symbol.body;
            if (visited.insert(body).second)
                visitDefault(*body);
        }
        else {
            visitDefault(symbol.body);
        }
    }
};

std::string replicatedDesign(int levels, int width) {
    std::ostringstream text;
    text << "module level0;\n";
    for (int i = 0; i < 32; ++i)
        text << "logic [31:0] signal" << i << ";\n";
    text << "endmodule\n";
    for (int level = 1; level <= levels; ++level) {
        text << "module level" << level << ";\n"
             << "for (genvar i=0; i<" << width << "; ++i) begin: g\n"
             << "level" << level - 1 << " child();\nend\nendmodule\n";
    }
    return text.str();
}

} // namespace

TEST_CASE("Canonical instance index preserves concrete paths and locations", "[instance-index]") {
    auto source = GENERATE(
        R"(
module leaf #(parameter W=8); logic [W-1:0] value; endmodule
module branch #(parameter W=8);
    leaf #(W) direct();
    for (genvar i=2; i>=0; --i) begin: g
        leaf #(W+i) children[2:1]();
    end
    if (W==8) begin: extra
        leaf special();
    end
endmodule
module wrapper; branch nested(); endmodule
module top;
    wrapper deep();
    branch z(), a(), a$(), \with.dot ();
    branch #(16) wide();
endmodule
)",
        R"(
interface bus; logic value; endinterface
module leaf(bus b); endmodule
module branch(bus b); leaf child(b); endmodule
module top;
    bus b1(), b2();
    branch first(b1), second(b1), third(b2);
endmodule
)",
        R"(
module leaf #(parameter W=8); logic [W-1:0] value; endmodule
module monitor; endmodule
module branch; leaf child(); endmodule
module top;
    branch first(), second(), third();
    defparam second.child.W=16;
endmodule
bind top.third monitor checker_inst();
)",
        R"(
module left;
    module leaf; logic a; endmodule
    leaf child();
endmodule
module right;
    module leaf; logic b; endmodule
    leaf child();
endmodule
module wrapper; left nested(); endmodule
module top; wrapper first(); right second(); endmodule
)");
    slang::ast::Compilation compilation;
    compilation.addSyntaxTree(slang::syntax::SyntaxTree::fromText(source));
    REQUIRE(compilation.getSemanticDiagnostics().empty());
    auto bytes = compilation.getTotalBytesAllocated();
    InstanceIndexer index;
    index.reset(&compilation.getRoot());
    for (const auto& [name, info] : index.getModules()) {
        CHECK(index.getInstances(name).size() == info.count);
    }
    // Even enumerating all paths must not instantiate duplicate AST bodies.
    CHECK(compilation.getTotalBytesAllocated() == bytes);

    ConcreteInstances concrete;
    compilation.getRoot().visit(concrete);
    REQUIRE(index.getModules().size() == concrete.modules.size());
    for (const auto& [name, expected] : concrete.modules) {
        CAPTURE(name);
        const auto& actual = index.getInstances(name);
        REQUIRE(actual.size() == expected.size());
        auto* info = index.findModule(name);
        REQUIRE(info);
        CHECK(info->count == expected.size());
        CHECK(&info->first->getDefinition() == &expected.front()->getDefinition());
        const slang::ast::InstanceSymbol* preferred = nullptr;
        auto rank = [](const slang::ast::InstanceSymbol& instance) {
            auto path = instance.getHierarchicalPath();
            auto location = instance.getSyntax() ? instance.getSyntax()->sourceRange().start()
                                                 : instance.location;
            return std::tuple{std::ranges::count(path, '.'), !location.valid(), location, path};
        };
        for (size_t i = 0; i < expected.size(); ++i) {
            CHECK(actual[i].path == expected[i]->getHierarchicalPath());
            CHECK(index.containsPath(name, actual[i].path));
            CHECK_FALSE(index.containsPath(name, actual[i].path + ".missing"));
            CHECK(actual[i].symbol->location == expected[i]->location);
            if (!preferred || rank(*expected[i]) < rank(*preferred))
                preferred = expected[i];
        }
        REQUIRE(preferred);
        CHECK(info->preferred.path == preferred->getHierarchicalPath());
    }
    CHECK(index.getInstances("missing").empty());
    CHECK_FALSE(index.containsPath("missing", "top"));
    CHECK_FALSE(index.containsPath("top", ""));
    CHECK_FALSE(index.containsPath("top", "top_missing"));
    index.reset(&compilation.getRoot());
    for (const auto& [name, expected] : concrete.modules)
        CHECK(index.getInstances(name).size() == expected.size());
    index.clear();
    CHECK(index.empty());
    CHECK(index.getInstances("top").empty());
    CHECK_FALSE(index.containsPath("top", "top"));
}

TEST_CASE("Canonical instance index avoids elaborating repeated bodies", "[instance-index]") {
    slang::ast::Compilation compilation;
    compilation.addSyntaxTree(slang::syntax::SyntaxTree::fromText(replicatedDesign(3, 8)));
    REQUIRE(compilation.getSemanticDiagnostics().empty());
    auto bytes = compilation.getTotalBytesAllocated();
    InstanceIndexer index;
    index.reset(&compilation.getRoot());
    REQUIRE(index.findModule("level0"));
    CHECK(index.findModule("level0")->count == 512);
    CHECK(index.getInstances("level0").size() == 512);
    CHECK(compilation.getTotalBytesAllocated() == bytes);
    ConcreteInstances concrete;
    compilation.getRoot().visit(concrete);
    CHECK(concrete.modules["level0"].size() == 512);
    CHECK(compilation.getTotalBytesAllocated() > bytes * 2);
}

TEST_CASE("Canonical hierarchy keeps selection and CodeLens paths after refresh",
          "[instance-index]") {
    ServerHarness server;
    auto doc = server.openFile("canonical_hierarchy.sv", R"(
module leaf #(parameter W=8); logic [W-1:0] value; endmodule
module branch;
    for (genvar i=0; i<2; ++i) begin: g
        leaf #(8+i) child();
    end
endmodule
module top; branch first(), second(); endmodule
)");
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    auto& compilation = *server.m_driver->comp;
    auto instances = server.getInstancesOfModule("leaf");
    REQUIRE(instances.size() == 4);
    CHECK(instances[3].instPath == "top.second.g[1].child");
    auto scopes = server.getScopesByModule({});
    auto leaf = std::ranges::find(scopes, "leaf", &hier::InstanceSet::declName);
    REQUIRE(leaf != scopes.end());
    CHECK(leaf->instCount == 4);
    REQUIRE(compilation.setActiveInstance(instances[3].instPath));
    auto checkSelection = [&] {
        auto* active = compilation.getActiveInstanceSymbol("leaf");
        REQUIRE(active);
        CHECK(active->getHierarchicalPath() == "top.second.g[1].child");
        CHECK(bool(active->body.find<slang::ast::ParameterSymbol>("W").getValue().integer() == 9));
        auto lenses = server.getDocCodeLens({.textDocument = {doc.m_uri}});
        REQUIRE(lenses);
        auto serialized = rfl::json::write(*lenses);
        for (const auto& instance : instances)
            CHECK(serialized.find(instance.instPath) != std::string::npos);
        CHECK(serialized.find("top.second.g[1].child (4)") != std::string::npos);
        auto bytes = active->body.getCompilation().getTotalBytesAllocated();
        for (int query = 0; query < 10; ++query)
            CHECK(compilation.getActiveInstanceSymbol("leaf") == active);
        CHECK(active->body.getCompilation().getTotalBytesAllocated() == bytes);
    };
    checkSelection();
    doc.append("\n// rebuild the full compilation\n");
    doc.save();
    checkSelection();
    CHECK(server.getInstancesOfModule("leaf").size() == 4);
}

TEST_CASE("Canonical hierarchy refresh preserves selections without parsing every path",
          "[instance-index]") {
    auto dir = fs::temp_directory_path() /
               ("slang_instance_refresh_" + std::to_string(slang::OS::getpid()));
    fs::create_directories(dir);
    auto cleanup = slang::ScopeGuard([&] { fs::remove_all(dir); });
    {
        std::ofstream source(dir / "top.sv");
        for (int i = 0; i < 128; ++i)
            source << "module child" << i << "; logic value; endmodule\n";
        source << "module top;\n";
        for (int i = 0; i < 128; ++i)
            source << "child" << i << " u" << i << "();\n";
        source << "endmodule\n";
    }
    std::ofstream(dir / "design.f") << dir / "top.sv" << '\n';
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {{lsp::WorkspaceFolder{URI::fromFile(dir), "instance_refresh"}}}});
    server.setBuildFile((dir / "design.f").string());
    auto& compilation = *server.m_driver->comp;
    auto allocated = [&] {
        const auto& modules = compilation.getInstances().getModules();
        REQUIRE(modules.size() == 129);
        return modules.begin()->second.first->body.getCompilation().getTotalBytesAllocated();
    };
    auto bytes = allocated();
    auto selections = rfl::json::write(compilation.getScopesByModule());
    compilation.refresh();
    CHECK(rfl::json::write(compilation.getScopesByModule()) == selections);
    // Resolving every auto-selected path through Compilation::parseName would allocate
    // another preprocessor's tokens for each module, slowing even unshared designs.
    CHECK(allocated() <= bytes);
}

TEST_CASE("Canonical instance index large hierarchy benchmark",
          "[.][performance][instance-index-benchmark]") {
    // Run each mode in a fresh process to compare peak RSS. The legacy mode reproduces
    // the original order: concrete indexing before semantic diagnostics.
    const auto* configuredMode = std::getenv("SLANG_INSTANCE_BENCHMARK_MODE");
    const std::string_view mode = configuredMode ? configuredMode : "canonical";
    REQUIRE((mode == "legacy" || mode == "dedupe" || mode == "canonical"));
    auto start = std::chrono::steady_clock::now();
    slang::ast::Compilation compilation;
    compilation.addSyntaxTree(slang::syntax::SyntaxTree::fromText(replicatedDesign(5, 10)));
    ConcreteInstances concrete;
    InstanceIndexer index;
    if (mode == "legacy")
        compilation.getRoot().visit(concrete);
    REQUIRE(compilation.getSemanticDiagnostics().empty());
    if (mode == "canonical")
        index.reset(&compilation.getRoot());
    else if (mode == "dedupe") {
        concrete.deduplicate = true;
        compilation.getRoot().visit(concrete);
    }
    size_t count = 0;
    if (mode != "canonical") {
        for (const auto& [_, instances] : concrete.modules)
            count += instances.size();
    }
    else {
        for (const auto& [_, module] : index.getModules())
            count += module.count;
    }
    CHECK(count == (mode == "dedupe" ? 51 : 111111));
    auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "instance-index mode=" << mode << " instances=" << count << " build_ms=" << elapsed
              << " ast_bytes=" << compilation.getTotalBytesAllocated() << '\n';
    if (mode == "canonical") {
        auto bytes = compilation.getTotalBytesAllocated();
        auto queryStart = std::chrono::steady_clock::now();
        auto& paths = index.getInstances("level0");
        CHECK(paths.size() == 100000);
        CHECK(paths.back().path == "level5.g[9].child.g[9].child.g[9].child.g[9].child.g[9].child");
        CHECK(compilation.getTotalBytesAllocated() == bytes);
        std::cout << "instance-index leaf_paths=" << paths.size() << " query_ms="
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               queryStart)
                         .count()
                  << '\n';
    }
}
