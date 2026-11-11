//------------------------------------------------------------------------------
// InstanceIndexer.h
// Compact instance hierarchy with lazy expansion of concrete paths
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slang::ast {
class Symbol;
class InstanceSymbol;
class InstanceBodySymbol;
} // namespace slang::ast

class InstanceIndexer {
public:
    struct Occurrence {
        // Supplies declaration / instantiation locations only. Its parent scope may belong
        // to a different occurrence of the shared body; resolve path for semantic queries.
        const slang::ast::InstanceSymbol* symbol = nullptr;
        std::string path;
    };

    struct ModuleInfo {
        size_t count = 0;
        const slang::ast::InstanceSymbol* first = nullptr;
        Occurrence preferred;
    };

    /// Call after slang's semantic diagnostics pass has populated canonical bodies.
    void reset(const slang::ast::Symbol* root);
    void clear();
    bool empty() const { return m_modules.empty(); }

    const auto& getModules() const { return m_modules; }
    const ModuleInfo* findModule(std::string_view name) const;

    /// Validate a saved selection without resolving or elaborating its concrete AST.
    bool containsPath(std::string_view name, std::string_view path) const;

    /// Enumerate all concrete paths in the original depth-first order, without elaborating
    /// duplicate instance bodies. Only requested modules' path lists are cached.
    const std::vector<Occurrence>& getInstances(std::string_view name) const;

private:
    struct Node;
    struct Edge {
        const slang::ast::InstanceSymbol* symbol;
        std::string suffix;
        Node* body;
    };
    struct Node {
        std::vector<Edge> children;
        size_t count = 0;
        std::optional<std::string> preferredPrefix;
        bool building = true;
    };

    Node* indexBody(const slang::ast::InstanceBodySymbol& body);
    void indexChildren(Node& node, const slang::ast::Symbol& scope, std::string_view prefix);

    std::vector<std::unique_ptr<Node>> m_nodes;
    std::unordered_map<const slang::ast::InstanceBodySymbol*, Node*> m_bodies;
    std::vector<Node*> m_postorder;
    std::map<std::string, ModuleInfo, std::less<>> m_modules;
    mutable std::map<std::string, std::vector<Occurrence>, std::less<>> m_paths;
};
