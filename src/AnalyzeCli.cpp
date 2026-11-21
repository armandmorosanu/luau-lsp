// Source: https://github.com/Roblox/luau/blob/master/CLI/Analyze.cpp; licensed under MIT License
#include "Analyze/AnalyzeCli.hpp"
#include "Analyze/CliConfigurationParser.hpp"
#include "Analyze/CliClient.hpp"
#include "Flags.hpp"

#include "LSP/Workspace.hpp"
#include "LSP/ClientConfiguration.hpp"
#include "Luau/TypeAttach.h"
#include "Luau/SolverStats.h"
#include "Luau/PrettyPrinter.h"
#include "LuauFileUtils.hpp"
#include "LSP/LuauExt.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <memory>
#include <unordered_set>
#include <vector>

LUAU_FASTFLAG(DebugLuauTimeTracing)
LUAU_FASTFLAG(LuauSolverV2)


static void report(ReportFormat format, const char* name, const Luau::Location& loc, const char* type, const char* message)
{
    switch (format)
    {
    case ReportFormat::Default:
        fprintf(stderr, "%s(%d,%d): %s: %s\n", name, loc.begin.line + 1, loc.begin.column + 1, type, message);
        break;

    case ReportFormat::Luacheck:
    {
        // Note: luacheck's end column is inclusive but our end column is exclusive
        // In addition, luacheck doesn't support multi-line messages, so if the error is multiline we'll fake end column as 100 and hope for the best
        unsigned int columnEnd = (loc.begin.line == loc.end.line) ? loc.end.column : 100;

        // Use stdout to match luacheck behavior
        fprintf(stdout, "%s:%d:%d-%d: (W0) %s: %s\n", name, loc.begin.line + 1, loc.begin.column + 1, columnEnd, type, message);
        break;
    }

    case ReportFormat::Gnu:
        // Note: GNU end column is inclusive but our end column is exclusive
        fprintf(stderr, "%s:%d.%d-%d.%d: %s: %s\n", name, loc.begin.line + 1, loc.begin.column + 1, loc.end.line + 1, loc.end.column, type, message);
        break;
    }
}

FilePathInformation getFilePath(const WorkspaceFileResolver* fileResolver, const Luau::ModuleName& moduleName)
{
    auto path = fileResolver->platform->resolveToRealPath(moduleName);
    LUAU_ASSERT(path);

    // For consistency, we want to map the error.moduleName to a relative path (if it is a real path)
    Luau::ModuleName errorFriendlyName = moduleName;
    if (!fileResolver->platform->isVirtualPath(moduleName))
        errorFriendlyName = path->lexicallyRelative(fileResolver->rootUri);

    return {*path, fileResolver->getHumanReadableModuleName(errorFriendlyName)};
}

static bool reportError(WorkspaceFolder& workspace, ReportFormat format, const Luau::TypeError& error)
{
    auto [uri, relativePath] = getFilePath(&workspace.fileResolver, error.moduleName);

    if (workspace.isIgnoredFile(uri))
        return false;

    if (const auto* syntaxError = Luau::get_if<Luau::SyntaxError>(&error.data))
        report(format, relativePath.c_str(), error.location, "SyntaxError", syntaxError->message.c_str());
    else
        report(format, relativePath.c_str(), error.location, "TypeError",
            Luau::toString(error, Luau::TypeErrorToStringOptions{workspace.frontend.fileResolver}).c_str());

    return true;
}

static void reportWarning(ReportFormat format, const char* name, const Luau::LintWarning& warning)
{
    report(format, name, warning.location, Luau::LintWarning::getName(warning.code), warning.text.c_str());
}

struct TaskScheduler
{
    explicit TaskScheduler(unsigned threadCount)
        : threadCount(threadCount)
    {
        for (unsigned i = 0; i < threadCount; i++)
            workers.emplace_back([this] { while (std::function<void()> task = pop()) task(); });
    }
    ~TaskScheduler()
    {
        for (unsigned i = 0; i < threadCount; i++)
            push({});
        for (std::thread& worker : workers)
            worker.join();
    }
    std::function<void()> pop()
    {
        std::unique_lock guard(mtx);
        cv.wait(guard, [this] { return !tasks.empty(); });
        std::function<void()> task = tasks.front();
        tasks.pop();
        return task;
    }
    void push(std::function<void()> task)
    {
        {
            std::unique_lock guard(mtx);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }
    unsigned threadCount;
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
};

bool analyzeFile(
    WorkspaceFolder& workspace, const std::string& path, ReportFormat format, bool annotate, std::unordered_set<std::string>& reportedModules)
{
    Luau::ModuleName name = workspace.fileResolver.getModuleName(Uri::file(path));

    // Use checkStrict when annotating to retain type graphs needed by attachTypeData
    Luau::CheckResult cr = annotate ? workspace.checkStrict(name, /* cancellationToken= */ nullptr, /* forAutocomplete= */ false)
                                    : workspace.checkSimple(name, /* cancellationToken= */ nullptr);

    if (!workspace.frontend.getSourceModule(name))
    {
        std::cerr << "Error opening " << name << "\n";
        return false;
    }

    // The check result holds the errors of every module checked for this file. For a module that was already checked, it holds
    // the module's errors again, along with those of every module it requires. Only report the errors of modules no previous file reported.
    std::unordered_set<std::string> modulesInResult;
    unsigned int reportedErrors = 0;
    for (auto& error : cr.errors)
    {
        if (reportedModules.find(error.moduleName) != reportedModules.end())
            continue;

        modulesInResult.insert(error.moduleName);
        reportedErrors += reportError(workspace, format, error);
    }
    reportedModules.insert(modulesInResult.begin(), modulesInResult.end());

    // For the human readable module name, we use a relative version
    auto [_, relativePath] = getFilePath(&workspace.fileResolver, path);
    for (auto& error : cr.lintResult.errors)
        reportWarning(format, relativePath.c_str(), error);
    for (auto& warning : cr.lintResult.warnings)
        reportWarning(format, relativePath.c_str(), warning);

    if (annotate)
    {
        Luau::SourceModule* sm = workspace.frontend.getSourceModule(name);
        Luau::ModulePtr m = workspace.getModule(name);

        Luau::attachTypeData(*sm, *m);

        std::string annotated = Luau::prettyPrintWithTypes(*sm->root);

        printf("%s", annotated.c_str());
    }

    return reportedErrors == 0 && cr.lintResult.errors.empty();
}

std::vector<std::string> getFilesToAnalyze(const std::vector<std::string>& paths, WorkspaceFolder* workspace)
{
    auto cwd = Luau::FileUtils::getCurrentWorkingDirectory();
    LUAU_ASSERT(cwd);
    auto cwdUri = Uri::file(*cwd);

    std::vector<std::string> files;
    for (const auto& pathString : paths)
    {
        Uri uri = cwdUri.resolvePath(pathString);
        if (!uri.exists())
        {
            std::cerr << "Cannot get " << uri.fsPath() << ": path does not exist\n";
            exit(1);
        }

        if (uri.isDirectory())
        {
            Luau::FileUtils::traverseDirectoryRecursive(uri.fsPath(),
                [&](const auto& path)
                {
                    auto uri = Uri::file(path);
                    auto ext = uri.extension();
                    if (ext == ".lua" || ext == ".luau")
                    {
                        if (!workspace || !workspace->isIgnoredFile(uri))
                            files.push_back(uri.fsPath());
                    }
                });
        }
        else
        {
            files.push_back(uri.fsPath());
        }
    }
    return files;
}

void applySettings(const std::string& settingsContents, CliClient& client)
{
    client.globalConfig = dottedToClientConfiguration(settingsContents);

    // Merge definitions from settings into client.definitionsFiles
    for (const auto& pair : client.globalConfig.types.definitionFiles)
        client.definitionsFiles.insert(pair);

    // Process any fflags
    if (client.globalConfig.fflags.enableNewSolver)
        FFlag::LuauSolverV2.value = true;
    registerFastFlagsCLI(client.globalConfig.fflags.override);
    if (!client.globalConfig.fflags.enableByDefault)
        std::cerr << "warning: `luau-lsp.fflags.enableByDefault` is not respected in CLI Analyze mode. Please instead use the CLI option "
                     "`--no-flags-enabled` to configure this.\n";
    if (client.globalConfig.fflags.sync)
        std::cerr << "warning: `luau-lsp.fflags.sync` is not supported in CLI Analyze mode. Instead, all FFlags are enabled by default. "
                     "Please manually configure necessary FFlags\n";
}

std::unordered_map<std::string, std::string> processDefinitionsFilePaths(const argparse::ArgumentParser& program)
{
    std::unordered_map<std::string, std::string> definitionsFiles{};
    size_t backwardsCompatibilityNameSuffix = 0;
    for (const auto& definition : program.get<std::vector<std::string>>("--definitions"))
    {
        std::string packageName = definition;
        std::string filePath = definition;

        size_t eqIndex = definition.find('=');
        if (eqIndex == std::string::npos)
        {
            // TODO: Remove Me - backwards compatibility
            packageName = "@roblox";
            if (backwardsCompatibilityNameSuffix > 0)
                packageName += std::to_string(backwardsCompatibilityNameSuffix);
            backwardsCompatibilityNameSuffix += 1;
        }
        else
        {
            packageName = definition.substr(0, eqIndex);
            filePath = definition.substr(eqIndex + 1, definition.length());
        }

        if (!Luau::startsWith(packageName, "@"))
            packageName = "@" + packageName;

        definitionsFiles.emplace(packageName, filePath);
    }

    return definitionsFiles;
}

int startAnalyze(const argparse::ArgumentParser& program)
{
    ReportFormat format = ReportFormat::Default;
    bool annotate = program.is_used("--annotate");
    auto sourcemapPath = program.present<std::string>("--sourcemap");
    auto baseLuaurc = program.present<std::string>("--base-luaurc");
    auto settingsPath = program.present<std::string>("--settings");
    FFlag::DebugLuauTimeTracing.value = program.is_used("--timetrace");

    CliClient client;

    auto currentWorkingDirectory = Luau::FileUtils::getCurrentWorkingDirectory();
    if (!currentWorkingDirectory)
    {
        fprintf(stderr, "Failed to determine current working directory\n");
        return 1;
    }

    if (settingsPath)
    {
        if (std::optional<std::string> contents = Luau::FileUtils::readFile(*settingsPath))
        {
            applySettings(contents.value(), client);
        }
        else
        {
            fprintf(stderr, "Failed to read settings at '%s'\n", settingsPath->c_str());
            return 1;
        }
    }

    // Apply CLI args after settings so they take precedence
    auto cliIgnoreGlobs = program.get<std::vector<std::string>>("--ignore");
    client.globalConfig.ignoreGlobs.insert(client.globalConfig.ignoreGlobs.end(), cliIgnoreGlobs.begin(), cliIgnoreGlobs.end());
    for (const auto& [key, value] : processDefinitionsFilePaths(program))
        client.definitionsFiles.insert_or_assign(key, value);

    auto filesArg = program.present<std::vector<std::string>>("files");
    if (!filesArg || filesArg->empty())
    {
        fprintf(stderr, "error: no files provided\n");
        return 1;
    }

    auto reportFormatter = program.get<std::string>("--formatter");
    if (reportFormatter == "default")
        format = ReportFormat::Default;
    else if (reportFormatter == "plain")
        format = ReportFormat::Luacheck;
    else if (reportFormatter == "gnu")
        format = ReportFormat::Gnu;

#if !defined(LUAU_ENABLE_TIME_TRACE)
    if (FFlag::DebugLuauTimeTracing)
    {
        printf("To run with --timetrace, Luau has to be built with LUAU_ENABLE_TIME_TRACE enabled\n");
        return 1;
    }
#endif

    if (auto platformArg = program.present("--platform"))
    {
        if (platformArg == "standard")
            client.globalConfig.platform.type = LSPPlatformConfig::Standard;
        else if (platformArg == "roblox")
            client.globalConfig.platform.type = LSPPlatformConfig::Roblox;
    }

    if (client.globalConfig.platform.type == LSPPlatformConfig::Roblox && client.definitionsFiles.empty())
    {
        fprintf(stderr, "WARNING: --platform is set to 'roblox' but no definitions files are provided. 'luau-lsp analyze' does not download "
                        "definitions files; use `--platform=standard` to silence\n");
    }

    // Configure sourcemap via configuration (handled by RobloxPlatform::setupWithConfiguration)
    if (sourcemapPath)
    {
        if (client.globalConfig.platform.type == LSPPlatformConfig::Roblox)
        {
            client.globalConfig.sourcemap.sourcemapFile = *sourcemapPath;
            client.globalConfig.sourcemap.enabled = true;
        }
        else
        {
            std::cerr << "warning: a sourcemap was provided, but the current platform is not `roblox`. Use `--platform roblox` to ensure the "
                         "sourcemap option is respected.\n";
        }
    }
    else
    {
        client.globalConfig.sourcemap.enabled = false;
    }

    // Handle deprecated --no-strict-dm-types flag via configuration
    if (program.is_used("--no-strict-dm-types"))
        client.globalConfig.diagnostics.strictDatamodelTypes = false;

    client.globalConfig.index.enabled = false;

    std::optional<Luau::Config> defaultConfig;
    if (baseLuaurc)
    {
        Luau::Config result;
        if (std::optional<std::string> contents = Luau::FileUtils::readFile(*baseLuaurc))
        {
            std::optional<std::string> error = WorkspaceFileResolver::parseConfig(Uri::file(*baseLuaurc), *contents, result);
            if (error)
            {
                fprintf(stderr, "%s: %s\n", baseLuaurc->c_str(), error->c_str());
                return 1;
            }
            defaultConfig = std::move(result);
        }
        else
        {
            fprintf(stderr, "Failed to read base .luaurc configuration at '%s'\n", baseLuaurc->c_str());
            return 1;
        }
    }

    auto rootUri = Uri::file(*currentWorkingDirectory);
    WorkspaceFolder workspace(&client, "CLI", rootUri, defaultConfig);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    auto files = getFilesToAnalyze(*filesArg, &workspace);

    if (files.empty())
    {
        fprintf(stderr, "error: no files provided\n");
        return 1;
    }

    int failed = 0;
    std::unordered_set<std::string> reportedModules;
    const bool phaseLog = std::getenv("LUAU_SOLVER_STATS") != nullptr; // EXPERIMENT
    auto phase = [&](const char* what)
    {
        if (phaseLog)
            fprintf(stderr, "STAT phase %-12s at %8.3f s\n", what, Luau::TimeTrace::getClock());
    };
    phase("files");

    const char* jobsEnv = std::getenv("LUAU_LSP_JOBS");
    unsigned jobs = jobsEnv ? unsigned(std::strtoul(jobsEnv, nullptr, 10)) : 0;
    if (!annotate && jobs > 1)
    {
        for (const auto& path : files)
            workspace.frontend.queueModuleCheck(workspace.fileResolver.getModuleName(Uri::file(path)));
        Luau::FrontendOptions options{/* retainFullTypeGraphs: */ false, /* forAutocomplete: */ false, /* runLintChecks: */ true};
        TaskScheduler scheduler(jobs);
        phase("queued");
        workspace.frontend.checkQueuedModules(
            options,
            [&](std::vector<std::function<void()>> tasks)
            {
                for (auto& task : tasks)
                    scheduler.push(std::move(task));
            }
        );
        phase("checked");
    }

    for (const auto& path : files)
        failed += !analyzeFile(workspace, path, format, annotate, reportedModules);

    phase("reported");
    if (!client.diagnostics.empty())
    {
        failed += int(client.diagnostics.size());

        for (const auto& [path, err] : client.diagnostics)
            fprintf(stderr, "%s: %s\n", path.fsPath().c_str(), err.c_str());
    }

    if (std::getenv("LUAU_SOLVER_STATS")) // EXPERIMENT
    {
        namespace st = Luau::stats;
        static const char* timers[] = {"moduleCheck", "generate", "solve", "typeCheck2", "dispatch", "unblock",
            "generalize", "subtyping", "normalize", "inhabited", "simplify", "reduceTypeFunctions", "overload",
            "fc.pre", "fc.resolve", "fc.unify", "fc.inst2", "fc.queuer", "reduce.collect", "reduce.reducers"};
        for (int k = 0; k < int(std::size(timers)); k++)
            fprintf(stderr, "STAT time %-20s %8.3f s   in tc2 %8.3f s\n", timers[k],
                double(st::values[k].load()) / 1e9, double(st::values[st::InTC2Base + k].load()) / 1e9);
        for (int k = 0; k < 32; k++)
            if (uint64_t n = st::values[st::CKind0 + k].load())
                fprintf(stderr, "STAT kind %2d attempts %10llu ok %10llu time %8.3f s\n", k,
                    (unsigned long long)n, (unsigned long long)st::values[st::CKindOk0 + k].load(),
                    double(st::values[st::TDispatchKind0 + k].load()) / 1e9);
        static const char* counters[] = {"passes", "forcePasses", "checks", "skips", "attempts", "successes",
            "forcedAttempts", "forcedFails", "eraseShift", "pushes", "peakUnsolved"};
        static const char* reduceCounters[] = {"reduce.calls", "reduce.cacheHit", "reduce.empty", "reduce.steps", "q.nodes", "q.persistent", "q.foreign", "ic.nodes", "ic.persistent", "ic.foreign", "sub.top", "sub.cov", "sub.cacheHit", "sub.envHit", "sub.identity", "sub.persistentPair", "amg.nodes", "amg.persistent", "amg.foreign", "amg.fnStop"};
        for (int k = 0; k < int(std::size(reduceCounters)); k++)
            fprintf(stderr, "STAT count %-16s %14llu\n", reduceCounters[k],
                (unsigned long long)st::values[st::CReduceCalls + k].load());
        for (auto& [name, v] : st::visits)
            fprintf(stderr, "STAT visit %-28s inst %9llu nodes %11llu persistent %10llu frozen %11llu\n",
                name.c_str(), (unsigned long long)v.instances, (unsigned long long)v.nodes,
                (unsigned long long)v.persistent, (unsigned long long)v.frozen);
        for (auto& [name, n] : st::named)
            fprintf(stderr, "STAT tf %-24s calls %9llu time %8.3f s\n", name.c_str(),
                (unsigned long long)n.calls, double(n.ns) / 1e9);
        for (int k = 0; k < int(std::size(counters)); k++)
            fprintf(stderr, "STAT count %-16s %14llu\n", counters[k], (unsigned long long)st::values[st::CPasses + k].load());
    }
    if (format == ReportFormat::Luacheck)
        return 0;
    else
        return failed ? 1 : 0;
}
