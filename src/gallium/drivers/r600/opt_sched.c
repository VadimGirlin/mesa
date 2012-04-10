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

#include "opt_core.h"

#define MAX_ALU_COUNT 128

static void clear_interferences(struct var_desc * v)
{
	int q;

	for (q=0;q<v->interferences->count; q++) {
		struct var_desc * i = v->interferences->keys[q];

		vset_remove(i->interferences, v);
	}

	vset_clear(v->interferences);
}

static void update_interferences(struct vset * live)
{
	int q;

	for (q=0;q<live->count; q++) {
		struct var_desc * v = live->keys[q];

		if (!(v->flags & VF_DEAD))
			vset_addset(v->interferences, live);
	}
}



static void gs_enqueue_blocks(struct shader_info * info, struct vque * blocks, struct ast_node * node)
{
	if (node->type == NT_OP || node->type == NT_REGION || node->type == NT_IF || node->type == NT_REPEAT || node->type==NT_DEPART
			|| node->subtype == NST_ALU_GROUP) {

		node->parent->child = NULL;

		if (!(node->flags & AF_DEAD))
			vque_enqueue(blocks, node->max_prio, node);
		else
			destroy_ast(node);
	} else {

		if (node->rest)
			gs_enqueue_blocks(info, blocks, node->rest);

		if (node->child)
			gs_enqueue_blocks(info, blocks, node->child);

	}

}

/* recreate ast list from blocks queue */

/* created alu clauses are just groups of subsequent alu instructions,
 * need to split them later according to instruction count, kcache lines restrictions etc
 */
static struct ast_node * gs_create_list(struct shader_info * info, struct vque * blocks)
{
	int q;
	struct ast_node * list = NULL, * clause = NULL, * lc;
	boolean last_alu = false;
	unsigned last_alu_prio=0, nalu = 0;

	for (q=0; q<blocks->count; q++) {
		struct ast_node * n = blocks->keys[q*2+1];
		boolean alu = n->subtype == NST_ALU_INST || n->subtype == NST_ALU_GROUP || n->subtype == NST_COPY;

		R600_DUMP("gs_create_list : ");
		dump_node(info, n, 0);

		if (alu) {
			if (n->subtype == NST_ALU_INST || n->subtype == NST_COPY) {
				boolean new_group = (n->min_prio!=last_alu_prio) || (nalu==info->max_slots-1);

				if (new_group) {
					n->alu->last = 1;
					nalu=0;
				} else
					n->alu->last = 0;

				 last_alu_prio = n->min_prio;
				nalu++;

			} else
				last_alu_prio = 0;

			if (!last_alu) {

				/* start new alu clause */

				clause = create_node(NT_GROUP);
				clause->subtype = NST_ALU_CLAUSE;

				if (list) {
					list->parent = create_node(NT_LIST);
					list->parent->rest = list;
					list = list->parent;
				} else
					list = create_node(NT_LIST);

				list->child = clause;
				lc = create_node(NT_LIST);
			} else {
				lc->parent = create_node(NT_LIST);
				lc->parent->rest = lc;
				lc = lc->parent;
			}

		} else {
			if (last_alu) {
				clause->child = lc;
				lc->parent = clause;
				clause = NULL;
			}

			if (list) {
				list->parent = create_node(NT_LIST);
				list->parent->rest = list;
				list = list->parent;
			} else
				list = create_node(NT_LIST);

			lc = list;
		}

		lc->child = n;
		n->parent = lc;

		last_alu = alu;
	}

	if (last_alu) {
		clause->child = lc;
		lc->parent = clause;
	}

	return list;
}

static void gs_schedule_node(struct shader_info * info, struct ast_node * node)
{
	struct vque * blocks = vque_create(16);
	struct ast_node * list;
	int q;

	gs_enqueue_blocks(info, blocks, node->child);

	R600_DUMP("\n##QUEUED BLOCKS:\n");

	for (q=blocks->count-1; q>=0; q--) {
		struct ast_node * n = blocks->keys[q*2+1];
		dump_node(info, n, 0);

		if (n->child && (n->type == NT_REGION || n->type == NT_REPEAT || n->type == NT_DEPART || n->type==NT_IF))
			gs_schedule_node(info, n);
	}

	R600_DUMP("##QUEUED BLOCKS END\n\n");

	list = gs_create_list(info, blocks);

	vque_destroy(blocks);

	destroy_ast(node->child);
	node->child = list;
	if (list)
		list->parent = node;
}

/* collect the sets of used vars for every block (region) */
static void gs_collect_vars_usage(struct ast_node * node)
{
	node->vars_used = vset_create(1);

	if (node->child) {
		gs_collect_vars_usage(node->child);
		vset_addset(node->vars_used, node->child->vars_used);
	}

	if (node->rest) {
		gs_collect_vars_usage(node->rest);
		vset_addset(node->vars_used, node->rest->vars_used);
	}

	if (node->p_split)
		vset_addvec(node->vars_used, node->p_split->ins);

	if (node->loop_phi) {
		gs_collect_vars_usage(node->loop_phi);
		vset_addset(node->vars_used, node->loop_phi->vars_used);
	}

	if (node->phi) {
		gs_collect_vars_usage(node->phi);
		vset_addset(node->vars_used, node->phi->vars_used);
	}

	if (node->ins)
		vset_addvec(node->vars_used, node->ins);

	if (node->flow_dep)
		vset_add(node->vars_used, node->flow_dep);
}


static enum node_subtype gs_prio_get_node_subtype(struct ast_node * node)
{
	switch (node->subtype) {
	case NST_ALU_INST:
	case NST_CF_INST:
	case NST_TEX_INST:
	case NST_VTX_INST:
	case NST_PHI:
	case NST_BLOCK:
		return node->subtype;

	case NST_COPY:
	case NST_PCOPY:
	case NST_ALU_GROUP:
		return NST_ALU_INST;

	default:
		return 0;
	}
}

static unsigned gs_calc_min_prio(struct shader_info * info, struct ast_node * node)
{
	unsigned pri = 0, max_child_prio = 0;

	if (node->flags & AF_DEAD)
		return 0;

	if (node->p_split_outs) {
		int mcp = gs_calc_min_prio(info, node->p_split_outs);
		max_child_prio = MAX2(max_child_prio, mcp);
	}

	if (node->phi) {
		int mcp = gs_calc_min_prio(info, node->phi);
		max_child_prio = MAX2(max_child_prio, mcp);
	}

	if (node->rest) {
		int mcp = gs_calc_min_prio(info, node->rest);
		max_child_prio = MAX2(max_child_prio, mcp);
	}

	if (node->child){
		int mcp = gs_calc_min_prio(info, node->child);
		max_child_prio = MAX2(max_child_prio, mcp);
	}


	pri = max_child_prio;

	if (node->outs) {
		int q;
		for (q=0; q<node->outs->count; q++) {
			struct var_desc * v = node->outs->keys[q];
			if (v && !(v->flags & VF_DEAD)) {
				if (v->prio > pri)
					pri = v->prio;
			}
		}
	}

	if (node->type != NT_LIST) {
		if (node->subtype == NST_TEX_INST || node->subtype == NST_VTX_INST) {
			unsigned lvl;

			pri = (pri-pri%ANP_PRIO_BLOCKSTEP) + ANP_PRIO_BLOCKSTEP;
			lvl = pri/ANP_PRIO_BLOCKSTEP;

			do {
				void * d = 0;

				if (VMAP_GET(info->fetch_levels, lvl, &d) && ((unsigned)d)==16) {
					lvl++;
					pri += ANP_PRIO_BLOCKSTEP;
					continue;
				}

				VMAP_SET(info->fetch_levels, lvl, ++d);
				break;

			} while (1);

		} else
			pri += ANP_PRIO_STEP;
	}

	node->min_prio = pri;

	if (node->outs) {
		int q;
		for (q=0; q<node->outs->count; q++) {
			struct var_desc * v = node->outs->keys[q];
			if (v && !(v->flags & VF_DEAD) && v->prio < node->min_prio)
				v->prio = node->min_prio;
		}
	}

	if (node->ins) {
		int q;
		for (q=0; q<node->ins->count; q++) {
			struct var_desc * v = node->ins->keys[q];
			if (v && !(v->flags & VF_DEAD) && (v->prio < pri))
				v->prio = pri;
		}
	}

	if ((node->type == NT_DEPART || node->type == NT_REPEAT) && node->vars_live) {
		int q;
		for (q=0; q<node->vars_live->count; q++) {
			struct var_desc * v = node->vars_live->keys[q];

			if (v && !(v->flags & VF_DEAD) && (v->prio < pri))
				v->prio = pri;
		}
	}



	if (node->loop_phi) {
		gs_calc_min_prio(info, node->loop_phi);
		gs_calc_min_prio(info, node->child);
		gs_calc_min_prio(info, node->loop_phi);
	}

	if (node->p_split)
		gs_calc_min_prio(info, node->p_split);

	if (node->vars_used
			&& (node->type == NT_REGION	|| node->type == NT_IF || node->subtype == NST_ALU_GROUP)) {
		int q;

		node->min_prio = max_child_prio;

		for (q=0; q<node->vars_used->count; q++) {
			struct var_desc * v = node->vars_used->keys[q];
			if (v->prio < node->min_prio)
				v->prio = node->min_prio;
		}
	}

	if (node->flow_dep) {
		struct var_desc * v = node->flow_dep;
		if (v->prio < node->min_prio)
			v->prio = node->min_prio;
	}

	return MAX2(max_child_prio, node->min_prio);
}

static void gs_calc_max_prio(struct shader_info * info, struct ast_node * node)
{
	unsigned pri = ~0;

	if (node->type != NT_LIST) {

		enum node_subtype st = gs_prio_get_node_subtype(node);
		boolean alu = (st == NST_ALU_INST);
		boolean fetch = (st == NST_TEX_INST || st == NST_VTX_INST);
		boolean fetch_dep = false;

		if (node->p_split)
			gs_calc_max_prio(info, node->p_split);

		if (node->flow_dep) {
			struct var_desc * v = node->flow_dep;
			if (v && !(v->flags & VF_DEAD) && (v->prio < pri))
				pri = v->prio;
		}

		if (node->ins) {
			int q;
			for (q=0; q<node->ins->count; q++) {
				struct var_desc * v = node->ins->keys[q];
				if (v && !(v->flags & VF_DEAD)) {
					if (v->prio < pri)
						pri = v->prio;
					fetch_dep |= v->fetch_dep;
				}
			}
		} else if (node->vars_used && (node->type == NT_REGION || node->type == NT_IF
						|| node->subtype == NST_ALU_GROUP)) {
			int q;
			for (q=0; q<node->vars_used->count; q++) {
				struct var_desc * v = node->vars_used->keys[q];
				if (v && !(v->flags & VF_DEAD ) && (v->prio < pri))
					pri = v->prio;
			}
		}

		if (!fetch_dep || !alu) {
			pri = node->min_prio;
			if (fetch)
				fetch_dep = true;
		} else
			pri -= 1;

		node->max_prio = pri;

		if (node->outs) {
			int q;
			for (q=0; q<node->outs->count; q++) {
				struct var_desc * v = node->outs->keys[q];
				if (v && !(v->flags & VF_DEAD )) {
					v->prio = pri;
					v->fetch_dep = fetch_dep;
				}
			}
		}

		if (node->p_split_outs)
			gs_calc_max_prio(info, node->p_split_outs);
	}

	if (node->child)
		gs_calc_max_prio(info, node->child);

	if (node->rest)
		gs_calc_max_prio(info, node->rest);
}

void gs_schedule(struct shader_info * info)
{
	gs_collect_vars_usage(info->root);

	info->fetch_levels = vmap_create(4);

	gs_calc_min_prio(info, info->root);

	vmap_destroy(info->fetch_levels);

	gs_calc_max_prio(info, info->root);

	dump_shader_tree(info);

	gs_schedule_node(info, info->root);
}

/* create ins and outs vectors for the alu instructions group */
static void create_group_iovecs(struct ast_node * g)
{
	struct ast_node *l = g->child;
	int ic = 0, oc = 0;

	assert (g->subtype == NST_ALU_GROUP);

	if (g->ins)
		return;

	if (g->p_split) {
		assert(g->p_split_outs);

		g->ins = vvec_createcopy(g->p_split->ins);
		g->outs = vvec_createcopy(g->p_split_outs->outs);

		return;
	}

	if (g->ins == NULL)
		g->ins = vvec_create(5*3);
	else
		vvec_clear(g->ins);

	if (g->outs == NULL)
		g->outs = vvec_create(5*3);
	else
		vvec_clear(g->outs);

	while (l && l->child) {
		struct ast_node * n = l->child;
		int q;

		for (q=0; q<n->outs->count; q++)
			g->outs->keys[oc++] = n->outs->keys[q];

		for (q=0; q<n->ins->count; q++)
			g->ins->keys[ic++] = n->ins->keys[q];

		l = l->rest;
	}

	assert(ic <= g->ins->count);
	assert(oc <= g->outs->count);

	while(ic<g->ins->count)
		g->ins->keys[ic++] = NULL;

	while(oc<g->outs->count)
		g->outs->keys[oc++] = NULL;
}

/* updating use counts map for variables */

/* FIXME: probably makes sense to store it directly in the var_desc structure
 * instead of separate map
 */
static void update_counts(struct vmap * uc, struct vvec * vars, int delta)
{
	int q;

	if (!vars)
		return;

	for (q=0; q<vars->count; q++) {
		struct var_desc * v = vars->keys[q];
		if (v) {
			void *d;

			if (VMAP_GET(uc, v, &d))
				d = (void*)(((intptr_t)d) + delta);
			else
				d = (void*)(intptr_t)delta;

			assert(((intptr_t)d)>=0);

			VMAP_SET(uc, v, d);
		}
	}
}



struct cbs_state {
	void * cycle_var[3][4];
	int cpair[2];
	struct ast_node * bs_slots[5];
};

struct kcache_bank {
	unsigned addr;
	unsigned mode;
};

struct scheduler_ctx {
	struct shader_info * info;

	struct r600_bytecode_kcache kc_sets[4];

	int alu_slot_count;
	int group_inst_count;

	/* pending instructions list - map of indices to instruction nodes, to keep them ordered */
	struct vmap * instructions;

	/* ready to schedule instructions */
	struct vmap * ready_inst;

	struct vset * live;
	struct vmap * use_count;
	struct vmap * reg_map;
	struct vmap * reg_map_save;

	struct ast_node * clause_node;
	struct ast_node * slots[3][5];
	struct ast_node * out_list;

	struct vset * locals;
	struct vset * globals;

	int idx[5];
	int free_slots;
	boolean restart;

	int curgroup;
	int empty_count;

	int count;

	/* for literals check */
	uint32_t literal[4];
	unsigned nliteral;

	/* for bank swizzle check */
	struct cbs_state cbs;
	/* remember modified bs data to allow rollback */
	int cpair_mdf[2];
	int cgpr_mdf[2];
	int cpair_mdf_count;
	int cgpr_mdf_count;
};

enum cbs_res {
	CBS_RES_CONST = 1,
	CBS_RES_GPR = 2,
	CBS_RES_ALL = 3
};

/* sched_cbs_* functions - bank swizzle check
 *
 * Typical usage for bank swizzle check is to check if we can add one more
 * slot to already checked slots, so we want to avoid
 * recomputing of related data as much as possible
 */

/* reset bank swizzle data */
static void sched_cbs_reset(struct scheduler_ctx * ctx, enum cbs_res res)
{
	if (res & CBS_RES_GPR)
		memset(&ctx->cbs.cycle_var, 0, 4 * 3 * sizeof(void*));
	if (res & CBS_RES_CONST)
		memset(&ctx->cbs.cpair, 0, 2 * sizeof(int));
}


/* rollback partial reservations for failed bs check */
static void sched_cbs_rollback(struct scheduler_ctx * ctx, enum cbs_res res)
{
	int q;
	if (ctx->cpair_mdf_count && (res & CBS_RES_CONST)) {
		for (q=0; q<ctx->cpair_mdf_count; q++) {
			int const_pairport = ctx->cpair_mdf[q];
			ctx->cbs.cpair[const_pairport] = 0;
		}
		ctx->cpair_mdf_count = 0;
	}
	if (ctx->cgpr_mdf_count && (res & CBS_RES_GPR)) {
		for (q=0; q<ctx->cgpr_mdf_count; q++) {
			int gpr_readport = ctx->cgpr_mdf[q];
			ctx->cbs.cycle_var[gpr_readport>>2][gpr_readport&3] = NULL;
		}
		ctx->cgpr_mdf_count = 0;
	}
}

/* reserve const pair read port */
/* cpair = (const index << 1) + (const_elem >> 1) + 1 */
static boolean sched_cbs_reserve_cpair(struct scheduler_ctx * ctx, int cpair)
{
	int q;
	for (q=0;q<2;q++) {
		if (ctx->cbs.cpair[q]==cpair)
			return true;
		else if (ctx->cbs.cpair[q]==0) {
			ctx->cbs.cpair[q] = cpair;
			ctx->cpair_mdf[ctx->cpair_mdf_count++] = q;
			return true;
		}
	}
	return false;
}

/* reserve gpr read port */
static boolean sched_cbs_reserve_gpr(struct scheduler_ctx * ctx, int cycle, int chan, void * vv)
{
	if (ctx->cbs.cycle_var[cycle][chan]==0) {
		ctx->cbs.cycle_var[cycle][chan] = vv;
		ctx->cgpr_mdf[ctx->cgpr_mdf_count++] = (cycle<<2)+chan;
	} else if (ctx->cbs.cycle_var[cycle][chan]!=vv)
		return false;
	return true;
}

/* bank swizzle element for vector */
static int sched_cbs_vec(int bs, int i)
{
	assert(bs<6);
	switch (i) {
	case 0: return bs>>1;
	case 1:	return bs>=3 ? (bs-3)>>1 : (bs+3)>>1;
	case 2:	return bs>=3 ? 5-bs : 2-bs;
	}
	assert(0);
	return -1;
}

/* bank swizzle element for scalar */
static int sched_cbs_scl(int bs, int i)
{
	assert(bs<4);
	return bs ? (bs-1 == i ? 1 : 2) : 2-i;
}

static boolean sched_cbs_try_slot(struct scheduler_ctx * ctx, struct ast_node * n, int bs, boolean scalar, enum cbs_res res)
{
	int q;

	assert(n);

	if (res & CBS_RES_CONST) {

		/* reset const rollback data */
		ctx->cpair_mdf_count = 0;

		if (n->const_ins_count) {
			if (scalar && n->const_ins_count>2)
				return false;

			for (q=0; q<n->ins->count; q++) {
				struct var_desc * v = n->ins->keys[q];

				if (!v) { /* const */
					int sel = n->alu->src[q].sel;

					if (sel>=BC_KCACHE_OFFSET) {/* kcache constant */
						int chan = n->alu->src[q].chan;
						int cpair = (n->alu->src[q].kc_bank<<20) + ((sel-BC_KCACHE_OFFSET)<<1) + (chan>>1) + 1;
						if (!sched_cbs_reserve_cpair(ctx, cpair)) {
							sched_cbs_rollback(ctx, CBS_RES_CONST);
							return false;
						}
					}
				}
			}
		}
	}

	if (res & CBS_RES_GPR) {
		void * vv0 = NULL, * vv;

		assert(!n->alu->bank_swizzle_force || n->alu->bank_swizzle == bs);
		assert(bs < (scalar ? 4:6));

		/* reset gpr rollback data */
		ctx->cgpr_mdf_count = 0;

		for (q=0; q<n->ins->count; q++) {
			struct var_desc * v = n->ins->keys[q];

			if (v) { /* gpr */
				vv = (v->chunk ? (void*)v->chunk : (void*)v);
				int swz;

				if (scalar) {
					swz = sched_cbs_scl(bs, q);
					if (swz < n->const_ins_count) {
						sched_cbs_rollback(ctx, res);
						return false;
					}
				} else {
					swz = sched_cbs_vec(bs, q);
					if (q==0)
						vv0 = vv;
					else if (q==1 && vv==vv0)
						continue;
				}

				int chan = KEY_CHAN(v->color);

				if (!sched_cbs_reserve_gpr(ctx, swz, chan, vv)) {
					sched_cbs_rollback(ctx, res);
					return false;
				}
			}
		}
	}
	return true;
}

/* init bank swizzle data for existing slots */
static boolean sched_cbs_init(struct scheduler_ctx * ctx)
{
	int q;

	for (q=0; q<ctx->info->max_slots; q++) {
		struct ast_node * n = ctx->slots[ctx->curgroup][q];
		if (n && !sched_cbs_try_slot(ctx, n, n->alu->bank_swizzle, q==4, CBS_RES_ALL)) {
			assert(0);
			return false;
		}
	}
	return true;
}

static boolean sched_cbs_add_slot(struct scheduler_ctx * ctx, int curgroup, int slot)
{
	struct ast_node * n = ctx->slots[curgroup][slot];
	boolean scalar = slot == 4;
	int swz_cnt = scalar ? 4 : 6, bs;
	int bss[5],q, max_slots = ctx->info->max_slots;
	boolean result = false, finished = false, backtrack = false;
	int cs=0, cs_first_modifiable=-1, cs_last_modifiable = -1;

	assert(n);

	/* check/reserve const first */
	if (!sched_cbs_try_slot(ctx, n, 0, scalar, CBS_RES_CONST))
		return false;

	ctx->cbs.bs_slots[slot] = n;

	/* fast path - try to find bs for the new slot only */
	if (n->alu->bank_swizzle_force) {
		if (sched_cbs_try_slot(ctx, n, n->alu->bank_swizzle, scalar, CBS_RES_GPR))
			result = true;
	} else {
		for (bs=0; bs<swz_cnt; bs++) {
			if (sched_cbs_try_slot(ctx, n, bs, scalar, CBS_RES_GPR)) {
				n->alu->bank_swizzle = bs;
				result = true;
				break;
			}
		}
	}

	if (result)
		return true;

	/* prepare to check all combinations */

	for (q=max_slots-1; q>=0; q--) {
		bss[q]=0;
		if (ctx->cbs.bs_slots[q]) {

			if (ctx->cbs.bs_slots[q]->alu->bank_swizzle_force)
				bss[q]=ctx->cbs.bs_slots[q]->alu->bank_swizzle;
			else {
				if (cs_last_modifiable==-1)
					cs_last_modifiable = q;

				cs_first_modifiable = q;
			}
		}
	}

	/* reinit data for slots wirh forced swizzle */
	sched_cbs_reset(ctx, CBS_RES_GPR);
	for (q=0; q<max_slots; q++)
		if (ctx->cbs.bs_slots[q] && ctx->cbs.bs_slots[q]->alu->bank_swizzle_force &&
				!sched_cbs_try_slot(ctx, ctx->cbs.bs_slots[q], bss[q], q==4, CBS_RES_GPR))
			assert(0);

	/* if all slots have forced bs - ok, nothing to do */
	if (cs_first_modifiable==-1) {
		return true;
	}

	cs = cs_first_modifiable;

	while(!finished) {

		if (backtrack) {
			/* reinit previous slots */
			sched_cbs_reset(ctx, CBS_RES_GPR);
			for (q=0; q<cs; q++)
				if (ctx->cbs.bs_slots[q] && !sched_cbs_try_slot(ctx, ctx->cbs.bs_slots[q], bss[q], q==4, CBS_RES_GPR))
					assert(0);
		}

		/* loop for current slot swizzles */
		while (1) {

			/* try current slot */
			if (backtrack || !sched_cbs_try_slot(ctx, ctx->cbs.bs_slots[cs], bss[cs], cs==4, CBS_RES_GPR)) {
				/* try next bank swizzle */
				bss[cs]++;

				backtrack=false;

				if (bss[cs] >= (cs==4 ? 4:6)) {
					if (cs == cs_first_modifiable) {
						result = false;
						finished=true;
						break;
					}

					bss[cs] = 0;

					/* to prev slot */
					backtrack=true;
					cs--;
					while (cs>cs_first_modifiable && !(ctx->cbs.bs_slots[cs] && !ctx->cbs.bs_slots[cs]->alu->bank_swizzle_force))
						cs--;
					break;
				}
			} else { /* swizzle is correct, switch to the next slot */
				cs++;
				while (cs<cs_last_modifiable && !(ctx->cbs.bs_slots[cs] && !ctx->cbs.bs_slots[cs]->alu->bank_swizzle_force))
					cs++;
				if (cs>cs_last_modifiable) { /* finished successfully */
					result = true;
					finished=true;
					break;
				}
			}
		}
	}

	if (result) {
		/* set computed bank swizzles */
		for (q=cs_first_modifiable; q<=cs_last_modifiable; q++) {
			struct ast_node * n = ctx->cbs.bs_slots[q];
			if (n)
				n->alu->bank_swizzle = bss[q];
		}
		return true;
	} else {
		ctx->cbs.bs_slots[slot] = NULL;
		return false;
	}
}

static void sched_check_chunks_types(struct vset * vars, struct vset * globals)
{
	int q;
	struct vset * processed_chunks = vset_create(16);

	for (q=0; q<vars->count; q++) {
		struct var_desc * v = vars->keys[q];
		if (v && !(v->flags & VF_DEAD) && v->chunk!=NULL &&
				v->chunk->flags==0 && !vset_contains(processed_chunks, v->chunk)) {

			struct affinity_chunk * c = v->chunk;

			if (vset_containsset(vars, c->vars) && !vset_intersects(globals, c->vars)) {
				c->flags |= ACF_LOCAL;
				R600_DUMP("local chunk : ");
				dump_chunk(c);
			}

			vset_add(processed_chunks, c);
		}
	}

	vset_destroy(processed_chunks);
}

static void sched_map_live_outs(struct scheduler_ctx * ctx)
{
	int i;

	/* allocating live-out vars */
	for (i=0; i<ctx->clause_node->vars_live_after->count; i++) {
		struct var_desc * v = ctx->clause_node->vars_live_after->keys[i];

		if (v->flags & (VF_SPECIAL | VF_UNDEFINED))
			continue;

		assert(v->color);

		if (v->chunk) {
			VMAP_SET(ctx->reg_map, v->color, v);

			R600_DUMP("mapping outs: ");
			print_var(v);
			R600_DUMP(" @ ");
			print_reg(v->color);
			R600_DUMP("\n");
		}
	}
}

static void sched_select_live_instructions(struct scheduler_ctx * ctx)
{
	struct ast_node *clause_node = ctx->clause_node, * c = clause_node->child;
	int i=0;

	assert(c->type == NT_LIST);

	R600_DUMP( "### creating instructions list\n");

	if (clause_node->vars_defined)
		vset_clear(clause_node->vars_defined);
	else
		clause_node->vars_defined = vset_create(16);

	/* select non-dead instructions for scheduling */
	while (c) {
		if (c->child) {
			if (c->child->flags & AF_DEAD) {
				c = c->rest;
				continue;
			}

			if (c->child->type == NT_GROUP) {
				create_group_iovecs(c->child);

				if (c->child->p_split)
					vset_addvec(clause_node->vars_defined, c->child->p_split->outs);
				if (c->child->p_split_outs) {
					vset_addvec(clause_node->vars_defined, c->child->p_split_outs->outs);
					vset_addvec(clause_node->vars_defined, c->child->p_split_outs->ins);
				}
			}

			/* accumulate use count for every var */
			update_counts(ctx->use_count, c->child->ins, +1);
			/* add instruction to the list */
			VMAP_SET(ctx->instructions, ++i, c->child);

			/* collect all defined vars */
			vset_addvec(clause_node->vars_defined, c->child->outs);
			dump_node(ctx->info, c->child, 0);
		}
		c = c->rest;
	}

	R600_DUMP("defined vars: ");
	dump_vset(clause_node->vars_defined);
	R600_DUMP("\n");
}

static void sched_init_local_var(struct var_desc * v)
{
	R600_DUMP("local var : ");
	print_var(v);
	R600_DUMP("\n");

	/* clear interferences - we'll recompute them during scheduling */
	if (v->interferences)
		clear_interferences(v);

	/* if var is a member of a chunk - it's local chunk, unfix all vars
	 * to enable recoloring of a chunk */
	if (v->chunk) {
		int p;
		for (p=0; p<v->chunk->vars->count; p++) {
			struct var_desc * v2 = v->chunk->vars->keys[p];
			v2->fixed = false;
		}
	}
}


static void sched_select_ready_instructions(struct scheduler_ctx * ctx)
{
	int i, q;
	struct ast_node * n;
	boolean skip;

	for (i = ctx->instructions->count-1; i>=0; i--) {
		int index = (intptr_t)ctx->instructions->keys[i*2];

		/* FIXME: instruction count isn't limited here anymore,
		 * so need to handle huge instruction lists better.
		 * here is the temporary workaround (otherwise piglit
		 * fp-long-alu is too slow) */
		if (ctx->instructions->count-i > 128 && ctx->ready_inst->count)
			break;

		n = ctx->instructions->keys[i*2+1];
		skip = false;

		if (!skip) {
			for (q=0; q<n->outs->count; q++) {
				struct var_desc * v = n->outs->keys[q];
				if (v) {
					void *d;

					/* use_count contains the number of remaining uses -
					 * we need to schedule all uses before definition of the var */

					if ((VMAP_GET(ctx->use_count, v, &d) && (d != NULL))) {
						skip = true;
						break;
					}
				}
			}
		}

		if (skip)
			continue;

		/* if this is a copy, check if the source and target are allocated to the same location,
		 * if so (that is, they were coalesced) - just skip the instruction */
		if (n->subtype == NST_COPY) {
			struct var_desc * i = n->ins->keys[0];
			struct var_desc * o = n->outs->keys[0];

			/* if output is not live, just skip for now */
			if (!vset_contains(ctx->live, o))
				continue;

			if (i->color == o->color) {

				if (!vset_removevec(ctx->live, n->outs))
					continue;

				struct var_desc * v;
				update_counts(ctx->use_count, n->ins, -1);
				vset_addvec(ctx->live, n->ins);

				R600_DUMP("copy coalesced @ ");
				print_reg(i->color);
				R600_DUMP(" : ");
				print_var(o);
				R600_DUMP(" <= ");
				print_var(i);
				R600_DUMP("\n");

				if (VMAP_GET(ctx->reg_map,o->color, &v)) {
					if (v==o || (v->chunk==o->chunk && v->chunk))
						VMAP_SET(ctx->reg_map, i->color, i);
				}

				R600_DUMP("skipping coalesced copy: ");
				dump_node(ctx->info, n, 0);
				VMAP_REMOVE(ctx->instructions, index);
				ctx->count--;

				continue;
			}
		}

		/* move to the ready to schedule list */
		VMAP_SET(ctx->ready_inst, index, n);
		VMAP_REMOVE(ctx->instructions, index);

	}
}

/* add the group to the list */
static void sched_add_group(struct scheduler_ctx * ctx)
{
	int last=1, j, max_slots = ctx->info->max_slots;
	boolean contains_group = false;
	struct ast_node * c, * n, * out_group;

	/* we may have 4-slot grouped instruction and a single instruction in
	 * the current group, handle them separately */

	/* process single instructions */
	for (j=max_slots-1; j>=0; j--) {
		c = ctx->slots[ctx->curgroup][j];
		if (c) {

			/* avoid recomputing bank swizzle when building bytecode */
			c->alu->bank_swizzle_force = 1;

			if (!(c->flags & AF_FOUR_SLOTS_INST)) {

				c->alu->last = last;
				last = 0;

				if (ctx->out_list) {
					ctx->out_list->parent = create_node(NT_LIST);
					ctx->out_list->parent->rest = ctx->out_list;
					ctx->out_list = ctx->out_list->parent;
				} else
					ctx->out_list = create_node(NT_LIST);

				ctx->out_list->child = c;
				c->parent = ctx->out_list;

				ctx->slots[ctx->curgroup][j] = NULL;
			} else
				contains_group = true;
		}
	}

	/* process 4-slot group */
	if (contains_group) {
		if (ctx->out_list) {
			ctx->out_list->parent = create_node(NT_LIST);
			ctx->out_list->parent->rest = ctx->out_list;
			ctx->out_list = ctx->out_list->parent;
		} else
			ctx->out_list = create_node(NT_LIST);

		out_group = ctx->slots[ctx->curgroup][0]->parent->parent;

		if (out_group->parent) {
			out_group->parent->child = NULL;
			out_group->parent = NULL;
		}

		ctx->out_list->child = out_group;
		out_group->parent = ctx->out_list;

		c = out_group->child;

		n=NULL;

		while (c) {
			if (c->child)
				c->child->alu->last = 0;

			n = c;
			c = c->rest;
		}

		assert(n);
		c = n->child;

		c->alu->last = last ? 1 : 0;

	}
}

/* check/alloc kcache sets for current alu group */
static boolean sched_alloc_kcache(struct scheduler_ctx * ctx)
{
	int q,w, max_slots = ctx->info->max_slots;

	struct r600_bytecode_kcache kcache[4];

	memcpy(kcache, ctx->kc_sets, 4 * sizeof(struct r600_bytecode_kcache));

	for (q=0; q<max_slots; q++) {
		struct ast_node * c = ctx->slots[ctx->curgroup][q];
		if (c && c->const_ins_count) {
			for (w=0; w<c->ins->count; w++) {
				int sel = c->alu->src[w].sel;

				if (sel>=BC_KCACHE_OFFSET) {
					if (r600_bytecode_alloc_kcache_line(&ctx->info->shader->bc, kcache, c->alu->src[w].kc_bank, (sel-BC_KCACHE_OFFSET)>>4))
						return false;
				}
			}
		}
	}

	memcpy(ctx->kc_sets, kcache, 4 * sizeof(struct r600_bytecode_kcache));

	return true;
}

/* set split flag for current group - this means it's the last
 * group of the clause ( or the first, if thinking in bottom-up order)
 */
static void sched_set_split(struct scheduler_ctx * ctx)
{
	int q, max_slots = ctx->info->max_slots;

	for (q=max_slots-1; q>=0; q--) {
		struct ast_node * c = ctx->slots[ctx->curgroup][q];
		if (c) {
			c->flags |= AF_ALU_CLAUSE_SPLIT;
			return;
		}
	}
	assert(0);
}

/* check instruction count, kcache, etc
 * set AF_ALU_CLAUSE_SPLIT to start new clause when needed */
static void sched_check_clause_limits(struct scheduler_ctx * ctx)
{
	boolean split = false;
	int literal_slot_count = ctx->nliteral ? ((ctx->nliteral+1)>>1) : 0;

	if (!sched_alloc_kcache(ctx))
		split = true;
	else {
		ctx->alu_slot_count += ctx->group_inst_count + literal_slot_count;
		if (ctx->alu_slot_count > MAX_ALU_COUNT)
			split = true;
	}

	if (split) {
		sched_set_split(ctx);
		memset(ctx->kc_sets, 0,4 * sizeof(struct r600_bytecode_kcache));
		ctx->alu_slot_count = ctx->group_inst_count + literal_slot_count;
		sched_alloc_kcache(ctx);
	}
}


static void sched_process_selected_group(struct scheduler_ctx * ctx)
{
	int j, max_slots = ctx->info->max_slots;
	struct ast_node * c;

	R600_DUMP( "##### group selected:\n");

	/* process 4-slot instruction (if any) */
	if (ctx->slots[ctx->curgroup][0] && (ctx->slots[ctx->curgroup][0]->flags & AF_FOUR_SLOTS_INST)) {
		struct ast_node * g = ctx->slots[ctx->curgroup][0]->parent->parent;
		assert(g->subtype == NST_ALU_GROUP);

		/* decrement use count for all ins */
		update_counts(ctx->use_count, g->ins, -1);

		/* outs are dead now */
		vset_removevec(ctx->live, g->outs);
		/* ins are live now */
		vset_addvec(ctx->live, g->ins);
	}

	ctx->group_inst_count = 0;

	/* process 1-slot instructions */
	for (j=0; j<max_slots; j++) {
		c = ctx->slots[ctx->curgroup][j];
		if (c)	{

			ctx->group_inst_count++;

			if (c->alu->dst.write==0)
				c->alu->dst.chan = j&3;

			R600_DUMP( "slot %d: ", j);
			dump_node(ctx->info,c, 0);

			if (!(c->flags & AF_FOUR_SLOTS_INST)) {
				update_counts(ctx->use_count, c->ins, -1);

				vset_removevec(ctx->live, c->outs);
				vset_addvec(ctx->live, c->ins);

				c->parent->child = NULL;
				c->parent = NULL;
			}
		}
	}
}

/* check interferences
 * if some var is used as input in the new instruction and its
 * allocation is fixed, but reg/component is currently in use - we have
 * to discard the instruction and return it to the list */
static boolean sched_check_interferences(struct scheduler_ctx * ctx)
{
	int inst_cnt = 0, j;
	boolean intf4 = false;
	struct ast_node * grp4 = NULL, *c;

	ctx->restart = false;

	dump_reg_map(ctx->reg_map);

	vmap_copy(ctx->reg_map_save, ctx->reg_map);

	/* after definition the variable is no more live, and we can allocate
	 * something else to this location*/
	for (j=0; j<ctx->info->max_slots; j++) {
		if (ctx->slots[ctx->curgroup][j]) {
			struct ast_node * c = ctx->slots[ctx->curgroup][j];
			struct var_desc * v = c->outs->keys[0], *v2;

			R600_DUMP("unmapping outs slot %d : ",j);
			dump_node(ctx->info, c, 0);

			if (v) {
				if (!v->chunk || (v->chunk->flags & ACF_LOCAL)) {
					/* if variable location is not fixed -
					 * we need to recolor it, using updated interferences set */
					if (!recolor_local(ctx->info, v))
						assert(0);
				} else if (VMAP_GET(ctx->reg_map, v->color, &v2)) {
					if (v2!=v && v2->chunk != v->chunk) {
						R600_DUMP("already mapped : ");
						print_var(v2);
						R600_DUMP(" @ ");
						print_reg(v2->color);
						R600_DUMP("\n");
						assert(0);
					} else
						VMAP_REMOVE(ctx->reg_map, v->color);
				}
			}
		}
	}

	/* after first use variable becomes live - check if we are able to
	 * allocate it, otherwise discard the instruction */
	for (j=0; j<ctx->info->max_slots; j++) {
		c = ctx->slots[ctx->curgroup][j];
		if (c)	{
			int q;

			inst_cnt++;

			R600_DUMP("mapping ins %d: ", j);
			dump_node(ctx->info,c, 0);

			for (q=0; q<c->ins->count; q++) {
				struct var_desc * v = c->ins->keys[q], *v2;
				boolean intf = false;

				/* check interferences */
				if (!intf && v) {

					/* skip locals - they will be recolored later,
					 * when we will schedule their definitions,
					 * so we'll have no interference */
					if (vset_contains(ctx->locals, v))
						continue;

					R600_DUMP("checking intf ");
					print_var(v);
					R600_DUMP(" @ ");
					print_reg(v->color);

					if (VMAP_GET(ctx->reg_map, v->color, &v2)) {
						R600_DUMP("  from regmap : ");
						print_var(v2);

						if (v2!=v && (((v->flags & VF_PIN_REG) || v->chunk) && v->chunk!=v2->chunk)) {
							R600_DUMP("interference ");
							print_var(v);
							print_var(v2);
							R600_DUMP(" @ ");
							print_reg(v->color);
							R600_DUMP("\n");

							intf = true;
						}
					} else if (v->chunk && !(v->chunk->flags & ACF_LOCAL)) {
						R600_DUMP("mapping global var ");
						print_var(v);
						R600_DUMP("\n");
						VMAP_SET(ctx->reg_map, v->color, v);
					}

					R600_DUMP("\n");
				}

				if (intf) {
					/* if we need to discard one of the instructions of
					 * 4-slot instruction, then we have to discard all 4 */
					if (c->flags & AF_FOUR_SLOTS_INST) {
						intf4 = true;
						j=4;
						grp4 = c->parent;
						while (grp4->subtype != NST_ALU_GROUP)
							grp4 = grp4->parent;
						break;
					}

					vset_addvec(ctx->live,c->outs);

					/* put it back to the instruction list */
					VMAP_SET(ctx->instructions, ctx->idx[j], c);
					VMAP_REMOVE(ctx->ready_inst, ctx->idx[j]);
					ctx->count++;
					ctx->slots[ctx->curgroup][j] = NULL;

					ctx->free_slots |= (1<<j);
					inst_cnt--;
					ctx->restart = true;
					break;
				}
			}
		}
	}

	if (intf4) {
		R600_DUMP("4slots interference : \n");
		dump_node(ctx->info, grp4, 0);

		VMAP_SET(ctx->instructions, ctx->idx[0], grp4);
		VMAP_REMOVE(ctx->ready_inst, ctx->idx[0]);

		ctx->count++;
		ctx->free_slots |= 0xF;

		vset_addvec(ctx->live, grp4->outs);

		for (j=0; j<4; j++) {
			c = ctx->slots[ctx->curgroup][j];
			if (c)	{
				ctx->slots[ctx->curgroup][j] = NULL;
				inst_cnt--;
			} else
				assert(0);
		}

		ctx->restart = true;
	}


	if (inst_cnt == 0) {
		R600_DUMP("empty group - restarting\n");

		++ctx->empty_count;

		assert(ctx->empty_count<5);

		/* fail to optimize the shader and fallback to the original bytecode */
		if (ctx->empty_count>=5)
			return false;

		ctx->restart = true;
	}

	if (ctx->restart)
		vmap_copy(ctx->reg_map, ctx->reg_map_save);

	return true;
}


/* schedule alu clause */
/* probably could be described as a bottom-up greedy cycle scheduler */
static boolean post_schedule_alu(struct shader_info *info, struct ast_node * clause_node)
{
	struct scheduler_ctx ctx;
	int i = 0, j, max_slots = info->max_slots;
	boolean contains_kill = false;
	struct ast_node * c;
	boolean result = true;

	if (!clause_node->child)
		return true;

	memset(&ctx, 0, sizeof(ctx));

	ctx.info = info;
	ctx.locals = vset_create(16);
	ctx.globals = vset_createcopy(clause_node->vars_live);
	ctx.instructions = vmap_create(64);
	ctx.ready_inst = vmap_create(64);
	ctx.use_count = vmap_create(64);
	ctx.reg_map = vmap_create(16);
	ctx.reg_map_save = vmap_create(16);
	ctx.live = vset_createcopy(clause_node->vars_live_after);
	ctx.clause_node = clause_node;

	memset(ctx.slots, 0, 2*5*sizeof(void*));

	vset_addset(ctx.globals, clause_node->vars_live_after);
	sched_map_live_outs(&ctx);
	sched_select_live_instructions(&ctx);
	sched_check_chunks_types(clause_node->vars_defined, ctx.globals);

	ctx.count = ctx.instructions->count;

	/* determining local vars - we may recolor them as needed while scheduling */

	for (i=0; i<clause_node->vars_defined->count; i++) {
		struct var_desc *v = clause_node->vars_defined->keys[i];

		if (!((v->chunk && !(v->chunk->flags & ACF_LOCAL)) || vset_contains(ctx.globals, v)) && vset_add(ctx.locals, v))
			sched_init_local_var(v);
	}

	R600_DUMP( "### %d instructions selected\n", ctx.count);

	/* while there are pending instructions - select and process next alu group */
	while (ctx.count) {
		struct ast_node * n;

		sched_cbs_reset(&ctx, CBS_RES_ALL);
		ctx.nliteral=0;

		if (ctx.restart) {
			int q;
			if (!sched_cbs_init(&ctx)) {
				assert(0);
				result=false;
				break;
			}

			for (q=0; q<max_slots; q++) {
				if (ctx.slots[ctx.curgroup][q]) {
					int r = r600_bytecode_alu_nliterals(&info->shader->bc, ctx.slots[ctx.curgroup][q]->alu, ctx.literal, &ctx.nliteral);
					assert(!r);
				} else
					/* update discarded slots for bs check */
					ctx.cbs.bs_slots[q] = NULL;
			}
		} else {
			memset(ctx.cbs.bs_slots, 0, 5*sizeof(void*));

			contains_kill = false;
			ctx.free_slots = (1<<max_slots)-1;
			memset(ctx.slots[ctx.curgroup], 0, 5*sizeof(void*));
			memset(ctx.idx, 0, 5*sizeof(int));

			sched_select_ready_instructions(&ctx);

			if (ctx.ready_inst->count == 0) {
				if (ctx.count) {
					assert(0);
					/* something went wrong, fallback to original shader */
					result = false;
					break;
				} else
					continue;
			}
		}

		/* TODO: prefer instructions that don't break kcache limit, to avoid
		 * splitting clauses too often */

		/* select instruction to add to the current group */
		/* 4-slot instruction is considered as a single instruction (instruction group) */
		/* selecting in bottom-up order */
		for (i=ctx.ready_inst->count-1; i>=0; i--) {
			int index = (intptr_t)ctx.ready_inst->keys[i*2];
			boolean kill;

			n = ctx.ready_inst->keys[i*2+1];

			memset(ctx.slots[2], 0, 5*sizeof(void*));

			kill = n->alu && is_alu_kill_inst(&info->shader->bc, n->alu);

			/* don't mix kill instructions with other instructions in the same group */
			/* FIXME: probably it's not required */
			if (kill) {
				if (!contains_kill && ctx.free_slots != (1<<max_slots)-1)
					continue;
			} else if (contains_kill)
				continue;


			/* handle 4-slot instructions */
			if (n->type == NT_GROUP) {
				c = n->child;

				if (~ctx.free_slots & 0x0F)
					continue;

				j = 0;
				dump_node(info, n, 0);

				while (c && c->child) {
					ctx.slots[2][j++] = c->child;
					c = c->rest;
				}

				ctx.idx[0] = index;
				assert (j==4);

			} else { /* single instruction */
				struct var_desc * out = n->outs->keys[0];
				int slot = -1, chan=-1;
				unsigned rc;

				/* if the instruction doesn't define live out var - we are free to choose any slot */
				if (!out || (n->flags & AF_KEEP_ALIVE) || (out->flags & VF_DEAD)) {
					rc=0;
					chan=-1;
				} else {
					rc = out->color;
					assert(rc);
					chan = KEY_CHAN(rc);
				}

				slot = -1;

				/* try vector slot first */
				if ((slot==-1) && (n->alu_allowed_slots_type & AT_VECTOR)) {
					if (chan>=0) {
						if (ctx.free_slots & (1<<chan))
							slot = chan;
					} else { /* chan is not fixed - choose any free vector slot */
						slot=0;
						while (slot<4 && !(ctx.free_slots & (1<<slot)))
							slot++;

						if (slot>=4)
							slot=-1;
					}

				}

				/* try assign trans slot */
				if ((slot==-1) && (n->alu_allowed_slots_type & AT_TRANS) && (ctx.free_slots & 0x10))
					slot = 4;

				/* unable to find slot, skip instruction */
				if (slot==-1) {
					R600_DUMP("skipping : no slots : ");
					dump_node(info, n, 0);

					continue;
				}

				if (slot<4)
					chan = slot;

				if (chan!=-1)
					n->alu->dst.chan = chan;

				R600_DUMP("adding %d to slot %d : ", index, slot);
				dump_node(info, n, 0);

				ctx.slots[2][slot] = n;

				if (kill)
					contains_kill = true;

				/* remember index - we'll want to put instruction back to the same position in the
				 * list of pending instructions if we'll discard it from current group */
				ctx.idx[slot] = index;
			}

			/* check cycle restrictions and literals count */
			int r = 0;
			unsigned nliteral = ctx.nliteral;

			for (j=0; j<max_slots; j++) {
				if (ctx.slots[2][j]) {
					r = r600_bytecode_alu_nliterals(&info->shader->bc, ctx.slots[2][j]->alu, ctx.literal, &ctx.nliteral);
					if (r)
						break;
				}
			}

			if (r) {
					R600_DUMP( "literals check failed\n");
			} else {
				int q;

				struct cbs_state st = ctx.cbs;

				for(q=0; q<max_slots; q++) {
					if (ctx.slots[2][q] && !sched_cbs_add_slot(&ctx, 2, q)) {
						r=1;
						break;
					}
				}

				if (r) {
					ctx.cbs = st;
					R600_DUMP( "bank swizzle failed\n");
				}
			}

			if (r) {
				ctx.nliteral = nliteral;
				continue;
			}

			/* all checks ok, assign */
			for (j=0; j<max_slots; j++) {
				if (ctx.slots[2][j]) {
					ctx.slots[ctx.curgroup][j] = ctx.slots[2][j];
					ctx.slots[2][j] = NULL;
					ctx.free_slots &= ~(1<<j);
				}
			}

			/* remove selected instruction from the list */

			VMAP_REMOVE(ctx.ready_inst, index);
			ctx.count--;

			/* if all slots assigned, stop selecting instructions */
			if (ctx.free_slots == 0)
				break;
		}

		if (!sched_check_interferences(&ctx))
			return false;

		if (ctx.restart)
			continue;

		ctx.empty_count=0;

		if (ctx.slots[ctx.curgroup][4]) {
			/* check if we discarded inst from vector slot, and have
			 * inst in the trans slot which we can (should) move to vector slot */
			int slot = ctx.slots[ctx.curgroup][4]->alu->dst.chan;

			if (ctx.slots[ctx.curgroup][slot]==NULL && (ctx.slots[ctx.curgroup][4]->alu_allowed_slots_type & AT_VECTOR)) {
				ctx.slots[ctx.curgroup][slot] = ctx.slots[ctx.curgroup][4];
				ctx.slots[ctx.curgroup][4] = NULL;

				R600_DUMP("moving inst from trans to %d ",slot);
				dump_node(info, ctx.slots[ctx.curgroup][slot], 0);
			}
		}

		sched_process_selected_group(&ctx);
		sched_check_clause_limits(&ctx);

		R600_DUMP( " updating interferences :");
		dump_vset(ctx.live);
		R600_DUMP("\n");

		update_interferences(ctx.live);
		sched_add_group(&ctx);

		ctx.curgroup = !ctx.curgroup;

	}

	vmap_destroy(ctx.instructions);
	vmap_destroy(ctx.ready_inst);
	vmap_destroy(ctx.use_count);
	vmap_destroy(ctx.reg_map);
	vset_destroy(ctx.live);
	vmap_destroy(ctx.reg_map_save);

	vset_destroy(ctx.globals);
	vset_destroy(ctx.locals);

	destroy_ast(clause_node->child);

	clause_node->child = ctx.out_list;
	return result;
}

static boolean post_schedule_node(struct shader_info *info, struct ast_node * node)
{
	if (node->subtype == NST_ALU_CLAUSE ) {

		if (!post_schedule_alu(info, node))
			return false;

	} else {
		if (node->rest)
			if (!post_schedule_node(info, node->rest))
				return false;

		if (node->child)
			if (!post_schedule_node(info, node->child))
				return false;
	}

	return true;
}

boolean post_schedule(struct shader_info *info)
{
	return post_schedule_node(info, info->root);
}
