// Copyright (c) 2012 Havok. All rights reserved. This file is distributed under the terms
// and conditions defined in file 'LICENSE.txt', which is part of this source code package.

#pragma warning(push,0)
	#include <llvm/Support/Host.h>
	#include <llvm/Support/ManagedStatic.h>
	#include <llvm/Support/CommandLine.h>
	#include <llvm/Support/Path.h>

	#include <clang/Frontend/Utils.h>
	#include <clang/Basic/DiagnosticOptions.h>
	#include <clang/Frontend/FrontendDiagnostic.h>
	#include <clang/Frontend/TextDiagnosticPrinter.h>
	#include <clang/Lex/Preprocessor.h>
	#include <clang/Parse/ParseAST.h>
	#include <clang/Basic/TargetInfo.h>
	#include <clang/Basic/FileManager.h>
	#include <clang/Lex/HeaderSearch.h>
	#include <clang/Lex/ModuleLoader.h>
	#include <clang/Sema/SemaDiagnostic.h>
#pragma warning(pop)

#include <iostream>
#include "extract.h"

#ifdef _WIN32
#include <Shlwapi.h>
static inline bool s_fileNameMatch(const char* path, const char* pattern)
{
	static char path_s[MAX_PATH]; // static storage
	strcpy(path_s, path);
	PathStripPath(path_s);
	return (PathMatchSpec(path_s, pattern) == TRUE);
}
#undef GetCurrentDirectory // remove annoying define from windows header
#else
#include <fnmatch.h>
#if defined(__APPLE__)
#include <libgen.h>
#endif
static inline bool s_fileNameMatch(const char* path, const char* pattern)
{
	const char* base = basename( const_cast<char*>(path) );
	return (fnmatch(pattern, base, 0) == 0);
}
#endif

namespace Havok
{
	// Preprocessor callbacks used to exclude specific included files (with #include)
	// When the preprocessor processes a file, it will generate callbacks to this object
	// on various events, when an inclusion directive is detected we will simply look
	// it up in our set of excluded inclusion, and if something matches we will basically
	// override that with an empty buffer.
	class FilenamePatternExcluder : public clang::PPCallbacks
	{
		public:

			FilenamePatternExcluder(clang::Preprocessor& preprocessor, clang::SourceManager& sourceManager)
				: PPCallbacks(), m_sourceManager(sourceManager), m_preprocessor(preprocessor)
			{}

			~FilenamePatternExcluder()
			{}

			void addExcludedPattern(const std::string& str)
			{
				m_excludedPatterns.push_back(str);
			}

			virtual void InclusionDirective(
				SourceLocation,
				const Token&,
				StringRef fileName,
				bool,
				CharSourceRange,
				const FileEntry* file,
				StringRef,
				StringRef,
				const Module*)
			{
				m_preprocessor.SetSuppressIncludeNotFoundError(false);
				for(unsigned int i = 0; i < m_excludedPatterns.size(); ++i)
				{
					if(s_fileNameMatch(fileName.str().c_str(), m_excludedPatterns[i].c_str()))
					{
						if(file)
						{
							// file was found
							m_sourceManager.overrideFileContents(file, llvm::MemoryBuffer::getNewMemBuffer(0), false);
						}
						else
						{
							// file was not found (but as it matches one of the excluded patterns we ignore it anyway)
							m_preprocessor.SetSuppressIncludeNotFoundError(true);
						}
						break;
					}
				}
			}

		protected:

			// Included file patterns that will be skipped, these string should contain
			// OS-style wildcards to exclude sets of files based on their name.
			std::vector<std::string> m_excludedPatterns;

			// Source manager used to exclude all the specified inclusions.
			clang::SourceManager& m_sourceManager;

			// Preprocessor used during parsing of the source
			clang::Preprocessor& m_preprocessor;

		private:
			FilenamePatternExcluder& operator=(const FilenamePatternExcluder& other);
	};
}

static llvm::cl::list<std::string> o_cppDefines(llvm::cl::ZeroOrMore, "D", llvm::cl::desc("Predefined preprocessor constants"), llvm::cl::value_desc("value") ); // Predefined constants
static llvm::cl::list<std::string> o_includePath(llvm::cl::ZeroOrMore,"I", llvm::cl::desc("Add to the include path"), llvm::cl::value_desc("dirname") ); // Include path directories
static llvm::cl::list<std::string> o_passAttributes(llvm::cl::ZeroOrMore, "A", llvm::cl::desc("Attributes to pass through to database"), llvm::cl::value_desc("token")); // Output file
static llvm::cl::list<std::string> o_forceInclude(llvm::cl::ZeroOrMore, "include", llvm::cl::desc("Include before processing"), llvm::cl::value_desc("filename") ); // File included at the beginning of the master file
static llvm::cl::list<std::string> o_excludeFilenames(llvm::cl::ZeroOrMore, "exclude", llvm::cl::desc("Files to exclude from parsing")); // Files excluded when encountered in an #include directive
static llvm::cl::list<std::string> o_excludeFilenamePatterns(llvm::cl::ZeroOrMore, "exclude-pattern", llvm::cl::desc("File name patterns to use when excluding additional files")); // File name patterns used to exclude additional files encountered in #include directives
static llvm::cl::list<std::string> o_inputFilenames(llvm::cl::ZeroOrMore, llvm::cl::Positional, llvm::cl::desc("<Input files>")); // Input files
static llvm::cl::opt<std::string> o_resourceDir(llvm::cl::Optional, "resource-dir", llvm::cl::desc("Directory containing standard LLVM includes"), llvm::cl::value_desc("dirname") ); // Directory containing standard LLVM includes
static llvm::cl::opt<std::string> o_outputFilename(llvm::cl::Required, "o", llvm::cl::desc("Output File (required)")); // Output file

int main(int argc, char **argv)
{
	int exitStatus;

	 //_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_DELAY_FREE_MEM_DF | _CRTDBG_CHECK_EVERY_1024_DF | _CRTDBG_LEAK_CHECK_DF );
	llvm::cl::ParseCommandLineOptions(argc, argv, "Help Text Here");
	llvm::MemoryBuffer* emptyMemoryBuffer = llvm::MemoryBuffer::getNewMemBuffer(0, "emptyMemoryBuffer");
	{
        llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> DO(new clang::DiagnosticOptions());
		clang::TextDiagnosticPrinter diagnosticConsumer(llvm::errs(), DO.get(), false);
		llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diagnosticIDs(new clang::DiagnosticIDs());
        llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnosticsEngine(new clang::DiagnosticsEngine(diagnosticIDs, DO.get(), &diagnosticConsumer, false));
		// ignored warnings
		//diagnosticsEngine.setDiagnosticMapping(clang::diag::warn_undefined_internal, clang::diag::MAP_IGNORE, clang::SourceLocation()); //-Wno-undefined-internal

        std::shared_ptr<clang::TargetOptions> targetOptions(new clang::TargetOptions());
		targetOptions->Triple = llvm::sys::getDefaultTargetTriple();
		llvm::IntrusiveRefCntPtr<clang::TargetInfo> targetInfo(clang::TargetInfo::CreateTargetInfo(*diagnosticsEngine, targetOptions ));
		clang::FileSystemOptions filesystemOptions;
		clang::FileManager fileManager(filesystemOptions);
		clang::SourceManager sourceManager(*diagnosticsEngine, fileManager);

		llvm::IntrusiveRefCntPtr<clang::HeaderSearchOptions> HSOpts(new clang::HeaderSearchOptions());
        //Havok::ModuleLoader moduleLoader; apparently not needed anymore
        // as we're going to use clang::CompilerInstance moduleLoader directly

        clang::CompilerInstance moduleLoader;
		clang::LangOptions langOptions;
		langOptions.CPlusPlus = 1;
		langOptions.CPlusPlus11 = 0;
		langOptions.Bool = 1;
		langOptions.ConstStrings = 1;
		langOptions.AssumeSaneOperatorNew = 1;
		langOptions.ImplicitInt = 0;
		langOptions.ElideConstructors = 0;

		clang::HeaderSearch headerSearch(HSOpts, sourceManager, *diagnosticsEngine, langOptions, targetInfo.get());

		llvm::IntrusiveRefCntPtr<clang::PreprocessorOptions> PPOpts (new clang::PreprocessorOptions());
        llvm::IntrusiveRefCntPtr<clang::Preprocessor> preprocessor (new clang::Preprocessor(PPOpts, *diagnosticsEngine, langOptions, sourceManager, headerSearch, moduleLoader));

        preprocessor->Initialize(*targetInfo);

        diagnosticConsumer.BeginSourceFile (langOptions, preprocessor.get());

		Havok::FilenamePatternExcluder* filenamePatternExcluder = new Havok::FilenamePatternExcluder(*preprocessor.get(), sourceManager);
		preprocessor->addPPCallbacks(filenamePatternExcluder); // the preprocessor is now owner of the FilenamePatternExcluder
		clang::HeaderSearchOptions headerSearchOptions;
		clang::FrontendOptions frontendOptions;

		std::string errorInfo;
		llvm::raw_fd_ostream outstream(o_outputFilename.c_str(), errorInfo, llvm::sys::fs::OpenFlags::F_None);
		{
			// Gather input files into a memory
			std::string mainFileText;
			llvm::raw_string_ostream stream(mainFileText);

			// Print the LLVMClangParser working directory
            {
                llvm::SmallString<128> cwd;
                llvm::sys::fs::current_path (cwd);
                outstream << "InvocationWorkingDirectory( path='" << cwd.c_str() << "' )\n";
            }

			// -D
			for( llvm::cl::list<std::string>::iterator iter = o_cppDefines.begin(), end = o_cppDefines.end(); iter != end; ++iter )
			{
				std::string::size_type index = iter->find_first_of('=');
				std::string macro, value;
				if(index != std::string::npos)
				{
					macro = iter->substr(0, index);
					value = iter->substr(index+1, std::string::npos);
				}
				else
				{
					macro = (*iter);
				}

				stream << "#define " << macro << ' ' << value << '\n';
				outstream << "InvocationDefine( name='" << macro << "', value='" << value << "' )\n";
			}

			// -I
			for( std::vector<std::string>::iterator iter = o_includePath.begin(), end = o_includePath.end(); iter != end; ++iter )
			{
				headerSearchOptions.AddPath(*iter, clang::frontend::Angled, true, false);
				outstream << "InvocationIncludePath( path='" << *iter << "' )\n";
			}

			// -exclude
			{
				// Do not free buffers associated with the compiler invocation
				PPOpts->RetainRemappedFileBuffers = true;
				for( std::vector<std::string>::iterator iter = o_excludeFilenames.begin(), end = o_excludeFilenames.end(); iter != end; ++iter )
				{
					PPOpts->addRemappedFile(iter->c_str(), emptyMemoryBuffer);
				}
			}

			// -exclude-pattern
			{
				for( std::vector<std::string>::iterator iter = o_excludeFilenamePatterns.begin(), end = o_excludeFilenamePatterns.end(); iter != end; ++iter )
				{
					filenamePatternExcluder->addExcludedPattern(*iter);
				}
			}

			// -A
			for( llvm::cl::list<std::string>::iterator iter = o_passAttributes.begin(), end = o_passAttributes.end(); iter != end; ++iter )
			{
				std::string::size_type index = iter->find_first_of('=');
				std::string macro, value;
				if(index != std::string::npos)
				{
					macro = iter->substr(0, index);
					value = iter->substr(index+1, std::string::npos);
				}
				else
				{
					macro = (*iter);
				}

				outstream << "InvocationAttribute( name='" << macro << "', value='" << value << "' )\n";
			}

			for( std::vector<std::string>::iterator iter = o_forceInclude.begin(), end = o_forceInclude.end(); iter != end; ++iter )
			{
				stream << "#include<" << iter->c_str() << ">\n";
				outstream << "InvocationForceInclude( path='" << iter->c_str() << "' )\n";
			}
			for( std::vector<std::string>::iterator iter = o_inputFilenames.begin(), end = o_inputFilenames.end(); iter != end; ++iter )
			{
				stream << "#include <" << iter->c_str() << ">\n";
				outstream << "InvocationInput( path='" << iter->c_str() << "' )\n";
			}
			stream.flush();

			llvm::MemoryBuffer* mainBuf = llvm::MemoryBuffer::getMemBufferCopy( llvm::StringRef(mainFileText.c_str(), mainFileText.size()), "masterInputFile" );
			sourceManager.setMainFileID(sourceManager.createFileID(mainBuf));
		}
		std::string resourceDir = o_resourceDir;
		if(!resourceDir.empty())
		{
			resourceDir += "/include";
			headerSearchOptions.AddPath(resourceDir, clang::frontend::System, false, false);
		}

		clang::InitializePreprocessor( *preprocessor.get(), *PPOpts, frontendOptions);

		Havok::ExtractASTConsumer consumer(outstream);
		clang::IdentifierTable identifierTable(langOptions);
		clang::SelectorTable selectorTable;
		clang::Builtin::Context builtinContext;

		llvm::IntrusiveRefCntPtr<clang::ASTContext> astcontext(new clang::ASTContext(langOptions, sourceManager, identifierTable, selectorTable, builtinContext));

        astcontext->InitBuiltinTypes(*targetInfo.get());

		#ifdef _DEBUG
			outstream.SetUnbuffered();
		#endif
		clang::ParseAST(*preprocessor.get(), &consumer, *astcontext.get());
        diagnosticConsumer.EndSourceFile();
		exitStatus = diagnosticsEngine->hasErrorOccurred() ? 1 : 0;
		if(exitStatus == 0)
		{
			// AST parsing succeeded, proceed with declaration dumping
			consumer.dumpAllDeclarations();
		}
		else
		{
			outstream << "## The diagnostic engine returned an error during code parsing.\n";
		}

		outstream.flush();
	}
	delete emptyMemoryBuffer;
	llvm::llvm_shutdown();

	return exitStatus;
}
