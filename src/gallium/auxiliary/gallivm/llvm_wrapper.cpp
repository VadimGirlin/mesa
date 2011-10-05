
#include <llvm-c/Core.h>

using namespace llvm;

#if HAVE_LLVM < 0x0300

extern "C" LLVMValueRef LLVMGetBasicBlockTerminator(LLVMBasicBlockRef BB) {
   return wrap(unwrap(BB)->getTerminator());
}

#endif
