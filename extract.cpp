
// Copyright (c) 2012 Havok. All rights reserved. This file is distributed under the terms
// and conditions defined in file 'LICENSE.txt', which is part of this source code package.

#include "extract.h"
#include <cstdio>

#pragma warning(push,0)
	#include <clang/Basic/SourceManager.h>
	#include <clang/Sema/Sema.h>
#pragma warning(pop)

using namespace clang;

#define printf poisoned

// ----------------------- Static Utility Functions ------------------------- //

static SourceLocation s_getExpansionLoc(const SourceLocation& loc, SourceManager& sm)
{
	assert(loc.isValid() && "invalid location for declaration");
	if (loc.isFileID()) {
		return loc;
	}

	SourceLocation expLoc = sm.getExpansionLoc(loc);
	return s_getExpansionLoc(expLoc, sm);
}

static const char* s_getFileName(std::string& buf, const SourceLocation& loc, SourceManager& sm)
{
	assert(loc.isFileID() && "not a file location");
	PresumedLoc pLoc = sm.getPresumedLoc(loc);
	assert(pLoc.isValid() && "invalid presumed location for declaration");

	buf = pLoc.getFilename();
	for( std::string::iterator it = buf.begin(), end = buf.end(); it != end; ++ it)
	{
		if(*it=='\\') *it = '/';
	}
	return buf.c_str();
}

static FileID s_getFileId(const SourceLocation& loc, SourceManager& sm)
{
	assert(loc.isFileID() && "not a file location");
	FileID ret = sm.getFileID(loc);
	assert(!ret.isInvalid() && "invalid file id for declaration");
	
	return ret;
}

static void s_printName(llvm::raw_ostream& os, const NamedDecl* decl)
{
	os << ", name='";
	decl->printName(os);
	os << "'";
}

static void s_printRecordFlags(llvm::raw_ostream& os, const CXXRecordDecl* decl)
{
	bool isPolymorphic = false;
	bool isAbstract = false;
	if(decl->hasDefinition())
	{
		// the class contains or inherits a virtual function
		isPolymorphic = decl->isPolymorphic();
		// the class contains or inherits a pure virtual function
		isAbstract = decl->isAbstract();
	}
	os << ", polymorphic=" << (isPolymorphic ? "True" : "False");
	os << ", abstract=" << (isAbstract ? "True" : "False");
}

inline static void s_printDefaultRecordFlags(llvm::raw_ostream& os)
{
	os << ", polymorphic=False, abstract=False";
}

static void s_printAnnotations(llvm::raw_ostream& os, const Decl* decl, int declId)
{
	if(decl->hasAttr<AnnotateAttr>())
	{
		const AttrVec& attrVec = decl->getAttrs();
		for( specific_attr_iterator<AnnotateAttr> iterator = specific_attr_begin<AnnotateAttr>(attrVec), end_iterator = specific_attr_end<AnnotateAttr>(attrVec);
			iterator != end_iterator; 
			++iterator )
		{
			os << "Annotation( refid=" << declId << ", text=\"\"\"";
			os << iterator->getAnnotation();
			os << "\"\"\" )\n";
		}
	}
}

static const Type* s_getTrueType(const Type* type)
{
	if(const SubstTemplateTypeParmType* substType = type->getAs<SubstTemplateTypeParmType>())
	{
		type = substType->getReplacementType().getTypePtr();
	}
	if(const ElaboratedType* elabType = type->getAs<ElaboratedType>())
	{
		return elabType->getNamedType().getTypePtr();
	}

	return type;
}

static const ClassTemplateDecl* s_getClassTemplateDefinition(const ClassTemplateDecl* classTemplateDecl)
{
	const ClassTemplateDecl* current = NULL;
	for( ClassTemplateDecl::redecl_iterator it = classTemplateDecl->redecls_begin();
		it != classTemplateDecl->redecls_end();
		++it )
	{
		current = dyn_cast<ClassTemplateDecl>(*it);
		assert(current && "invalid redeclaration of the class template declaration");
		if(current->isThisDeclarationADefinition())
			return current;
	}
	return NULL;
}

// ------------------- ExtractASTConsumer Implementation -------------------- //

// Initialize the database object with its global state. Each consumer object is only expected to be used once
Havok::ExtractASTConsumer::ExtractASTConsumer(llvm::raw_ostream& os)
	: m_context(0), m_sema(0), m_os(os), m_dumpBits( DUMP_DEFAULT /*DUMP_FUNCTIONS*/ )
{
}

Havok::ExtractASTConsumer::~ExtractASTConsumer()
{
}

// Called by the parser at the start of the process
void Havok::ExtractASTConsumer::Initialize(ASTContext &Context)
{
	// Remember the context so we can use it to find file names and locations
	m_context = &Context;
}

void Havok::ExtractASTConsumer::InitializeSema(Sema& sema)
{
	// Remember the sema instance so we can use it to perform semantic analysis
	m_sema = &sema;
}

void Havok::ExtractASTConsumer::declareImplicitMethods(Decl* declIn)
{
	if( !isa<CXXRecordDecl>(declIn) &&
		!isa<NamespaceDecl>(declIn) )
		return;

	if( NamespaceDecl* namespaceDecl = dyn_cast<NamespaceDecl>(declIn) )
	{
		DeclContext::decl_iterator it;
		for(it = namespaceDecl->decls_begin(); it != namespaceDecl->decls_end(); ++it)
		{
			declareImplicitMethods(*it);
		}
	}
	else // it must be a CXXRecordDecl
	{
		CXXRecordDecl* recordDecl = dyn_cast<CXXRecordDecl>(declIn);
		assert(recordDecl && "The declaration should be a record declaration.");
		
		if(!recordDecl->isCompleteDefinition())
			return;

		m_sema->ForceDeclarationOfImplicitMembers(recordDecl);

		typedef CXXRecordDecl::specific_decl_iterator<CXXRecordDecl> NestedRecordIterator;
		for( NestedRecordIterator fi(recordDecl->decls_begin()), fe(recordDecl->decls_end()); fi != fe; ++fi )
		{
			declareImplicitMethods( *fi );
		}
	}
}

// Called by the parser for each top-level declaration encountered.
// This includes top-level classes, namespaces, typedefs, constants, enums, functions
// in every included header file. Contained elements are children of a top-level element.
// Every element that is parsed is in here somewhere
bool Havok::ExtractASTConsumer::HandleTopLevelDecl(DeclGroupRef declGroupIn)
{
	// If there are multiple declarations with the same semantic type in the same scope they are
	// returned as a group. [ e.g. class A { ... } B; ] Usually each group only contains one declaration.
	for (DeclGroupRef::iterator iter = declGroupIn.begin(), iterEnd = declGroupIn.end(); iter != iterEnd; ++iter)
	{
		declareImplicitMethods(*iter);
		m_decls.push_back(*iter);
	}

	return true;
}

void Havok::ExtractASTConsumer::dumpAllDeclarations()
{
	DumpEntry::dumpDefaultEntries(m_os);

	for( std::list<const clang::Decl*>::const_iterator it = m_decls.begin();
	     it != m_decls.end();
	     ++it )
	{
		dumpDecl_i(*it);
	}
}


static const char* entryNames[] =
{
	"Method",
	"Constructor",
	"Destructor",
	"Field",
	NULL
};

struct KeyValuePair
{
	const char* m_key;
	const char* m_value;
};

static const KeyValuePair methodDefaults[] =
{
	{"static", "False"},
	{"const", "False"},
	{"isCopyAssignment", "False"},
	{"isImplicit", "False"},
	{"access", "\"public\""},
	{"numParamDefaults", "0"},
	{NULL, NULL}
};

static const KeyValuePair constructorDefaults[] =
{
	{"static", "False"},
	{"const", "False"},
	{"isCopyAssignment", "False"},
	{"isImplicit", "False"},
	{"isCopyConstructor", "False"},
	{"isDefaultConstructor", "False"},
	{"access", "\"public\""},
	{"numParamDefaults", "0"},
	{NULL, NULL}
};

static const KeyValuePair* destructorDefaults = methodDefaults;

static const KeyValuePair fieldDefaults[] =
{
	{"access", "\"public\""},
	{NULL, NULL}
};


static const KeyValuePair* defaults[] =
{
	methodDefaults,
	constructorDefaults,
	destructorDefaults,
	fieldDefaults,
	NULL,
};

Havok::ExtractASTConsumer::DumpEntry::DumpEntry(llvm::raw_ostream& os, EntryType et)
	: m_entryType(et)
	, m_hasOutputKeyValuePair(false)
	, m_os(os)
{
	assert(0 <= et);
	assert(et < NUM_ENTRIES);
	// Check the entry names array has the correct size.
	assert((sizeof(entryNames) / sizeof(const char*)) == NUM_ENTRIES + 1);
	assert((sizeof(defaults) / sizeof(const KeyValuePair*)) == NUM_ENTRIES + 1);
	m_os << entryNames[et] << "( ";
}

void Havok::ExtractASTConsumer::DumpEntry::checkOutputComma()
{
	if (m_hasOutputKeyValuePair)
	{
		m_os << ", ";
	}
	m_hasOutputKeyValuePair = true;
}

void Havok::ExtractASTConsumer::DumpEntry::finishEntry()
{
	m_os << " )\n";
}

void Havok::ExtractASTConsumer::DumpEntry::dumpKeyValuePair(const char* key, const char* val)
{
	const KeyValuePair* kvpairs = defaults[m_entryType];
	if (kvpairs != NULL)
	{
		while (kvpairs->m_key != NULL)
		{
			if (strcmp(kvpairs->m_key, key) == 0)
			{
				if (strcmp(kvpairs->m_value, val) == 0)
				{
					return;
				}
				else
				{
					break;
				}
			}
			++kvpairs;
		}
	}
	checkOutputComma();
	m_os << key << "=" << val;
}

void Havok::ExtractASTConsumer::DumpEntry::dumpKeyValuePair(const char* key, int i)
{
	char buf[10];
	snprintf(buf, sizeof(buf), "%d", i);
	dumpKeyValuePair(key, buf);
}

void Havok::ExtractASTConsumer::DumpEntry::dumpKeyValuePair(const char* key, bool b)
{
	dumpKeyValuePair(key, (b ? "True" : "False"));
}

void Havok::ExtractASTConsumer::DumpEntry::dumpKeyValuePair(const char* key, AccessSpecifier as)
{
	switch(as)
	{
	case AS_private:
		dumpKeyValuePair(key, "\"private\"");
		break;
	case AS_protected:
		dumpKeyValuePair(key, "\"protected\"");
		break;
	case AS_public:
		dumpKeyValuePair(key, "\"public\"");
		break;
	default:
		assert(false);
	}
}

void Havok::ExtractASTConsumer::DumpEntry::dumpDefaultEntries(llvm::raw_ostream& os)
{
	for (int i = 0; i < NUM_ENTRIES; ++i)
	{
		os << "DefaultsFor" << entryNames[i] << "( ";
		const KeyValuePair* kvpairs = defaults[i];
		if (kvpairs->m_key != NULL)
		{
			os << kvpairs->m_key << "=" << kvpairs->m_value;
			++kvpairs;
		}
		while (kvpairs->m_key != NULL)
		{
			os << ", " << kvpairs->m_key << "=" << kvpairs->m_value;
			++kvpairs;
		}
		os << " )\n";
	}
}

int Havok::ExtractASTConsumer::dumpDecl_i(const Decl* declIn)
{
	if( dyn_cast<AccessSpecDecl>(declIn) )
	{
		// Skipped silently
	}
	else if ( const ClassTemplateSpecializationDecl* classTemplateSpecializationDecl = dyn_cast<ClassTemplateSpecializationDecl>(declIn))
	{
		dumpTemplateClassSpecialization_i(classTemplateSpecializationDecl);
	}
	else if( const ClassTemplateDecl* classTemplateDecl = dyn_cast<ClassTemplateDecl>(declIn) )
	{
		// templates do not represent types, but we dump a representation for the template itself
		// (and not for its instances).
		dumpTemplateClass_i(classTemplateDecl);
	}
	else if( const TagDecl* tagDecl = dyn_cast<TagDecl>(declIn) )
	{
		const TagDecl* tagDef = tagDecl->getDefinition();
		int scopeId = dumpScope_i(tagDef != NULL ? tagDef : tagDecl);
		int typeId = dumpType_i(m_context->getTagDeclType(tagDecl), scopeId);
		if( tagDecl->isCompleteDefinition() )
		{
			s_printAnnotations(m_os, tagDecl, typeId);
			dumpTagDefinition_i(tagDecl, typeId);
		}
	}
	else if( const TypedefDecl* typedefDecl = dyn_cast<TypedefDecl>(declIn) )
	{
		int scopeId = dumpScope_i(typedefDecl);
		dumpType_i(m_context->getTypedefType(typedefDecl), scopeId);
	}
	else if( const FieldDecl* fieldDecl = dyn_cast<FieldDecl>(declIn) )
	{
		int tid = dumpType_i( fieldDecl->getType() );
		// the containing record type has already been dumped
		int recId = getTypeId_i( m_context->getRecordType(fieldDecl->getParent()).getTypePtr() );
		int fieldId = m_uid.alloc();
		DumpEntry e(m_os, DumpEntry::ENTRY_FIELD);
		e.dumpKeyValuePair("id", fieldId);
		e.dumpKeyValuePair("recordid", recId);
		e.dumpKeyValuePair("typeid", tid);
		e.dumpKeyValuePair("access", fieldDecl->getAccess());
		s_printName(m_os, fieldDecl);
		e.finishEntry();
		s_printAnnotations(m_os, fieldDecl, fieldId);
	}
	else if( const CXXMethodDecl* methodDecl = dyn_cast<CXXMethodDecl>(declIn) )
	{
		// only handling non-template functions
		FunctionDecl::TemplatedKind tk = methodDecl->getTemplatedKind();
		const CXXRecordDecl* parentRecord = methodDecl->getParent();
		const ClassTemplateDecl* templateRecord = parentRecord->getDescribedClassTemplate();
		// only dumping information for non-template functions of non-template records or
		// non-template functions of template records (but not non-template functions
		// of template record specializations or instantiations).
		if( methodDecl == methodDecl->getFirstDeclaration() &&
			tk == FunctionDecl::TK_NonTemplate && // non template function
			( templateRecord != NULL || // function from a template class declaration
			  !isa<ClassTemplateSpecializationDecl>(parentRecord) ) ) // not a template instantiation or specialization
		{
			int tid = dumpType_i( methodDecl->getType() );
			// the containing record type has already been dumped
			int recId = getTypeId_i( m_context->getRecordType(parentRecord).getTypePtr() );
			int methodId = m_uid.alloc();

			const CXXConstructorDecl* constructorDecl = dyn_cast<CXXConstructorDecl>(methodDecl);
			const CXXDestructorDecl* destructorDecl = dyn_cast<CXXDestructorDecl>(methodDecl);
			DumpEntry::EntryType et;

			if (constructorDecl)
			{
				et = DumpEntry::ENTRY_CONSTRUCTOR;
			}
			else if (destructorDecl)
			{
				et = DumpEntry::ENTRY_DESTRUCTOR;
			}
			else
			{
				et = DumpEntry::ENTRY_METHOD;
			}

			DumpEntry e(m_os, et);

			e.dumpKeyValuePair("id", methodId);
			e.dumpKeyValuePair("recordid", recId);
			e.dumpKeyValuePair("typeid", tid);
			e.dumpKeyValuePair("static", methodDecl->isStatic());
			e.dumpKeyValuePair("const", (bool)((methodDecl->getTypeQualifiers() & Qualifiers::Const)));
			{
				// Determine if this was explicitly declared by user, or auto-generated.
				bool implicitlyDeclared = false;
				bool defaultConstructor = false;
				bool copyConstructor = false;
				bool copyAssignment = false;

				if ( constructorDecl )
				{
					if (constructorDecl->isCopyConstructor())
					{
						copyConstructor = true;
						if (!parentRecord->hasUserDeclaredCopyConstructor())
						{
							implicitlyDeclared = true;
						}
					}
					// Any user declared constructor suppresses the auto-generation of a default constructor.
					else if (constructorDecl->isDefaultConstructor())
					{
						defaultConstructor = true;
						if (!parentRecord->hasUserDeclaredConstructor())
						{
							implicitlyDeclared = true;
						}
					}
					e.dumpKeyValuePair("isDefaultConstructor", defaultConstructor);
					e.dumpKeyValuePair("isCopyConstructor", copyConstructor);
				}
				else if ( destructorDecl )
				{
					if (!parentRecord->hasUserDeclaredDestructor())
					{
						implicitlyDeclared = true;
					}
				}
				else if (methodDecl->isCopyAssignmentOperator())
				{
					copyAssignment = true;
					if(!parentRecord->hasUserDeclaredCopyAssignment())
					{
						implicitlyDeclared = true;
					}
				}
				e.dumpKeyValuePair("isCopyAssignment", copyAssignment);
				e.dumpKeyValuePair("isImplicit", implicitlyDeclared);
			}
			e.dumpKeyValuePair("access", methodDecl->getAccess());
			// We use this value as it has a more obvious default.
			e.dumpKeyValuePair("numParamDefaults", (int)(methodDecl->getNumParams() - methodDecl->getMinRequiredArguments()));
			s_printName(m_os, methodDecl);
			e.finishEntry();
			s_printAnnotations(m_os, methodDecl, methodId);
		}
	}
	else if( const EnumConstantDecl* enumConstantDecl = dyn_cast<EnumConstantDecl>(declIn) )
	{	
		// the enum has already been dumped
		int enumId = getTypeId_i( enumConstantDecl->getType().getTypePtr() );
		m_os << "EnumConstant( enumId=" << enumId;
		s_printName(m_os, enumConstantDecl);
		m_os << ", value='" << enumConstantDecl->getInitVal() << "' )\n";
		s_printAnnotations(m_os, enumConstantDecl, enumId);
	}
	else if( const NamespaceDecl* namespaceDecl = dyn_cast<NamespaceDecl>(declIn) )
	{
		// namespaces are not typed declarations, they have to be treated differently
		dumpNamespace_i(namespaceDecl);
	}
	else if( const VarDecl* varDecl = dyn_cast<VarDecl>(declIn) )
	{
		if( varDecl->isStaticDataMember() )
		{
			int typeId = dumpType_i(varDecl->getType());
			const RecordDecl* recordDecl = dyn_cast<RecordDecl>(varDecl->getDeclContext());
			assert(recordDecl && "static member is not in a record declaration");
			int recordId = getTypeId_i(m_context->getRecordType(recordDecl).getTypePtr());
			int fieldId = m_uid.alloc();
			m_os << "StaticField( id=" << fieldId << ", recordid=" << recordId << ", typeid=" << typeId;
			s_printName(m_os, varDecl);
			m_os << " )\n";
			s_printAnnotations(m_os, varDecl, fieldId);
		}
	}
	else if( m_dumpBits & DUMP_VERBOSE )
	{
		m_os << "### Skipped " << declIn->getDeclKindName();
		if( const NamedDecl* nd = dyn_cast<NamedDecl>(declIn) )
		{
			s_printName(m_os, nd);
		}
		m_os << '\n';
	}
	return -1;
}

int Havok::ExtractASTConsumer::dumpType_i(QualType qualTypeIn, int scopeId)
{
	const Type *const typeIn = qualTypeIn.getTypePtr();
	int id = dumpNonQualifiedType_i(typeIn, scopeId);

	if (qualTypeIn.getQualifiers() & Qualifiers::Const)
	{
		int retId = findConstTypeId_i(id);
		if (retId == -1)
		{
			retId = m_uid.alloc();
			m_constTypeIdMap[id] = retId;
			m_os << "ConstType( id=" << retId << ", typeid=" << id << ")\n";
		}
		id = retId;
	}
	return id;
}

int Havok::ExtractASTConsumer::dumpNonQualifiedType_i(const Type* typeIn, int scopeId)
{
	assert(typeIn && "invalid type specified");

	// Dump any name specifiers used to resolve this type (eg. A::B::Type)
	// Not all preceding types might have been dumped correctly, in case
	// of a template we might refer to an instance which has never been encountered
	// before, and now we are referring to a nested type:
	//
	// template <typename T> class C
	// {
	//     Struct S {};
	// };
	// class D
	// {
	//     C<int>::S m_s;
	// };
	dumpTypeSpecifiers_i(typeIn);

	return dumpNonQualifiedSimpleType_i(typeIn, scopeId);
}


int Havok::ExtractASTConsumer::dumpSimpleType_i(QualType qualTypeIn, int scopeId)
{
	const Type *const typeIn = qualTypeIn.getTypePtr();
	int id = dumpNonQualifiedSimpleType_i(typeIn, scopeId);

	if (qualTypeIn.getQualifiers() & Qualifiers::Const)
	{
		int retId = findConstTypeId_i(id);
		if (retId == -1)
		{
			retId = m_uid.alloc();
			m_constTypeIdMap[id] = retId;
			m_os << "ConstType( id=" << retId << ", typeid=" << id << ")\n";
		}
		id = retId;
	}
	return id;
}

int Havok::ExtractASTConsumer::dumpNonQualifiedSimpleType_i(const Type* typeIn, int scopeId)
{

	// clean the type from any sugar used to specify it in source code (elaborated types)
	typeIn = s_getTrueType(typeIn);
	
	{
		const TemplateSpecializationType* templateSpecializationType = typeIn->getAs<TemplateSpecializationType>();
		if( templateSpecializationType &&
			(typeIn->getAs<TypedefType>() == NULL) ) // not a typedef to a template specialization type
		{
			// this type is dumped in case of an explicit instantiation when this function 
			// is called from dumpTemplateClassSpecialization_i(), or in case of an
			// implicit instantiation when this function is called in a generic way to
			// dump a needed type.

			return dumpTemplateInstantiationType_i(templateSpecializationType);
		}
	}
	
	// seen this type before?
	int retId = findTypeId_i(typeIn);
	if(retId != -1)
	{
		return retId;
	}
	
	if( const TypedefType* bt = typeIn->getAs<TypedefType>() )
	{
		// sometimes TypedefTypes can also be casted to InjectedClassNameTypes, 
		// for this reason we need to handle this first.
		int tid = dumpType_i(bt->getDecl()->getUnderlyingType());
		retId = m_uid.alloc();
		m_os << "TypedefType( id=" << retId << ", typeid=" << tid;
		s_printName(m_os, bt->getDecl());
	}
	else if( const TemplateTypeParmType* bt = typeIn->getAs<TemplateTypeParmType>() )
	{
		// skipped, this is treated specially after calling this function
	}
	else if( const BuiltinType* bt = typeIn->getAs<BuiltinType>() )
	{
		retId = m_uid.alloc();
		m_os << "BuiltinType( id=" << retId << ", name='" << bt->getName(m_context->getPrintingPolicy()) << "'";
	}
	else if( const PointerType* bt = typeIn->getAs<PointerType>() )
	{
		int pt = dumpType_i(bt->getPointeeType());
		retId = m_uid.alloc();
		m_os << "PointerType( id=" << retId << ", typeid=" << pt;
	}
	else if( const ReferenceType* bt = typeIn->getAs<ReferenceType>() )
	{
		int pt = dumpType_i(bt->getPointeeType());
		retId = m_uid.alloc();
		m_os << "ReferenceType( id=" << retId << ", typeid=" << pt;
	}
	else if( const MemberPointerType* bt = typeIn->getAs<MemberPointerType>())
	{
		// C++ pointer to member (function or data)
		int pt = dumpType_i(bt->getPointeeType());
		int rt = dumpNonQualifiedType_i(bt->getClass());
		retId = m_uid.alloc();
		m_os << "MemberPointerType( id=" << retId << ", recordid=" << rt << ", typeid=" << pt;
	}
	else if( const RecordType* bt = typeIn->getAs<RecordType>() )
	{
		retId = m_uid.alloc();
		const CXXRecordDecl* decl = dyn_cast<CXXRecordDecl>(bt->getDecl());
		assert(decl && "retrieved declaration is not a CXX record declaration");
		m_os << "RecordType( id=" << retId;
		s_printName(m_os, decl);
		s_printRecordFlags(m_os, decl);
	}
	else if( const EnumType* bt = typeIn->getAs<EnumType>() )
	{
		retId = m_uid.alloc();
		m_os << "EnumType( id=" << retId;
		const NamedDecl* decl = bt->getDecl();
		s_printName(m_os, decl);
	}
	else if( const ConstantArrayType* bt = dyn_cast<ConstantArrayType>(typeIn) )
	{
		uint64_t sz = bt->getSize().getZExtValue();
		int pt = dumpType_i(bt->getElementType());
		retId = m_uid.alloc();
		m_os << "ConstantArrayType( id=" << retId << ", typeid=" << pt << ", count=" << int(sz);
	}
	else if( const ParenType* bt = typeIn->getAs<ParenType>() )
	{
		int pt = dumpType_i(bt->getInnerType());
		retId = m_uid.alloc();
		m_os << "ParenType( id=" << retId << ", typeid=" << pt;
	}
	else if( const FunctionProtoType* bt = typeIn->getAs<FunctionProtoType>() )
	{
		int resType = dumpType_i(bt->getResultType());
		const int numArgs = bt->getNumArgs();
		std::vector<int> paramTypes;
		for(int i = 0; i < numArgs; ++i)
		{
			paramTypes.push_back(dumpType_i(bt->getArgType(i)));
		}
		retId = m_uid.alloc();
		m_os << "FunctionProtoType( id=" << retId << ", rettypeid=" << resType << ", paramtypeids=[";
		for(unsigned int i = 0; i < paramTypes.size(); ++i)
		{
			m_os << paramTypes[i];
			if(i != paramTypes.size()-1)
				m_os << ',';
		}
		m_os << "]";
		m_os << ", isVariadic=" << (bt->isVariadic() ? "True" : "False");
	}
	else if( const ArrayType* bt = typeIn->getAsArrayTypeUnsafe() )
	{
		retId = m_uid.alloc();
		m_os << "BuiltinType( id=" << retId << ", name='" << "unsupported" << "'";

		// todo
// 		int eid = _dumpType( bt->getElementType().getTypePtr() );
// 		retId = m_uid.alloc();
// 		m_os << "ArrayType id='" << retId << "' elemId='" << eid << "' count='" << bt->getSizeExpr() ->getType().getTypePtr() << "'\n";
	}		
	else if( const DependentNameType* bt = typeIn->getAs<DependentNameType>() )
	{
		retId = m_uid.alloc();
		//int tid = _dumpType( bt->desugar().getSingleStepDesugaredType(*m_context).getTypePtr(), scopeId );
		m_os << "BuiltinType( id=" << retId << ", name='" << "unsupported" << "'";
		// todo
	}
	else if( const InjectedClassNameType* bt = typeIn->getAs<InjectedClassNameType>() )
	{
		// templates are handled using a different code path, this function should never
		// end up printing template class declaration information.
		assert(false && "this type is expected for template class declarations, which should not be handled by this function");
	}
	else
	{
		const char* name = typeIn->getTypeClassName();
		m_os << "###Type kind='" << name << "'\n";
		assert(0 && "Type not supported");
	}
	if(retId < 0)
	{
		// type was skipped (it is supported but we don't have to do anything)
		retId = m_uid.alloc();
	}
	else
	{
		if(scopeId >= 0)
			m_os << ", scopeid=" << scopeId;
		m_os << " )\n";
	}
	m_knownTypes[typeIn] = retId;
	return retId;
}

int Havok::ExtractASTConsumer::dumpTemplateInstantiationType_i(
	const TemplateSpecializationType* templateSpecializationType )
{
	// A template instantiation type associated with a certain template instantiation
	// class is not unique. We can have multiple Clang Types object that refer to the
	// same template instantiation. Even if they are not the same, we still have always
	// the same canonical type. We add the canonical type to the m_knownTypes map to
	// avoid the same template instantiation to be dumped again. The associated id is
	// the one associated with the template instantiation. When dumping members or
	// methods of a specific template instantiation, Clang will generate the address
	// of the canonical type as the parent type, thus the whole process works correctly.
	const Type* canonicalInstantiationType = templateSpecializationType->getCanonicalTypeInternal().getTypePtr();

	int retId = findTypeId_i(canonicalInstantiationType);
	if(retId == -1) // not found
	{
		TemplateName templateName = templateSpecializationType->getTemplateName();
		TemplateDecl* templateDecl = templateName.getAsTemplateDecl();
		assert(templateDecl && "could not retrieve template declaration");
		
		ClassTemplateDecl* classTemplateDecl = dyn_cast<ClassTemplateDecl>(templateDecl);
		const Decl* scopeDiscoveryDecl = NULL;
		const ClassTemplateSpecializationDecl* classTemplateInstantiationDecl = NULL;
		int templateId = -1;
		if(classTemplateDecl)
		{
			scopeDiscoveryDecl = classTemplateDecl;
			// retrieve corresponding declaration
			const ClassTemplateDecl::spec_iterator specBegin = const_cast<ClassTemplateDecl*>(classTemplateDecl)->spec_begin();
			const ClassTemplateDecl::spec_iterator specEnd = const_cast<ClassTemplateDecl*>(classTemplateDecl)->spec_end();
			for( ClassTemplateDecl::spec_iterator it = specBegin;
				it != specEnd;
				++it )
			{
				if( m_context->getRecordType(*it).getCanonicalType().getTypePtr() ==
					canonicalInstantiationType )
				{
					classTemplateInstantiationDecl = (*it);
					break;
				}
			}
			assert((classTemplateInstantiationDecl || templateSpecializationType->isDependentType()) && 
				"could not retrieve template specialization declaration for instantiation");
			// classTemplateInstantiationDecl will be NULL only if the template instance is dependent from a template parameter
			if(classTemplateInstantiationDecl != NULL)
			{
				scopeDiscoveryDecl = classTemplateInstantiationDecl;
			}

			const CXXRecordDecl* templatedDecl = dyn_cast<CXXRecordDecl>(classTemplateDecl->getTemplatedDecl());
			assert(templatedDecl && "could not retrieve templated declaration");
			templateId = getTypeId_i(m_context->getRecordType(templatedDecl).getTypePtr());
		}
		else
		{
			const TemplateTemplateParmDecl* templateTemplateParmDecl = 
				dyn_cast<TemplateTemplateParmDecl>(templateDecl);
			assert(templateTemplateParmDecl && "template declaration is not a class template or template parameter");
			scopeDiscoveryDecl = templateTemplateParmDecl->getCanonicalDecl();
			KnownTemplateTemplateParamMap::const_iterator it = m_knowTemplateTemplateParams.find(scopeDiscoveryDecl);
			assert((it != m_knowTemplateTemplateParams.end()) && "template template parameter not found in map");
			templateId = it->second;
		}

		int scopeid = dumpScope_i(scopeDiscoveryDecl);

		retId = m_uid.alloc();
		m_os << "TemplateRecordInstantiationType( id=" << retId << ", templateid=" 
			<< templateId;
		if(classTemplateInstantiationDecl != NULL)
		{
			s_printRecordFlags(m_os, classTemplateInstantiationDecl);
		} 
		else
		{
			s_printDefaultRecordFlags(m_os);
		}
		m_os << ", scopeid=" << scopeid << " )\n";
		m_knownTypes[templateSpecializationType] = retId;
		m_knownTypes[canonicalInstantiationType] = retId;

		const int argc = templateSpecializationType->getNumArgs();
		const TemplateArgument* argv = templateSpecializationType->getArgs();
		dumpTemplateArgumentList_i(argv, argc, retId);

		if(classTemplateInstantiationDecl != NULL)
		{
			m_knownTypes[m_context->getRecordType(classTemplateInstantiationDecl).getTypePtr()] = retId;

			const CXXRecordDecl* classTemplateInstantiationDefRecord = classTemplateInstantiationDecl->getDefinition();
			if(classTemplateInstantiationDefRecord)
			{
				const ClassTemplateSpecializationDecl* classTemplateInstantiationDef =
					dyn_cast<ClassTemplateSpecializationDecl>(classTemplateInstantiationDefRecord);

				if(classTemplateInstantiationDef != NULL)
				{
					dumpTagDefinition_i(classTemplateInstantiationDef, retId);
				}
			}
			// the definition for a template instantiation might not be found when it's only used for typedefs
			// or never referred explicitely when allocating storage.
		}
	}

	return retId;
}

int Havok::ExtractASTConsumer::dumpTemplateSpecializationType_i(
	const ClassTemplateSpecializationDecl* classTemplateSpecializationDecl, 
	int scopeId )
{
	const TemplateArgumentList& argList = classTemplateSpecializationDecl->getTemplateArgs();
	const QualType canonType = m_context->getRecordType(classTemplateSpecializationDecl);
	TemplateName tempName(classTemplateSpecializationDecl->getSpecializedTemplate());
	const TemplateSpecializationType* templateSpecializationType = m_context->
		getTemplateSpecializationType(tempName, argList.data(), argList.size(), canonType).getTypePtr()->
		getAs<TemplateSpecializationType>();
	assert(templateSpecializationType && "not a template specialization type");

	const Type* recordType = m_context->getRecordType(classTemplateSpecializationDecl).getTypePtr();

	// seen this type before?
	int retId = findTypeId_i(recordType);
	if(retId != -1)
	{
		return retId;
	}

	// While the type is always TemplateSpecializationType, we handle here just full
	// or partial template specializations, template instantiations are handled in the
	// usual _dumpType() function.

	const CXXRecordDecl* templatedDecl = dyn_cast<CXXRecordDecl>(templateSpecializationType->getTemplateName().getAsTemplateDecl()->getTemplatedDecl());
	assert(templatedDecl && "could not retrieve templated declaration");
	int templateId = getTypeId_i(m_context->getRecordType(templatedDecl).getTypePtr());

	retId = m_uid.alloc();
	m_os << "TemplateRecordSpecialization( id=" << retId << ", templateid=" 
		<< templateId;
	s_printRecordFlags(m_os, classTemplateSpecializationDecl);
	m_os << ", scopeid=" << scopeId << " )\n";

	if(const ClassTemplatePartialSpecializationDecl* classTemplatePartialSpecializationDecl = 
		dyn_cast<ClassTemplatePartialSpecializationDecl>(classTemplateSpecializationDecl))
	{
		// 2: dump the parameters
		dumpTemplateParameterList_i(classTemplatePartialSpecializationDecl->getTemplateParameters(), retId);

		// 3: add special entries in the type map for types referred by template arguments, consider the following specialization:
		//
		// template<typename T1>
		// class A <T1, 5, int>
		// {
		//     T1 m_a
		// };
		//
		// We dump type T1 (type template parameter) when dumping the Parameter list above, but when dumping the argument
		// list we might refer to the types dumped in the parameter list (T1 in the above example). This cannot happen in
		// case of explicit instantiation of course.
		// The type returned by LLVM for T1 in the argument list <T1, 5, int> is not the same we dumped in the previous step,
		// that type must be present in the map for everything to work properly, and that's why we do a special operation 
		// adding (or replacing) all those types with the id obtained by looking up the Parameter list type.
		addOrReplaceSpecializationTypeParameterTypes_i(classTemplatePartialSpecializationDecl->getTemplateParameters());
	}

	// 4: dump the argument list
	dumpTemplateArgumentList_i(templateSpecializationType->getArgs(), 
		templateSpecializationType->getNumArgs(), 
		retId);

	m_knownTypes[recordType] = retId;
	return retId;
}

int Havok::ExtractASTConsumer::dumpScope_i( const Decl* decl )
{
	int retScopeId = -1;

	const DeclContext* declContext = decl->getDeclContext();
	if(const NamedDecl* namedDecl = dyn_cast<NamedDecl>(declContext))
	{
		// it has already been dumped, just get the id
		if(const TypeDecl* typeDecl = dyn_cast<TypeDecl>(namedDecl))
		{
			// return the id using the type information
			retScopeId = getTypeId_i( m_context->getTypeDeclType(typeDecl).getTypePtr() );
		}
		else if(const NamespaceDecl* namespaceDecl = dyn_cast<NamespaceDecl>(namedDecl))
		{
			// return the namespace id
			retScopeId = getNamespaceId_i(namespaceDecl);
		} else
		{
			assert(false && "Invalid declaration scope");
		}
	}
	else
	{
		// we need to refer to the containing file, and dump if is not in the known file map
		SourceLocation loc = decl->getLocation();
		loc = s_getExpansionLoc(loc, m_context->getSourceManager());
		FileID fileId = s_getFileId(loc, m_context->getSourceManager());
		KnownFilesMap::iterator it = m_knownFiles.find(fileId);
		if(it == m_knownFiles.end())
		{
			retScopeId = m_uid.alloc();
			m_knownFiles[fileId] = retScopeId;
			std::string buf;
			const char* fileName = s_getFileName(buf, loc, m_context->getSourceManager());
			m_os << "File( id=" << retScopeId << ", location='" << fileName << "' )\n";
		}
		else
		{
			retScopeId = it->second;
		}
	}

	return retScopeId;
}

void Havok::ExtractASTConsumer::dumpSpecifiersRecursive_i(const NestedNameSpecifier* nestedNameSpecifier)
{
	const NestedNameSpecifier* nameSpecifierPrefix = nestedNameSpecifier->getPrefix();
	if(nameSpecifierPrefix)
	{
		dumpSpecifiersRecursive_i(nameSpecifierPrefix);
	}
	if(const Type* specifierType = nestedNameSpecifier->getAsType())
	{
		if( dyn_cast<TemplateSpecializationType>(specifierType) )
			dumpNonQualifiedSimpleType_i(specifierType);
	} 
}

void Havok::ExtractASTConsumer::dumpTypeSpecifiers_i( const Type* type )
{
	const ElaboratedType* elabType = type->getAs<ElaboratedType>();
	if(elabType)
	{
		NestedNameSpecifier* nestedNameSpecifier = elabType->getQualifier();
		if(nestedNameSpecifier)
		{
			dumpSpecifiersRecursive_i(nestedNameSpecifier);
		}
	}
}

void Havok::ExtractASTConsumer::dumpTagDefinition_i(const TagDecl* tagDecl, int recordId)
{
	if( const RecordDecl* recordDecl = dyn_cast<RecordDecl>(tagDecl) )
	{
		// class/struct/union
		if( const CXXRecordDecl* cxxDecl = dyn_cast<CXXRecordDecl>(recordDecl) )
		{
			for( CXXRecordDecl::base_class_const_iterator bi = cxxDecl->bases_begin(), be = cxxDecl->bases_end(); bi != be; ++bi )
			{
				int pid = dumpType_i( bi->getType() );
				m_os << "Inherit( id=" << recordId << ", parent=" << pid << " )\n";
			}
		}
	}

	// class/struct/union or enum
	dumpDeclContext_i(tagDecl);
}

void Havok::ExtractASTConsumer::dumpDeclContext_i(const DeclContext* context)
{
	typedef RecordDecl::decl_iterator NestedIterator;
	for( NestedIterator fi = context->decls_begin(), fe = context->decls_end(); fi != fe; ++fi )
	{
		dumpDecl_i( *fi );
	}
}

void Havok::ExtractASTConsumer::dumpNamespace_i(const NamespaceDecl* namespaceDecl)
{
	// check if already seen (if not, dump the declaration)
	const NamespaceDecl* originalNamespaceDecl = namespaceDecl->getOriginalNamespace();
	KnownNamespacesMap::iterator it = m_knownNamespaces.find(originalNamespaceDecl);
	if( it == m_knownNamespaces.end() )
	{
		int scopeId = dumpScope_i(namespaceDecl);
		int newId = m_uid.alloc();
		m_os << "Namespace( id=" << newId;
		s_printName(m_os, namespaceDecl);
		m_os << ", scopeid=" << scopeId << " )\n";
		m_knownNamespaces[originalNamespaceDecl] = newId;
	}

	// dump declarations in the namespace
	dumpDeclContext_i(namespaceDecl);
}

void Havok::ExtractASTConsumer::dumpTemplateClass_i(const ClassTemplateDecl* classTemplateDecl)
{
	const CXXRecordDecl* templatedRecordDecl = classTemplateDecl->getTemplatedDecl();
	const Type* injectedClassnameType = m_context->getRecordType(templatedRecordDecl).getTypePtr();
	const ClassTemplateDecl* classTemplateDef = s_getClassTemplateDefinition(classTemplateDecl);

	int templateId;
	templateId = findTypeId_i(injectedClassnameType);
	if(templateId == -1)
	{
		int scopeId = dumpScope_i(classTemplateDef != NULL ? classTemplateDef : classTemplateDecl);

		templateId = m_uid.alloc();
		m_os << "TemplateRecord( id=" << templateId;
		s_printName(m_os, templatedRecordDecl);
		s_printRecordFlags(m_os, templatedRecordDecl);

		m_os << ", scopeid=" << scopeId << " )\n";
		s_printAnnotations(m_os, classTemplateDef != NULL ? classTemplateDef->getTemplatedDecl() : templatedRecordDecl, templateId);
		m_knownTypes[injectedClassnameType] = templateId;

		// 1: dump template parameter list
		const TemplateParameterList* paramList = 
			classTemplateDef != NULL ? 
			classTemplateDef->getTemplateParameters() : 
			classTemplateDecl->getTemplateParameters();	
		dumpTemplateParameterList_i(paramList, templateId);
	}

	if(classTemplateDecl->isThisDeclarationADefinition())
	{
		// 2: add special entries in the type map for types referred by template parameters, consider the following template class declaration:
		//
		// template<typename T1>
		// class A : public B<T1, 5, int>
		// {
		//     int m_a;
		// };
		//
		// We dump type T1 (type template parameter) when dumping the Parameter list above, but when dumping the parent
		// type we might refer to the types dumped in the parameter list (T1 in the above example).
		// The type returned by LLVM for T1 in the argument list <T1, 5, int> is not the same we dumped in the previous step,
		// that type must be present in the map for everything to work properly, and that's why we do a special operation 
		// adding (or replacing) all those types with the id obtained by looking up the Parameter list type.
		addOrReplaceSpecializationTypeParameterTypes_i(classTemplateDecl->getTemplateParameters());

		// dump underlying record (base classes and members)
		dumpTagDefinition_i(templatedRecordDecl, templateId);
	}
}

void Havok::ExtractASTConsumer::dumpTemplateClassSpecialization_i(const ClassTemplateSpecializationDecl* classTemplateSpecializationDecl)
{
	// it might be:
	// - explicit instantiation (dump only a TemplateRecordExplicitInstantiation)
	// - full specialization (dump a TemplateSpecializationRecord with arguments and members)
	// - partial specialization (dump a TemplateSpecializationRecord with arguments and members)

	TemplateSpecializationKind kind = classTemplateSpecializationDecl->getSpecializationKind();

	if(kind == TSK_ExplicitSpecialization)
	{
		const ClassTemplateSpecializationDecl* classTemplateSpecializationDef = NULL;
 		const CXXRecordDecl* classTemplateSpecializationDefRecord = classTemplateSpecializationDecl->getDefinition();
		if(classTemplateSpecializationDefRecord != NULL)
			classTemplateSpecializationDef = dyn_cast<ClassTemplateSpecializationDecl>(classTemplateSpecializationDefRecord);

		int scopeId = dumpScope_i(classTemplateSpecializationDefRecord != NULL ? classTemplateSpecializationDefRecord : classTemplateSpecializationDecl);
 		
		// 1: dump the TemplateSpecializationRecord entry
		// dump template specializations using specific function
		int templateId = dumpTemplateSpecializationType_i(classTemplateSpecializationDef != NULL ? classTemplateSpecializationDef : classTemplateSpecializationDecl, scopeId);

		if(classTemplateSpecializationDecl->isThisDeclarationADefinition())
		{
			if(const ClassTemplatePartialSpecializationDecl* classTemplatePartialSpecializationDecl = 
				dyn_cast<ClassTemplatePartialSpecializationDecl>(classTemplateSpecializationDecl))
			{
				// 2: add special entries in the type map for types referred by template parameters, consider the following template class declaration:
				//
				// template<typename T1>
				// class A<T1*, 5, 10> : public B<T1>
				// {
				//     int m_a;
				// };
				//
				// We dump type T1 (type template parameter) when dumping the Parameter list above, but when dumping the parent
				// type we might refer to the types dumped in the parameter list (T1 in the above example).
				// The type returned by LLVM for T1 in the argument list <T1, 5, int> is not the same we dumped in the previous step,
				// that type must be present in the map for everything to work properly, and that's why we do a special operation 
				// adding (or replacing) all those types with the id obtained by looking up the Parameter list type.
				addOrReplaceSpecializationTypeParameterTypes_i(classTemplatePartialSpecializationDecl->getTemplateParameters());
			}

			// 3: dump underlying record (base classes and members)
			dumpTagDefinition_i(classTemplateSpecializationDecl, templateId);
		}
	}
	else if(kind == TSK_ExplicitInstantiationDefinition)
	{	
		const TemplateArgumentList& argList = classTemplateSpecializationDecl->getTemplateArgs();
		const QualType canonType = m_context->getRecordType(classTemplateSpecializationDecl);
		TemplateName tempName(classTemplateSpecializationDecl->getSpecializedTemplate());
		QualType templateSpecializationType = m_context->getTemplateSpecializationType(tempName, argList.data(), argList.size(), canonType);
		assert(templateSpecializationType.getTypePtr()->getAs<TemplateSpecializationType>() && "not a template specialization type");

		int scopeId = dumpScope_i(classTemplateSpecializationDecl);
		// dump explicit instantiation using the standard dumpType function
		dumpType_i(templateSpecializationType, scopeId);
	}
	else
	{
		assert(0 && "Template specialization declaration kind not supported");
	}
}

void Havok::ExtractASTConsumer::dumpTemplateParameterList_i(const TemplateParameterList* paramList, int templateId)
{
	for( TemplateParameterList::const_iterator it = paramList->begin();
		 it != paramList->end();
		 ++it )
	{
		const NamedDecl* paramDecl = (*it);
		if ( const NonTypeTemplateParmDecl* nonTypeTemplateParmDecl = dyn_cast<NonTypeTemplateParmDecl>(paramDecl) )
		{
			int typeId = dumpType_i(nonTypeTemplateParmDecl->getType());
			m_os << "TemplateNonTypeParam( templateid=" << templateId << ", typeid=" << typeId;
			s_printName(m_os, nonTypeTemplateParmDecl);
			m_os << " )\n";
		}
		else if ( const TemplateTypeParmDecl* templateTypeParmDecl = dyn_cast<TemplateTypeParmDecl>(paramDecl) )
		{
			int typeId = dumpType_i(m_context->getTemplateTypeParmType(
				templateTypeParmDecl->getDepth(), 
				templateTypeParmDecl->getIndex(), 
				templateTypeParmDecl->isParameterPack(),
				const_cast<TemplateTypeParmDecl*>(templateTypeParmDecl)));
			m_os << "TemplateTypeParamType( templateid=" << templateId << ", id=" << typeId;
			s_printName(m_os, templateTypeParmDecl);
			m_os << " )\n";
		}
		else if ( const TemplateTemplateParmDecl* templateTemplateParmDecl = dyn_cast<TemplateTemplateParmDecl>(paramDecl) )
		{
			int retId = m_uid.alloc();
			const Decl* canonical = templateTemplateParmDecl->getCanonicalDecl();
			m_knowTemplateTemplateParams[canonical] = retId;
			m_os << "TemplateTemplateParam( templateid=" << templateId << ", id=" << retId;
			s_printName(m_os, templateTemplateParmDecl);
			m_os << " )\n";
			// dump template parameter list
			const TemplateParameterList* paramList = templateTemplateParmDecl->getTemplateParameters();
			dumpTemplateParameterList_i(paramList, retId);
		}
		else
		{
			m_os << "### Template param declaration not supported";
			s_printName(m_os, paramDecl);
			m_os << "\n";
			assert(0);
		}
	}
}

void Havok::ExtractASTConsumer::dumpTemplateArgumentList_i(const TemplateArgument* argv, int argc, int templateId)
{
	// Note that the argument list for a given template instantiation might not include an argument
	// for every template parameters, this happens when we have default arguments associated with
	// some template parameters.
	for(int i = 0; i < argc; ++i)
	{
		if(argv[i].getKind() == TemplateArgument::Type)
		{
			QualType qualType = argv[i].getAsType(); 
			int typeId = dumpType_i(qualType);
			m_os << "TemplateSpecializationTypeArg( recordid=" << templateId << ", typeid=" << typeId << " )\n";
		} 
		else if(argv[i].getKind() == TemplateArgument::Template)
		{
			TemplateName templateName = argv[i].getAsTemplate();
			TemplateDecl* templateDecl = templateName.getAsTemplateDecl();
			int argTemplateId = -1;
			ClassTemplateDecl* classTemplateDecl = dyn_cast<ClassTemplateDecl>(templateDecl);
			if(classTemplateDecl)
			{
				const CXXRecordDecl* templatedRecordDecl = classTemplateDecl->getTemplatedDecl();
				const Type* injectedClassnameType = m_context->getRecordType(templatedRecordDecl).getTypePtr();
				argTemplateId = findTypeId_i(injectedClassnameType);
			}
			else
			{
				const TemplateTemplateParmDecl* templateTemplateParmDecl = 
					dyn_cast<TemplateTemplateParmDecl>(templateDecl);
				assert(templateTemplateParmDecl && "template declaration is not a class template or template parameter");
				KnownTemplateTemplateParamMap::const_iterator it = m_knowTemplateTemplateParams.find(templateTemplateParmDecl->getCanonicalDecl());
				assert((it != m_knowTemplateTemplateParams.end()) && "template template parameter not found in map");
				argTemplateId = it->second;
			}
			m_os << "TemplateSpecializationTemplateArg( recordid=" << templateId << ", templateid=" << argTemplateId << " )\n";
		}
		else if( (argv[i].getKind() == TemplateArgument::Integral) ||
			     (argv[i].getKind() == TemplateArgument::Expression) )
		{
			m_os << "TemplateSpecializationNonTypeArg( recordid=" << templateId << ", value='";
			argv[i].print(m_context->getPrintingPolicy(), m_os);
			m_os << "' )\n";
		}
		else
		{
			m_os << "### Template argument kind not supported\n";
			assert(0);
		}
	}
}

int Havok::ExtractASTConsumer::findTypeId_i(const Type* typeIn)
{
	KnownTypeMap::const_iterator it = m_knownTypes.find(typeIn);
	if(it == m_knownTypes.end())
	{
		return -1;
	}
	return it->second;
}

int Havok::ExtractASTConsumer::findConstTypeId_i(int typeId)
{
	ConstTypeIdMap::const_iterator it = m_constTypeIdMap.find(typeId);
	if(it == m_constTypeIdMap.end())
	{
		return -1;
	}
	return it->second;
}

int Havok::ExtractASTConsumer::getTypeId_i(const Type* typeIn)
{
	// clean the type from any sugar used to specify it in source code (elaborated types)
	typeIn = s_getTrueType(typeIn);

	int id = findTypeId_i(typeIn);
	assert(id != -1 && "type not found in map");
	return id;
}

int Havok::ExtractASTConsumer::getNamespaceId_i(const NamespaceDecl* namespaceDecl)
{
	const NamespaceDecl* originalNamespaceDecl = namespaceDecl->getOriginalNamespace();
	int id = m_knownNamespaces[originalNamespaceDecl];
	assert(id != 0 && "namespace Id not found");
	return id;
}

void Havok::ExtractASTConsumer::addOrReplaceSpecializationTypeParameterTypes_i(const TemplateParameterList* paramList)
{
	for( TemplateParameterList::const_iterator it = paramList->begin();
		it != paramList->end();
		++it )
	{
		const NamedDecl* paramDecl = (*it);
		if ( const TemplateTypeParmDecl* templateTypeParmDecl = dyn_cast<TemplateTypeParmDecl>(paramDecl))
		{
			int typeId = getTypeId_i(m_context->getTemplateTypeParmType(
				templateTypeParmDecl->getDepth(), 
				templateTypeParmDecl->getIndex(), 
				templateTypeParmDecl->isParameterPack(),
				const_cast<TemplateTypeParmDecl*>(templateTypeParmDecl)).getTypePtr());
			const Type* newType = m_context->getTemplateTypeParmType(
				templateTypeParmDecl->getDepth(), 
				templateTypeParmDecl->getIndex(), 
				templateTypeParmDecl->isParameterPack()).getTypePtr();
			m_knownTypes[newType] = typeId;
		} 
	}
}

// -------------------------------------------------------------------------- //
