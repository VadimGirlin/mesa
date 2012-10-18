/*
 * Copyright 2011 Vadim Girlin <vadimgirlin@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "eg_sq.h"
#include "evergreend.h"
#include "r600_shader.h"


#include "pipe/p_compiler.h"

#include "r600_pipe.h"
#include "r600_asm.h"
#include "r600_opcodes.h"



#include "opt_core.h"

static char * chans = "xyzwt";

#define R600_DISASM_ARGS_OFFSET 16

int _r600_opt_dump_level = -1;

char * eg_alu_op2_inst_names[256] =
{
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_PRED_SETE_INT] = "PRED_SETE_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOT_INT] = "NOT_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_OR_INT] = "OR_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_XOR_INT] = "XOR_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGT_INT] = "SETGT_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGE_UINT] = "SETGE_UINT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGE_INT] = "SETGE_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETE_INT] = "SETE_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIP_INT] = "RECIP_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIP_UINT] = "RECIP_UINT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MULLO_INT] = "MULLO_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MULHI_INT] = "MULHI_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MULLO_UINT] = "MULLO_UINT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MULHI_UINT] = "MULHI_UINT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ASHR_INT] = "ASHR_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LSHR_INT] = "LSHR_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LSHL_INT] = "LSHL_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLT_TO_UINT] = "FLT_TO_UINT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_UINT_TO_FLT] = "UINT_TO_FLT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_LOAD_P0] = "INTERP_LOAD_P0",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SUB_INT] = "SUB_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETE_INT] = "SETE_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_AND_INT] = "AND_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLT_TO_INT] = "FLT_TO_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT] = "INT_TO_FLT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_PRED_SETE] = "PRED_SETE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_PRED_SETNE_INT] = "PRED_SETNE_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SIN] = "SIN",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LOG_IEEE] = "LOG_IEEE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLOOR] = "FLOOR",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_TRUNC] = "TRUNC",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_COS] = "COS",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FRACT] = "FRACT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETNE] = "SETNE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGE] = "SETGE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETE]= "SETE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD] = "ADD",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL] = "MUL",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGT] = "KILLGT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV] = "MOV",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_XY] = "INTERP_XY",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_ZW] = "INTERP_ZW",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_CUBE] = "CUBE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4] = "DOT4",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIP_IEEE] = "RECIP_IEEE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIPSQRT_IEEE] = "RECIPSQRT_IEEE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MAX] = "MAX",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MIN] = "MIN",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGT] = "SETGT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_PRED_SETNE] = "PRED_SETNE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_EXP_IEEE] = "EXP_IEEE",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LOG_CLAMPED] = "LOG_CLAMPED",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP] = "NOP",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIPSQRT_CLAMPED] = "RECIPSQRT_CLAMPED",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT] = "ADD_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOVA_INT] = "MOVA_INT",
		[EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLT_TO_INT_FLOOR] = "FLT_TO_INT_FLOOR"


};

char * eg_alu_op3_inst_names[256] =
{
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDE_INT] = "CNDE_INT",
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDGT_INT] = "CNDGT_INT",
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDGE_INT] = "CNDGE_INT",
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDE] = "CNDE",
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDGT] = "CNDGT",
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDGE] = "CNDGE",
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD] = "MULADD",
		[EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MUL_LIT] = "MUL_LIT"
};

char * eg_cf_alu_inst_names[256] =
{
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU)] = "ALU",
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE)] = "ALU_PUSH_BEFORE",
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_POP_AFTER)] = "ALU_POP_AFTER",
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_POP2_AFTER)] = "ALU_POP2_AFTER",
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_EXTENDED)] = "EXTENDED",
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_CONTINUE)] = "ALU_CONTINUE",
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_BREAK)] = "ALU_BREAK",
		[EG_G_SQ_CF_ALU_WORD1_CF_INST(EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_ELSE_AFTER)] = "ALU_ELSE_AFTER"
};

char * eg_cf_inst_names[256] =
{
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF0)] = "MEM_STREAM0_BUF0",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF1)] = "MEM_STREAM0_BUF1",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF2)] = "MEM_STREAM0_BUF2",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF3)] = "MEM_STREAM0_BUF3",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF0)] = "MEM_STREAM1_BUF0",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF1)] = "MEM_STREAM1_BUF1",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF2)] = "MEM_STREAM1_BUF2",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF3)] = "MEM_STREAM1_BUF3",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF0)] = "MEM_STREAM2_BUF0",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF1)] = "MEM_STREAM2_BUF1",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF2)] = "MEM_STREAM2_BUF2",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF3)] = "MEM_STREAM2_BUF3",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF0)] = "MEM_STREAM3_BUF0",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF1)] = "MEM_STREAM3_BUF1",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF2)] = "MEM_STREAM3_BUF2",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF3)] = "MEM_STREAM3_BUF3",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_VTX)] = "VC",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_VC_ACK)] = "VC_ACK",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX)] = "TC",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_TC_ACK)] = "TC_ACK",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT)] = "EXPORT",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)] = "EXPORT_DONE",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP)] = "JUMP",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_ELSE)]="ELSE",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_CALL_FS)] = "CALL_FS",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_POP)] = "POP",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_RETURN)] = "RETURN",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_START)] = "LOOP_START",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_START_DX10)] = "LOOP_START_DX10",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_END)] = "LOOP_END",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_CONTINUE)] = "LOOP_CONTINUE",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_BREAK)] = "LOOP_BREAK",
		[EG_G_SQ_CF_WORD1_CF_INST(EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_START_NO_AL)] = "LOOP_START_NO_AL"
};


char * eg_tex_inst_names[256] =
{
		[SQ_TEX_INST_LD] = "LD",
		[SQ_TEX_INST_GET_TEXTURE_RESINFO] = "GET_TEXTURE_RESINFO",
		[SQ_TEX_INST_SAMPLE] = "SAMPLE",
		[SQ_TEX_INST_SAMPLE_L] = "SAMPLE_L",
		[SQ_TEX_INST_SAMPLE_LB] = "SAMPLE_LB",
		[SQ_TEX_INST_SAMPLE_LZ] = "SAMPLE_LZ",
		[SQ_TEX_INST_SAMPLE_G] = "SAMPLE_G",
		[SQ_TEX_INST_SAMPLE_G_L] = "SAMPLE_G_L",
		[SQ_TEX_INST_SAMPLE_G_LB] = "SAMPLE_G_LB",
		[SQ_TEX_INST_SAMPLE_G_LZ] = "SAMPLE_G_LZ",
		[SQ_TEX_INST_SAMPLE_C] = "SAMPLE_C",
		[SQ_TEX_INST_SAMPLE_C_G] = "SAMPLE_C_G",

		[SQ_TEX_INST_GET_GRADIENTS_H] = "GET_GRADIENTS_H",
		[SQ_TEX_INST_GET_GRADIENTS_V] = "GET_GRADIENTS_V",
		[SQ_TEX_INST_SET_GRADIENTS_H] = "SET_GRADIENTS_H",
		[SQ_TEX_INST_SET_GRADIENTS_V] = "SET_GRADIENTS_V"
};

char * eg_vtx_inst_names[256] =
{
		[0] = "FETCH"
};

static void fprint_alu_src(FILE * f, struct r600_bytecode_alu_src * src)
{
	unsigned sel = src->sel, chan = src->chan, rel = src->rel;
	boolean prchan = true;

	if (src->neg)
		fprintf(f, "-");

	if (src->abs)
		fprintf(f, "|");

	if (sel<128)
		fprintf(f, "R%u", sel);
	else if (sel<160)
		fprintf(f, "KC0[%u]", sel-128);
	else if (sel<192)
		fprintf(f, "KC1[%u]", sel-160);
	else if (sel>=256) {
		if (sel>=512)
			fprintf(f, "CC%u",sel-512);
		else if (sel>=448)
			fprintf(f, "Param%u",sel-448);
		else if (sel>=288)
			fprintf(f, "KC3[%u]",sel-288);
		else if (sel>=256)
			fprintf(f, "KC2[%u]",sel-256);
	} else {
		switch (sel) {
		case V_SQ_ALU_SRC_0:
			fprintf(f,"0");
			prchan=false;
			break;
		case V_SQ_ALU_SRC_1:
			fprintf(f,"1.0f");
			prchan=false;
			break;
		case V_SQ_ALU_SRC_1_INT:
			fprintf(f,"1");
			prchan=false;
			break;
		case V_SQ_ALU_SRC_M_1_INT:
			fprintf(f,"-1");
			prchan=false;
			break;
		case V_SQ_ALU_SRC_0_5:
			fprintf(f,"0.5f");
			prchan=false;
			break;
		case V_SQ_ALU_SRC_LITERAL:
			fprintf(f,"( %f [0x%08X] )", *(float*)&src->value, src->value); prchan=false;
			break;
		case V_SQ_ALU_SRC_PV:
			fprintf(f,"PV");
			break;
		case V_SQ_ALU_SRC_PS:
			fprintf(f,"PS"); prchan=false;
			break;
		default:
			fprintf(f,"V%u",sel);
		}
	}

	if (rel)
		fprintf(f,"[AR]");

	if (prchan)
		fprintf(f,".%c",chans[chan]);

	if (src->abs)
		fprintf(f, "|");

}

static void print_src(struct r600_bytecode_alu_src * src)
{
	fprint_alu_src(stderr, src);
}

static void fprint_alu_dst(FILE * f, struct r600_bytecode_alu_dst * dst)
{
	boolean prchan=true;

	unsigned sel = dst->sel;

	if (dst->write)
		fprintf(f, "R%u", sel);
	else
		fprintf(f,"__");

	if (dst->rel) {
		fprintf(f,"[AR]");
	}

	if (prchan) fprintf(f,".%c",chans[dst->chan]);
}


static void print_dst(struct r600_bytecode_alu_dst * dst)
{
	fprint_alu_dst(stderr, dst);
}

static void fprint_swz(FILE *f, unsigned s) {
	static char * ssels="xyzw01 _";
	fprintf(f,"%c",ssels[s]);
}

static void fprint_swz_n(FILE *f, unsigned *s, unsigned count) {
	int q;
	for (q=0; q<count; q++)
		fprint_swz(f, s[q]);
}

static void print_rev_dst_swz(unsigned *s)
{
	unsigned d[4] = {7,7,7,7}, q;

	for (q=0; q<4; q++)
		if (s[q]<4) d[s[q]] = q;

	fprintf(stderr, " {");

	for (q=0; q<4; q++)
		fprint_swz(stderr, d[q]);

	fprintf(stderr, "} ");
}

void indent(int level)
{
	while (level--)
		fprintf(stderr, "        ");
}

void dump_tex(struct shader_info * info, int level, struct r600_bytecode_tex * tex)
{
	indent(level);
	if (eg_tex_inst_names[tex->inst])
		fprintf(stderr,"\t\t%s\t ",eg_tex_inst_names[tex->inst]);
	else
		fprintf(stderr,"\t\t(tex_inst_0x%X)\t ", tex->inst);

	fprintf(stderr,"R%u.",tex->dst_gpr);
	unsigned swz[4];
	swz[0] = tex->dst_sel_x;
	swz[1] = tex->dst_sel_y;
	swz[2] = tex->dst_sel_z;
	swz[3] = tex->dst_sel_w;

	fprint_swz_n(stderr, swz, 4);

	print_rev_dst_swz(swz);

	fprintf(stderr,", R%u.",tex->src_gpr);
	fprint_swz(stderr, tex->src_sel_x);
	fprint_swz(stderr, tex->src_sel_y);
	fprint_swz(stderr, tex->src_sel_z);
	fprint_swz(stderr, tex->src_sel_w);
	fprintf(stderr,", t%u, s%u\n",tex->resource_id,tex->sampler_id);
}

void dump_vtx(struct shader_info * info, int level, struct r600_bytecode_vtx * vtx)
{
	if (0) {
		indent(level);
		fprintf(stderr,  "\t\tINST:%d ", vtx->inst);
		fprintf(stderr,  "FETCH_TYPE:%d ", vtx->fetch_type);
		fprintf(stderr,  "BUFFER_ID:%d\n", vtx->buffer_id);

		indent(level);
		fprintf(stderr,  "\t\tSRC(GPR:%d ", vtx->src_gpr);
		fprintf(stderr,  "SEL_X:%d) ", vtx->src_sel_x);

		if (info->shader->bc.chip_class < CAYMAN)
			fprintf(stderr,  "MEGA_FETCH_COUNT:%d ", vtx->mega_fetch_count);
		else
			fprintf(stderr,  "SEL_Y:%d) ", 0);
		fprintf(stderr,  "DST(GPR:%d ", vtx->dst_gpr);
		fprintf(stderr,  "SEL_X:%d ", vtx->dst_sel_x);
		fprintf(stderr,  "SEL_Y:%d ", vtx->dst_sel_y);
		fprintf(stderr,  "SEL_Z:%d ", vtx->dst_sel_y);
		fprintf(stderr,  "SEL_W:%d) ", vtx->dst_sel_w);
		fprintf(stderr,  "USE_CONST_FIELDS:%d ", vtx->use_const_fields);
		fprintf(stderr,  "FORMAT(DATA:%d ", vtx->data_format);
		fprintf(stderr,  "NUM:%d ", vtx->num_format_all);
		fprintf(stderr,  "COMP:%d ", vtx->format_comp_all);
		fprintf(stderr,  "MODE:%d)\n", vtx->srf_mode_all);

		indent(level);
		fprintf(stderr,  "\t\tENDIAN:%d ", vtx->endian);
		fprintf(stderr,  "OFFSET:%d\n", vtx->offset);
	}

	indent(level);
	if (eg_vtx_inst_names[vtx->inst])
		fprintf(stderr,"\t\t%s\t ",eg_vtx_inst_names[vtx->inst]);
	else
		fprintf(stderr,"\t\t(vtx_inst_0x%X)\t ", vtx->inst);

	fprintf(stderr,"R%u.",vtx->dst_gpr);

	unsigned swz[4];
	swz[0] = vtx->dst_sel_x;
	swz[1] = vtx->dst_sel_y;
	swz[2] = vtx->dst_sel_z;
	swz[3] = vtx->dst_sel_w;

	fprint_swz_n(stderr, swz, 4);

	print_rev_dst_swz(swz);

	fprintf(stderr,", R%u.",vtx->src_gpr);
	fprint_swz(stderr, vtx->src_sel_x);

	fprintf(stderr,"\n");
}

void dump_cf(struct shader_info * info, int level, struct r600_bytecode_cf * cf)
{
	unsigned id = cf->id;
	struct r600_bytecode * bc = &info->shader->bc;
	int q, exp_inst = 0;

	switch (cf->inst) {
	case (EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU):
	case (EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_POP_AFTER):
	case (EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_POP2_AFTER):
	case (EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE):

	if (cf->eg_alu_extended) {
		indent(level);
		fprintf(stderr, "%04d %08X ALU_EXT0 ", id, bc->bytecode[id]);
		fprintf(stderr, "KCACHE_BANK2:%X ", cf->kcache[2].bank);
		fprintf(stderr, "KCACHE_BANK3:%X ", cf->kcache[3].bank);
		fprintf(stderr, "KCACHE_MODE2:%X\n", cf->kcache[2].mode);
		id++;
		indent(level);
		fprintf(stderr, "%04d %08X ALU_EXT1 ", id, bc->bytecode[id]);
		fprintf(stderr, "KCACHE_MODE3:%X ", cf->kcache[3].mode);
		fprintf(stderr, "KCACHE_ADDR2:%X ", cf->kcache[2].addr);
		fprintf(stderr, "KCACHE_ADDR3:%X\n", cf->kcache[3].addr);
		id++;
	}

	indent(level);
	fprintf(stderr, "%04d %08X ALU ", id, bc->bytecode[id]);
	fprintf(stderr, "ADDR:%d ", cf->addr);
	fprintf(stderr, "KCACHE_MODE0:%X ", cf->kcache[0].mode);
	fprintf(stderr, "KCACHE_BANK0:%X ", cf->kcache[0].bank);
	fprintf(stderr, "KCACHE_BANK1:%X\n", cf->kcache[1].bank);
	id++;
	indent(level);
	fprintf(stderr, "%04d %08X ALU ", id, bc->bytecode[id]);
	fprintf(stderr, "INST:0x%X %s ", EG_G_SQ_CF_ALU_WORD1_CF_INST(cf->inst), eg_cf_alu_inst_names[EG_G_SQ_CF_ALU_WORD1_CF_INST(cf->inst)]);
	fprintf(stderr, "KCACHE_MODE1:%X ", cf->kcache[1].mode);
	fprintf(stderr, "KCACHE_ADDR0:%X ", cf->kcache[0].addr);
	fprintf(stderr, "KCACHE_ADDR1:%X ", cf->kcache[1].addr);
	fprintf(stderr, "COUNT:%d\n", cf->ndw / 2);
	break;
	case EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX:
		indent(level);
		fprintf(stderr, "%04d %08X TEX ", id, bc->bytecode[id]);
		fprintf(stderr, "ADDR:%d\n", cf->addr);
		id++;
		indent(level);
		fprintf(stderr, "%04d %08X TEX ", id, bc->bytecode[id]);
		fprintf(stderr, "INST:%X %s ", EG_G_SQ_CF_WORD1_CF_INST(cf->inst), eg_cf_inst_names[EG_G_SQ_CF_WORD1_CF_INST(cf->inst)]);
		fprintf(stderr, "COUNT:%d\n", cf->ndw / 4);
		break;
	case EG_V_SQ_CF_WORD1_SQ_CF_INST_VTX:
		indent(level);
		fprintf(stderr, "%04d %08X VTX ", id, bc->bytecode[id]);
		fprintf(stderr, "ADDR:%d\n", cf->addr);
		id++;
		indent(level);
		fprintf(stderr, "%04d %08X VTX ", id, bc->bytecode[id]);
		fprintf(stderr, "INST:%X %s ", EG_G_SQ_CF_WORD1_CF_INST(cf->inst), eg_cf_inst_names[EG_G_SQ_CF_WORD1_CF_INST(cf->inst)]);
		fprintf(stderr, "COUNT:%d\n", cf->ndw / 4);
		break;
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE:
		exp_inst = 1;
		/* fallthrough */
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF0:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF1:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF2:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM0_BUF3:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF0:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF1:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF2:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM1_BUF3:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF0:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF1:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF2:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM2_BUF3:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF0:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF1:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF2:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_STREAM3_BUF3:

		id++;
		indent(level);

		fprintf(stderr,"\t\t %s\t ", eg_cf_inst_names[EG_G_SQ_CF_ALLOC_EXPORT_WORD1_CF_INST(cf->output.inst)]);

		if (exp_inst) {
			switch (cf->output.type) {
			case V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM : fprintf(stderr,"PARAM"); break;
			case V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL : fprintf(stderr,"PIXEL"); break;
			case V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS : fprintf(stderr,"POS  "); break;
			default : fprintf(stderr,"???"); break;
			}
		} else {
			switch (cf->output.type) {
			case 0 : fprintf(stderr, "WRITE"); break;
			case 1 : fprintf(stderr, "WRITE_IND"); break;
			case 2 : fprintf(stderr, "WRITE_ACK"); break;
			case 3 : fprintf(stderr, "WRITE_IND_ACK"); break;
			default : fprintf(stderr,"???"); break;
			}
		}

		if (cf->output.burst_count<=1)
			fprintf(stderr," %d,\tR%u.",cf->output.array_base, cf->output.gpr);
		else
			fprintf(stderr," %d-%d,\tR(%u-%u).",cf->output.array_base, cf->output.array_base+cf->output.burst_count-1,
					cf->output.gpr,cf->output.gpr+cf->output.burst_count-1);

		if (exp_inst) {
			fprint_swz(stderr, cf->output.swizzle_x);
			fprint_swz(stderr, cf->output.swizzle_y);
			fprint_swz(stderr, cf->output.swizzle_z);
			fprint_swz(stderr, cf->output.swizzle_w);
		} else {
			for (q=0; q<4; q++)
				fprint_swz(stderr, ((cf->output.comp_mask >> q)&1) ? q : 7);
		}

		fprintf(stderr,"\n");

		break;



		case EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_ELSE:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_POP:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_START_NO_AL:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_START_DX10:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_END:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_CONTINUE:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_BREAK:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_CALL_FS:
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_RETURN:
		case CM_V_SQ_CF_WORD1_SQ_CF_INST_END:
			indent(level);
			fprintf(stderr, "%04d %08X CF ", id, bc->bytecode[id]);
			fprintf(stderr, "ADDR:%d\n", cf->cf_addr);
			id++;
			indent(level);
			fprintf(stderr, "%04d %08X CF ", id, bc->bytecode[id]);
			fprintf(stderr, "INST:%X %s ", EG_G_SQ_CF_WORD1_CF_INST(cf->inst), eg_cf_inst_names[EG_G_SQ_CF_WORD1_CF_INST(cf->inst)]);
			fprintf(stderr, "COND:%X ", cf->cond);
			fprintf(stderr, "POP_COUNT:%X ", cf->pop_count);
			fprintf(stderr, "\n");
			break;

		default:
			fprintf(stderr, "unknown cf instruction 0x%X\n", cf->inst);
			assert(0);

	}

}

void dump_alu(struct shader_info * info, int level, struct r600_bytecode_alu * alu)
{
	struct r600_bytecode *bc = &info->shader->bc;
	int sz, num_op = r600_bytecode_get_num_operands(bc,alu);
	char *pn;

	R600_DUMP( "%c ", alu->last ? '*' : ' ');

	if (alu->is_op3) {

		pn = eg_alu_op3_inst_names[alu->inst];

		if (pn)
			sz = fprintf(stderr,"%s", pn);
		else
			sz = fprintf(stderr,"(inst_op3__0x%X)", alu->inst);

	} else {

		pn = eg_alu_op2_inst_names[alu->inst];

		if (pn)
			sz = fprintf(stderr,"%s", pn);
		else
			sz = fprintf(stderr,"(inst_op2_0x%X)", alu->inst);
	}

	if (alu->dst.clamp)
		sz += fprintf(stderr,"_sat");

	sz = MIN2(sz, R600_DISASM_ARGS_OFFSET-2);

	while (sz++ < R600_DISASM_ARGS_OFFSET)
		fprintf(stderr," ");

	print_dst(&alu->dst);

	for (int q=0;q<num_op;q++) {
		fprintf(stderr,", ");
		print_src(&alu->src[q]);
	}

	fprintf(stderr,"\n");
}


void fprint_reg(FILE *f, unsigned k)
{
	fprintf(f, "R%d.", KEY_REG(k));
	fprint_swz(f, KEY_CHAN(k));
	fprintf(f, "  ");
}

void print_reg(unsigned k)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	fprint_reg(stderr, k);

}

void fprint_var(FILE *f, struct var_desc * v)
{
	boolean pr_chan = true;

	if (v && (v->flags & VF_DEAD))
		fprintf(f,"{");

	if (v==NULL) {
		fprintf(f," __ ");

		return;
	} else if (v->flags & VF_TEMP) {
		fprintf(f, "v%d", v->reg.reg);
		pr_chan=false;

	} else if (v->reg.reg & REG_SPECIAL) {
		unsigned r = v->reg.reg;

		if (r==REG_PR) {
			fprintf(f,"PR");
			pr_chan = false;
		} else if (r==REG_AM) {
			fprintf(f,"AM");
			pr_chan = false;
		} else if (r==REG_AR)
			fprintf(f,"AR");
		else if (r==REG_GR)
			fprintf(f,"GR");
		else if (r==REG_AL)
			fprintf(f,"AL");
		else
			fprintf(f,"S%u.",r);

	} else
		fprintf(f,"R%u.",v->reg.reg);

	if (pr_chan && (v->reg.chan >= 0)) {
		assert(v->reg.chan<4);
		fprint_swz(f, v->reg.chan);
	}

	if (v->index) {
		fprintf(f,".%u",v->index);
	}

	if (v && (v->flags & VF_DEAD))
		fprintf(f,"}");

}

void print_var(struct var_desc * v)
{

	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	fprint_var(stderr, v);

	fprintf(stderr, "  ");
}

void dump_vars(void ** vars, unsigned count)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	int q;
	for (q=0; q < count; q++)
		print_var(vars[q]);
}

void dump_vset(struct vset * s)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	if (s)
		dump_vars(s->keys, s->count);
}

static void dump_vars_colors(void ** vars, unsigned count)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;


	int q;
	for (q=0; q < count; q++) {
		struct var_desc * v = vars[q];

		if (q) R600_DUMP(", ");

		print_var(v);
		if (v->color) {
			R600_DUMP("@");
			print_reg(v->color);
		}
	}
}

void dump_vset_colors(struct vset * s)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	if (s)
		dump_vars_colors(s->keys, s->count);
}


void dump_vvec(struct vvec * s)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	if (s)
		dump_vars(s->keys, s->count);
}

void dump_node(struct shader_info * info, struct ast_node * node, int level)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	if (node->p_split)
	{
		indent(level);
		R600_DUMP("p_split:\n");
		dump_node(info, node->p_split, level);
	}

	switch (node->type) {
	case NT_REGION:
		indent(level);
		R600_DUMP( "region %d", node->label);
		R600_DUMP( "dc=%u rc=%u ",node->depart_count,node->repeat_count);
		break;
	case NT_DEPART:
		indent(level);
		R600_DUMP( "depart %d", node->target);
		if (node->child) R600_DUMP(" after");
		R600_DUMP( "   dn=%u ",node->depart_number);
		break;
	case NT_REPEAT:
		indent(level);
		R600_DUMP( "repeat %d ", node->target);
		if (node->child) R600_DUMP(" after");
		R600_DUMP( "   rn=%u ",node->repeat_number);
		break;
	case NT_IF:
		indent(level);
		R600_DUMP( "IF ");
		break;
	case NT_OP:
		indent(level);
		R600_DUMP( "=");
		break;
	case NT_GROUP:
		indent(level);
		R600_DUMP( "{}");
		break;
	case NT_LIST:
		break;
	default:
		indent(level);
		R600_DUMP( "!!!!INVALID NODE TYPE %u", node->type);
		break;
	}

	if (node->flow_dep) {
		R600_DUMP(" fd: ");
		print_var(node->flow_dep);
	}

	if (node->type != NT_LIST) {
		if (node->min_prio) {
			R600_DUMP(" p=%X ", node->min_prio);
			if (node->max_prio)
				R600_DUMP("... %X ", node->max_prio);
		}
	}

	if (node->subtype == NST_COPY) {
		R600_DUMP(" COPY ");
	}
	if (node->subtype == NST_PCOPY) {
		R600_DUMP(" PARALLEL_COPY ");
	}

	if (node->slot>0) {
		R600_DUMP("  SLOT: %u  ", node->slot);
	}

	if (node->flags & AF_ALU_CLAUSE_SPLIT) {
		R600_DUMP(" ALU_CLAUSE_SPLIT ");
	}

	if (node->flags & AF_DEAD) {
		R600_DUMP(" DEAD ");
	}

	if (node->flags & AF_FOUR_SLOTS_INST) {
		R600_DUMP(" 4S ");
	}

	if (node->flags & AF_ALU_DELETE)
		R600_DUMP("  ALU_DELETE ");


	if (node->flags & AF_ALU_CLAMP_DST)
		R600_DUMP("  AF_ALU_CLAMP_DST ");

	if (node->flags & AF_REG_CONSTRAINT)
		R600_DUMP("  AF_REGS_CONSTRAINT ");

	if (node->flags & AF_CHAN_CONSTRAINT)
		R600_DUMP("  AF_CHAN_CONSTRAINT ");

	if (node->flags & AF_COPY_HINT)
		R600_DUMP("  AF_COPY_HINT ");

	if (info->liveness_correct && node->type!=NT_LIST && node->subtype!=NST_PHI) {
		R600_DUMP("  \tlive: ");
		dump_vset(node->vars_live);

		R600_DUMP("  \tlive_after: ");
		dump_vset(node->vars_live_after);
	}

	R600_DUMP("\n");

	if (node->type != NT_LIST && node->vars_used) {
		indent(level);
		R600_DUMP("used vars: ");
		dump_vset(node->vars_used);
		R600_DUMP("\n");
	}


	if (node->cf)
		dump_cf(info, level+3, node->cf);

	if (node->alu) {
		indent(level+3);
		dump_alu(info, level+3, node->alu);
	}

	if (node->tex)
		dump_tex(info, level+3, node->tex);

	if (node->vtx)
		dump_vtx(info, level+3, node->vtx);

	if (node->outs) {
		indent(level+3);
		R600_DUMP("outs: ");
		dump_vvec(node->outs);
	}

	if (node->ins) {
		if (!node->outs)
			indent(level+3);
		R600_DUMP("   ins: ");
		dump_vvec(node->ins);
		R600_DUMP("\n");
	}

	if (!node->ins && node->outs)
		R600_DUMP(" \n");

	if (node->loop_phi)
	{
		indent(level);
		R600_DUMP("loop_phi:\n");
		dump_node(info, node->loop_phi, level);
	}

	if (node->child)
		dump_node(info, node->child, level+1);

	if (node->rest)
		dump_node(info, node->rest, level);


	if (node->phi)
	{
		indent(level);
		R600_DUMP("phi:\n");
		dump_node(info, node->phi, level);
	}

	if (node->p_split_outs)
	{
		indent(level);
		R600_DUMP("p_split_outs:\n");
		dump_node(info, node->p_split_outs, level);
	}

	return;
}

void dump_shader_tree(struct shader_info * info)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP("#######################\nTREE_START\n");
	dump_node(info, info->root, 1);
	R600_DUMP("TREE_END\n");
}

void dump_var_desc(struct var_desc * v)
{
	if (v->def && (v->def->flags & AF_DEAD))
		R600_DUMP( "    #DEAD# ");


	print_var(v);

	if (v->color) {
		R600_DUMP( "c:%d ",v->color);
		print_reg(v->color);
	}

	if (v->value_hint) {
		R600_DUMP( " val = ");
		print_var(v->value_hint);
	}

	if (v->reg.reg!=-1) {
		R600_DUMP( " areg = %d", v->reg.reg);
	}

	if (v->reg.chan!=-1) {
		R600_DUMP( " achan = %d", v->reg.chan);
	}


	if (v->flags & VF_UNDEFINED)
		R600_DUMP( " UNDEFINED ");

	if (v->flags & VF_PIN_CHAN)
		R600_DUMP( " PIN_CHAN ");

	if (v->flags & VF_PIN_REG)
		R600_DUMP( " PIN_REG ");

	if (v->constraint) {
		R600_DUMP( "\n\t\tconstraint ");
		dump_vvec(v->constraint->comps);
	}

	if (v->bs_constraint) {
		R600_DUMP( "\n\t\tbs_constraint ");
		dump_vvec(v->bs_constraint->comps);
	}

	if (v->interferences) {
		R600_DUMP( "\n\t\t\tinterferences: ");
		dump_vset(v->interferences);
	}

	R600_DUMP( "\n");
}


void dump_var_table(struct shader_info * info)
{
	int q;

	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP( "###### VAR_TABLE START\n");

	for (q=0; q<info->vars->count; q++) {
		struct var_desc * v = info->vars->keys[q*2+1];

		dump_var_desc(v);

	}

	R600_DUMP( "###### VAR_TABLE END\n");

}

void dump_chunk(struct affinity_chunk * c)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP(" === chunk cost: %d, vars: ", c->cost);
	dump_vset(c->vars);
	R600_DUMP("\n");

}

void dump_chunk_group(struct chunk_group * g)
{
	int q;
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP("CHUNK GROUP (cost = %d) :\n", g->cost);

	for (q=0; q<g->chunks->count; q++) {
		struct affinity_chunk * c = g->chunks->keys[q];
		dump_chunk(c);
	}

	R600_DUMP("\n");
}


void dump_chunks_queue(struct shader_info * info)
{
	int q;

	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP("### CHUNKS \n");

	for (q=info->chunk_queue->count-1; q>=0; q--) {
		struct affinity_chunk * c = info->chunk_queue->keys[q*2+1];

		dump_chunk(c);
	}
}


void dump_chunk_group_queue(struct shader_info * info)
{
	int q;

	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP("### CHUNK GROUPS: \n");

	for (q=info->chunk_groups->count-1; q>=0; q--) {
		struct chunk_group * c = info->chunk_groups->keys[q*2+1];

		dump_chunk_group(c);
	}
}



void dump_reg_map(struct vmap * map)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP("### REGMAP: ");

	for (int q=0; q< map->count; q++) {
		print_reg((uintptr_t)map->keys[q*2]);
		R600_DUMP("-> ");
		print_var(map->keys[q*2+1]);
		R600_DUMP(", ");
	}
	R600_DUMP("\n");
}

void dump_color_set(struct vset * c)
{
	if (!check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG))
		return;

	R600_DUMP(" [ ");
	for (int q=0; q< c->count; q++) {
		print_reg((uintptr_t)c->keys[q]);

	}
	R600_DUMP(" ] ");
}

void dump_bytecode(struct shader_info * info)
{
	struct r600_bytecode *bc = &info->shader->bc;
	struct r600_bytecode_cf *cf = NULL;
	struct r600_bytecode_alu *alu = NULL;
	struct r600_bytecode_vtx *vtx = NULL;
	struct r600_bytecode_tex *tex = NULL;
	int level = 0;
	unsigned ngroup = 1, new_alu_group;
	unsigned i, id;
	uint32_t literal[4];
	unsigned nliteral;
	char chip = '6';

	if (!check_dump_level(R600_OPT_DUMP_LEVEL_SHADERS))
		return;

	switch (bc->chip_class) {
	case R700:
		chip = '7';
		break;
	case EVERGREEN:
		chip = 'E';
		break;
	case CAYMAN:
		chip = 'C';
		break;
	case R600:
	default:
		chip = '6';
		break;
	}
	fprintf(stderr, "shader %d:\nbytecode %d dw -- %d gprs ---------------------\n", info->shader_index, bc->ndw, bc->ngpr);
	fprintf(stderr, "     %c\n", chip);

	LIST_FOR_EACH_ENTRY(cf, &bc->cf, list) {

		dump_cf(info, level, cf);

		id = cf->addr;
		nliteral = 0;
		new_alu_group = 1;

		LIST_FOR_EACH_ENTRY(alu, &cf->alu, list) {
			r600_bytecode_alu_nliterals(bc, alu, literal, &nliteral);

			id++;

			if (new_alu_group) {
				fprintf(stderr, "\t\t%3d\t", ngroup);
				new_alu_group = 0;
			} else {
				fprintf(stderr, "\t\t\t");
			}

			dump_alu(info, level, alu);

			id++;
			if (alu->last) {
				for (i = 0; i < nliteral; i++, id++) {
					float *f = (float*)(bc->bytecode + id);
					fprintf(stderr, "\t\t\t\t   %04d %08X\t%f\n", id, bc->bytecode[id], *f);
				}
				id += nliteral & 1;
				nliteral = 0;

				new_alu_group=1;
				ngroup++;
			}

		}

		LIST_FOR_EACH_ENTRY(tex, &cf->tex, list) {
			dump_tex(info,level,tex);
		}

		LIST_FOR_EACH_ENTRY(vtx, &cf->vtx, list) {
			dump_vtx(info, level, vtx);
		}
	}

	fprintf(stderr, "--------------------------------------\n");
}
