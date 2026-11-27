//===- ClangScanDeps.cpp - Implementation of clang-scan-deps --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ClangScanDeps.hpp"

// #include "clang/DependencyScanning/DependencyConsumer.h"
#include "clang/DependencyScanning/DependencyScanningService.h"
#include "clang/DependencyScanning/DependencyScanningWorker.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/DependencyScanningTool.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/LLVMDriver.h"
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

template <typename Container>
static auto toJSONStrings(llvm::json::OStream& JOS, Container&& Strings) {
    return [&JOS, Strings = std::forward<Container>(Strings)] {
        for (StringRef Str : Strings)
            // Not reporting SDKSettings.json so that test checks can remain (mostly)
            // platform-agnostic.
            if (!Str.ends_with("SDKSettings.json"))
                JOS.value(Str);
    };
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

static auto toJSONSorted(llvm::json::OStream& JOS, SmallVector<Module::LinkLibrary, 2> LinkLibs) {
    llvm::sort(LinkLibs, [](const auto& LHS, const auto& RHS) {
        return LHS.Library < RHS.Library;
    });
    return [&JOS, LinkLibs = std::move(LinkLibs)] {
        for (const auto& LL : LinkLibs)
            JOS.object([&] {
                JOS.attribute("isFramework", LL.IsFramework);
                JOS.attribute("link-name", StringRef(LL.Library));
            });
    };
}

static auto toJSONSorted(llvm::json::OStream& JOS, std::vector<std::string> V) {
    llvm::sort(V);
    return [&JOS, V = std::move(V)] {
        for (const StringRef Entry : V)
            JOS.value(Entry);
    };
}

// Thread safe.
class FullDeps {
public:
    FullDeps(size_t NumInputs) : Inputs(NumInputs) { }

    void mergeDeps(StringRef Input, TranslationUnitDeps TUDeps, size_t InputIndex) {
        mergeDeps(std::move(TUDeps.ModuleGraph), InputIndex);

        InputDeps ID;
        ID.FileName = std::string(Input);
        ID.ContextHash = std::move(TUDeps.ID.ContextHash);
        ID.FileDeps = std::move(TUDeps.FileDeps);
        ID.NamedModule = std::move(TUDeps.ID.ModuleName);
        ID.NamedModuleDeps = std::move(TUDeps.NamedModuleDeps);
        ID.ClangModuleDeps = std::move(TUDeps.ClangModuleDeps);
        ID.VisibleModules = std::move(TUDeps.VisibleModules);
        ID.DriverCommandLine = std::move(TUDeps.DriverCommandLine);
        ID.Commands = std::move(TUDeps.Commands);

        assert(InputIndex < Inputs.size() && "Input index out of bounds");
        assert(Inputs[InputIndex].FileName.empty() && "Result already populated");
        Inputs[InputIndex] = std::move(ID);
    }

    void mergeDeps(ModuleDepsGraph Graph, size_t InputIndex) {
        std::vector<ModuleDeps*> NewMDs;
        {
            std::unique_lock<std::mutex> ul(Lock);
            for (ModuleDeps& MD : Graph) {
                auto I = Modules.find({ MD.ID, 0 });
                if (I != Modules.end()) {
                    I->first.InputIndex = std::min(I->first.InputIndex, InputIndex);
                    continue;
                }
                auto Res = Modules.insert(I, { { MD.ID, InputIndex }, std::move(MD) });
                NewMDs.push_back(&Res->second);
            }
        }
        // First call to \c getBuildArguments is somewhat expensive. Let's call it
        // on the current thread (instead of the main one), and outside the
        // critical section.
        for (ModuleDeps* MD : NewMDs)
            (void)MD->getBuildArguments();
    }

    bool roundTripCommand(ArrayRef<std::string> ArgStrs, DiagnosticsEngine& Diags) {
        if (ArgStrs.empty() || ArgStrs[0] != "-cc1")
            return false;
        SmallVector<const char*> Args;
        for (const std::string& Arg : ArgStrs)
            Args.push_back(Arg.c_str());
        return !CompilerInvocation::checkCC1RoundTrip(Args, Diags);
    }

    // Returns \c true if any command lines fail to round-trip. We expect
    // commands already be canonical when output by the scanner.
    bool roundTripCommands(raw_ostream& ErrOS) {
        DiagnosticOptions DiagOpts;
        TextDiagnosticPrinter DiagConsumer(ErrOS, DiagOpts);
        IntrusiveRefCntPtr<DiagnosticsEngine> Diags = CompilerInstance::createDiagnostics(*llvm::vfs::getRealFileSystem(), DiagOpts, &DiagConsumer,
                                                                                          /*ShouldOwnClient=*/false);

        for (auto&& M : Modules)
            if (roundTripCommand(M.second.getBuildArguments(), *Diags))
                return true;

        for (auto&& I : Inputs)
            for (const auto& Cmd : I.Commands)
                if (roundTripCommand(Cmd.Arguments, *Diags))
                    return true;

        return false;
    }

    void printFullOutput(raw_ostream& OS) {
        // Skip sorting modules and constructing the JSON object if the output
        // cannot be observed anyway. This makes timings less noisy.
        if (&OS == &llvm::nulls())
            return;

        // Sort the modules by name to get a deterministic order.
        std::vector<IndexedModuleID> ModuleIDs;
        for (auto&& M : Modules)
            ModuleIDs.push_back(M.first);
        llvm::sort(ModuleIDs);

        llvm::json::OStream JOS(OS, /*IndentSize=*/2);

        JOS.object([&] {
            JOS.attributeArray("modules", [&] {
                for (auto&& ModID : ModuleIDs) {
                    auto& MD = Modules[ModID];
                    JOS.object([&] {
                        if (MD.IsInStableDirectories)
                            JOS.attribute("is-in-stable-directories", MD.IsInStableDirectories);
                        JOS.attributeArray("clang-module-deps", toJSONSorted(JOS, MD.ClangModuleDeps));
                        JOS.attribute("clang-modulemap-file", StringRef(MD.ClangModuleMapFile));
                        JOS.attributeArray("command-line", toJSONStrings(JOS, MD.getBuildArguments()));
                        JOS.attribute("context-hash", StringRef(MD.ID.ContextHash));
                        JOS.attributeArray("file-deps", [&] {
                            MD.forEachFileDep([&](StringRef FileDep) {
                                // Not reporting SDKSettings.json so that test checks can remain
                                // (mostly) platform-agnostic.
                                if (!FileDep.ends_with("SDKSettings.json"))
                                    JOS.value(FileDep);
                            });
                        });
                        JOS.attributeArray("link-libraries", toJSONSorted(JOS, MD.LinkLibraries));
                        JOS.attribute("name", StringRef(MD.ID.ModuleName));
                    });
                }
            });

            JOS.attributeArray("translation-units", [&] {
                for (auto&& I : Inputs) {
                    JOS.object([&] {
                        JOS.attributeArray("commands", [&] {
                            if (I.DriverCommandLine.empty()) {
                                for (const auto& Cmd : I.Commands) {
                                    JOS.object([&] {
                                        JOS.attribute("clang-context-hash", StringRef(I.ContextHash));
                                        if (!I.NamedModule.empty())
                                            JOS.attribute("named-module", (I.NamedModule));
                                        if (!I.NamedModuleDeps.empty())
                                            JOS.attributeArray("named-module-deps", [&] {
                                                for (const auto& Dep : I.NamedModuleDeps)
                                                    JOS.value(Dep);
                                            });
                                        JOS.attributeArray("clang-module-deps", toJSONSorted(JOS, I.ClangModuleDeps));
                                        JOS.attributeArray("command-line", toJSONStrings(JOS, Cmd.Arguments));
                                        JOS.attribute("executable", StringRef(Cmd.Executable));
                                        JOS.attributeArray("file-deps", toJSONStrings(JOS, I.FileDeps));
                                        JOS.attribute("input-file", StringRef(I.FileName));
                                        if (EmitVisibleModules)
                                            JOS.attributeArray("visible-clang-modules", toJSONSorted(JOS, I.VisibleModules));
                                    });
                                }
                            } else {
                                JOS.object([&] {
                                    JOS.attribute("clang-context-hash", StringRef(I.ContextHash));
                                    if (!I.NamedModule.empty())
                                        JOS.attribute("named-module", (I.NamedModule));
                                    if (!I.NamedModuleDeps.empty())
                                        JOS.attributeArray("named-module-deps", [&] {
                                            for (const auto& Dep : I.NamedModuleDeps)
                                                JOS.value(Dep);
                                        });
                                    JOS.attributeArray("clang-module-deps", toJSONSorted(JOS, I.ClangModuleDeps));
                                    JOS.attributeArray("command-line", toJSONStrings(JOS, I.DriverCommandLine));
                                    JOS.attribute("executable", "clang");
                                    JOS.attributeArray("file-deps", toJSONStrings(JOS, I.FileDeps));
                                    JOS.attribute("input-file", StringRef(I.FileName));
                                    if (EmitVisibleModules)
                                        JOS.attributeArray("visible-clang-modules", toJSONSorted(JOS, I.VisibleModules));
                                });
                            }
                        });
                    });
                }
            });
        });
    }

private:
    struct IndexedModuleID {
        ModuleID ID;

        // FIXME: This is mutable so that it can still be updated after insertion
        //  into an unordered associative container. This is "fine", since this
        //  field doesn't contribute to the hash, but it's a brittle hack.
        mutable size_t InputIndex;

        bool operator==(const IndexedModuleID& Other) const {
            return ID == Other.ID;
        }

        bool operator<(const IndexedModuleID& Other) const {
            /// We need the output of clang-scan-deps to be deterministic. However,
            /// the dependency graph may contain two modules with the same name. How
            /// do we decide which one to print first? If we made that decision based
            /// on the context hash, the ordering would be deterministic, but
            /// different across machines. This can happen for example when the inputs
            /// or the SDKs (which both contribute to the "context" hash) live in
            /// different absolute locations. We solve that by tracking the index of
            /// the first input TU that (transitively) imports the dependency, which
            /// is always the same for the same input, resulting in deterministic
            /// sorting that's also reproducible across machines.
            return std::tie(ID.ModuleName, InputIndex) < std::tie(Other.ID.ModuleName, Other.InputIndex);
        }

        struct Hasher {
            std::size_t operator()(const IndexedModuleID& IMID) const {
                return llvm::hash_value(IMID.ID);
            }
        };
    };

    struct InputDeps {
        std::string FileName;
        std::string ContextHash;
        std::vector<std::string> FileDeps;
        std::string NamedModule;
        std::vector<std::string> NamedModuleDeps;
        std::vector<ModuleID> ClangModuleDeps;
        std::vector<std::string> VisibleModules;
        std::vector<std::string> DriverCommandLine;
        std::vector<Command> Commands;
    };

    std::mutex Lock;
    std::unordered_map<IndexedModuleID, ModuleDeps, IndexedModuleID::Hasher>
        Modules;
    std::vector<InputDeps> Inputs;
};

static bool handleModuleResult(StringRef ModuleName, llvm::Expected<TranslationUnitDeps>& MaybeTUDeps, FullDeps& FD, size_t InputIndex, SharedStream& OS, SharedStream& Errs) {
    if (!MaybeTUDeps) {
        llvm::handleAllErrors(MaybeTUDeps.takeError(), [&ModuleName, &Errs](llvm::StringError& Err) {
            Errs.applyLocked([&](raw_ostream& OS) {
                OS << "Error while scanning dependencies for "
                   << ModuleName << ":\n";
                OS << Err.getMessage();
            });
        });
        return true;
    }
    FD.mergeDeps(std::move(MaybeTUDeps->ModuleGraph), InputIndex);
    return false;
}

static void handleErrorWithInfoString(StringRef Info, llvm::Error E, SharedStream& OS, SharedStream& Errs) {
    llvm::handleAllErrors(std::move(E), [&Info, &Errs](llvm::StringError& Err) {
        Errs.applyLocked([&](raw_ostream& OS) {
            OS << "Error: " << Info << ":\n";
            OS << Err.getMessage();
        });
    });
}

class P1689Deps {
public:
    void printDependencies(raw_ostream& OS) {
        addSourcePathsToRequires();
        // Sort the modules by name to get a deterministic order.
        llvm::sort(Rules, [](const P1689Rule& A, const P1689Rule& B) {
            return A.PrimaryOutput < B.PrimaryOutput;
        });

        using namespace llvm::json;
        Array OutputRules;
        for (const P1689Rule& R : Rules) {
            Object O { { "primary-output", R.PrimaryOutput } };

            if (R.Provides) {
                Array Provides;
                Object Provided { { "logical-name", R.Provides->ModuleName },
                                  { "source-path", R.Provides->SourcePath },
                                  { "is-interface", R.Provides->IsStdCXXModuleInterface } };
                Provides.push_back(std::move(Provided));
                O.insert({ "provides", std::move(Provides) });
            }

            Array Requires;
            for (const P1689ModuleInfo& Info : R.Requires) {
                Object RequiredInfo { { "logical-name", Info.ModuleName } };
                if (!Info.SourcePath.empty())
                    RequiredInfo.insert({ "source-path", Info.SourcePath });
                Requires.push_back(std::move(RequiredInfo));
            }

            if (!Requires.empty())
                O.insert({ "requires", std::move(Requires) });

            OutputRules.push_back(std::move(O));
        }

        Object Output {
            { "version", 1 }, { "revision", 0 }, { "rules", std::move(OutputRules) }
        };

        OS << llvm::formatv("{0:2}\n", Value(std::move(Output)));
    }

    void addRules(P1689Rule& Rule) {
        std::unique_lock<std::mutex> LockGuard(Lock);
        Rules.push_back(Rule);
    }

private:
    void addSourcePathsToRequires() {
        llvm::DenseMap<StringRef, StringRef> ModuleSourceMapper;
        for (const P1689Rule& R : Rules)
            if (R.Provides && !R.Provides->SourcePath.empty())
                ModuleSourceMapper[R.Provides->ModuleName] = R.Provides->SourcePath;

        for (P1689Rule& R : Rules) {
            for (P1689ModuleInfo& Info : R.Requires) {
                auto Iter = ModuleSourceMapper.find(Info.ModuleName);
                if (Iter != ModuleSourceMapper.end())
                    Info.SourcePath = Iter->second;
            }
        }
    }

    std::mutex Lock;
    std::vector<P1689Rule> Rules;
};

/// Construct a path for the explicitly built PCM.
static std::string constructPCMPath(ModuleID MID, StringRef OutputDir) {
    SmallString<256> ExplicitPCMPath(OutputDir);
    llvm::sys::path::append(ExplicitPCMPath, MID.ContextHash, MID.ModuleName + "-" + MID.ContextHash + ".pcm");
    return std::string(ExplicitPCMPath);
}

static std::string lookupModuleOutput(const ModuleDeps& MD, ModuleOutputKind MOK, StringRef OutputDir) {
    std::string PCMPath = constructPCMPath(MD.ID, OutputDir);
    switch (MOK) {
    case ModuleOutputKind::ModuleFile:
        return PCMPath;
    case ModuleOutputKind::DependencyFile:
        return PCMPath + ".d";
    case ModuleOutputKind::DependencyTargets:
        // Null-separate the list of targets.
        return join(ModuleDepTargets, StringRef("\0", 1));
    case ModuleOutputKind::DiagnosticSerializationFile:
        return PCMPath + ".diag";
    }
    llvm_unreachable("Fully covered switch above!");
}

static std::string getModuleCachePath(ArrayRef<std::string> Args) {
    for (StringRef Arg : llvm::reverse(Args)) {
        Arg.consume_front("/clang:");
        if (Arg.consume_front("-fmodules-cache-path="))
            return std::string(Arg);
    }
    SmallString<128> Path;
    driver::Driver::getDefaultModuleCachePath(Path);
    return std::string(Path);
}

/// Attempts to construct the compilation database from '-compilation-database'
/// or from the arguments following the positional '--'.
static std::unique_ptr<tooling::CompilationDatabase>
getCompilationDatabase(int argc, char** argv, std::string& ErrorMessage) {
    ParseArgs(argc, argv);

    if (!(CommandLine.empty() ^ CompilationDB.empty())) {
        llvm::errs() << "The compilation command line must be provided either via "
                        "'-compilation-database' or after '--'.";
        return nullptr;
    }

    if (!CompilationDB.empty())
        return tooling::JSONCompilationDatabase::loadFromFile(
            CompilationDB, ErrorMessage, tooling::JSONCommandLineSyntax::AutoDetect);

    DiagnosticOptions DiagOpts;
    llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags = CompilerInstance::createDiagnostics(*llvm::vfs::getRealFileSystem(), DiagOpts);
    driver::Driver TheDriver(CommandLine[0], llvm::sys::getDefaultTargetTriple(), *Diags);
    TheDriver.setCheckInputsExist(false);
    std::unique_ptr<driver::Compilation> C(
        TheDriver.BuildCompilation(CommandLine));
    if (!C || C->getJobs().empty())
        return nullptr;

    auto Cmd = C->getJobs().begin();
    auto CI = std::make_unique<CompilerInvocation>();
    CompilerInvocation::CreateFromArgs(*CI, Cmd->getArguments(), *Diags, CommandLine[0]);
    if (!CI)
        return nullptr;

    FrontendOptions& FEOpts = CI->getFrontendOpts();
    if (FEOpts.Inputs.size() != 1) {
        llvm::errs()
            << "Exactly one input file is required in the per-file mode ('--').\n";
        return nullptr;
    }

    // There might be multiple jobs for a compilation. Extract the specified
    // output filename from the last job.
    auto LastCmd = C->getJobs().end();
    LastCmd--;
    if (LastCmd->getOutputFilenames().size() != 1) {
        llvm::errs()
            << "Exactly one output file is required in the per-file mode ('--').\n";
        return nullptr;
    }
    StringRef OutputFile = LastCmd->getOutputFilenames().front();

    class InplaceCompilationDatabase : public tooling::CompilationDatabase {
    public:
        InplaceCompilationDatabase(StringRef InputFile, StringRef OutputFile, ArrayRef<const char*> CommandLine) : Command(".", InputFile, { }, OutputFile) {
            for (auto* C : CommandLine)
                Command.CommandLine.push_back(C);
        }

        std::vector<tooling::CompileCommand>
        getCompileCommands(StringRef FilePath) const override {
            if (FilePath != Command.Filename)
                return { };
            return { Command };
        }

        std::vector<std::string> getAllFiles() const override {
            return { Command.Filename };
        }

        std::vector<tooling::CompileCommand>
        getAllCompileCommands() const override {
            return { Command };
        }

    private:
        tooling::CompileCommand Command;
    };

    return std::make_unique<InplaceCompilationDatabase>(
        FEOpts.Inputs[0].getFile(), OutputFile, CommandLine);
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
