//===- ClangScanDeps.cpp - Implementation of clang-scan-deps --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ClangScanDeps.hpp"

#include "clang/DependencyScanning/DependencyScanningService.h"
#include "clang/DependencyScanning/DependencyScanningWorker.h"
#include "clang/Driver/Compilation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/DependencyScanningTool.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/TargetParser/Host.h"
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Opts.inc"

using namespace clang;
using namespace tooling;
using namespace dependencies;

namespace {

    using namespace llvm::opt;
    enum ID {
        OPT_INVALID = 0, // This is not an option ID.
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
    };

#define OPTTABLE_STR_TABLE_CODE
#include "Opts.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Opts.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

    const llvm::opt::OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
    };

    class NixjaDependencyConsumer : public DependencyConsumer {
    public:
        std::vector<std::string> Dependencies;

        void handleFileDependency(StringRef File) override {
            Dependencies.emplace_back(File.str());
        }

        void handleDependencyOutputOpts(
            const DependencyOutputOptions&) override { }

        void handlePrebuiltModuleDependency(
            PrebuiltModuleDep) override { }

        void handleModuleDependency(ModuleDeps) override { }

        void handleDirectModuleDependency(ModuleID) override { }

        void handleVisibleModule(std::string) override { }

        void handleContextHash(std::string) override { }
    };

    class ScanDepsOptTable : public llvm::opt::GenericOptTable {
    public:
        ScanDepsOptTable() : GenericOptTable(OptionStrTable, OptionPrefixesTable, InfoTable) {
            setGroupedShortOptions(true);
        }
    };

    enum ResourceDirRecipeKind {
        RDRK_ModifyCompilerPath,
        RDRK_InvokeCompiler,
    };

    static std::string OutputFileName = "-";
    static ScanningMode ScanMode = ScanningMode::DependencyDirectivesScan;
    static ScanningOutputFormat Format = ScanningOutputFormat::Make;
    static ScanningOptimizations OptimizeArgs;
    static std::string ModuleFilesDir;
    static bool EagerLoadModules;
    static unsigned NumThreads = 0;
    static std::string CompilationDB;
    static std::optional<std::string> ModuleNames;
    static std::vector<std::string> ModuleDepTargets;
    static std::string TranslationUnitFile;
    static bool DeprecatedDriverCommand;
    static ResourceDirRecipeKind ResourceDirRecipe;
    static bool Verbose;
    static bool PrintTiming;
    static bool EmitVisibleModules;
    static llvm::BumpPtrAllocator Alloc;
    static llvm::StringSaver Saver { Alloc };
    static std::vector<const char*> CommandLine;

#ifndef NDEBUG
    static constexpr bool DoRoundTripDefault = true;
#else
    static constexpr bool DoRoundTripDefault = false;
#endif

    static bool RoundTripArgs = DoRoundTripDefault;
    static bool VerbatimArgs = false;

    static void ParseArgs(int argc, char** argv) {
        ScanDepsOptTable Tbl;
        llvm::StringRef ToolName = argv[0];
        llvm::opt::InputArgList Args = Tbl.parseArgs(argc, argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
            llvm::errs() << Msg << '\n';
            std::exit(1);
        });

        if (Args.hasArg(OPT_help)) {
            Tbl.printHelp(llvm::outs(), "clang-scan-deps [options]", "clang-scan-deps");
            std::exit(0);
        }
        if (Args.hasArg(OPT_version)) {
            llvm::outs() << ToolName << '\n';
            llvm::cl::PrintVersionMessage();
            std::exit(0);
        }
        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_mode_EQ)) {
            auto ModeType = llvm::StringSwitch<std::optional<ScanningMode>>(A->getValue())
                                .Case("preprocess-dependency-directives", ScanningMode::DependencyDirectivesScan)
                                .Case("preprocess", ScanningMode::CanonicalPreprocessing)
                                .Default(std::nullopt);
            if (!ModeType) {
                llvm::errs() << ToolName
                             << ": for the --mode option: Cannot find option named '"
                             << A->getValue() << "'\n";
                std::exit(1);
            }
            ScanMode = *ModeType;
        }

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_format_EQ)) {
            auto FormatType = llvm::StringSwitch<std::optional<ScanningOutputFormat>>(A->getValue())
                                  .Case("make", ScanningOutputFormat::Make)
                                  .Case("p1689", ScanningOutputFormat::P1689)
                                  .Case("experimental-full", ScanningOutputFormat::Full)
                                  .Default(std::nullopt);
            if (!FormatType) {
                llvm::errs() << ToolName
                             << ": for the --format option: Cannot find option named '"
                             << A->getValue() << "'\n";
                std::exit(1);
            }
            Format = *FormatType;
        }

        std::vector<std::string> OptimizationFlags = Args.getAllArgValues(OPT_optimize_args_EQ);
        OptimizeArgs = ScanningOptimizations::None;
        for (const auto& Arg : OptimizationFlags) {
            auto Optimization = llvm::StringSwitch<std::optional<ScanningOptimizations>>(Arg)
                                    .Case("none", ScanningOptimizations::None)
                                    .Case("header-search", ScanningOptimizations::HeaderSearch)
                                    .Case("system-warnings", ScanningOptimizations::SystemWarnings)
                                    .Case("vfs", ScanningOptimizations::VFS)
                                    .Case("canonicalize-macros", ScanningOptimizations::Macros)
                                    .Case("ignore-current-working-dir", ScanningOptimizations::IgnoreCWD)
                                    .Case("all", ScanningOptimizations::All)
                                    .Default(std::nullopt);
            if (!Optimization) {
                llvm::errs()
                    << ToolName
                    << ": for the --optimize-args option: Cannot find option named '"
                    << Arg << "'\n";
                std::exit(1);
            }
            OptimizeArgs |= *Optimization;
        }
        if (OptimizationFlags.empty())
            OptimizeArgs = ScanningOptimizations::Default;

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_module_files_dir_EQ))
            ModuleFilesDir = A->getValue();

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_o))
            OutputFileName = A->getValue();

        EagerLoadModules = Args.hasArg(OPT_eager_load_pcm);

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_j)) {
            StringRef S { A->getValue() };
            if (!llvm::to_integer(S, NumThreads, 0)) {
                llvm::errs() << ToolName << ": for the -j option: '" << S
                             << "' value invalid for uint argument!\n";
                std::exit(1);
            }
        }

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_compilation_database_EQ))
            CompilationDB = A->getValue();

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_module_names_EQ))
            ModuleNames = A->getValue();

        for (const llvm::opt::Arg* A : Args.filtered(OPT_dependency_target_EQ))
            ModuleDepTargets.emplace_back(A->getValue());

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_tu_buffer_path_EQ))
            TranslationUnitFile = A->getValue();

        DeprecatedDriverCommand = Args.hasArg(OPT_deprecated_driver_command);

        if (const llvm::opt::Arg* A = Args.getLastArg(OPT_resource_dir_recipe_EQ)) {
            auto Kind = llvm::StringSwitch<std::optional<ResourceDirRecipeKind>>(A->getValue())
                            .Case("modify-compiler-path", RDRK_ModifyCompilerPath)
                            .Case("invoke-compiler", RDRK_InvokeCompiler)
                            .Default(std::nullopt);
            if (!Kind) {
                llvm::errs() << ToolName
                             << ": for the --resource-dir-recipe option: Cannot find "
                                "option named '"
                             << A->getValue() << "'\n";
                std::exit(1);
            }
            ResourceDirRecipe = *Kind;
        }

        PrintTiming = Args.hasArg(OPT_print_timing);

        EmitVisibleModules = Args.hasArg(OPT_emit_visible_modules);

        Verbose = Args.hasArg(OPT_verbose);

        RoundTripArgs = Args.hasArg(OPT_round_trip_args);

        VerbatimArgs = Args.hasArg(OPT_verbatim_args);

        if (const llvm::opt::Arg* A = Args.getLastArgNoClaim(OPT_DASH_DASH))
            CommandLine.assign(A->getValues().begin(), A->getValues().end());
    }

    class SharedStream {
    public:
        SharedStream(raw_ostream& OS) : OS(OS) { }
        void applyLocked(llvm::function_ref<void(raw_ostream& OS)> Fn) {
            std::unique_lock<std::mutex> LockGuard(Lock);
            Fn(OS);
            OS.flush();
        }

    private:
        std::mutex Lock;
        raw_ostream& OS;
    };

    class ResourceDirectoryCache {
    public:
        /// findResourceDir finds the resource directory relative to the clang
        /// compiler being used in Args, by running it with "-print-resource-dir"
        /// option and cache the results for reuse. \returns resource directory path
        /// associated with the given invocation command or empty string if the
        /// compiler path is NOT an absolute path.
        StringRef findResourceDir(const tooling::CommandLineArguments& Args, bool ClangCLMode) {
            if (Args.size() < 1)
                return "";

            const std::string& ClangBinaryPath = Args[0];
            if (!llvm::sys::path::is_absolute(ClangBinaryPath))
                return "";

            const std::string& ClangBinaryName = std::string(llvm::sys::path::filename(ClangBinaryPath));

            std::unique_lock<std::mutex> LockGuard(CacheLock);
            const auto& CachedResourceDir = Cache.find(ClangBinaryPath);
            if (CachedResourceDir != Cache.end())
                return CachedResourceDir->second;

            const std::array<StringRef, 2> PrintResourceDirArgs {
                ClangBinaryName,
                ClangCLMode ? "/clang:-print-resource-dir" : "-print-resource-dir"
            };

            llvm::SmallString<64> OutputFile, ErrorFile;
            llvm::sys::fs::createTemporaryFile("print-resource-dir-output", "" /*no-suffix*/, OutputFile);
            llvm::sys::fs::createTemporaryFile("print-resource-dir-error", "" /*no-suffix*/, ErrorFile);
            llvm::FileRemover OutputRemover(OutputFile.c_str());
            llvm::FileRemover ErrorRemover(ErrorFile.c_str());
            std::optional<StringRef> Redirects[] = {
                { "" }, // Stdin
                OutputFile.str(),
                ErrorFile.str(),
            };
            if (llvm::sys::ExecuteAndWait(ClangBinaryPath, PrintResourceDirArgs, { }, Redirects)) {
                auto ErrorBuf = llvm::MemoryBuffer::getFile(ErrorFile.c_str(), /*IsText=*/true);
                llvm::errs() << ErrorBuf.get()->getBuffer();
                return "";
            }

            auto OutputBuf = llvm::MemoryBuffer::getFile(OutputFile.c_str(), /*IsText=*/true);
            if (!OutputBuf)
                return "";
            StringRef Output = OutputBuf.get()->getBuffer().rtrim('\n');

            return Cache[ClangBinaryPath] = Output.str();
        }

    private:
        std::map<std::string, std::string> Cache;
        std::mutex CacheLock;
    };

} // end anonymous namespace

/// Prints any diagnostics produced during a dependency scan.
static void handleDiagnostics(StringRef Input, StringRef Diagnostics, SharedStream& Errs) {
    if (Diagnostics.empty())
        return;

    Errs.applyLocked([&](raw_ostream& OS) {
        OS << "Diagnostics while scanning dependencies for '" << Input << "':\n";
        OS << Diagnostics;
    });
}

// Technically, we don't need to sort the dependency list to get determinism.
// Leaving these be will simply preserve the import order.
static auto toJSONSorted(llvm::json::OStream& JOS, std::vector<ModuleID> V) {
    llvm::sort(V);
    return [&JOS, V = std::move(V)] {
        for (const ModuleID& MID : V)
            JOS.object([&] {
                JOS.attribute("context-hash", StringRef(MID.ContextHash));
                JOS.attribute("module-name", StringRef(MID.ModuleName));
            });
    };
}

std::vector<ScanResult> clang_scan_deps_main(
    int argc,
    char** argv,
    std::unique_ptr<tooling::CompilationDatabase> Compilations) {
    llvm::InitializeAllTargetInfos();
    ParseArgs(argc, argv);

    if (!Compilations)
        throw std::runtime_error("Missing compilation database");

    if (Format != ScanningOutputFormat::Make)
        throw std::runtime_error("Unsupported scanning format");

    llvm::cl::PrintOptionValues();

    if (!VerbatimArgs) {
        Compilations = expandResponseFiles(
            std::move(Compilations),
            llvm::vfs::getRealFileSystem());

        Compilations = inferTargetAndDriverMode(
            std::move(Compilations));

        Compilations = inferToolLocation(
            std::move(Compilations));
    }

    auto AdjustingCompilations = std::make_unique<tooling::ArgumentsAdjustingCompilations>(
        std::move(Compilations));

    ResourceDirectoryCache ResourceDirCache;

    auto ArgsAdjuster =
        [&ResourceDirCache](
            const tooling::CommandLineArguments& Args,
            StringRef FileName) {
            std::string LastO;
            bool HasResourceDir = false;
            bool ClangCLMode = false;

            auto FlagsEnd = llvm::find(Args, "--");

            if (FlagsEnd != Args.begin()) {
                ClangCLMode = llvm::sys::path::stem(Args[0])
                                  .contains_insensitive("clang-cl")
                    || llvm::is_contained(
                                  Args, "--driver-mode=cl");

                auto R = std::make_reverse_iterator(FlagsEnd);
                auto E = Args.rend();

                if (E != R)
                    E--;

                for (auto I = R; I != E; ++I) {
                    StringRef Arg = *I;

                    if (ClangCLMode) {
                        if ((I + 1) != E && I[1] == "-Xclang")
                            continue;

                        if (LastO.empty()) {
                            if ((Arg == "-o" || Arg == "/o") && I != R)
                                LastO = I[-1];
                            else if (Arg.starts_with("/Fo") || Arg.starts_with("-Fo"))
                                LastO = Arg.drop_front(3).str();
                            else if (Arg.starts_with("/o") || Arg.starts_with("-o"))
                                LastO = Arg.drop_front(2).str();

                            if (!LastO.empty() && !llvm::sys::path::has_extension(LastO))
                                LastO.append(".obj");
                        }
                    }

                    if (Arg == "-resource-dir")
                        HasResourceDir = true;
                }
            }

            tooling::CommandLineArguments AdjustedArgs(
                Args.begin(), FlagsEnd);

            if (ClangCLMode && !LastO.empty()) {
                AdjustedArgs.push_back("/clang:-o");
                AdjustedArgs.push_back("/clang:" + LastO);
            }

            if (!HasResourceDir && ResourceDirRecipe == RDRK_InvokeCompiler) {
                StringRef ResourceDir = ResourceDirCache.findResourceDir(
                    Args, ClangCLMode);

                if (!ResourceDir.empty()) {
                    AdjustedArgs.push_back("-resource-dir");
                    AdjustedArgs.push_back(
                        std::string(ResourceDir));
                }
            }

            AdjustedArgs.insert(
                AdjustedArgs.end(), FlagsEnd, Args.end());

            return AdjustedArgs;
        };

    if (!VerbatimArgs)
        AdjustingCompilations->appendArgumentsAdjuster(
            ArgsAdjuster);

    SharedStream Errs(llvm::errs());

    const auto Inputs = AdjustingCompilations->getAllCompileCommands();

    std::vector<ScanResult> Results(Inputs.size());

    if (Inputs.empty())
        return Results;

    std::atomic<bool> HadErrors(false);
    std::atomic<size_t> Index(0);

    auto ScanningTask = [&](DependencyScanningService& Service) {
        DependencyScanningWorker Worker(
            Service,
            llvm::vfs::createPhysicalFileSystem());

        while (true) {
            const size_t LocalIndex = Index.fetch_add(1);

            if (LocalIndex >= Inputs.size())
                break;

            const auto& Input = Inputs[LocalIndex];

            std::string S;
            llvm::raw_string_ostream OS(S);

            DiagnosticOptions DiagOpts;
            DiagOpts.ShowCarets = false;

            TextDiagnosticPrinter DiagConsumer(OS, DiagOpts);

            NixjaDependencyConsumer Consumer;
            CallbackActionController Controller(nullptr);

            const bool Success = tooling::computeDependencies(
                Worker,
                Input.Directory,
                Input.CommandLine,
                Consumer,
                Controller,
                DiagConsumer);

            handleDiagnostics(Input.Filename, S, Errs);

            if (!Success) {
                HadErrors = true;
                continue;
            }

            Results[LocalIndex] = {
                Input.Filename,
                Input.Output,
                std::move(Consumer.Dependencies)
            };
        }
    };

    DependencyScanningService Service(
        ScanMode,
        ScanningOutputFormat::Make,
        OptimizeArgs,
        EagerLoadModules,
        /*TraceVFS=*/false);

    llvm::Timer T;
    T.startTimer();

    if (Inputs.size() == 1) {
        ScanningTask(Service);
    } else {
        llvm::DefaultThreadPool Pool(
            llvm::hardware_concurrency(NumThreads));

        if (Verbose) {
            llvm::errs()
                << "Running clang-scan-deps on "
                << Inputs.size()
                << " files using "
                << Pool.getMaxConcurrency()
                << " workers\n";
        }

        for (unsigned I = 0;
             I < Pool.getMaxConcurrency();
             ++I) {
            Pool.async([&]() {
                ScanningTask(Service);
            });
        }

        Pool.wait();
    }

    T.stopTimer();

    if (PrintTiming) {
        llvm::errs()
            << "wall time [s]\t"
            << "process time [s]\t"
            << "instruction count\n";

        const llvm::TimeRecord& R = T.getTotalTime();

        llvm::errs()
            << llvm::format("%0.4f", R.getWallTime()) << "\t"
            << llvm::format("%0.4f", R.getProcessTime()) << "\t"
            << llvm::format("%llu", R.getInstructionsExecuted())
            << "\n";
    }

    if (HadErrors)
        throw std::runtime_error("Dependency scanning failed");

    return Results;
}

std::vector<ScanResult> clang_scan_deps(
    std::unique_ptr<tooling::CompilationDatabase>
        compilation_database) {
    char tool_name[] = "clang-scan-deps";
    char format[] = "-format=make";

    char* args[] = { tool_name, format };

    return clang_scan_deps_main(
        2, args, std::move(compilation_database));
}
