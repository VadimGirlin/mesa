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

#ifndef OPT_CORE_H_

#include "pipe/p_compiler.h"

#include "r600_pipe.h"
#include "r600_asm.h"
#include "r600_opcodes.h"

#include "opt_vars.h"
#include "opt_dump.h"
#include "opt_color.h"

union fui
{
	float f;
	unsigned u;
	int i;
};


/* ast node flags */
enum ast_flags
{
	AF_NONE = 0,

	/* Some instructions have the reg constraint for inputs/outputs,
	 * e.g. all inputs for SAMPLE should be in same gpr,
	 * same for EXPORT, ...
	 */
	AF_REG_CONSTRAINT = (1<<0),

	/* reduction instructions and interp_* should be kept together
	 */
	AF_FOUR_SLOTS_INST = (1<<1),

	/* alu instruction with clamp output modifier */
	AF_ALU_CLAMP_DST = (1<<2),

	/* Channel constraint means that we shouldn't change original channels,
	 * e.g CUBE, INTERP_*
	 */
	AF_CHAN_CONSTRAINT = (1<<3),

	/* Some MOV instructions could be removed if we can coalesce input and output,
	 * this flag is used for such instructions
	 */
	AF_COPY_HINT = (1<<5),

	/* this flag means the node is dead and can be removed
	 * (such nodes are just ignored during processing)
	 */
	AF_DEAD = (1<<8),

	/* some instructions should be always live even when they have no live outputs
	 * (KILL, SET_GRADIENTS, MOVA etc)
	 *
	 * FIXME: probably it's possible to simplify it by using fake outputs
	 */
	AF_KEEP_ALIVE = (1<<10),

	/* for generated alu instructions (copy insertion), need to delete them later */
	AF_ALU_DELETE = (1<<12),

	/* alu clause split (due to instruction count limit, kcache lines limit, ... */
	/* set on the last group of the clause */
	AF_ALU_CLAUSE_SPLIT = (1<<13)

};

enum alu_slots {
	AT_VECTOR = 1,
	AT_TRANS = 2,
	AT_ANY = 3
};


enum node_condition {

	NC_ALWAYS,
	NC_ACTIVE,
	NC_BOOL,
};

/* FIXME: this data could be organized more efficiently
 */
struct ast_node
{
	enum node_type type;
	enum node_subtype subtype;
	enum node_op_class op_class;

	enum ast_flags flags;

	struct ast_node * parent;
	struct ast_node * child;

	unsigned insn;
	unsigned slot;

	/* region */
	int label;

	int repeat_count;
	int depart_count;

	/* depart & repeat */
	int target;
	struct ast_node *target_node;

	int depart_number;
	int repeat_number;

	int stack_level;



	/* list */
	struct ast_node * rest;


	struct vset * vars_defined;
	struct vset * vars_used;

	struct vset * vars_live;
	struct vset * vars_live_after;

	struct vvec * ins;
	struct vvec * outs;

	/* special input to describe control flow dependency with SSA form
	 * e.g. active mask for CF, predicate for ALU,
	 * FIXME: probably ALU clause should begin with pseudo-op copying AM to PR
	 */
	struct var_desc * flow_dep;

	/* if/predicate */
	enum node_condition condition;




	struct ast_node * phi;
	struct ast_node * loop_phi;

	struct ast_node * p_split;
	struct ast_node * p_split_outs;

	struct r600_bytecode_cf * cf;
	struct r600_bytecode_alu * alu;
	struct r600_bytecode_tex * tex;
	struct r600_bytecode_vtx * vtx;

	struct r600_bytecode_cf * new_cf; /* used for bytecode rebuilding */

	enum alu_slots alu_allowed_slots_type;


	int const_ins_count;

	/* FIXME: probably uint32 is enough. int32 is not enough (fp-indirections2) */
	unsigned min_prio;
	unsigned max_prio;

};

struct shader_stats
{
	unsigned ndw;
	unsigned ngpr;
	unsigned nalugroups;
};

enum shader_opt_result
{
	SR_UNKNOWN,
	SR_SUCCESS,

	/* optimization skipped due to relative addressing usage */
	SR_SKIP_RELADDR,

	/* post_schedule failed */
	SR_FAIL_SCHEDULE,

	/* insert_copies failed */
	SR_FAIL_INSERT_COPIES
};

struct shader_info
{
	struct r600_context * rctx;
	struct r600_shader * shader;
	int shader_index;
	boolean built;

	struct ast_node * root;

	struct vmap * def_count;

	unsigned next_temp;
	int dump;

	int max_slots;

	int last_input_gpr;

	int last_color;
	boolean enable_last_color_update;

	int temp_gprs;

	struct vmap * vars;

	struct r600_bytecode * bc;

	/* used during bytecode build */
	boolean force_cf;

	struct vque * edge_queue;
	struct vque * chunk_queue;
	struct vque * chunk_groups;


	/* used for dot graph building, to compare real dependencies with expected */
	struct vmap * rdefs;
	struct vmap * edefs;
	struct vmap * p_rdefs;
	struct vmap * p_edefs;

	int stack_level;

	struct shader_stats stats[2];

	enum shader_opt_result result_code;

	/* for dump - it doesn't make sense to dump live sets when
	 * they are not correct */
	boolean liveness_correct;

	struct vmap * fetch_levels;
};


int is_alu_kill_inst(struct r600_bytecode * bc, struct r600_bytecode_alu *alu);
boolean post_schedule(struct shader_info *info);
struct ast_node * create_node(enum node_type type);
void destroy_ast(struct ast_node * n);
void gs_schedule(struct shader_info * info);


int r600_shader_optimize(struct r600_context * rctx, struct r600_pipe_shader *pipeshader, int dump);



#endif /* OPT_CORE_H_ */
