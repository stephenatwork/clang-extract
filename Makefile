
CONFIG := Debug
LLVM_DIR ?= $(shell llvm-config --obj-root)

EXENAME := ./clang-extract.$(CONFIG)


CXXFLAGS += -std=c++11 -stdlib=libc++ -I$(LLVM_DIR)/include -I$(LLVM_DIR)/tools/clang/include \
	-nostdinc++ -I$(LLVM_DIR)/include/c++/v1 \
	-D_GNU_SOURCE -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS \
	-pedantic -fomit-frame-pointer -fno-exceptions -fno-rtti -fPIC -fno-strict-aliasing \
	-W -Wall -Woverloaded-virtual -Wcast-qual -Wno-long-long -Wno-unused-parameter -Wwrite-strings \
	-Wno-variadic-macros -Wno-reorder -Wno-trigraphs -Wno-unknown-pragmas -Wno-unused
LDFLAGS += -L$(LLVM_DIR)/lib
LIBS := -lclangFrontend -lclangSerialization -lclangParse -lclangSema -lclangAnalysis -lclangAST \
	-lclangLex -lclangBasic -lclangEdit \
	-lLLVMLTO -lLLVMObjCARCOpts -lLLVMLinker -lLLVMipo -lLLVMVectorize -lLLVMBitWriter -lLLVMCppBackendCodeGen \
	-lLLVMCppBackendInfo -lLLVMTableGen -lLLVMDebugInfo -lLLVMOption -lLLVMX86Disassembler -lLLVMX86AsmParser \
	-lLLVMX86CodeGen -lLLVMSelectionDAG -lLLVMAsmPrinter -lLLVMMCParser -lLLVMX86Desc -lLLVMX86Info \
	-lLLVMX86AsmPrinter -lLLVMX86Utils -lLLVMJIT -lLLVMIRReader -lLLVMBitReader -lLLVMAsmParser \
	-lLLVMMCDisassembler -lLLVMInstrumentation -lLLVMInterpreter -lLLVMCodeGen -lLLVMScalarOpts \
	-lLLVMInstCombine -lLLVMTransformUtils -lLLVMipa -lLLVMAnalysis -lLLVMMCJIT -lLLVMTarget \
	-lLLVMRuntimeDyld -lLLVMExecutionEngine -lLLVMMC -lLLVMObject -lLLVMCore -lLLVMSupport -lcurses \
	-lclangLex -lclangBasic -lLLVMMC -lLLVMCore -lLLVMSupport -lclangDriver -lpthread -ldl -lm -lz

ifeq ($(CONFIG),Debug)
	CXXFLAGS += -D_DEBUG -DDEBUG -UNDEBUG -g3 -O0
endif
ifeq ($(CONFIG),Release)
	CXXFLAGS += -DNDEBUG -g3 -O3
endif

SRCS := extract.cpp main.cpp 
$(EXENAME) : $(SRCS) extract.h Makefile
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)

test : test1.h #$(EXENAME)
	$(EXENAME) -I . test1.h -o test.out

.PHONY: clean
clean :
	rm -fr $(EXENAME)

