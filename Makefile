
CONFIG := Release
CONFIG := Debug
LLVM_DIR ?= /PATH/TO/LLVM/

EXENAME := ./clang-extract.$(CONFIG)

CXXFLAGS := -I$(LLVM_DIR)/include -I$(LLVM_DIR)/tools/clang/include \
	-DNDEBUG -D_GNU_SOURCE -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS \
	-pedantic -fomit-frame-pointer -fno-exceptions -fno-rtti -fPIC -fno-strict-aliasing \
	-W -Wall -Woverloaded-virtual -Wcast-qual -Wno-long-long -Wno-unused-parameter -Wwrite-strings \
	-Wno-variadic-macros -Wno-reorder -Wno-trigraphs -Wno-unknown-pragmas -Wno-unused
LDFLAGS := -L$(LLVM_DIR)/$(CONFIG)/lib
LIBS := -lclangFrontend -lclangSerialization -lclangParse -lclangSema -lclangAnalysis -lclangAST \
	-lclangLex -lclangBasic -lclangEdit -lLLVMMC -lLLVMCore -lLLVMSupport -lpthread -ldl -lm

ifeq ($(CONFIG),"Debug")
	CXXFLAGS += -DDEBUG -g
endif
ifeq ($(CONFIG),"Release")
	CXXFLAGS += -O3
endif

SRCS := extract.cpp main.cpp 
$(EXENAME) : $(SRCS) extract.h Makefile
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)

test : test1.h #$(EXENAME)
	$(EXENAME) -I . test1.h -o test.out
