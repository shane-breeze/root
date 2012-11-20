//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// version: $Id$
// author:  Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------

#include "cling/Interpreter/Interpreter.h"

#include "DynamicLookup.h"
#include "ExecutionContext.h"
#include "IncrementalParser.h"

#include "cling/Interpreter/CIFactory.h"
#include "cling/Interpreter/CompilationOptions.h"
#include "cling/Interpreter/InterpreterCallbacks.h"
#include "cling/Interpreter/LookupHelper.h"
#include "cling/Interpreter/StoredValueRef.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/Utils/AST.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Mangle.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaInternal.h"

#include "llvm/Linker.h"
#include "llvm/LLVMContext.h"
#include "llvm/Support/Path.h"

#include <sstream>
#include <vector>

using namespace clang;

namespace {

static bool canWrapForCall(const std::string& input_line) {
   // Whether input_line can be wrapped into a function.
   // "1" can, "#include <vector>" can't.
   if (input_line.length() > 1 && input_line[0] == '#') return false;
   if (input_line.compare(0, strlen("extern "), "extern ") == 0) return false;
   if (input_line.compare(0, strlen("using "), "using ") == 0) return false;
   return true;
}

} // unnamed namespace


// "Declared" to the JIT in RuntimeUniverse.h
extern "C" 
int cling__runtime__internal__local_cxa_atexit(void (*func) (void*), void* arg,
                                               void* dso,
                                               void* interp) {
   return ((cling::Interpreter*)interp)->CXAAtExit(func, arg, dso);
}

namespace cling {
#if (!_WIN32)
  // "Declared" to the JIT in RuntimeUniverse.h
  namespace runtime {
    namespace internal {
      struct __trigger__cxa_atexit {
        ~__trigger__cxa_atexit();
      } S;
      __trigger__cxa_atexit::~__trigger__cxa_atexit() {
        if (std::getenv("bar") == (char*)-1) {
          llvm::errs() <<
            "UNEXPECTED cling::runtime::internal::__trigger__cxa_atexit\n";
        }
      }
    }
  }
#endif

  // This function isn't referenced outside its translation unit, but it
  // can't use the "static" keyword because its address is used for
  // GetMainExecutable (since some platforms don't support taking the
  // address of main, and some platforms can't implement GetMainExecutable
  // without being given the address of a function in the main executable).
  llvm::sys::Path GetExecutablePath(const char *Argv0) {
    // This just needs to be some symbol in the binary; C++ doesn't
    // allow taking the address of ::main however.
    void *MainAddr = (void*) (intptr_t) GetExecutablePath;
    return llvm::sys::Path::GetMainExecutable(Argv0, MainAddr);
  }

  const Parser& Interpreter::getParser() const {
    return *m_IncrParser->getParser();
  }

  CodeGenerator* Interpreter::getCodeGenerator() const {
    return m_IncrParser->getCodeGenerator();
  }

  void Interpreter::unload() {
    m_IncrParser->unloadTransaction(0);
  }

  Interpreter::Interpreter(int argc, const char* const *argv,
                           const char* llvmdir /*= 0*/) :
    m_UniqueCounter(0), m_PrintAST(false), m_DynamicLookupEnabled(false) {

    m_LLVMContext.reset(new llvm::LLVMContext);
    std::vector<unsigned> LeftoverArgsIdx;
    m_Opts = InvocationOptions::CreateFromArgs(argc, argv, LeftoverArgsIdx);
    std::vector<const char*> LeftoverArgs;

    for (size_t I = 0, N = LeftoverArgsIdx.size(); I < N; ++I) {
      LeftoverArgs.push_back(argv[LeftoverArgsIdx[I]]);
    }

    m_IncrParser.reset(new IncrementalParser(this, LeftoverArgs.size(),
                                             &LeftoverArgs[0],
                                             llvmdir));
    Sema& SemaRef = getSema();
    m_LookupHelper.reset(new LookupHelper(new Parser(SemaRef.getPreprocessor(), 
                                                     SemaRef, 
                                                     /*SkipFunctionBodies*/false,
                                                     /*isTemp*/true)));

    m_ExecutionContext.reset(new ExecutionContext());

    // Add path to interpreter's include files
    // Try to find the headers in the src folder first
#ifdef CLING_SRCDIR_INCL
    llvm::sys::Path SrcP(CLING_SRCDIR_INCL);
    if (SrcP.canRead())
      AddIncludePath(SrcP.str());
#endif

    llvm::sys::Path P = GetExecutablePath(argv[0]);
    if (!P.isEmpty()) {
      P.eraseComponent();  // Remove /cling from foo/bin/clang
      P.eraseComponent();  // Remove /bin   from foo/bin
      // Get foo/include
      P.appendComponent("include");
      if (P.canRead())
        AddIncludePath(P.str());
      else {
#ifdef CLING_INSTDIR_INCL
        llvm::sys::Path InstP(CLING_INSTDIR_INCL);
        if (InstP.canRead())
          AddIncludePath(InstP.str());
#endif
      }
    }

    m_ExecutionContext->addSymbol("cling__runtime__internal__local_cxa_atexit",
                  (void*)(intptr_t)&cling__runtime__internal__local_cxa_atexit);

    // Enable incremental processing, which prevents the preprocessor destroying
    // the lexer on EOF token.
    getSema().getPreprocessor().enableIncrementalProcessing();

    if (getCI()->getLangOpts().CPlusPlus) {
      // Set up common declarations which are going to be available
      // only at runtime
      // Make sure that the universe won't be included to compile time by using
      // -D __CLING__ as CompilerInstance's arguments
#ifdef _WIN32
	  // We have to use the #defined __CLING__ on windows first. 
      //FIXME: Find proper fix.
      declare("#ifdef __CLING__ \n#endif");  
#endif
      declare("#include \"cling/Interpreter/RuntimeUniverse.h\"");
      declare("#include \"cling/Interpreter/ValuePrinter.h\"");

      // Set up the gCling variable
      std::stringstream initializer;
      initializer << "gCling=(cling::Interpreter*)" << (uintptr_t)this << ";";
      execute(initializer.str());
    }
    else {
      declare("#include \"cling/Interpreter/CValuePrinter.h\"");
    }

    handleFrontendOptions();
  }

  Interpreter::~Interpreter() {
    for (size_t I = 0, N = m_AtExitFuncs.size(); I < N; ++I) {
      const CXAAtExitElement& AEE = m_AtExitFuncs[N - I - 1];
      (*AEE.m_Func)(AEE.m_Arg);
    }
  }

  const char* Interpreter::getVersion() const {
    return "$Id$";
  }

  void Interpreter::handleFrontendOptions() {
    if (m_Opts.ShowVersion) {
      llvm::outs() << getVersion() << '\n';
    }
    if (m_Opts.Help) {
      m_Opts.PrintHelp();
    }
  }

  void Interpreter::AddIncludePath(llvm::StringRef incpath)
  {
    // Add the given path to the list of directories in which the interpreter
    // looks for include files. Only one path item can be specified at a
    // time, i.e. "path1:path2" is not supported.

    CompilerInstance* CI = getCI();
    HeaderSearchOptions& headerOpts = CI->getHeaderSearchOpts();
    const bool IsUserSupplied = true;
    const bool IsFramework = false;
    const bool IsSysRootRelative = true;
    headerOpts.AddPath(incpath, frontend::Angled, IsUserSupplied, IsFramework,
                       IsSysRootRelative);

    Preprocessor& PP = CI->getPreprocessor();
    ApplyHeaderSearchOptions(PP.getHeaderSearchInfo(), headerOpts,
                                    PP.getLangOpts(),
                                    PP.getTargetInfo().getTriple());
  }

  void Interpreter::DumpIncludePath() {
    llvm::SmallVector<std::string, 100> IncPaths;
    GetIncludePaths(IncPaths, true /*withSystem*/, true /*withFlags*/);
    // print'em all
    for (unsigned i = 0; i < IncPaths.size(); ++i) {
      llvm::outs() << IncPaths[i] <<"\n";
    }
  }

  // Adapted from clang/lib/Frontend/CompilerInvocation.cpp
  void Interpreter::GetIncludePaths(llvm::SmallVectorImpl<std::string>& incpaths,
                                   bool withSystem, bool withFlags) {
    const HeaderSearchOptions Opts(getCI()->getHeaderSearchOpts());

    if (withFlags && Opts.Sysroot != "/") {
      incpaths.push_back("-isysroot");
      incpaths.push_back(Opts.Sysroot);
    }

    /// User specified include entries.
    for (unsigned i = 0, e = Opts.UserEntries.size(); i != e; ++i) {
      const HeaderSearchOptions::Entry &E = Opts.UserEntries[i];
      if (E.IsFramework && (E.Group != frontend::Angled || !E.IsUserSupplied))
        llvm::report_fatal_error("Invalid option set!");
      if (E.IsUserSupplied) {
        switch (E.Group) {
        case frontend::After:
          if (withFlags) incpaths.push_back("-idirafter");
          break;

        case frontend::Quoted:
          if (withFlags) incpaths.push_back("-iquote");
          break;

        case frontend::System:
          if (!withSystem) continue;
          if (withFlags) incpaths.push_back("-isystem");
          break;

        case frontend::IndexHeaderMap:
          if (!withSystem) continue;
          if (withFlags) incpaths.push_back("-index-header-map");
          if (withFlags) incpaths.push_back(E.IsFramework? "-F" : "-I");
          break;

        case frontend::CSystem:
          if (!withSystem) continue;
          if (withFlags) incpaths.push_back("-c-isystem");
          break;

        case frontend::CXXSystem:
          if (!withSystem) continue;
          if (withFlags) incpaths.push_back("-cxx-isystem");
          break;

        case frontend::ObjCSystem:
          if (!withSystem) continue;
          if (withFlags) incpaths.push_back("-objc-isystem");
          break;

        case frontend::ObjCXXSystem:
          if (!withSystem) continue;
          if (withFlags) incpaths.push_back("-objcxx-isystem");
          break;

        case frontend::Angled:
          if (withFlags) incpaths.push_back(E.IsFramework ? "-F" : "-I");
          break;
        }
      } else {
        if (!withSystem) continue;
        if (E.Group != frontend::Angled && E.Group != frontend::System)
          llvm::report_fatal_error("Invalid option set!");
        if (withFlags)
          incpaths.push_back(E.Group == frontend::Angled ?
                             "-iwithprefixbefore" :
                             "-iwithprefix");
      }
      incpaths.push_back(E.Path);
    }

    if (withSystem && !Opts.ResourceDir.empty()) {
      if (withFlags) incpaths.push_back("-resource-dir");
      incpaths.push_back(Opts.ResourceDir);
    }
    if (withSystem && withFlags && !Opts.ModuleCachePath.empty()) {
      incpaths.push_back("-fmodule-cache-path");
      incpaths.push_back(Opts.ModuleCachePath);
    }
    if (withSystem && withFlags && !Opts.UseStandardSystemIncludes)
      incpaths.push_back("-nostdinc");
    if (withSystem && withFlags && !Opts.UseStandardCXXIncludes)
      incpaths.push_back("-nostdinc++");
    if (withSystem && withFlags && Opts.UseLibcxx)
      incpaths.push_back("-stdlib=libc++");
    if (withSystem && withFlags && Opts.Verbose)
      incpaths.push_back("-v");
  }

  CompilerInstance* Interpreter::getCI() const {
    return m_IncrParser->getCI();
  }

  const Sema& Interpreter::getSema() const {
    return getCI()->getSema();
  }

  Sema& Interpreter::getSema() {
    return getCI()->getSema();
  }

   llvm::ExecutionEngine* Interpreter::getExecutionEngine() const {
    return m_ExecutionContext->getExecutionEngine();
  }

  llvm::Module* Interpreter::getModule() const {
    return m_IncrParser->getCodeGenerator()->GetModule();
  }

  ///\brief Maybe transform the input line to implement cint command line
  /// semantics (declarations are global) and compile to produce a module.
  ///
  Interpreter::CompilationResult
  Interpreter::process(const std::string& input, StoredValueRef* V /* = 0 */,
                       const Decl** D /* = 0 */) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 1;
    CO.ValuePrinting = CompilationOptions::VPAuto;
    CO.ResultEvaluation = (bool)V;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();

    if (!canWrapForCall(input))
      return declare(input, D);

    if (EvaluateInternal(input, CO, V) == Interpreter::kFailure) {
      if (D)
        *D = 0;
      return Interpreter::kFailure;
    }

    if (D)
      *D = m_IncrParser->getLastTransaction()->getFirstDecl().getSingleDecl();

    return Interpreter::kSuccess;
  }

  Interpreter::CompilationResult
  Interpreter::parse(const std::string& input) {
    CompilationOptions CO;
    CO.CodeGeneration = 0;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();

    return DeclareInternal(input, CO);
  }

  Interpreter::CompilationResult
  Interpreter::declare(const std::string& input, const Decl** D /* = 0 */) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();

    return DeclareInternal(input, CO, D);
  }

  Interpreter::CompilationResult
  Interpreter::evaluate(const std::string& input, StoredValueRef& V) {
    // Here we might want to enforce further restrictions like: Only one
    // ExprStmt can be evaluated and etc. Such enforcement cannot happen in the
    // worker, because it is used from various places, where there is no such
    // rule
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 1;

    return EvaluateInternal(input, CO, &V);
  }

  Interpreter::CompilationResult
  Interpreter::echo(const std::string& input, StoredValueRef* V /* = 0 */) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = CompilationOptions::VPEnabled;
    CO.ResultEvaluation = 0;

    return EvaluateInternal(input, CO, V);
  }

  Interpreter::CompilationResult
  Interpreter::execute(const std::string& input) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = 0;
    CO.Debug = isPrintingAST();

    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    // TODO: Here might be useful to issue unused variable diagnostic,
    // because we don't do declaration extraction and the decl won't be visible
    // anymore.
    ignoreFakeDiagnostics();

    // Wrap the expression
    std::string WrapperName;
    std::string Wrapper = input;
    WrapInput(Wrapper, WrapperName);
    
    const Transaction* lastT = m_IncrParser->Compile(Wrapper, CO);
    if (lastT->getIssuedDiags() == Transaction::kNone)
      if (lastT->getState() == Transaction::kCommitted
          && RunFunction(lastT->getWrapperFD()) < kExeFirstError)
        return Interpreter::kSuccess;

    return Interpreter::kFailure;
  }

  void Interpreter::WrapInput(std::string& input, std::string& fname) {
    fname = createUniqueWrapper();
    input.insert(0, "void " + fname + "() {\n ");
    input.append("\n;\n}");
  }

  Interpreter::ExecutionResult
  Interpreter::RunFunction(const FunctionDecl* FD, StoredValueRef* res /*=0*/) {
    if (getCI()->getDiagnostics().hasErrorOccurred())
      return kExeCompilationError;

    if (!m_IncrParser->hasCodeGenerator()) {
      return kExeNoCodeGen;
    }

    if (!FD)
      return kExeUnkownFunction;

    std::string mangledNameIfNeeded;
    mangleName(FD, mangledNameIfNeeded);
    ExecutionContext::ExecutionResult ExeRes =
       m_ExecutionContext->executeFunction(mangledNameIfNeeded.c_str(),
                                           getCI()->getASTContext(),
                                           FD->getResultType(), res);
    switch (ExeRes) {
    case ExecutionContext::kExeSuccess:
      return kExeSuccess;
    case ExecutionContext::kExeFunctionNotCompiled:
      return kExeFunctionNotCompiled;
    case ExecutionContext::kExeUnresolvedSymbols:
      return kExeUnresolvedSymbols;
    default: break;
    }
    // Should not end up here...
    return kExeSuccess;
  }

  void Interpreter::createUniqueName(std::string& out) {
    out = utils::Synthesize::UniquePrefix;
    llvm::raw_string_ostream(out) << m_UniqueCounter++;
  }

  bool Interpreter::isUniqueName(llvm::StringRef name) {
    return name.startswith(utils::Synthesize::UniquePrefix);
  }

  llvm::StringRef Interpreter::createUniqueWrapper() {
    const size_t size 
      = sizeof(utils::Synthesize::UniquePrefix) + sizeof(m_UniqueCounter);
    llvm::SmallString<size> out(utils::Synthesize::UniquePrefix);
    llvm::raw_svector_ostream(out) << m_UniqueCounter++;

    return (getCI()->getASTContext().Idents.getOwn(out)).getName();
  }

  bool Interpreter::isUniqueWrapper(llvm::StringRef name) {
    return name.startswith(utils::Synthesize::UniquePrefix);
  }

  Interpreter::CompilationResult
  Interpreter::DeclareInternal(const std::string& input, 
                               const CompilationOptions& CO,
                               const clang::Decl** D /* = 0 */) {

    const Transaction* lastT = m_IncrParser->Compile(input, CO);
    if (lastT->getIssuedDiags() != Transaction::kErrors) {
      if (D)
        *D = lastT->getFirstDecl().getSingleDecl();
      return Interpreter::kSuccess;
    }

    return Interpreter::kFailure;
  }

  Interpreter::CompilationResult
  Interpreter::EvaluateInternal(const std::string& input, 
                                const CompilationOptions& CO,
                                StoredValueRef* V /* = 0 */) {
    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    ignoreFakeDiagnostics();

    // Wrap the expression
    std::string WrapperName;
    std::string Wrapper = input;
    WrapInput(Wrapper, WrapperName);
    Transaction* lastT = 0;
    if (V) {
      lastT = m_IncrParser->Parse(Wrapper, CO);
      assert(lastT->size() && "No decls created by Parse!");

      m_IncrParser->commitTransaction(lastT);
    }
    else
      lastT = m_IncrParser->Compile(Wrapper, CO);

    if (lastT->getState() == Transaction::kCommitted
        && RunFunction(lastT->getWrapperFD(), V) < kExeFirstError)
      return Interpreter::kSuccess;
    else if (V)
        *V = StoredValueRef::invalidValue();

    return Interpreter::kFailure;
  }

  void Interpreter::addLoadedFile(const std::string& name,
                                  Interpreter::LoadedFileInfo::FileType type,
                                  const llvm::sys::DynamicLibrary* dyLib) {
    m_LoadedFiles.push_back(new LoadedFileInfo(name, type, dyLib));
  }

  Interpreter::CompilationResult
  Interpreter::loadFile(const std::string& filename,
                        bool allowSharedLib /*=true*/) {
    if (allowSharedLib) {
      bool tryCode;
      if (loadLibrary(filename, false, &tryCode)
          == kLoadLibSuccess)
        return kSuccess;
      if (!tryCode)
        return kFailure;
    }

    std::string code;
    code += "#include \"" + filename + "\"";
    CompilationResult res = declare(code);
    if (res == kSuccess)
      addLoadedFile(filename, LoadedFileInfo::kSource);
    return res;
  }


  Interpreter::LoadLibResult
  Interpreter::tryLinker(const std::string& filename, bool permanent,
                         bool& tryCode) {
    using namespace llvm::sys;
    tryCode = true;
    llvm::Module* module = m_IncrParser->getCodeGenerator()->GetModule();
    assert(module && "Module must exist for linking!");

    llvm::Linker L("cling", module, llvm::Linker::QuietWarnings
                   | llvm::Linker::QuietErrors);
    struct LinkerModuleReleaseRAII {
      LinkerModuleReleaseRAII(llvm::Linker& L): m_L(L) {}
      ~LinkerModuleReleaseRAII() { m_L.releaseModule(); }
      llvm::Linker& m_L;
    } LinkerModuleReleaseRAII_(L);

    const InvocationOptions& Opts = getOptions();
    for (std::vector<Path>::const_iterator I
           = Opts.LibSearchPath.begin(), E = Opts.LibSearchPath.end(); I != E;
         ++I) {
      L.addPath(*I);
    }
    L.addSystemPaths();
    bool Native = true;
    if (L.LinkInLibrary(filename, Native)) {
      // that didn't work, try bitcode:
      Path FilePath(filename);
      std::string Magic;
      if (!FilePath.getMagicNumber(Magic, 64)) {
        // filename doesn't exist...
        // tryCode because it might be found through -I
        return kLoadLibError;
      }
      if (IdentifyFileType(Magic.c_str(), 64) != Bitcode_FileType) {
        // Nothing the linker can handle
        return kLoadLibError;
      }
      // We are promised a bitcode file, complain if it fails
      L.setFlags(0);
      if (L.LinkInFile(Path(filename), Native)) {
        tryCode = false;
        return kLoadLibError;
      }
      addLoadedFile(filename, LoadedFileInfo::kBitcode);
      return kLoadLibSuccess;
    }
    if (Native) {
      tryCode = false;
      // native shared library, load it!
      Path SoFile = L.FindLib(filename);
      assert(!SoFile.isEmpty() && "The shared lib exists but can't find it!");
      std::string errMsg;
      // TODO: !permanent case
      DynamicLibrary DyLib
        = DynamicLibrary::getPermanentLibrary(SoFile.str().c_str(), &errMsg);
      if (!DyLib.isValid()) {
        llvm::errs() << "cling::Interpreter::tryLinker(): " << errMsg << '\n';
        return kLoadLibError;
      }
      std::pair<std::set<llvm::sys::DynamicLibrary>::iterator, bool> insRes
        = m_DyLibs.insert(DyLib);
      if (!insRes.second)
        return kLoadLibExists;
      addLoadedFile(SoFile.str(), LoadedFileInfo::kDynamicLibrary,
                    &(*insRes.first));
      return kLoadLibSuccess;
    }
    return kLoadLibError;
  }

  Interpreter::LoadLibResult
  Interpreter::loadLibrary(const std::string& filename, bool permanent,
                           bool* tryCode) {
    bool tryCodeDummy;
    LoadLibResult res = tryLinker(filename, permanent,
                                  tryCode ? *tryCode : tryCodeDummy);
    if (res != kLoadLibError) {
      return res;
    }
    if (filename.compare(0, 3, "lib") == 0) {
      // starts with "lib", try without (the llvm::Linker forces
      // a "lib" in front, which makes it liblib...
      res = tryLinker(filename.substr(3, std::string::npos), permanent,
                      tryCodeDummy);
      if (res != kLoadLibError)
        return res;
    }
    return kLoadLibError;
  }

  void Interpreter::installLazyFunctionCreator(void* (*fp)(const std::string&)) {
    m_ExecutionContext->installLazyFunctionCreator(fp);
  }

  void Interpreter::suppressLazyFunctionCreatorDiags(bool suppressed/*=true*/) {
    m_ExecutionContext->suppressLazyFunctionCreatorDiags(suppressed);
  }

  StoredValueRef Interpreter::Evaluate(const char* expr, DeclContext* DC,
                                       bool ValuePrinterReq) {
    Sema& TheSema = getCI()->getSema();
    if (!DC)
      DC = TheSema.getASTContext().getTranslationUnitDecl();

    // Set up the declaration context
    DeclContext* CurContext;

    CurContext = TheSema.CurContext;
    TheSema.CurContext = DC;

    StoredValueRef Result;
    if (TheSema.getExternalSource()) {
      (ValuePrinterReq) ? echo(expr, &Result) : evaluate(expr, Result);
    }
    else
      (ValuePrinterReq) ? echo(expr, &Result) : evaluate(expr, Result);

    TheSema.CurContext = CurContext;

    return Result;
  }

  void Interpreter::setCallbacks(InterpreterCallbacks* C) {
    // We need it to enable LookupObject callback.
    m_Callbacks.reset(C);
  }

  const Transaction* Interpreter::getFirstTransaction() const {
    return m_IncrParser->getFirstTransaction();
  }

  void Interpreter::enableDynamicLookup(bool value /*=true*/) {
    m_DynamicLookupEnabled = value;

    if (isDynamicLookupEnabled()) {
      declare("#include \"cling/Interpreter/DynamicLookupRuntimeUniverse.h\"");
    }
    else
      setCallbacks(0);
  }

  void Interpreter::runStaticInitializersOnce() const {
    // Forward to ExecutionContext; should not be called by
    // anyone except for IncrementalParser.
    llvm::Module* module = m_IncrParser->getCodeGenerator()->GetModule();
    m_ExecutionContext->runStaticInitializersOnce(module);
  }

  int Interpreter::CXAAtExit(void (*func) (void*), void* arg, void* dso) {
    // Register a CXAAtExit function
    Decl* LastTLD 
      = m_IncrParser->getLastTransaction()->getLastDecl().getSingleDecl();
    m_AtExitFuncs.push_back(CXAAtExitElement(func, arg, dso, LastTLD));
    return 0; // happiness
  }

  void Interpreter::mangleName(const clang::NamedDecl* D,
                               std::string& mangledName) const {
    ///Get the mangled name of a NamedDecl.
    ///
    ///D - mangle this decl's name
    ///mangledName - put the mangled name in here
    if (!m_MangleCtx) {
      m_MangleCtx.reset(getCI()->getASTContext().createMangleContext());
    }
    if (m_MangleCtx->shouldMangleDeclName(D)) {
      llvm::raw_string_ostream RawStr(mangledName);
      m_MangleCtx->mangleName(D, RawStr);
      RawStr.flush();
    } else {
      mangledName = D->getNameAsString();
    }
  }

  void Interpreter::ignoreFakeDiagnostics() const {
    DiagnosticsEngine& Diag = getCI()->getDiagnostics();
    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    Diag.setDiagnosticMapping(clang::diag::warn_unused_expr,
                              clang::diag::MAP_IGNORE, SourceLocation());
    Diag.setDiagnosticMapping(clang::diag::warn_unused_call,
                              clang::diag::MAP_IGNORE, SourceLocation());
    Diag.setDiagnosticMapping(clang::diag::warn_unused_comparison,
                              clang::diag::MAP_IGNORE, SourceLocation());
    Diag.setDiagnosticMapping(clang::diag::ext_return_has_expr,
                              clang::diag::MAP_IGNORE, SourceLocation());
  }

  bool Interpreter::addSymbol(const char* symbolName,  void* symbolAddress) {
    // Forward to ExecutionContext;
    if (!symbolName || !symbolAddress )
      return false;

    return m_ExecutionContext->addSymbol(symbolName,  symbolAddress);
  }

  void* Interpreter::getAddressOfGlobal(const clang::NamedDecl* D,
                                        bool* fromJIT /*=0*/) const {
    // Return a symbol's address, and whether it was jitted.
    std::string mangledName;
    mangleName(D, mangledName);
    return getAddressOfGlobal(mangledName.c_str(), fromJIT);
  }

  void* Interpreter::getAddressOfGlobal(const char* SymName,
                                        bool* fromJIT /*=0*/) const {
    // Return a symbol's address, and whether it was jitted.
    llvm::Module* module = m_IncrParser->getCodeGenerator()->GetModule();
    return m_ExecutionContext->getAddressOfGlobal(module, SymName, fromJIT);
  }

} // namespace cling
