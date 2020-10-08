#include <set>
#include <vector>
#include <string>
#include <cassert>
#include <iostream>
#include <fstream>

#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include <llvm/Config/llvm-config.h>

#if (LLVM_VERSION_MAJOR < 3)
#error "Unsupported version of LLVM"
#endif

#include "dg/tools/llvm-slicer.h"
#include "dg/tools/llvm-slicer-opts.h"
#include "dg/tools/llvm-slicer-utils.h"

// ignore unused parameters in LLVM libraries
#if (__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

//#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/InstIterator.h>

#if (__clang__)
#pragma clang diagnostic pop // ignore -Wunused-parameter
#else
#pragma GCC diagnostic pop
#endif

#include "dg/ADT/Queue.h"
#include "dg/util/debug.h"

using namespace dg;

using llvm::errs;
using dg::LLVMPointerAnalysisOptions;
using dg::LLVMDataDependenceAnalysisOptions;

using AnnotationOptsT
    = dg::debug::LLVMDGAssemblyAnnotationWriter::AnnotationOptsT;

llvm::cl::opt<bool> enable_debug("dbg",
    llvm::cl::desc("Enable debugging messages (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> should_verify_module("dont-verify",
    llvm::cl::desc("Verify sliced module (default=true)."),
    llvm::cl::init(true), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> remove_unused_only("remove-unused-only",
    llvm::cl::desc("Only remove unused parts of module (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> statistics("statistics",
    llvm::cl::desc("Print statistics about slicing (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> dump_dg("dump-dg",
    llvm::cl::desc("Dump dependence graph to dot (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> dump_dg_only("dump-dg-only",
    llvm::cl::desc("Only dump dependence graph to dot,"
                   " do not slice the module (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> dump_bb_only("dump-bb-only",
    llvm::cl::desc("Only dump basic blocks of dependence graph to dot"
                   " (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<std::string> annotationOpts("annotate",
    llvm::cl::desc("Save annotated version of module as a text (.ll).\n"
                   "Options:\n"
                    "  dd: data dependencies,\n"
                    "  cd:control dependencies,\n"
                    "  pta: points-to information,\n"
                    "  memacc: memory accesses of instructions,\n"
                    "  slice: comment out what is going to be sliced away).\n"
                    "for more options, use comma separated list"),
    llvm::cl::value_desc("val1,val2,..."), llvm::cl::init(""),
    llvm::cl::cat(SlicingOpts));


// mapping of AllocaInst to the names of C variables
std::map<const llvm::Value *, std::string> valuesToVariables;

static void maybe_print_statistics(llvm::Module *M, const char *prefix = nullptr)
{
    if (!statistics)
        return;

    using namespace llvm;
    uint64_t inum, bnum, fnum, gnum;
    inum = bnum = fnum = gnum = 0;

    for (auto I = M->begin(), E = M->end(); I != E; ++I) {
        // don't count in declarations
        if (I->size() == 0)
            continue;

        ++fnum;

        for (const BasicBlock& B : *I) {
            ++bnum;
            inum += B.size();
        }
    }

    for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I)
        ++gnum;

    if (prefix)
        errs() << prefix;

    errs() << "Globals/Functions/Blocks/Instr.: "
           << gnum << " " << fnum << " " << bnum << " " << inum << "\n";
}

static AnnotationOptsT parseAnnotationOptions(const std::string& annot)
{
    if (annot.empty())
        return {};

    AnnotationOptsT opts{};
    std::vector<std::string> lst = splitList(annot);
    for (const std::string& opt : lst) {
        if (opt == "dd")
            opts |= AnnotationOptsT::ANNOTATE_DD;
        else if (opt == "cd")
            opts |= AnnotationOptsT::ANNOTATE_CD;
        else if (opt == "rd")
            opts |= AnnotationOptsT::ANNOTATE_DU;
        else if (opt == "pta")
            opts |= AnnotationOptsT::ANNOTATE_PTR;
        else if (opt == "memacc")
            opts |= AnnotationOptsT::ANNOTATE_MEMORYACC;
        else if (opt == "slice" || opt == "sl" || opt == "slicer")
            opts |= AnnotationOptsT::ANNOTATE_SLICE;
    }

    return opts;
}

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext& context,
                                          const SlicerOptions& options)
{
    llvm::SMDiagnostic SMD;

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 5))
    auto _M = llvm::ParseIRFile(options.inputFile, SMD, context);
    auto M = std::unique_ptr<llvm::Module>(_M);
#else
    auto M = llvm::parseIRFile(options.inputFile, SMD, context);
    // _M is unique pointer, we need to get Module *
#endif

    if (!M) {
        SMD.print("llvm-slicer", llvm::errs());
    }

    return M;
}

#ifndef USING_SANITIZERS
void setupStackTraceOnError(int argc, char *argv[])
{

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 9
    llvm::sys::PrintStackTraceOnErrorSignal();
#else
    llvm::sys::PrintStackTraceOnErrorSignal(llvm::StringRef());
#endif
    llvm::PrettyStackTraceProgram X(argc, argv);

}
#else
void setupStackTraceOnError(int, char **) {}
#endif // not USING_SANITIZERS

// defined in llvm-slicer-crit.cpp
std::set<LLVMNode *> getSlicingCriteriaNodes(LLVMDependenceGraph& dg,
                                             const std::string& slicingCriteria);

std::pair<std::set<std::string>, std::set<std::string>>
parseSecondarySlicingCriteria(const std::string& slicingCriteria);

bool findSecondarySlicingCriteria(std::set<LLVMNode *>& criteria_nodes,
                                  const std::set<std::string>& secondaryControlCriteria,
                                  const std::set<std::string>& secondaryDataCriteria);


int main(int argc, char *argv[])
{
    setupStackTraceOnError(argc, argv);

    SlicerOptions options = parseSlicerOptions(argc, argv, true /* require crit*/);

    if (enable_debug) {
        DBG_ENABLE();
    }

    // dump_dg_only implies dumg_dg
    if (dump_dg_only) {
        dump_dg = true;
    }

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> M = parseModule(context, options);
    if (!M) {
        llvm::errs() << "Failed parsing '" << options.inputFile << "' file:\n";
        return 1;
    }

    if (!M->getFunction(options.dgOptions.entryFunction)) {
        llvm::errs() << "The entry function not found: "
                     << options.dgOptions.entryFunction << "\n";
        return 1;
    }

    maybe_print_statistics(M.get(), "Statistics before ");

    // remove unused from module, we don't need that
    ModuleWriter writer(options, M.get());
    writer.removeUnusedFromModule();

    if (remove_unused_only) {
        errs() << "[llvm-slicer] removed unused parts of module, exiting...\n";
        maybe_print_statistics(M.get(), "Statistics after ");
        return writer.saveModule(should_verify_module);
    }

    /// ---------------
    // slice the code
    /// ---------------

    ::Slicer slicer(M.get(), options);
    if (!slicer.buildDG()) {
        errs() << "ERROR: Failed building DG\n";
        return 1;
    }

    ModuleAnnotator annotator(options, &slicer.getDG(),
                              parseAnnotationOptions(annotationOpts));

    auto criteria_nodes = getSlicingCriteriaNodes(slicer.getDG(),
                                                  options.slicingCriteria);
    if (criteria_nodes.empty()) {
        llvm::errs() << "Did not find slicing criteria: '"
                     << options.slicingCriteria << "'\n";
        if (annotator.shouldAnnotate()) {
            slicer.computeDependencies();
            annotator.annotate();
        }

        if (!slicer.createEmptyMain())
            return 1;

        maybe_print_statistics(M.get(), "Statistics after ");
        return writer.cleanAndSaveModule(should_verify_module);
    }

    auto secondaryCriteria
        = parseSecondarySlicingCriteria(options.secondarySlicingCriteria);
    const auto& secondaryControlCriteria = secondaryCriteria.first;
    const auto& secondaryDataCriteria = secondaryCriteria.second;

    // mark nodes that are going to be in the slice
    if (!findSecondarySlicingCriteria(criteria_nodes,
                                      secondaryControlCriteria,
                                      secondaryDataCriteria)) {
        llvm::errs() << "Finding secondary slicing criteria nodes failed\n";
        return 1;
    }

    // mark nodes that are going to be in the slice
    if (!slicer.mark(criteria_nodes)) {
        llvm::errs() << "Finding dependent nodes failed\n";
        return 1;
    }

    // print debugging llvm IR if user asked for it
    if (annotator.shouldAnnotate())
        annotator.annotate(&criteria_nodes);

    DGDumper dumper(options, &slicer.getDG(), dump_bb_only);
    if (dump_dg) {
        dumper.dumpToDot();

        if (dump_dg_only)
            return 0;
    }

    // slice the graph
    if (!slicer.slice()) {
        errs() << "ERROR: Slicing failed\n";
        return 1;
    }

    if (dump_dg) {
        dumper.dumpToDot(".sliced.dot");
    }

    // remove unused from module again, since slicing
    // could and probably did make some other parts unused
    maybe_print_statistics(M.get(), "Statistics after ");
    return writer.cleanAndSaveModule(should_verify_module);
}
