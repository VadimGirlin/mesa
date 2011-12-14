
#include "tgsi_llvm.h"

#include "gallivm/llvm_wrapper.h"
#include "gallivm/lp_bld_arit.h"
#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_flow.h"
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_intr.h"
#include "gallivm/lp_bld_tgsi.h"
#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_parse.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include <assert.h>

#include <llvm-c/Transforms/Scalar.h>

#include <stdio.h>
#include <stdlib.h>

struct lp_build_context *
tgsi_llvm_get_base(struct tgsi_llvm_context * ctx)
{
   if (ctx->aos) {
      return &ctx->bld_ctx.aos.base;
   } else {
      return &ctx->bld_ctx.soa.base;
   }
}

unsigned tgsi_llvm_reg_index_soa(
   unsigned index,
   unsigned chan)
{
   return (index * 4) + chan;
}

static void
emit_declaration(
   struct tgsi_llvm_context * ctx,
   const struct tgsi_full_declaration *decl)
{
   if (ctx->aos) {
      lp_emit_declaration_aos(&ctx->bld_ctx.aos, decl);
   } else {
      lp_emit_declaration_soa(&ctx->bld_ctx.soa, decl);
   }
}

static void
add_instruction(
   struct tgsi_llvm_context * ctx,
   struct tgsi_full_instruction *inst)
{
   if (ctx->aos) {
      lp_bld_tgsi_add_instruction(&ctx->bld_ctx.aos.inst_list, inst);
   } else {
      lp_bld_tgsi_add_instruction(&ctx->bld_ctx.soa.inst_list, inst);
   }
}

static void
store_values(
   struct tgsi_llvm_context * ctx,
   const struct tgsi_full_instruction *inst,
   LLVMValueRef values[4])
{
   unsigned chan;
   FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan) {
      assert(values[chan]);
      ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, chan,
                                  NULL, values[chan]);
   }
}

static void
emit_intr_binary(
   struct tgsi_llvm_context * ctx,
   const char * intr,
   const struct tgsi_full_instruction *inst)
{
   LLVMValueRef src0, src1, dst;
   LLVMTypeRef ret_type;
   LLVMValueRef values[4] = {NULL, NULL, NULL, NULL};

   if (ctx->aos) {
      src0 = lp_emit_fetch_aos(&ctx->bld_ctx.aos, inst, 0);
      src1 = lp_emit_fetch_aos(&ctx->bld_ctx.aos, inst, 1);
      ret_type = ctx->bld_ctx.aos.base.vec_type;
      dst = lp_build_intrinsic_binary(ctx->bld_ctx.aos.base.gallivm->builder,
                                      intr, ret_type, src0, src1);
      ctx->bld_ctx.aos.emit_store(&ctx->bld_ctx.aos, inst, 0, dst);
   } else {
      unsigned chan_index;
      ret_type = ctx->bld_ctx.soa.base.elem_type;
      FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan_index) {
         src0 = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, chan_index);
         src1 = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 1, chan_index);
         values[chan_index] = lp_build_intrinsic_binary(
                                         ctx->bld_ctx.soa.base.gallivm->builder,
                                         intr, ret_type, src0, src1);
      }
      store_values(ctx, inst, values);
   }
}

static void
emit_intr_unary(
   struct tgsi_llvm_context * ctx,
   const char * intr,
   const struct tgsi_full_instruction * inst)
{
   LLVMValueRef src, dst;
   LLVMTypeRef ret_type;
   LLVMValueRef values[4] = {NULL, NULL, NULL, NULL};

   if (ctx->aos) {
      src = lp_emit_fetch_aos(&ctx->bld_ctx.aos, inst, 0);
      ret_type = ctx->bld_ctx.aos.base.vec_type;
      dst = lp_build_intrinsic_unary(ctx->bld_ctx.aos.base.gallivm->builder,
                                     intr, ret_type, src);
      ctx->bld_ctx.aos.emit_store(&ctx->bld_ctx.aos, inst, 0, dst);
   } else {
      unsigned chan_index;
      ret_type = ctx->bld_ctx.soa.base.elem_type;
      FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan_index) {
         src = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, chan_index);
         values[chan_index] = lp_build_intrinsic_unary(
                                        ctx->bld_ctx.soa.base.gallivm->builder,
                                        intr, ret_type, src);
      }
      store_values(ctx, inst, values);
   }
}

static void
emit_store_switch_file_soa(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_dst_register *reg,
   unsigned chan_index,
   LLVMValueRef pred,
   LLVMValueRef value)
{
   LLVMBuilderRef builder = bld->base.gallivm->builder;
   LLVMValueRef temp_ptr;

   switch(reg->Register.File) {
   case TGSI_FILE_OUTPUT:
      temp_ptr = bld->outputs[reg->Register.Index][chan_index];
      break;

   case TGSI_FILE_TEMPORARY:
      temp_ptr = lp_get_temp_ptr_soa(bld, reg->Register.Index, chan_index);
      break;

   default:
      return;
   }
   LLVMBuildStore(builder, value, temp_ptr);
}

static void
emit_store_soa(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_instruction *inst,
   unsigned index,
   unsigned chan_index,
   LLVMValueRef pred,
   LLVMValueRef value)
{
   struct tgsi_llvm_context * ctx = bld->userdata;
   struct gallivm_state *gallivm = bld->base.gallivm;
   const struct tgsi_full_dst_register *reg = &inst->Dst[index];

   if (inst->Instruction.Saturate != TGSI_SAT_NONE) {
      if (ctx->intr_names[TGSI_OPCODE_CLAMP]) {
         LLVMTypeRef ret_type = bld->base.elem_type;
         LLVMValueRef args[3];
         args[0] = value;
         args[2] = bld->base.one;
         switch(inst->Instruction.Saturate) {
         case TGSI_SAT_ZERO_ONE:
            args[1] = bld->base.zero;
            break;
         case TGSI_SAT_MINUS_PLUS_ONE:
            args[1] = LLVMConstReal(ret_type, -1.0f);
            break;
         default:
            assert(0);
         }
         value = lp_build_intrinsic(gallivm->builder,
                                    ctx->intr_names[TGSI_OPCODE_CLAMP],
                                    ret_type, args, 3);
      } else {
         switch(inst->Instruction.Saturate) {
         case TGSI_SAT_ZERO_ONE:
            value = lp_build_max(&bld->base, value, bld->base.zero);
            value = lp_build_min(&bld->base, value, bld->base.one);
            break;
         case TGSI_SAT_MINUS_PLUS_ONE:
            value = lp_build_max(&bld->base, value, lp_build_const_vec(bld->base.gallivm, bld->base.type, -1.0));
            value = lp_build_min(&bld->base, value, bld->base.one);
            break;
         default:
            assert(0);
         }
      }
   }

   emit_store_switch_file_soa(bld, reg, chan_index, pred, value);
}

static LLVMValueRef
emit_store_switch_file(
   struct lp_build_tgsi_aos_context *bld,
   const struct tgsi_full_dst_register *reg,
   LLVMValueRef value)
{

   switch(reg->Register.File) {
   case TGSI_FILE_OUTPUT:
      return bld->outputs[reg->Register.Index];

   /* XXX: The following case statements are cut and pasted from lp_bld_tgsi_aos.c */
   case TGSI_FILE_TEMPORARY:
      return bld->temps[reg->Register.Index];

   case TGSI_FILE_ADDRESS:
      return bld->addr[reg->Indirect.Index];
      break;

   case TGSI_FILE_PREDICATE:
      return bld->preds[reg->Register.Index];
      break;

   default:
      assert(0);
      return bld->base.undef;
   }
}


static void emit_store_aos (
   struct lp_build_tgsi_aos_context *bld,
   const struct tgsi_full_instruction *inst,
   unsigned index,
   LLVMValueRef value)
{
   LLVMBuilderRef builder = bld->base.gallivm->builder;
   const struct tgsi_full_dst_register *reg = &inst->Dst[index];
   LLVMValueRef ptr;

   /* XXX: Cut and pasted from lp_bld_tgsi_aos.c */
   /*
    * Saturate the value
    */

   switch (inst->Instruction.Saturate) {
   case TGSI_SAT_NONE:
      break;

   case TGSI_SAT_ZERO_ONE:
      value = lp_build_max(&bld->base, value, bld->base.zero);
      value = lp_build_min(&bld->base, value, bld->base.one);
      break;

   case TGSI_SAT_MINUS_PLUS_ONE:
      value = lp_build_max(&bld->base, value, lp_build_const_vec(bld->base.gallivm, bld->base.type, -1.0));
      value = lp_build_min(&bld->base, value, bld->base.one);
      break;

   default:
      assert(0);
   }

   /*
    * Translate the register file
    */

   assert(!reg->Register.Indirect);

   ptr = emit_store_switch_file(bld, reg, value);

   if (!ptr)
      return;

   LLVMBuildStore(builder, value, ptr);
}

static LLVMValueRef
merge_elements(
   struct tgsi_llvm_context * ctx,
   LLVMValueRef * elements)
{
   unsigned chan;
   struct gallivm_state * gallivm = ctx->bld_ctx.soa.base.gallivm;
   LLVMTypeRef vec_type = LLVMVectorType(ctx->bld_ctx.soa.base.elem_type, 4);
   LLVMBuilderRef builder = ctx->aos ? ctx->bld_ctx.aos.base.gallivm->builder :
                                       ctx->bld_ctx.soa.base.gallivm->builder;
   assert(!ctx->aos);
   LLVMValueRef vec = lp_build_alloca(gallivm, vec_type, "");
   vec = LLVMBuildLoad(builder, vec, "");
   for (chan = 0; chan < 4; chan++) {
      LLVMValueRef elem = elements[chan];
      LLVMValueRef chan_value = lp_build_const_int32(gallivm, chan);
      vec = LLVMBuildInsertElement(builder, vec, elem, chan_value, "");
   }
   return vec;
}

static void emit_tex(
   struct tgsi_llvm_context * ctx,
   LLVMValueRef * elements,
   const struct tgsi_full_instruction * inst,
   const char * intr_name)
{
   unsigned chan;
   struct gallivm_state * gallivm = ctx->bld_ctx.soa.base.gallivm;
   LLVMTypeRef vec_type = LLVMVectorType(ctx->bld_ctx.soa.base.elem_type, 4);
   LLVMValueRef args[3];
   LLVMValueRef res;

   assert(!ctx->aos);

   args[0] = merge_elements(ctx, elements);
   args[1] = lp_build_const_int32(gallivm, inst->Src[1].Register.Index);
   args[2] = lp_build_const_int32(gallivm, inst->Texture.Texture);
   res = lp_build_intrinsic(gallivm->builder, intr_name, vec_type, args, 3);

   FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan) {
      LLVMValueRef index = lp_build_const_int32(
                                          ctx->bld_ctx.soa.base.gallivm, chan);
      LLVMValueRef elem = LLVMBuildExtractElement(gallivm->builder, res, index,
                                                  "");
      ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, chan, NULL, elem);
   }
}

/**
 * (a * b) - (c * d)
 */
static LLVMValueRef
xpd_helper(
  struct tgsi_llvm_context * ctx,
  LLVMValueRef a,
  LLVMValueRef b,
  LLVMValueRef c,
  LLVMValueRef d)
{
  LLVMValueRef tmp0, tmp1;
  assert(ctx->intr_names[TGSI_OPCODE_MUL]);
  tmp0 = lp_build_intrinsic_binary(ctx->bld_ctx.soa.base.gallivm->builder,
                                   ctx->intr_names[TGSI_OPCODE_MUL],
                                   ctx->bld_ctx.soa.base.elem_type,
                                   a, b);
  tmp1 = lp_build_intrinsic_binary(ctx->bld_ctx.soa.base.gallivm->builder,
                                   ctx->intr_names[TGSI_OPCODE_MUL],
                                   ctx->bld_ctx.soa.base.elem_type,
                                   c, d);

  return LLVMBuildFSub(ctx->bld_ctx.soa.base.gallivm->builder, tmp0, tmp1, "");
}

static void
emit_instruction(
   struct tgsi_llvm_context * ctx,
   const struct tgsi_full_instruction *inst,
   const struct tgsi_opcode_info *info,
   int *pc)
{

   unsigned opcode = inst->Instruction.Opcode;
   const char * intrinsic_name = ctx->intr_names[opcode];
   LLVMValueRef dst;
   LLVMValueRef src0, src1;
   LLVMTypeRef ret_type;
   LLVMValueRef values[4] = {NULL, NULL, NULL, NULL};
   LLVMBuilderRef builder = ctx->aos ? ctx->bld_ctx.aos.base.gallivm->builder :
                                       ctx->bld_ctx.soa.base.gallivm->builder;
   struct lp_build_context * base = tgsi_llvm_get_base(ctx);
   struct tgsi_llvm_branch * current_branch = ctx->branch_depth > 0 ?
                                  ctx->branch + (ctx->branch_depth - 1) : NULL;
   struct tgsi_llvm_loop * current_loop = ctx->loop_depth > 0 ?
                                      ctx->loop + (ctx->loop_depth - 1) : NULL;
   LLVMBasicBlockRef current_block = LLVMGetInsertBlock(builder);

   switch(opcode) {

   case TGSI_OPCODE_BGNLOOP:
      {
         LLVMBasicBlockRef loop_block;
         LLVMBasicBlockRef endloop_block;

         endloop_block = LLVMAppendBasicBlockInContext(base->gallivm->context,
                                                       ctx->main_fn, "ENDLOOP");
         loop_block = LLVMInsertBasicBlockInContext(base->gallivm->context,
                                                    endloop_block, "LOOP");
         LLVMBuildBr(builder, loop_block);
         LLVMPositionBuilderAtEnd(builder, loop_block);
         ctx->loop_depth++;
         ctx->loop[ctx->loop_depth - 1].loop_block = loop_block;
         ctx->loop[ctx->loop_depth - 1].endloop_block = endloop_block;
         break;
      }

   case TGSI_OPCODE_BRK:
      LLVMBuildBr(builder, current_loop->endloop_block);
      break;

   case TGSI_OPCODE_CONT:
      LLVMBuildBr(builder, current_loop->loop_block);
      break;

   case TGSI_OPCODE_DPH:
   case TGSI_OPCODE_DP2:
   case TGSI_OPCODE_DP3:
   case TGSI_OPCODE_DP4:
      {
         unsigned chan;
         LLVMValueRef elements[2][4];
         assert(!ctx->aos);
         unsigned dp_components = (opcode == TGSI_OPCODE_DP2 ? 2 :
                                  (opcode == TGSI_OPCODE_DP3 ? 3 : 4));
         for (chan = 0 ; chan < dp_components; chan++) {
            elements[0][chan] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst,
                                               0, chan);
            elements[1][chan] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst,
                                               1, chan);
         }

         for ( ; chan < 4; chan++) {
            elements[0][chan] = base->zero;
            elements[1][chan] = base->zero;
         }

         /* Fix up for DPH */
         if (opcode == TGSI_OPCODE_DPH) {
            elements[0][CHAN_W] = base->one;
         }

         src0 = merge_elements(ctx, elements[0]);
         src1 = merge_elements(ctx, elements[1]);

         FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan) {
            dst = lp_build_intrinsic_binary(builder,
                                         ctx->intr_names[TGSI_OPCODE_DP4],
                                         base->elem_type, src0, src1);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, chan,
                                        NULL, dst);
         }
         break;
      }

   case TGSI_OPCODE_DST:
      assert(!ctx->aos);
      assert(ctx->intr_names[TGSI_OPCODE_MUL]);
      if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_X) {
         values[CHAN_X] = base->one;
      }

      if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Y) {
         LLVMValueRef src0y = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst,
                                                0, CHAN_Y);
         LLVMValueRef src1y = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst,
                                                1, CHAN_Y);
         values[CHAN_Y] = lp_build_intrinsic_binary(builder,
                                               ctx->intr_names[TGSI_OPCODE_MUL],
                                               base->elem_type, src0y, src1y);
      }

      if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Z) {
         values[CHAN_Z] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, CHAN_Z);
      }

      if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_W) {
         values[CHAN_W] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 1, CHAN_W);
      }

      store_values(ctx, inst, values);
      break;

   case TGSI_OPCODE_ELSE:
      assert(!ctx->aos);
      assert(current_branch);
      /* We need to add a terminator to the current block if the previous
       * instruction was an ENDIF.  Example:
       * IF
       * [code]
       *   IF
       *   [code]
       *   ELSE
       *   [code]
       *   ENDIF   <--
       * ELSE      <--
       * [code]
       * ENDIF
       */
      if (current_block != current_branch->if_block) {
         LLVMBuildBr(builder, current_branch->endif_block);
      }
      if (!LLVMGetBasicBlockTerminator(current_branch->if_block)) {
         LLVMBuildBr(builder, current_branch->endif_block);
      }
      current_branch->has_else = 1;
      LLVMPositionBuilderAtEnd(builder, current_branch->else_block);
      break;

   case TGSI_OPCODE_ENDIF:
      assert(!ctx->aos);
      assert(current_branch);
      /* If we have consecutive ENDIF instructions, then the first ENDIF
       * will not have a terminator, so we need to add one. */
      if (current_block != current_branch->if_block
          && current_block != current_branch->else_block
          && !LLVMGetBasicBlockTerminator(current_block)) {

         LLVMBuildBr(builder, current_branch->endif_block);
      }
      if (!LLVMGetBasicBlockTerminator(current_branch->else_block)) {
         LLVMPositionBuilderAtEnd(builder, current_branch->else_block);
         LLVMBuildBr(builder, current_branch->endif_block);

      }
      if (!LLVMGetBasicBlockTerminator(current_branch->if_block)) {
         LLVMPositionBuilderAtEnd(builder, current_branch->if_block);
         LLVMBuildBr(builder, current_branch->endif_block);
      }
      LLVMPositionBuilderAtEnd(builder, current_branch->endif_block);
      ctx->branch_depth--;
      break;

   case TGSI_OPCODE_ENDLOOP:
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
         LLVMBuildBr(builder, current_loop->loop_block);
      }

      LLVMPositionBuilderAtEnd(builder, current_loop->endloop_block);
      ctx->loop_depth--;
      break;

   case TGSI_OPCODE_EXP:
      {
         assert(!ctx->aos);
         assert(ctx->intr_names[TGSI_OPCODE_FLR]);
         assert(ctx->intr_names[TGSI_OPCODE_EXP]);
         goto fallback;
         LLVMValueRef src_x, floor_x, dst_x, dst_y, dst_z;
         src_x = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 0);
         floor_x = lp_build_intrinsic_unary(builder,
                                           ctx->intr_names[TGSI_OPCODE_FLR],
                                           base->elem_type, src_x);
         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_X) {
            dst_x = lp_build_intrinsic_unary(builder,
                                            ctx->intr_names[TGSI_OPCODE_EXP],
                                            base->elem_type, floor_x);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 0, NULL,
                                        dst_x);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Y) {
            dst_y = LLVMBuildFSub(builder, src_x, floor_x, "");
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 1, NULL,
                                        dst_y);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Z) {
            dst_z = lp_build_intrinsic_unary(builder,
                                            ctx->intr_names[TGSI_OPCODE_EXP],
                                            base->elem_type, src_x);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 2, NULL,
                                        dst_z);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_W) {
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 3, NULL,
                                        base->one);
         }
         break;
      }

   case TGSI_OPCODE_IF:
      {
         assert(!ctx->aos);
         LLVMValueRef cond;
         LLVMBasicBlockRef if_block, else_block, endif_block;
         cond = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 0);
         cond = LLVMBuildFCmp(builder, LLVMRealOEQ, cond,
                              ctx->bld_ctx.soa.base.one, "");

         endif_block = LLVMAppendBasicBlockInContext(base->gallivm->context,
                                                     ctx->main_fn, "ENDIF");
         if_block = LLVMInsertBasicBlockInContext(base->gallivm->context,
                                                  endif_block, "IF");
         else_block = LLVMInsertBasicBlockInContext(base->gallivm->context,
                                                  endif_block, "ELSE");
         LLVMBuildCondBr(builder, cond, if_block, else_block);
         LLVMPositionBuilderAtEnd(builder, if_block);

         ctx->branch_depth++;
         ctx->branch[ctx->branch_depth - 1].endif_block = endif_block;
         ctx->branch[ctx->branch_depth - 1].if_block = if_block;
         ctx->branch[ctx->branch_depth - 1].else_block = else_block;
         ctx->branch[ctx->branch_depth - 1].has_else = 0;
         break;
      }

   case TGSI_OPCODE_RSQ:
      {
         unsigned chan_index;
         LLVMValueRef abs_x, src_x;
         assert(!ctx->aos);
         assert(ctx->intr_names[TGSI_OPCODE_RSQ]);
         assert(ctx->intr_names[TGSI_OPCODE_ABS]);

         src_x = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, CHAN_X);
         abs_x = lp_build_intrinsic_unary(builder,
                                          ctx->intr_names[TGSI_OPCODE_ABS],
                                          base->elem_type, src_x);
         FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan_index) {
            LLVMValueRef res = lp_build_intrinsic_unary(builder,
                                               ctx->intr_names[TGSI_OPCODE_RSQ],
                                               base->elem_type, abs_x);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, chan_index,
                                        NULL, res);
         }
         break;
      }

   case TGSI_OPCODE_SCS:
      {
         LLVMValueRef dst[4];
         unsigned chan_index;
         LLVMValueRef src0;
         assert(!ctx->aos);
         if (!ctx->intr_names[TGSI_OPCODE_SIN] ||
             !ctx->intr_names[TGSI_OPCODE_COS]) {
            goto fallback;
         }
         src0 = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 0);
         dst[0] = lp_build_intrinsic_unary(builder,
                                           ctx->intr_names[TGSI_OPCODE_COS],
                                           base->elem_type, src0);
         dst[1] = lp_build_intrinsic_unary(builder,
                                           ctx->intr_names[TGSI_OPCODE_SIN],
                                           base->elem_type, src0);
         dst[2] = base->zero;
         dst[3] = base->one;

         FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan_index) {
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, chan_index,
                                        NULL, dst[chan_index]);
         }
         break;
      }

   case TGSI_OPCODE_SLT:
      if (ctx->aos) {
         src0 = lp_emit_fetch_aos(&ctx->bld_ctx.aos, inst, 0);
         src1 = lp_emit_fetch_aos(&ctx->bld_ctx.aos, inst, 1);
         ret_type = ctx->bld_ctx.aos.base.vec_type;
         dst = lp_build_intrinsic_binary(ctx->bld_ctx.aos.base.gallivm->builder,
                                         ctx->intr_names[TGSI_OPCODE_SGT],
                                         ret_type, src1, src0);
      } else {
         unsigned chan_index;
         ret_type =ctx->bld_ctx.soa.base.elem_type;
         FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan_index) {
            src0 = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, chan_index);
            src1 = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 1, chan_index);
            dst = lp_build_intrinsic_binary(ctx->bld_ctx.soa.base.gallivm->builder,
                                   ctx->intr_names[TGSI_OPCODE_SGT], ret_type, src1, src0);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, chan_index,
                                  NULL, dst);
         }
      }
      break;

   case TGSI_OPCODE_SLE:
      {
         unsigned chan_index;
         assert(ctx->intr_names[TGSI_OPCODE_SGE]);
         FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan_index) {
            src0 = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, chan_index);
            src1 = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 1, chan_index);
            dst = lp_build_intrinsic_binary(builder,
                                            ctx->intr_names[TGSI_OPCODE_SGE],
                                            ctx->bld_ctx.soa.base.elem_type,
                                            src1, src0);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, chan_index,
                                  NULL, dst);
         }
         break;
      }

   /* These are a special case because the do not have a dst reg */
   case TGSI_OPCODE_KILP:
      {
         assert(!ctx->aos);
         lp_build_intrinsic(builder, ctx->intr_names[TGSI_OPCODE_KILP],
                        LLVMVoidTypeInContext(base->gallivm->context), NULL, 0);
         break;
      }

   case TGSI_OPCODE_LOG:
      {
         LLVMValueRef x, abs_x, log_abs_x, floor_log_abs_x;

         assert(!ctx->aos);
         assert(ctx->intr_names[TGSI_OPCODE_ABS]);
         assert(ctx->intr_names[TGSI_OPCODE_EX2]);
         assert(ctx->intr_names[TGSI_OPCODE_FLR]);
         assert(ctx->intr_names[TGSI_OPCODE_LG2]);
         assert(ctx->intr_names[TGSI_OPCODE_MUL]);

         x = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 0);
         abs_x = lp_build_intrinsic_unary(builder,
                                          ctx->intr_names[TGSI_OPCODE_ABS],
                                          ctx->bld_ctx.soa.base.elem_type,
                                          x);
         log_abs_x = lp_build_intrinsic_unary(builder,
                                              ctx->intr_names[TGSI_OPCODE_LG2],
                                              ctx->bld_ctx.soa.base.elem_type,
                                              abs_x);
         floor_log_abs_x = lp_build_intrinsic_unary(builder,
                                              ctx->intr_names[TGSI_OPCODE_FLR],
                                              ctx->bld_ctx.soa.base.elem_type,
                                              log_abs_x);
         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_X) {
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_X,
                                        NULL, floor_log_abs_x);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Y) {
            LLVMValueRef ex2 = lp_build_intrinsic_unary(builder,
                                              ctx->intr_names[TGSI_OPCODE_EX2],
                                              ctx->bld_ctx.soa.base.elem_type,
                                              floor_log_abs_x);
            LLVMValueRef res = lp_build_intrinsic_binary(builder,
                                              ctx->intr_names[TGSI_OPCODE_DIV],
                                              ctx->bld_ctx.soa.base.elem_type,
                                              abs_x, ex2);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_Y,
                                        NULL, res);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Z) {
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_Z,
                                        NULL, log_abs_x);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_W) {
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_W,
                                        NULL, ctx->bld_ctx.soa.base.one);
         }
         break;
      }

   case TGSI_OPCODE_LIT:
      {
         LLVMValueRef args[3];
         LLVMTypeRef ret_type = ctx->bld_ctx.soa.base.elem_type;
         LLVMBuilderRef builder = ctx->bld_ctx.soa.base.gallivm->builder;
         LLVMValueRef one, zero, src_x, src_y, src_w, dst_y, dst_z;
         assert(!ctx->aos);
         src_x = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 0);
         src_y = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 1);
         src_w = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 3);
         zero = ctx->bld_ctx.soa.base.zero;
         one  = ctx->bld_ctx.soa.base.one;

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_X) {
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 0, NULL,
                                        one);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Y) {
            dst_y = lp_build_intrinsic_binary(builder,
                                              ctx->intr_names[TGSI_OPCODE_MAX],
                                              ret_type, src_x, zero);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 1, NULL,
                                        dst_y);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Z) {
            args[0] = src_x;
            args[1] = src_y;
            args[2] = src_w;
            dst_z = lp_build_intrinsic(builder, "llvm.TGSI.lit.z", ret_type,
                                       args, 3);
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 2, NULL,
                                        dst_z);
         }

         if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_W) {
            ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, 3, NULL,
                                        one);
         }
         break;
      }

   case TGSI_OPCODE_DDX:
   case TGSI_OPCODE_DDY:
   case TGSI_OPCODE_TEX:
   case TGSI_OPCODE_TXB:
   case TGSI_OPCODE_TXD:
   case TGSI_OPCODE_TXL:
      {
         assert(!ctx->aos);
         unsigned chan;
         LLVMValueRef elements[4];
         for (chan = 0; chan < 4; chan++) {
            elements[chan] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst,
                                               0, chan);
         }
         emit_tex(ctx, elements, inst, intrinsic_name);
         break;
      }

   case TGSI_OPCODE_TXP:
      {
         LLVMValueRef elements[4];
         unsigned chan;
         LLVMValueRef rcp_src_w;
         assert(!ctx->aos);
         if (!ctx->intr_names[TGSI_OPCODE_DIV]) {
            goto fallback;
         }
         rcp_src_w = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, 3);
         for (chan = 0; chan < 3; chan++) {
            LLVMValueRef rcp_src = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0,
                                                      chan);
            LLVMValueRef elem = lp_build_intrinsic_binary(builder,
                                               ctx->intr_names[TGSI_OPCODE_DIV],
                                               ctx->bld_ctx.soa.base.elem_type,
                                               rcp_src, rcp_src_w);
            elements[chan] = elem;
         }
         elements[3] = ctx->bld_ctx.soa.base.one;
         emit_tex(ctx, elements, inst, ctx->intr_names[TGSI_OPCODE_TEX]);
         break;
      }

    case TGSI_OPCODE_XPD:
      {
        LLVMValueRef src[2][3];
        LLVMValueRef res;
        unsigned src_index, chan;
        for (src_index = 0; src_index < 2; src_index++) {
          for (chan = 0; chan < 3; chan++) {
            src[src_index][chan] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst,
                                                     src_index, chan);
          }
        }
        if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_X) {
          res = xpd_helper(ctx, src[0][CHAN_Y], src[1][CHAN_Z],
                                src[1][CHAN_Y], src[0][CHAN_Z]);
          ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_X,
                                      NULL, res);
        }

        if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Y) {
          res = xpd_helper(ctx, src[0][CHAN_Z], src[1][CHAN_X],
                                src[1][CHAN_Z], src[0][CHAN_X]);
          ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_Y,
                                      NULL, res);
        }

        if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_Z) {
          res = xpd_helper(ctx, src[0][CHAN_X], src[1][CHAN_Y],
                                src[1][CHAN_X], src[0][CHAN_Y]);
          ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_Z,
                                      NULL, res);
        }

        if (inst->Dst[0].Register.WriteMask & TGSI_WRITEMASK_W) {
          ctx->bld_ctx.soa.emit_store(&ctx->bld_ctx.soa, inst, 0, CHAN_W,
                                      NULL, ctx->bld_ctx.soa.base.one);
        }
        break;
      }
   default:
      if (intrinsic_name) {
         switch (info->num_src) {
         case 0:
         default:
            goto fallback;
         case 1:
            emit_intr_unary(ctx, intrinsic_name, inst);
            break;
         case 2:
            emit_intr_binary(ctx, intrinsic_name, inst);
            break;
         case 3:
            {
               unsigned chan_index;
               LLVMValueRef values[4] = {NULL, NULL, NULL, NULL};
               assert(!ctx->aos);
               FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan_index) {
                  LLVMValueRef args[3];
                  args[0] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 0, chan_index);
                  args[1] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 1, chan_index);
                  args[2] = lp_emit_fetch_soa(&ctx->bld_ctx.soa, inst, 2, chan_index);
                  values[chan_index] = lp_build_intrinsic(builder, intrinsic_name,
                                           ctx->bld_ctx.soa.base.elem_type,
                                           args, 3);
               }
               store_values(ctx, inst, values);
            }
            break;
         }
      } else {
         goto fallback;
      }
      break;
   }
   (*pc)++;
   return;

fallback:
   if (ctx->aos) {
      lp_emit_instruction_aos(&ctx->bld_ctx.aos, inst, info, pc);
   } else {
      lp_emit_instruction_soa(&ctx->bld_ctx.soa, inst, info, pc);
   }
}

static void emit_instructions(struct tgsi_llvm_context * ctx)
{
   int pc = 0;
   while (pc != -1) {
      struct tgsi_full_instruction *instr;
      if (ctx->aos) {
         instr = ctx->bld_ctx.aos.inst_list.instructions + pc;
      } else {
         instr = ctx->bld_ctx.soa.inst_list.instructions + pc;
      }
      const struct tgsi_opcode_info *opcode_info =
         tgsi_get_opcode_info(instr->Instruction.Opcode);
      emit_instruction(ctx, instr, opcode_info, &pc);
   }
}

static LLVMValueRef
emit_array_index(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_src_register *reg,
   const unsigned swizzle)
{
   LLVMBuilderRef builder = bld->base.gallivm->builder;

   LLVMValueRef addr = LLVMBuildLoad(builder,
                             bld->addr[reg->Indirect.Index][swizzle], "");
   LLVMValueRef offset = lp_build_const_int32(bld->base.gallivm,
                                              reg->Register.Index);
   LLVMValueRef hw_index = LLVMBuildAdd(builder, addr, offset, "");
   LLVMValueRef soa_index = LLVMBuildMul(builder, hw_index,
                          lp_build_const_int32(bld->base.gallivm, 4), "");
   LLVMValueRef array_index = LLVMBuildAdd(builder, soa_index,
                    lp_build_const_int32(bld->base.gallivm, swizzle), "");

   return array_index;
}

static LLVMValueRef
emit_fetch_switch_file_soa(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_src_register *reg,
   const unsigned swizzle)
{
   struct tgsi_llvm_context * ctx = bld->userdata;
   LLVMBuilderRef builder = bld->base.gallivm->builder;
   LLVMValueRef reg_index = lp_build_const_int32(bld->base.gallivm,
                         tgsi_llvm_reg_index_soa(reg->Register.Index, swizzle));
   LLVMTypeRef ret_type = bld->base.elem_type;
   LLVMValueRef res;

   switch(reg->Register.File) {
   case TGSI_FILE_CONSTANT:
      return lp_build_intrinsic_unary(builder, ctx->load_const_intr,
                                       ret_type, reg_index);

   case TGSI_FILE_IMMEDIATE:
      return bld->immediates[reg->Register.Index][swizzle];

   case TGSI_FILE_INPUT:
      return ctx->inputs[tgsi_llvm_reg_index_soa(reg->Register.Index, swizzle)];

   case TGSI_FILE_OUTPUT:
      if (reg->Register.Indirect) {
         LLVMValueRef array_index = emit_array_index(bld, reg, swizzle);
         LLVMValueRef ptr = LLVMBuildGEP(builder, bld->outputs_array, &array_index,
                                         1, "");
         return LLVMBuildLoad(builder, ptr, "");
      } else {
         LLVMValueRef temp_ptr;
         temp_ptr = lp_get_output_ptr(bld, reg->Register.Index, swizzle);
         res = LLVMBuildLoad(builder, temp_ptr, "");
         return res;
      }

   case TGSI_FILE_TEMPORARY:
      if (reg->Register.Indirect) {
         LLVMValueRef array_index = emit_array_index(bld, reg, swizzle);
         LLVMValueRef ptr = LLVMBuildGEP(builder, bld->temps_array, &array_index,
					 1, "");
         return LLVMBuildLoad(builder, ptr, "");
      } else {
         LLVMValueRef temp_ptr;
         temp_ptr = lp_get_temp_ptr_soa(bld, reg->Register.Index, swizzle);
         res = LLVMBuildLoad(builder, temp_ptr, "");
         return res;
      }
   default:
      return bld->base.undef;
   }
}

static LLVMValueRef
emit_fetch_switch_file(
   struct lp_build_tgsi_aos_context *bld,
   const struct tgsi_full_src_register *reg)
{
   struct tgsi_llvm_context * ctx = bld->userdata;
   LLVMBuilderRef builder = bld->base.gallivm->builder;
   LLVMValueRef reg_index = lp_build_const_int32(bld->base.gallivm,
                                                 reg->Register.Index);
   LLVMTypeRef ret_type = bld->base.vec_type;
   LLVMValueRef res;

   switch(reg->Register.File) {
   case TGSI_FILE_CONSTANT:
      return lp_build_intrinsic_unary(builder, ctx->load_const_intr,
                                      ret_type, reg_index);

   case TGSI_FILE_IMMEDIATE:
      return bld->immediates[reg->Register.Index];

   case TGSI_FILE_INPUT:
      return ctx->inputs[reg->Register.Index];

   case TGSI_FILE_TEMPORARY:
      {
         LLVMValueRef temp_ptr;
         temp_ptr = bld->temps[reg->Register.Index];
         res = LLVMBuildLoad(builder, temp_ptr, "");
         if (!res)
            return bld->base.undef;
         else
            return res;
      }

   default:
       return bld->base.undef;
   }
}

static LLVMValueRef
emit_swizzle_aos(struct lp_build_tgsi_aos_context *bld,
                 LLVMValueRef a,
                 unsigned swizzle_x,
                 unsigned swizzle_y,
                 unsigned swizzle_z,
                 unsigned swizzle_w)
{
   struct tgsi_llvm_context *ctx = bld->userdata;
   unsigned full_swizzle = swizzle_x | (swizzle_y << 2)
                           | (swizzle_z << 4) | (swizzle_w << 6);

   LLVMTypeRef ret_type = bld->base.vec_type;
   LLVMValueRef swizzle_value = lp_build_const_int32(bld->base.gallivm,
                                                     full_swizzle);
   return lp_build_intrinsic_binary(bld->base.gallivm->builder,
                                    ctx->swizzle_intr, ret_type,
                                    a, swizzle_value);
}

static void
tgsi_llvm_setup(
   struct tgsi_llvm_context *ctx,
   struct tgsi_shader_info * shader_info)
{
   struct lp_type type;

   memset(&type, 0, sizeof(type));

   if (ctx->aos) {
      unsigned int chan;

      const unsigned char swizzles[4] = {
         TGSI_SWIZZLE_X,
         TGSI_SWIZZLE_Y,
         TGSI_SWIZZLE_Z,
         TGSI_SWIZZLE_W
      };

      memset(&ctx->bld_ctx.aos, 0, sizeof(ctx->bld_ctx.aos));

      /* Initialize lp_type:  XXX: Why is this set globally?  It seems like some
       * isntructions might need a different lp_type. */
      type.floating = TRUE;
      type.sign = TRUE;
      type.width = 32;
      type.length = 4;

      /* Initialze the build context */
      lp_build_context_init(&ctx->bld_ctx.aos.base, &ctx->gallivm, type);
      lp_build_context_init(&ctx->bld_ctx.aos.int_bld, &ctx->gallivm,
                            lp_int_type(type));
      /* XXX: This should be set to tgsi_shader_info->indirect_files; */
      ctx->bld_ctx.aos.indirect_files = 0;
      ctx->bld_ctx.aos.emit_fetch_switch_file_fn = emit_fetch_switch_file;
      ctx->bld_ctx.aos.emit_store = emit_store_aos; 
      ctx->bld_ctx.aos.emit_swizzle = emit_swizzle_aos;
      ctx->bld_ctx.aos.userdata = ctx;

      /* Setup swizzle mapping */
      for (chan = 0; chan < 4; chan++) {
         ctx->bld_ctx.aos.swizzles[chan] = swizzles[chan];
         ctx->bld_ctx.aos.inv_swizzles[swizzles[chan]] = chan;
      }

      /* Allocate the instruction list */
      lp_bld_tgsi_list_init(&ctx->bld_ctx.aos.inst_list);

   } else {
      memset(&ctx->bld_ctx.soa, 0, sizeof(ctx->bld_ctx.soa));
      type.floating = TRUE;
      type.sign = TRUE;
      type.width = 32;
      type.length = 1;

      lp_build_context_init(&ctx->bld_ctx.soa.base, &ctx->gallivm, type);
      lp_build_context_init(&ctx->bld_ctx.soa.uint_bld, &ctx->gallivm,
                           lp_uint_type(type));
      lp_build_context_init(&ctx->bld_ctx.soa.elem_bld, &ctx->gallivm,
                            lp_elem_type(type));
      ctx->bld_ctx.soa.indirect_files = shader_info->indirect_files;
      ctx->bld_ctx.soa.emit_fetch_switch_file_fn = emit_fetch_switch_file_soa;
      ctx->bld_ctx.soa.emit_store = emit_store_soa;
      ctx->bld_ctx.soa.userdata = ctx;

      /* XXX: Copied from lp_bld_tgsi_soa */
      if (ctx->bld_ctx.soa.indirect_files & (1 << TGSI_FILE_TEMPORARY)) {
         LLVMValueRef array_size = lp_build_const_int32(&ctx->gallivm,
                                shader_info->file_max[TGSI_FILE_TEMPORARY] * 4);
         ctx->bld_ctx.soa.temps_array = lp_build_array_alloca(&ctx->gallivm,
                     ctx->bld_ctx.soa.base.elem_type, array_size, "temp_array");
      }

      /* Allocate the instruction list */
      lp_bld_tgsi_list_init(&ctx->bld_ctx.soa.inst_list);
   }
}

LLVMModuleRef tgsi_llvm(
   struct tgsi_llvm_context * ctx,
   const struct tgsi_token * tokens)
{
   LLVMTypeRef main_fn_type;
   LLVMBasicBlockRef main_fn_body;

   struct tgsi_parse_context parse;

   LLVMValueRef outputs[TGSI_LLVM_MAX_OUTPUTS][NUM_CHANNELS];

   struct tgsi_shader_info shader_info;

   unsigned num_immediates = 0;
   unsigned i;

   /* Initialize the gallivm object:
    * We are only using the module, context, and builder fields of this struct.
    * This should be enough for us to be able to pass our gallivm struct to the
    * helper functions in the gallivm module.
    */
   memset(&ctx->gallivm, 0, sizeof (ctx->gallivm));
   ctx->gallivm.context = LLVMContextCreate();
   ctx->gallivm.module = LLVMModuleCreateWithNameInContext("tgsi",
                                                          ctx->gallivm.context);
   ctx->gallivm.builder = LLVMCreateBuilderInContext(ctx->gallivm.context);

   /* Setup the module */
   main_fn_type = LLVMFunctionType(LLVMVoidTypeInContext(ctx->gallivm.context),
                                   NULL, 0, 0);
   ctx->main_fn = LLVMAddFunction(ctx->gallivm.module, "main", main_fn_type);
   main_fn_body = LLVMAppendBasicBlockInContext(ctx->gallivm.context,
                                                ctx->main_fn, "main_body");
   LLVMPositionBuilderAtEnd(ctx->gallivm.builder, main_fn_body);

   tgsi_scan_shader(tokens, &shader_info);
   tgsi_llvm_setup(ctx, &shader_info);

   if (ctx->emit_prologue) {
      ctx->emit_prologue(ctx);
   }

   /* Begin TGSI -> LLVM conversion */
   tgsi_parse_init(&parse, tokens);

   while (!tgsi_parse_end_of_tokens(&parse)) {
      tgsi_parse_token(&parse);
      switch(parse.FullToken.Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION:
         {
            const struct tgsi_full_declaration *decl =
                                             &parse.FullToken.FullDeclaration;
            switch(decl->Declaration.File) {
            case TGSI_FILE_ADDRESS:
            {
               unsigned idx;
               for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
                  unsigned chan;
                  for (chan = 0; chan < NUM_CHANNELS; chan++) {
                     ctx->bld_ctx.soa.addr[idx][chan] = lp_build_alloca(
                                                                  &ctx->gallivm,
                                       ctx->bld_ctx.soa.uint_bld.elem_type, "");
                  }
               }
               break;
            }
            case TGSI_FILE_TEMPORARY:
               emit_declaration(ctx, decl);
               break;

            case TGSI_FILE_INPUT:
               {
                  unsigned idx;
                  for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
                     ctx->load_input(ctx, idx, decl);
                  }
                  break;
               }

            case TGSI_FILE_OUTPUT:
               {
                  assert(!ctx->aos);
                  unsigned idx;
                  for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
                     unsigned chan;
                     assert(idx < TGSI_LLVM_MAX_OUTPUTS);
                     for (chan = 0; chan < NUM_CHANNELS; chan++) {
                        outputs[idx][chan] = lp_build_alloca(&ctx->gallivm,
                                          ctx->bld_ctx.soa.base.elem_type, "");
                     }
                  }
                  ctx->output_reg_count = MAX2(ctx->output_reg_count,
                                               decl->Range.Last + 1);
                  break;
               }

            default:
               break;
            }
            break;
         }
      case TGSI_TOKEN_TYPE_INSTRUCTION:
         add_instruction(ctx, &parse.FullToken.FullInstruction);
         break;

      case TGSI_TOKEN_TYPE_IMMEDIATE:
         {
            if (ctx->aos) {
            /* XXX: Cut and pasted from lp_bld_tgsi_aos.c.  This should be a function somewhere */
               const uint size = parse.FullToken.FullImmediate.Immediate.NrTokens - 1;
               float imm[4];
               unsigned chan;
               assert(size <= 4);
               assert(num_immediates < LP_MAX_TGSI_IMMEDIATES);
               for (chan = 0; chan < 4; ++chan) {
                  imm[chan] = 0.0f;
               }
               for (chan = 0; chan < size; ++chan) {
                  unsigned swizzle = ctx->bld_ctx.aos.swizzles[chan];
                  imm[swizzle] = parse.FullToken.FullImmediate.u[chan].Float;
               }
               ctx->bld_ctx.aos.immediates[num_immediates] =
                        lp_build_const_aos(&ctx->gallivm,
                                           ctx->bld_ctx.aos.base.type,
                                           imm[0], imm[1], imm[2], imm[3],
                                           NULL);
               num_immediates++;
            } else {
            /* XXX: Cut and pasted from lp_bld_tgsi_soa.c. */
               const uint size = parse.FullToken.FullImmediate.Immediate.NrTokens - 1;
               assert(size <= 4);
               assert(num_immediates < LP_MAX_TGSI_IMMEDIATES);
               for( i = 0; i < size; ++i )
                  ctx->bld_ctx.soa.immediates[num_immediates][i] =
                     lp_build_const_vec(&ctx->gallivm, ctx->bld_ctx.soa.base.type,
                                        parse.FullToken.FullImmediate.u[i].Float);
               for( i = size; i < 4; ++i )
                  ctx->bld_ctx.soa.immediates[num_immediates][i] = ctx->bld_ctx.soa.base.undef;
               num_immediates++;

            }
            break;
         }
      default:
         break;
      }
   }

   /* Allocate outputs */
   ctx->bld_ctx.soa.outputs = outputs;

   /* Emit the instructions */
   emit_instructions(ctx);

   if (ctx->emit_epilogue) {
      ctx->emit_epilogue(ctx);
   }

   /* End the main function with */
   LLVMBuildRetVoid(ctx->gallivm.builder);

   /* Create the pass manager */
   ctx->gallivm.passmgr = LLVMCreateFunctionPassManagerForModule(
                                                        ctx->gallivm.module);

   /* This pass should eliminate all the load and store instructions */
   LLVMAddPromoteMemoryToRegisterPass(ctx->gallivm.passmgr);

   /* Add some optimization passes */
   LLVMAddScalarReplAggregatesPass(ctx->gallivm.passmgr);
   LLVMAddCFGSimplificationPass(ctx->gallivm.passmgr);

   /* Run the passs */
   LLVMRunFunctionPassManager(ctx->gallivm.passmgr, ctx->main_fn);

  /* Clean up */
   if (ctx->aos) {
      FREE(ctx->bld_ctx.aos.inst_list.instructions);
   } else {
      FREE(ctx->bld_ctx.soa.inst_list.instructions);
   }

   LLVMDisposeBuilder(ctx->gallivm.builder);
   LLVMDisposePassManager(ctx->gallivm.passmgr);

   return ctx->gallivm.module;
}

void tgsi_llvm_dispose(struct tgsi_llvm_context * ctx)
{
   LLVMDisposeModule(ctx->gallivm.module);
   LLVMContextDispose(ctx->gallivm.context);
}
