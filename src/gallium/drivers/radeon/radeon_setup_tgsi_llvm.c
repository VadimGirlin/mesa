/*
 * Copyright 2011 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors: Tom Stellard <thomas.stellard@amd.com>
 *
 */
#include "tgsi/tgsi_llvm.h"

#include "radeon_llvm.h"

void radeon_setup_tgsi_llvm(struct tgsi_llvm_context * ctx)
{
	memset(ctx, 0, sizeof(struct tgsi_llvm_context));

	ctx->store_output_intr = "llvm.AMDISA.store.output.";
	ctx->load_const_intr = "llvm.AMDISA.load.const";
	ctx->swizzle_intr = "llvm.AMDISA.swizzle";
	ctx->aos = 0;

	ctx->intr_names[TGSI_OPCODE_ABS] = "llvm.AMDIL.fabs.";
	ctx->intr_names[TGSI_OPCODE_ARL] = "llvm.AMDISA.arl";
	ctx->intr_names[TGSI_OPCODE_CLAMP] = "llvm.AMDIL.clamp.";
	ctx->intr_names[TGSI_OPCODE_CMP] = "llvm.AMDISA.cndlt";
	ctx->intr_names[TGSI_OPCODE_COS] = "llvm.AMDISA.cos";
	ctx->intr_names[TGSI_OPCODE_DDX] = "llvm.AMDISA.ddx";
	ctx->intr_names[TGSI_OPCODE_DDY] = "llvm.AMDISA.ddy";
	ctx->intr_names[TGSI_OPCODE_DIV] = "llvm.AMDISA.div";
	ctx->intr_names[TGSI_OPCODE_DP4] = "llvm.AMDISA.dp4";
	ctx->intr_names[TGSI_OPCODE_EX2] = "llvm.AMDIL.exp.";
	ctx->intr_names[TGSI_OPCODE_EXP] = "llvm.AMDIL.exp.";
	ctx->intr_names[TGSI_OPCODE_FLR] = "llvm.AMDISA.floor";
	ctx->intr_names[TGSI_OPCODE_FRC] = "llvm.AMDIL.fraction.";
	ctx->intr_names[TGSI_OPCODE_KIL] = "llvm.AMDISA.kill";
	ctx->intr_names[TGSI_OPCODE_KILP] = "llvm.AMDISA.kilp";
	ctx->intr_names[TGSI_OPCODE_LG2] = "llvm.AMDIL.log.";
	ctx->intr_names[TGSI_OPCODE_LRP] = "llvm.AMDISA.lrp";
	ctx->intr_names[TGSI_OPCODE_MIN] = "llvm.AMDIL.min.";
	ctx->intr_names[TGSI_OPCODE_MAD] = "llvm.AMDIL.mad.";
	ctx->intr_names[TGSI_OPCODE_MAX] = "llvm.AMDIL.max.";
	ctx->intr_names[TGSI_OPCODE_MUL] = "llvm.AMDISA.mul";
	ctx->intr_names[TGSI_OPCODE_POW] = "llvm.AMDISA.pow";
	ctx->intr_names[TGSI_OPCODE_RCP] = "llvm.AMDISA.rcp";
	ctx->intr_names[TGSI_OPCODE_RSQ] = "llvm.AMDISA.rsq";
	ctx->intr_names[TGSI_OPCODE_SSG] = "llvm.AMDISA.ssg";
	ctx->intr_names[TGSI_OPCODE_SGE] = "llvm.AMDISA.sge.";
	ctx->intr_names[TGSI_OPCODE_SEQ] = "llvm.AMDISA.seq";
	ctx->intr_names[TGSI_OPCODE_SNE] = "llvm.AMDISA.sne";
	ctx->intr_names[TGSI_OPCODE_SGT] = "llvm.AMDISA.sgt";
	ctx->intr_names[TGSI_OPCODE_SIN] = "llvm.AMDISA.sin";
	ctx->intr_names[TGSI_OPCODE_TEX] = "llvm.AMDISA.tex";
	ctx->intr_names[TGSI_OPCODE_TXB] = "llvm.AMDISA.txb";
	ctx->intr_names[TGSI_OPCODE_TXD] = "llvm.AMDISA.txd";
	ctx->intr_names[TGSI_OPCODE_TXL] = "llvm.AMDISA.txl";
	ctx->intr_names[TGSI_OPCODE_TRUNC] = "llvm.AMDISA.trunc";
}

