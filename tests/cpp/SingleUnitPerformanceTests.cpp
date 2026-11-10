// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

#if defined(__unix__) || defined(__APPLE__)
#    include <sys/resource.h>
#endif

#include "slang/util/ScopeGuard.h"
#include "slang/util/TimeTrace.h"

namespace {

struct PerformanceRepo {
    fs::path dir;

    explicit PerformanceRepo(size_t fileCount) :
        dir(fs::weakly_canonical(fs::temp_directory_path() /
                                 ("slang_single_unit_performance_" + std::to_string(fileCount)))) {
        fs::remove_all(dir);
        fs::create_directories(dir / ".slang");
        std::ofstream(dir / ".slang/server.json") << R"({"flags":"--single-unit --top consumer"})";
        std::ofstream definitions(dir / "defs.sv");
        definitions << "`define WIDTH 8\n";
        for (size_t i = 0; i < fileCount; ++i)
            definitions << "`define UNUSED_" << i << ' ' << i << '\n';

        std::ofstream build(dir / "design.f");
        build << dir / "defs.sv" << '\n';
        for (size_t i = 0; i < fileCount - 3; ++i) {
            auto path = dir / ("filler_" + std::to_string(i) + ".sv");
            std::ofstream source(path);
            source << "module filler_" << i << ";\n";
            for (size_t signal = 0; signal < 32; ++signal)
                source << "logic [`WIDTH-1:0] value_" << signal << " = " << signal << ";\n";
            source << "endmodule\n";
            build << path << '\n';
        }
        std::ofstream(dir / "consumer.sv")
            << "module consumer(input logic [`WIDTH-1:0] a, output logic [`WIDTH-1:0] y);\n"
               "leaf u_leaf(.a(a), .y(y));\nendmodule\n";
        std::ofstream(dir / "leaf.sv")
            << "module leaf(input logic [`WIDTH-1:0] a, output logic [`WIDTH-1:0] y);\n"
               "assign y = a;\nendmodule\n";
        build << dir / "consumer.sv" << '\n' << dir / "leaf.sv" << '\n';
    }

    ~PerformanceRepo() { fs::remove_all(dir); }
};

class PerformanceServer final : public ServerHarness {
public:
    using ServerHarness::ServerHarness;
};

struct RealProjectConfig {
    std::string name;
    std::string revision;
    std::string workspace;
    std::string buildFile;
    std::vector<std::string> sourceFiles;
    std::vector<std::string> editFiles;
    int cycles;
    int editsPerCycle;
};

struct RealProjectCase {
    std::string file;
    double openMs;
    double editMedianMs;
    double editMaxMs;
    double saveMedianMs;
    double repeatedSaveMedianMs;
    double cycleMedianMs;
    size_t maxLocalErrors = 0;
    size_t editUnitSourceVisits = 0;
};

struct RealProjectResult {
    std::string name;
    std::string revision;
    size_t sourceFiles;
    size_t sourceBuffers;
    size_t instances;
    size_t diagnostics;
    double initializeMs;
    double buildMs;
    double peakRssMiB;
    std::vector<RealProjectCase> cases;
};

double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

double medianMs(std::vector<double> values) {
    std::ranges::sort(values);
    return values[values.size() / 2];
}

size_t instanceCount(ServerHarness& server) {
    size_t count = 0;
    for (const auto& [_, instances] : server.m_driver->comp->getInstances().moduleToInstances)
        count += instances.size();
    return count;
}

std::vector<URI> diagnosticFiles(server::SlangDoc& unit) {
    std::set<fs::path> paths;
    for (auto buffer : unit.getUnitTree()->getSourceBufferIds()) {
        const auto& path = unit.getSourceManager().getFullPath(buffer);
        if (!path.empty())
            paths.insert(path);
    }
    std::vector<URI> result;
    for (const auto& path : paths)
        result.push_back(URI::fromFile(path));
    return result;
}

std::vector<std::string> diagnosticFingerprint(ServerHarness& server,
                                               const std::vector<URI>& files) {
    std::vector<std::string> result;
    for (const auto& file : files) {
        for (const auto& diagnostic : server.client.getDiagnostics(file))
            result.push_back(std::string(file.getPath()) + rfl::json::write(diagnostic));
    }
    std::ranges::sort(result);
    return result;
}

size_t errorCount(ServerHarness& server, const std::vector<URI>& files) {
    size_t result = 0;
    for (const auto& file : files) {
        for (const auto& diagnostic : server.client.getDiagnostics(file))
            result += diagnostic.severity == lsp::DiagnosticSeverity::Error;
    }
    return result;
}

double peakRssMiB() {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#    if defined(__APPLE__)
    return double(usage.ru_maxrss) / (1024 * 1024);
#    else
    return double(usage.ru_maxrss) / 1024;
#    endif
#else
    return 0;
#endif
}

} // namespace

TEST_CASE("SingleUnit edit work stays local as the build grows", "[single-unit][performance]") {
    const size_t fileCount = GENERATE(size_t(16), size_t(128));
    const std::string editedFile = GENERATE("leaf.sv", "consumer.sv");
    CAPTURE(fileCount, editedFile);
    PerformanceRepo repo(fileCount);
    PerformanceServer server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(repo.dir), .name = "test"}}}});
    server.setBuildFile((repo.dir / "design.f").string());
    auto edited = server.openFile(editedFile);
    DocumentHandle unit(server, URI::fromFile(repo.dir / "leaf.sv"), "");
    REQUIRE(unit.doc);
    auto snapshot = unit.doc->getUnitTree();
    REQUIRE(snapshot);

    size_t unitSources = 0;
    auto observeUnit = [&](slang::BufferID, bool isBack, bool isSkip) {
        if (!isBack && !isSkip)
            ++unitSources;
    };
    unit.setBufferChangeCallback(observeUnit);
    auto reset = slang::ScopeGuard([&] { unit.setBufferChangeCallback({}); });

    for (int edit = 0; edit < 4; ++edit) {
        CAPTURE(edit);
        edited.append("// ordinary edit\n");
        edited.publishChanges();
        CHECK(unitSources == 0);
        CHECK(unit.doc->getUnitTree() == snapshot);
        CHECK(edited.doc->getSyntaxTree()->getSourceBufferIds().size() == 1);
        CHECK(edited.doc->getAnalysis()->getShallowCompilation()->hasValidBuffers());
    }
    edited.save();
    CHECK(unitSources == fileCount);
    CHECK(unit.doc->getUnitTree() != snapshot);
}

// Explicitly requested benchmark: no timing threshold is used in the normal test suite.
TEST_CASE("SingleUnit edit latency benchmark", "[.][single-unit-benchmark]") {
    const size_t fileCount = GENERATE(size_t(128), size_t(1500));
    const std::string editedFile = GENERATE("leaf.sv", "consumer.sv");
    CAPTURE(fileCount, editedFile);
    PerformanceRepo repo(fileCount);
    PerformanceServer server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(repo.dir), .name = "test"}}}});
    server.setBuildFile((repo.dir / "design.f").string());
    REQUIRE(server.m_driver->comp);
    auto edited = server.openFile(editedFile);
    DocumentHandle unit(server, URI::fromFile(repo.dir / "leaf.sv"), "");
    REQUIRE(unit.doc);

    size_t unitSources = 0;
    auto observe = [&](slang::BufferID, bool isBack, bool isSkip) {
        if (!isBack && !isSkip)
            ++unitSources;
    };
    unit.setBufferChangeCallback(observe);
    auto reset = slang::ScopeGuard([&] { unit.setBufferChangeCallback({}); });
    std::vector<double> milliseconds;
    for (int i = 0; i < 8; ++i) {
        const auto start = std::chrono::steady_clock::now();
        edited.append("// ordinary edit\n");
        edited.publishChanges();
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
    }
    std::ranges::sort(milliseconds);
    std::cout << "single-unit-benchmark files=" << fileCount << " edited=" << editedFile
              << " edits=" << milliseconds.size() << " unit_source_visits=" << unitSources
              << " median_ms=" << milliseconds[milliseconds.size() / 2]
              << " max_ms=" << milliseconds.back() << '\n';
}

TEST_CASE("SingleUnit save latency benchmark", "[.][single-unit-save-benchmark]") {
    const size_t fileCount = GENERATE(size_t(128), size_t(1500));
    const std::string editedFile = GENERATE("leaf.sv", "consumer.sv");
    CAPTURE(fileCount, editedFile);
    PerformanceRepo repo(fileCount);
    PerformanceServer server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(repo.dir), .name = "test"}}}});
    server.setBuildFile((repo.dir / "design.f").string());
    REQUIRE(server.m_driver->comp);
    auto edited = server.openFile(editedFile);
    DocumentHandle unit(server, URI::fromFile(repo.dir / "leaf.sv"), "");
    REQUIRE(unit.doc);

    size_t unitSources = 0;
    auto observe = [&](slang::BufferID, bool isBack, bool isSkip) {
        if (!isBack && !isSkip)
            ++unitSources;
    };
    unit.setBufferChangeCallback(observe);
    auto reset = slang::ScopeGuard([&] { unit.setBufferChangeCallback({}); });

    using Clock = std::chrono::steady_clock;
    auto elapsed = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    std::vector<double> saves, repeatedSaves, cycles, recordedParseTimes;
    for (int cycle = 0; cycle < 5; ++cycle) {
        const auto cycleStart = Clock::now();
        for (int edit = 0; edit < 8; ++edit) {
            edited.append("// ordinary edit before save\n");
            edited.publishChanges();
        }
        unitSources = 0;
        {
            slang::TimeTrace::initialize();
            auto stopTrace = slang::ScopeGuard([] { slang::TimeTrace::destroy(); });
            const auto start = Clock::now();
            edited.save();
            saves.push_back(elapsed(start));
            recordedParseTimes.push_back(double(slang::TimeTrace::getDurationForKey("parseFile")) /
                                         1000.0);
        }
        cycles.push_back(elapsed(cycleStart));
        CHECK(unitSources == fileCount);

        unitSources = 0;
        const auto start = Clock::now();
        edited.save();
        repeatedSaves.push_back(elapsed(start));
        CHECK(unitSources == 0);
    }
    auto median = [](std::vector<double> values) {
        std::ranges::sort(values);
        return values[values.size() / 2];
    };
    std::cout << "single-unit-save-benchmark files=" << fileCount << " edited=" << editedFile
              << " cycles=5 edits_per_cycle=8 save_median_ms=" << median(saves)
              << " save_max_ms=" << *std::ranges::max_element(saves)
              << " recorded_parse_median_ms=" << median(recordedParseTimes)
              << " repeated_save_median_ms=" << median(repeatedSaves)
              << " edit_save_cycle_median_ms=" << median(cycles) << '\n';
}

TEST_CASE("SingleUnit real project benchmark", "[.][single-unit-real-project]") {
    const auto* manifestPath = std::getenv("SLANG_REAL_PROJECT_BENCHMARK");
    if (!manifestPath)
        SKIP("Set SLANG_REAL_PROJECT_BENCHMARK to an external benchmark manifest");
    std::ifstream input(manifestPath);
    REQUIRE(input);
    const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto parsed = rfl::json::read<RealProjectConfig>(json);
    REQUIRE(parsed);
    const auto config = parsed.value();
    REQUIRE(config.cycles > 0);
    REQUIRE(config.editsPerCycle > 0);
    REQUIRE(config.sourceFiles.size() >= 100);
    REQUIRE_FALSE(config.editFiles.empty());

    auto start = std::chrono::steady_clock::now();
    PerformanceServer server(lsp::InitializeParams{
        .capabilities = {.experimental = rfl::to_generic(lsp::ExperimentalClientCapabilities{
                             .inactiveRegions =
                                 lsp::InactiveRegionsClientCapabilities{.inactiveRegions = true}})},
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(config.workspace), .name = config.name}}}});
    const auto initializeMs = elapsedMs(start);
    start = std::chrono::steady_clock::now();
    server.setBuildFile(config.buildFile);
    const auto buildMs = elapsedMs(start);
    REQUIRE(server.m_driver->comp);
    auto unitDoc = server.getDoc(URI::fromFile(config.sourceFiles.back()));
    REQUIRE(unitDoc);
    REQUIRE(unitDoc->getUnitSources().size() == config.sourceFiles.size());
    for (const auto& path : config.sourceFiles)
        REQUIRE(server.getDoc(URI::fromFile(path)));
    const auto files = diagnosticFiles(*unitDoc);
    REQUIRE(errorCount(server, files) == 0);
    const auto baselineDiagnostics = diagnosticFingerprint(server, files);
    const auto instances = instanceCount(server);
    REQUIRE(instances >= 1000);

    RealProjectResult result{.name = config.name,
                             .revision = config.revision,
                             .sourceFiles = config.sourceFiles.size(),
                             .sourceBuffers = unitDoc->getUnitTree()->getSourceBufferIds().size(),
                             .instances = instances,
                             .diagnostics = baselineDiagnostics.size(),
                             .initializeMs = initializeMs,
                             .buildMs = buildMs};
    for (const auto& path : config.editFiles) {
        INFO("edited source: " << path);
        start = std::chrono::steady_clock::now();
        auto edited = server.openFile(path);
        RealProjectCase timing{.file = path, .openMs = elapsedMs(start)};
        const auto original = edited.getText();
        DocumentHandle unit(server, unitDoc->getURI(), "");
        size_t visits = 0;
        auto observe = [&](slang::BufferID, bool isBack, bool isSkip) {
            if (!isBack && !isSkip)
                ++visits;
        };
        unit.setBufferChangeCallback(observe);
        auto reset = slang::ScopeGuard([&] { unit.setBufferChangeCallback({}); });
        std::vector<double> editTimes, saveTimes, repeatTimes, cycleTimes;
        for (int cycle = 0; cycle < config.cycles; ++cycle) {
            CAPTURE(cycle);
            const auto snapshot = unitDoc->getUnitTree();
            const auto cycleStart = std::chrono::steady_clock::now();
            visits = 0;
            for (int edit = 0; edit < config.editsPerCycle; ++edit) {
                start = std::chrono::steady_clock::now();
                edited.append("// slang-server real project benchmark\n");
                edited.publishChanges();
                editTimes.push_back(elapsedMs(start));
                for (const auto& diagnostic : edited.getDiagnostics())
                    CHECK(diagnostic.message.find("unknown macro") == std::string::npos);
                timing.maxLocalErrors = std::max(timing.maxLocalErrors,
                                                 errorCount(server, {edited.m_uri}));
            }
            timing.editUnitSourceVisits += visits;
            CHECK(visits == 0);
            CHECK(unitDoc->getUnitTree() == snapshot);
            start = std::chrono::steady_clock::now();
            edited.save();
            saveTimes.push_back(elapsedMs(start));
            cycleTimes.push_back(elapsedMs(cycleStart));
            CHECK(visits == unitDoc->getUnitTree()->getSourceBufferIds().size());
            CHECK(unitDoc->getUnitTree() != snapshot);
            CHECK(instanceCount(server) == instances);
            CHECK(std::ranges::equal(diagnosticFingerprint(server, files), baselineDiagnostics));
            const auto savedSnapshot = unitDoc->getUnitTree();
            visits = 0;
            start = std::chrono::steady_clock::now();
            edited.save();
            repeatTimes.push_back(elapsedMs(start));
            CHECK(visits == 0);
            CHECK(unitDoc->getUnitTree() == savedSnapshot);
        }
        // Verify both preprocessing and semantic error detection, followed by exact recovery.
        auto semanticError = original;
        const auto endModule = semanticError.rfind("endmodule");
        REQUIRE(endModule != std::string::npos);
        semanticError.insert(endModule,
                             "initial $display(\"%0d\", __slang_benchmark_missing_symbol);\n");
        const std::string invalidSources[]{original + "\n`__SLANG_BENCHMARK_MISSING_MACRO\n",
                                           semanticError};
        for (const auto& invalid : invalidSources) {
            edited.replaceAll(invalid);
            edited.publishChanges();
            CHECK(errorCount(server, {edited.m_uri}) > 0);
            edited.save();
            CHECK(errorCount(server, files) > 0);
            edited.replaceAll(original);
            edited.publishChanges();
            edited.save();
            CHECK(errorCount(server, files) == 0);
            CHECK(std::ranges::equal(diagnosticFingerprint(server, files), baselineDiagnostics));
            CHECK(instanceCount(server) == instances);
        }
        edited.close();
        timing.editMedianMs = medianMs(editTimes);
        timing.editMaxMs = *std::ranges::max_element(editTimes);
        timing.saveMedianMs = medianMs(saveTimes);
        timing.repeatedSaveMedianMs = medianMs(repeatTimes);
        timing.cycleMedianMs = medianMs(cycleTimes);
        result.cases.push_back(timing);
        std::cout << "real-project-case " << rfl::json::write(timing) << '\n';
    }
    result.peakRssMiB = peakRssMiB();
    const auto output = fs::path(manifestPath).parent_path() / "results.json";
    std::ofstream(output) << rfl::json::write(result, YYJSON_WRITE_PRETTY_TWO_SPACES) << '\n';
    std::cout << "real-project-result " << rfl::json::write(result) << '\n';
}
