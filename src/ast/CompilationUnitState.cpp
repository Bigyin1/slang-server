// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "ast/CompilationUnitState.h"

#include "util/Logging.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <unordered_set>

#include "slang/syntax/AllSyntax.h"

namespace server {
using namespace slang;

CompilationUnitState::CompilationUnitState(SourceManager& sourceManager,
                                           std::shared_ptr<syntax::SyntaxTree> tree, Bag options) :
    m_sourceManager(sourceManager), m_options(std::move(options)), m_tree(std::move(tree)) {
    std::unordered_set<uint32_t> included;
    for (const auto& include : m_tree->getIncludeDirectives()) {
        if (include.buffer.id.valid())
            included.insert(include.buffer.id.getId());
    }
    for (auto id : m_tree->getSourceBufferIds()) {
        const auto& path = m_sourceManager.getFullPath(id);
        if (!included.contains(id.getId()) && !path.empty())
            m_sources.push_back(path);
    }
    // Slang pushes the top-level buffers onto its lexer stack in reverse file order.
    std::ranges::reverse(m_sources);
    for (size_t i = 0; i < m_sources.size(); ++i)
        m_sourceIndices.emplace(m_sources[i], i);
    indexBuffers();
}

void CompilationUnitState::indexBuffers() {
    m_bufferIndices.clear();
    for (auto id : m_tree->getSourceBufferIds()) {
        if (auto it = m_sourceIndices.find(m_sourceManager.getFullPath(id));
            it != m_sourceIndices.end())
            m_bufferIndices.emplace(id.getId(), it->second);
    }
}

size_t CompilationUnitState::sourceIndex(SourceLocation location) const {
    if (location == SourceLocation::NoLocation || !location.valid())
        return std::string_view::npos;
    auto buffer = m_sourceManager.getFullyExpandedLoc(location).buffer();
    for (auto from = m_sourceManager.getIncludedFrom(buffer); from.valid();
         from = m_sourceManager.getIncludedFrom(buffer))
        buffer = from.buffer();
    const auto it = m_bufferIndices.find(buffer.getId());
    return it == m_bufferIndices.end() ? std::string_view::npos : it->second;
}

std::shared_ptr<syntax::SyntaxTree> CompilationUnitState::refresh() {
    if (hasValidBuffers(m_sourceManager, m_tree))
        return m_tree;
    std::vector<SourceBuffer> buffers;
    buffers.reserve(m_sources.size());
    for (const auto& path : m_sources) {
        auto buffer = m_sourceManager.readSource(path, m_tree->getSourceLibrary());
        if (!buffer) {
            ERROR("Failed to re-read {} for its single-unit tree: {}", path.string(),
                  buffer.error().message());
            return m_tree;
        }
        buffers.push_back(*buffer);
    }
    auto tree = syntax::SyntaxTree::fromBuffers(buffers, m_sourceManager, m_options);
    tree->isLibraryUnit = m_tree->isLibraryUnit;
    m_tree = std::move(tree);
    m_macroContexts.clear();
    indexBuffers();
    return m_tree;
}

const CompilationUnitState::Macros& CompilationUnitState::macrosBefore(
    const std::filesystem::path& path) {
    const auto start = m_sourceIndices.at(path);
    if (auto it = m_macroContexts.find(start); it != m_macroContexts.end())
        return it->second;
    Macros inherited;
    if (start != 0) {
        std::unordered_map<std::string_view, const syntax::DefineDirectiveSyntax*> active;
        for (auto* macro : m_tree->getDefinedMacros())
            active[macro->name.valueText()] = macro;
        auto refs = m_tree->getMacroRefs();
        for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
            if (it->syntax->kind == syntax::SyntaxKind::MacroUsage)
                continue;
            const auto at = sourceIndex(it->syntax->getFirstToken().location());
            if (at != std::string_view::npos && at >= start)
                active[it->definition->name.valueText()] = it->definition;
        }
        for (const auto& [_, macro] : active) {
            if (sourceIndex(macro->name.location()) < start)
                inherited.push_back(macro);
        }
    }
    return m_macroContexts.emplace(start, std::move(inherited)).first->second;
}

} // namespace server
