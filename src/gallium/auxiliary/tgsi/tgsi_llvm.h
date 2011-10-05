#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_tgsi.h"
#include "pipe/p_shader_tokens.h"

#define TGSI_LLVM_MAX_INPUTS 16 * 4
#define TGSI_LLVM_MAX_OUTPUTS 16 * 4
#define TGSI_LLVM_MAX_BRANCH_DEPTH 16
#define TGSI_LLVM_MAX_LOOP_DEPTH 16

struct tgsi_token;

struct tgsi_llvm_branch {
      LLVMBasicBlockRef endif_block;
      LLVMBasicBlockRef if_block;
      LLVMBasicBlockRef else_block;
      unsigned has_else;
};

struct tgsi_llvm_loop {
      LLVMBasicBlockRef loop_block;
      LLVMBasicBlockRef endloop_block;
};

struct tgsi_llvm_context {

   /*=== Front end configuration ===*/

   /** Boolean value if set to non-zero tgsi_llvm will emit llvm code in vector
     * form.  If it is set to zero tgsi_llvm will emit scalar llvm code.
     * XXX: Vector code is not fully supported, if you set this to non-zero,
     * it won't work.  This should be set to zero. */
   unsigned aos;

   /* Special Intrinsics */

   /** Write to an output register: float store_output(float, i32) */
   const char * store_output_intr;

   /** Read from a constant register: float load_const(i32) */
   const char * load_const_intr;

   /** Swizzle a vector value: <4 x float> swizzle(<4 x float>, i32)
    * The swizzle is an unsigned integer that encodes a TGSI_SWIZZLE_* value
    * in 2-bits.
    * Swizzle{0-1} = X Channel
    * Swizzle{2-3} = Y Channel
    * Swizzle{4-5} = Z Channel
    * Swizzle{6-7} = W Channel
    */
   const char * swizzle_intr;

   /** This array contains a mapping of TGSI opcodes to llvm intrinsics.
     * The intrinsics defined here can be used in one of two ways:
     * 1. If the TGSI opcode performs the same operation for every channel it
     *    writes (e.g. TGSI_OPCODE_MUL), then it will be replaced with its
     *    corresponding intrinsic.
     *
     * 2. If the TGSI opcode performs different operations depending on the
     * channel that is written (e.g. TGSI_OPCODE_LOG), then the intrinsic will
     * be used to assist in calculating the value for each channel.  For
     * example, TGSI_OPCODE_LOG needs to calculate floor(log2(|src.x|)) for the
     * X component, so the intrinsic defined for TGSI_OPCODE_LOG will be used for
     * the log2 function in that formula.
     *
     * NOTE: The instruction TGSI_OPCODE_LIT uses an intrinsic called
     * llvm.TGSI.lit.z, that you will need to implement in your backend.  See
     * the definition of TGSI_OPCODE_LIT in gallium/docs/source/tgsi.rst
     * for information on how to implement it. This intrinsic is designed to
     * give backends with a MUL_LIT instruction (e.g. R600) a chance to use it.
     */
   const char * intr_names[TGSI_OPCODE_LAST];

   /** This function allows the user to insert some instructions at the
     * beginning of the program.  It is optional and does not need to be
     * implemented.
     */
   void (*emit_prologue)(struct tgsi_llvm_context *);

   /** This function allows the user to insert some instructions at the end of
     * the program.  This callback is intended to be used for emitting
     * instructions to handle the export for the output registers, but it can
     * be used for any purpose.  Implementing this function is optiona, but
     * recommended.
     */
   void (*emit_epilogue)(struct tgsi_llvm_context *);

   /** This function is responsible for initilizing the inputs array and will be
     * called once for each input declared in the TGSI shader.
     */
   void (*load_input)(struct tgsi_llvm_context *,
                              unsigned input_index,
                              const struct tgsi_full_declaration *decl);


   /** User data to use with the callbacks */
   void * userdata;

   /** This array contains the input values for the shader.  Typically these
     * values will be in the form of a target intrinsic that will inform the
     * backend how to load the actual inputs to the shader. 
     */
   LLVMValueRef inputs[TGSI_LLVM_MAX_INPUTS];

   unsigned output_reg_count;

   union {
      struct lp_build_tgsi_aos_context aos;
      struct lp_build_tgsi_soa_context soa;
   } bld_ctx;

   struct gallivm_state gallivm;

   /*=== Private Members ===*/

   struct tgsi_llvm_branch branch[TGSI_LLVM_MAX_BRANCH_DEPTH];
   struct tgsi_llvm_loop loop[TGSI_LLVM_MAX_LOOP_DEPTH];

   unsigned branch_depth;
   unsigned loop_depth;


   LLVMValueRef main_fn;

};

/** Compile TGSI to an LLVM module */
LLVMModuleRef tgsi_llvm(struct tgsi_llvm_context * ctx,
                        const struct tgsi_token * tokens);

/** Clean up LLVM data structures */
void tgsi_llvm_dispose(struct tgsi_llvm_context * ctx);


/*=== Helper Functions ===*/

struct lp_build_context * tgsi_llvm_get_base(struct tgsi_llvm_context * ctx);

unsigned tgsi_llvm_reg_index_soa(unsigned index, unsigned chan); 
