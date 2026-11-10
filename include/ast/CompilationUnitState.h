// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

class DocumentHandle;

namespace server {

/// A build snapshot shared by the documents in a multi-file compilation unit.
/// Local analysis reads this snapshot; only build refreshes replace it.
class CompilationUnitState {
public:
    using Macros = std::vector<const slang::syntax::DefineDirectiveSyntax*>;

    CompilationUnitState(slang::SourceManager& sourceManager,
                         std::shared_ptr<slang::syntax::SyntaxTree> tree, slang::Bag options);

    const auto& sources() const { return m_sources; }
    const auto& tree() const { return m_tree; }
    std::shared_ptr<slang::syntax::SyntaxTree> refresh();
    const Macros& macrosBefore(const std::filesystem::path& path);

private:
    friend class ::DocumentHandle;
    void indexBuffers();
    size_t sourceIndex(slang::SourceLocation location) const;

    slang::SourceManager& m_sourceManager;
    slang::Bag m_options;
    std::shared_ptr<slang::syntax::SyntaxTree> m_tree;
    std::vector<std::filesystem::path> m_sources;
    std::unordered_map<std::filesystem::path, size_t> m_sourceIndices;
    std::unordered_map<uint32_t, size_t> m_bufferIndices;
    std::unordered_map<size_t, Macros> m_macroContexts;
};

} // namespace server
