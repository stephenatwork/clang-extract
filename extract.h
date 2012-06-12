// Copyright (c) 2012 Havok. All rights reserved. This file is distributed under the terms
// and conditions defined in file 'LICENSE.txt', which is part of this source code package.

#ifndef RAW_DUMP_H
#define RAW_DUMP_H

#pragma warning(push,0)
	#include "clang/AST/AST.h"
	#include "clang/Sema/SemaConsumer.h"
	#include "clang/Frontend/CompilerInstance.h"
#pragma warning(pop)

namespace Havok 
{
	using namespace clang;

	/// Havok AST consumer class
	class ExtractASTConsumer : public SemaConsumer
	{
		public:

			ExtractASTConsumer(llvm::raw_ostream& os);
			virtual ~ExtractASTConsumer();
			virtual void Initialize(ASTContext& context);
			virtual void InitializeSema(Sema& sema);
			// Base callback coming from LLVM (used to accumulate all declarations)
			virtual void HandleTopLevelDecl(DeclGroupRef DG);

			enum DumpBits
			{
				DUMP_DEFAULT = 0,
				DUMP_VERBOSE = 1,
				DUMP_FUNCTIONS = 2
			};

			// delayed dumping of all declarations
			void dumpAllDeclarations();

		protected:

			// Basic function to fix declarations of C++ special methods (e.g. copy constructor) in classes
			void declareImplicitMethods(Decl* declIn);
			// Basic function that dumps a generic declaration
			int dumpDecl_i(const Decl* declIn);
			// Functions used to dump the type referred by a declaration, the type is what we use to identify an entity
			int dumpType_i(QualType qualTypeIn, int scopeId = -1);
			int dumpSimpleType_i(QualType qualTypeIn, int scopeId = -1);
			int dumpNonQualifiedType_i(const Type* typeIn, int scopeId = -1);
			int dumpNonQualifiedSimpleType_i(const Type* typeIn, int scopeId = -1);
			int dumpTemplateInstantiationType_i(const TemplateSpecializationType* templateSpecializationType);
			int dumpTemplateSpecializationType_i(const ClassTemplateSpecializationDecl* classTemplateSpecializationDecl, int scopeId);
			// More dumping functions
			int dumpScope_i(const Decl* decl);
			void dumpSpecifiersRecursive_i(const NestedNameSpecifier* nestedNameSpecifier);
			void dumpTypeSpecifiers_i(const Type* type);
			void dumpTagDefinition_i(const TagDecl* tagDecl, int recordId);
			void dumpDeclContext_i(const DeclContext* context);
			void dumpNamespace_i(const NamespaceDecl* namespaceDecl);
			void dumpTemplateClass_i(const ClassTemplateDecl* classTemplateDecl);
			void dumpTemplateClassSpecialization_i(const ClassTemplateSpecializationDecl* classTemplateSpecializationDecl);
			void dumpTemplateParameterList_i(const TemplateParameterList* paramList, int templateId);
			void dumpTemplateArgumentList_i(const TemplateArgument* argv, int argc, int templateId);
			// Functions used for lookups in the internal structures
			int findTypeId_i(const Type* typeIn);
			int findConstTypeId_i(int typeId);
			int getTypeId_i(const Type* typeIn);
			int getNamespaceId_i(const NamespaceDecl* namespaceDecl);
			// More utility functions
			void addOrReplaceSpecializationTypeParameterTypes_i(const TemplateParameterList* paramList);
			
			// Some types of dump entry are routed through this class to unify default handling.
			class DumpEntry
			{
			public:
				enum EntryType
				{
					// Only these types are currently supported.
					ENTRY_METHOD,
					ENTRY_CONSTRUCTOR,
					ENTRY_DESTRUCTOR,
					ENTRY_FIELD,

					NUM_ENTRIES
				};

				// Construct a new entry (outputs name to stream)
				DumpEntry(llvm::raw_ostream& os, EntryType t);

				// Output a key value pair when the value isn't the default value.
				void dumpKeyValuePair(const char* key, const char* val);
				void dumpKeyValuePair(const char* key, int i);
				void dumpKeyValuePair(const char* key, bool b);
				void dumpKeyValuePair(const char* key, AccessSpecifier as);

				// Output a closing parenthesis and newline.
				void finishEntry();

				// Output a series of entries summarizing the defaults.
				static void dumpDefaultEntries(llvm::raw_ostream& os);

			protected:
				// Output a comma, if necessary.
				void checkOutputComma();
			
			protected:
				EntryType m_entryType;
				/// Used to put commas in the correct position.
				bool m_hasOutputKeyValuePair;
				llvm::raw_ostream& m_os;

			private:
				// Private and unimplemented.
				DumpEntry(DumpEntry& other);
				DumpEntry& operator=(DumpEntry& other);
			};

			
			// List of declarations, declarations are collected and then dumped in a second phase
			std::list<const clang::Decl*> m_decls;

			// Output stream
			llvm::raw_ostream& m_os;

			// AST context used during consumption of the AST
			ASTContext* m_context;

			// Map of know types (types are used to identify declarations of the same entity)
			typedef llvm::DenseMap<const Type*, int> KnownTypeMap;
			KnownTypeMap m_knownTypes;

			// Maps a type id to the id of a const version of that type.
			typedef llvm::DenseMap<int, int> ConstTypeIdMap;
			ConstTypeIdMap m_constTypeIdMap;

			// Map of known namespaces (used to identify a certain namespace as scope)
			typedef llvm::DenseMap<const NamespaceDecl*, int> KnownNamespacesMap;
			KnownNamespacesMap m_knownNamespaces;

			// Map of known files (used to identify a certain file, considering it the largest scope a declaration can be in).
			typedef llvm::DenseMap<FileID, int> KnownFilesMap;
			KnownFilesMap m_knownFiles;

			// Map of known template template parameters (used to indentify a template template parameter)
			typedef llvm::DenseMap<const Decl*, int> KnownTemplateTemplateParamMap;
			KnownTemplateTemplateParamMap m_knowTemplateTemplateParams;

			// Allocator object for entity identifiers
			class UidAllocator
			{
				public:
					UidAllocator() : m_uidNext(1) {}
					int alloc() { return m_uidNext++; }
				private:
					int m_uidNext;
			};
			UidAllocator m_uid;
			
			// Dumping configuration bits
			DumpBits m_dumpBits;

			// clang Sema instance used to perform semantic analysis
			Sema* m_sema;

		private:
			
			ExtractASTConsumer& operator=(ExtractASTConsumer& other);
	}; 
}

#endif //RAW_DUMP_H
