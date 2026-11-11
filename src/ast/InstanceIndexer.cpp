//------------------------------------------------------------------------------
// InstanceIndexer.cpp
// Compact instance hierarchy with lazy expansion of concrete paths
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "ast/InstanceIndexer.h"

#include <algorithm>
#include <ranges>
#include <unordered_set>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/InstanceSymbols.h"

using namespace slang::ast;

namespace {

struct ImmediateInstances : ASTVisitor<ImmediateInstances, VisitFlags::Symbols> {
    std::vector<const InstanceSymbol*> instances;

    void handle(const InstanceSymbol& symbol) {
        if (!symbol.body.flags.has(InstanceFlags::Uninstantiated))
            instances.push_back(&symbol);
        // The enclosing indexer follows the canonical body separately.
    }
};

auto pathDepth(std::string_view path) {
    return std::ranges::count(path, '.');
}

bool prefer(const InstanceIndexer::Occurrence& candidate,
            const InstanceIndexer::Occurrence& current) {
    if (!current.symbol)
        return true;
    if (auto a = pathDepth(candidate.path), b = pathDepth(current.path); a != b)
        return a < b;
    auto sortLocation = [](const InstanceSymbol& symbol) {
        auto* syntax = symbol.getSyntax();
        return syntax ? syntax->sourceRange().start() : symbol.location;
    };
    auto a = sortLocation(*candidate.symbol), b = sortLocation(*current.symbol);
    if (a.valid() != b.valid())
        return a.valid();
    if (a.valid() && a != b)
        return a < b;
    return candidate.path < current.path;
}

} // namespace

void InstanceIndexer::clear() {
    m_paths.clear();
    m_modules.clear();
    m_postorder.clear();
    m_bodies.clear();
    m_nodes.clear();
}

InstanceIndexer::Node* InstanceIndexer::indexBody(const InstanceBodySymbol& body) {
    if (auto it = m_bodies.find(&body); it != m_bodies.end()) {
        // An erroneous recursive design has no finite hierarchy to expand.
        return it->second->building ? nullptr : it->second;
    }
    auto* node = m_nodes.emplace_back(std::make_unique<Node>()).get();
    m_bodies.emplace(&body, node);
    indexChildren(*node, body, body.getHierarchicalPath());
    return node;
}

void InstanceIndexer::indexChildren(Node& node, const Symbol& scope, std::string_view prefix) {
    ImmediateInstances visitor;
    scope.visit(visitor);
    for (auto* instance : visitor.instances) {
        auto& module = m_modules[std::string(instance->getDefinition().name)];
        if (!module.first)
            module.first = instance;
        auto path = instance->getHierarchicalPath();
        SLANG_ASSERT(path.starts_with(prefix));
        auto* canonical = instance->getCanonicalBody();
        auto* body = indexBody(canonical ? *canonical : instance->body);
        node.children.push_back({instance, path.substr(prefix.size()), body});
    }
    node.building = false;
    m_postorder.push_back(&node);
}

void InstanceIndexer::reset(const Symbol* root) {
    clear();
    auto* rootNode = m_nodes.emplace_back(std::make_unique<Node>()).get();
    indexChildren(*rootNode, *root, "");
    rootNode->count = 1;
    rootNode->preferredPrefix = "";

    // Propagate multiplicities through the DAG, not through every expanded occurrence.
    // The best prefix also yields the best occurrence of each child, using the same
    // depth / source location / path ordering as the original concrete instance index.
    for (auto* node : std::views::reverse(m_postorder)) {
        for (const auto& edge : node->children) {
            auto& module = m_modules[std::string(edge.symbol->getDefinition().name)];
            module.count += node->count;
            Occurrence occurrence{edge.symbol, *node->preferredPrefix + edge.suffix};
            if (prefer(occurrence, module.preferred))
                module.preferred = occurrence;
            if (auto* body = edge.body) {
                body->count += node->count;
                // Include the separator when comparing prefixes: e.g. a$.child precedes
                // a.child even though the bare string "a" sorts before "a$".
                if (!body->preferredPrefix ||
                    pathDepth(occurrence.path) < pathDepth(*body->preferredPrefix) ||
                    (pathDepth(occurrence.path) == pathDepth(*body->preferredPrefix) &&
                     occurrence.path + "." < *body->preferredPrefix + ".")) {
                    body->preferredPrefix = std::move(occurrence.path);
                }
            }
        }
    }
}

const InstanceIndexer::ModuleInfo* InstanceIndexer::findModule(std::string_view name) const {
    auto it = m_modules.find(name);
    return it == m_modules.end() ? nullptr : &it->second;
}

bool InstanceIndexer::containsPath(std::string_view name, std::string_view path) const {
    auto* module = findModule(name);
    if (!module)
        return false;
    // Unique modules are automatically selected; their refresh must stay constant-time.
    if (path == module->preferred.path)
        return true;

    auto search = [&](auto&& self, const Node& node, std::string_view remaining) -> bool {
        for (const auto& edge : node.children) {
            if (!remaining.starts_with(edge.suffix))
                continue;
            auto rest = remaining.substr(edge.suffix.size());
            if (rest.empty() && edge.symbol->getDefinition().name == name)
                return true;
            if (edge.body && self(self, *edge.body, rest))
                return true;
        }
        return false;
    };
    return search(search, *m_nodes.front(), path);
}

const std::vector<InstanceIndexer::Occurrence>& InstanceIndexer::getInstances(
    std::string_view name) const {
    static const std::vector<Occurrence> empty;
    auto* module = findModule(name);
    if (!module)
        return empty;
    if (auto it = m_paths.find(name); it != m_paths.end())
        return it->second;

    // Prune branches that cannot contain the requested module before expanding paths.
    std::unordered_set<const Node*> containsModule;
    for (auto* node : m_postorder) {
        for (const auto& edge : node->children) {
            if (edge.symbol->getDefinition().name == name || containsModule.contains(edge.body)) {
                containsModule.insert(node);
                break;
            }
        }
    }

    std::vector<Occurrence> result;
    result.reserve(module->count);
    auto expand = [&](auto&& self, const Node& node, const std::string& prefix) -> void {
        for (const auto& edge : node.children) {
            bool matches = edge.symbol->getDefinition().name == name;
            bool descend = containsModule.contains(edge.body);
            if (!matches && !descend)
                continue;
            auto path = prefix + edge.suffix;
            if (matches)
                result.push_back({edge.symbol, path});
            if (descend)
                self(self, *edge.body, path);
        }
    };
    expand(expand, *m_nodes.front(), "");
    return m_paths.emplace(std::string(name), std::move(result)).first->second;
}
