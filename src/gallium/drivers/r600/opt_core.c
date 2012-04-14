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

#include "opt_core.h"

static void destroy_var(struct var_desc * v)
{
	vset_destroy(v->interferences);
	vset_destroy(v->uses);
	vset_destroy(v->aff_edges);

	free(v);
}

int is_alu_kill_inst(struct r600_bytecode * bc, struct r600_bytecode_alu *alu)
{
	return !alu->is_op3 &&
			(alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLE ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLE_INT ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGE ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGE_INT ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGE_UINT ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGT ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGT_INT ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGT_UINT ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLNE ||
			alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLNE_INT);
}

// instructions with single output replicated in all channels (r600_is_alu_reduction_inst except CUBE)
static int is_alu_replicate_inst(struct r600_bytecode *bc, struct r600_bytecode_alu *alu)
{
	switch (bc->chip_class) {
	case R600:
	case R700:
		return !alu->is_op3 && (
				alu->inst == V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4 ||
				alu->inst == V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4_IEEE ||
				alu->inst == V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MAX4);
	case EVERGREEN:
	case CAYMAN:
	default:
		return !alu->is_op3 && (
				alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4 ||
				alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4_IEEE ||
				alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MAX4);
	}
}

static int is_alu_four_slots_inst(struct r600_bytecode *bc, struct r600_bytecode_alu *alu)
{
	return is_alu_reduction_inst(bc, alu) ||
			(bc->chip_class >= EVERGREEN && !alu->is_op3 &&
					(alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_XY ||
					alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_ZW));
}

static struct var_desc * create_var(struct shader_info * info)
{
	struct var_desc * v = calloc(1,sizeof(struct var_desc));
	v->key = ++info->next_var_key;
	v->reg.reg = -1;
	v->reg.chan = -1;
	return v;
}

static struct var_desc * get_var(struct shader_info * info, int reg, int chan, int index)
{
	uintptr_t key = (index<<16) + (reg<<2) + chan + 1;
	struct var_desc * v;

	assert((reg & ~(REG_SPECIAL|REG_TEMP)) <= VKEY_MAX_REG);
	assert(index <= VKEY_MAX_INDEX);

	if (!VMAP_GET(info->vars, key, &v)) {
		v = create_var(info);
		v->reg.reg = reg;
		v->reg.chan = chan;
		v->index = index;

		if (reg & REG_SPECIAL)
			v->flags |= VF_SPECIAL;

		VMAP_SET(info->vars, key, v);
	}

	return v;
}

static struct var_desc * create_temp_var(struct shader_info * info)
{
	unsigned reg = info->next_temp | REG_TEMP;
	uintptr_t key = (reg<<2);
	struct var_desc * t = create_var(info);

	assert(info->next_temp <= VKEY_MAX_REG);

	t->index = 0;
	t->reg.reg = reg;
	t->reg.chan = -1;

	VMAP_SET(info->vars, key, t);
	info->next_temp++;
	return t;
}

struct ast_node * create_node(enum node_type type)
{
	struct ast_node * node = calloc(1,sizeof(struct ast_node));
	node->type = type;
	return node;
}

static struct ast_node * create_alu_copy(struct shader_info * info, struct var_desc * dst, struct var_desc * src)
{
	struct ast_node * n = create_node(NT_OP);
	n->subtype = NST_COPY;

	n->flags |= AF_COPY_HINT | AF_SPLIT_COPY | AF_ALU_DELETE;

	n->alu_allowed_slots_type = AT_ANY;

	n->ins = vvec_create(1);
	n->outs = vvec_create(1);

	n->outs->keys[0] = dst;
	n->ins->keys[0] = src;

	n->alu = calloc(1, sizeof(struct r600_bytecode_alu));
	n->alu->inst = EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;

	return n;
}

static void set_constraint(struct ast_node * node, boolean in)
{
	int q, count;
	struct rc_constraint * rc;
	struct vset * vars;

	struct vvec * vv = in ? node->ins : node->outs;

	if (!vv || vv->count<=1)
		return;

	count = MIN2(vv->count, 4);

	vars = vset_create(4, 1);

	for (q=0; q<count; q++) {
		struct var_desc * v = vv->keys[q];

		if (v)
			vset_add(vars, v);
	}

	if (vars->count<=1) {
		vset_destroy(vars);
		return;
	}

	rc = calloc(1, sizeof(struct rc_constraint));
	rc->comps = vvec_create_clean(vars->count);
	rc->node = node;
	rc->in = in;

	for (q=0; q<vars->count; q++) {
		struct var_desc * v = vars->keys[q];
		rc->comps->keys[q] = v;
		v->constraint = rc;
	}

	vset_destroy(vars);
}

/* split live intervals for non-alu instructions */
static struct ast_node * build_split(struct shader_info * info, struct vvec * vv, boolean out)
{
	struct ast_node *g = NULL, *l = NULL, *n = NULL;
	int count, q;
	struct vmap * vm;

	/* we could have more than 4 vars with the gradients (see parse_cf_tex),
	 * but we don't need to handle special vars here, so limiting to 4 */
	count = MIN2(vv->count, 4);

	if (count<=1)
		return 0;


	vm = vmap_create(4);

	for(q=0; q<count; q++) {
		struct var_desc * v = vv->keys[q], *t;
		if (!v)
			continue;

		if (VMAP_GET(vm, v, &t)) {
			vv->keys[q] = t;
			continue;
		}

		t = create_temp_var(info);
		t->reg.chan = v->reg.chan;

		VMAP_SET(vm, v, t);
		vv->keys[q] = t;

		if (!l) {
			g = create_node(NT_GROUP);
			g->subtype = NST_ALU_CLAUSE;
			l = create_node(NT_LIST);
			g->child = l;
			l->parent = g;
		} else {
			l->rest = create_node(NT_LIST);
			l->rest->parent = l;
			l = l->rest;
		}

		if (out) {
			n = create_alu_copy(info, v, t);
		} else {
			n = create_alu_copy(info, t, v);
		}

		l->child = n;
		n->parent = l;
	}

	if (n) {
		n->alu->last = 1;
	}

	vmap_destroy(vm);

	return g;
}



static boolean parse_cf_tex(struct shader_info * info, struct ast_node * node, struct r600_bytecode_cf * cf)
{
	struct r600_bytecode_tex * tex;
	struct ast_node * cl, * split;
	boolean first = true;

	LIST_FOR_EACH_ENTRY(tex,&cf->tex,list) {
		struct ast_node * tn = create_node(NT_OP);

		if (tex->dst_rel || tex->src_rel)
			return false;

		tn->subtype = NST_TEX_INST;

		if (first) {
			cl = create_node(NT_LIST);
			node->child=cl;
			cl->parent=node;
			first = false;
		} else {
			cl->rest = create_node(NT_LIST);
			cl->rest->parent = cl;
			cl = cl->rest;
		}

		tn->tex = tex;

		int out_gh = 0, out_gv = 0, in_g = false;

		int i = 4, o=4;

		switch (tex->inst) {
		case SQ_TEX_INST_SET_GRADIENTS_H:
			out_gh = o++;
			break;
		case SQ_TEX_INST_SET_GRADIENTS_V:
			out_gv = o++;
			break;
		case SQ_TEX_INST_SAMPLE_G:
		case SQ_TEX_INST_SAMPLE_G_L:
		case SQ_TEX_INST_SAMPLE_G_LB:
		case SQ_TEX_INST_SAMPLE_G_LZ:
		case SQ_TEX_INST_SAMPLE_C_G:
		case SQ_TEX_INST_SAMPLE_C_G_L:
		case SQ_TEX_INST_SAMPLE_C_G_LB:
		case SQ_TEX_INST_SAMPLE_C_G_LZ:
			in_g = i;
			i+=2;
			break;
		}


		tn->ins = vvec_create(i);
		tn->outs = vvec_create(o);

		tn->ins->keys[0] = tex->src_sel_x < 4 ? get_var(info, tex->src_gpr, tex->src_sel_x, 0) : NULL;
		tn->ins->keys[1] = tex->src_sel_y < 4 ? get_var(info, tex->src_gpr, tex->src_sel_y, 0) : NULL;
		tn->ins->keys[2] = tex->src_sel_z < 4 ? get_var(info, tex->src_gpr, tex->src_sel_z, 0) : NULL;
		tn->ins->keys[3] = tex->src_sel_w < 4 ? get_var(info, tex->src_gpr, tex->src_sel_w, 0) : NULL;
		tn->outs->keys[0] = tex->dst_sel_x < 4 ? get_var(info, tex->dst_gpr, tex->dst_sel_x, 0) : NULL;
		tn->outs->keys[1] = tex->dst_sel_y < 4 ? get_var(info, tex->dst_gpr, tex->dst_sel_y, 0) : NULL;
		tn->outs->keys[2] = tex->dst_sel_z < 4 ? get_var(info, tex->dst_gpr, tex->dst_sel_z, 0) : NULL;
		tn->outs->keys[3] = tex->dst_sel_w < 4 ? get_var(info, tex->dst_gpr, tex->dst_sel_w, 0) : NULL;

		if (in_g) {
			tn->ins->keys[in_g+0] = get_var(info, REG_GR, 0, 0);
			tn->ins->keys[in_g+1] = get_var(info, REG_GR, 1, 0);
		}

		if (out_gv)
			tn->outs->keys[out_gv] = get_var(info, REG_GR, 0, 0);

		if (out_gh)
			tn->outs->keys[out_gh] = get_var(info, REG_GR, 1, 0);

		tn->flags |= AF_REG_CONSTRAINT;

		if (tex->inst == SQ_TEX_INST_SET_GRADIENTS_H ||
				tex->inst == SQ_TEX_INST_SET_GRADIENTS_V)
			tn->flags |= AF_KEEP_ALIVE;

		tn->flow_dep = get_var(info, REG_AM, 0, 0);



		split = build_split(info, tn->ins, false);

		if (split) {
			cl->child = split;
			split->parent = cl;

			cl->rest = create_node(NT_LIST);
			cl->rest->parent = cl;
			cl = cl->rest;
		}

		cl->child = tn;
		tn->parent = cl;

		split = build_split(info, tn->outs, true);

		if (split) {
			cl->rest = create_node(NT_LIST);
			cl->rest->parent = cl;
			cl = cl->rest;

			cl->child = split;
			split->parent = cl;
		}
	}
	return true;
}

static boolean parse_cf_vtx(struct shader_info * info, struct ast_node * node, struct r600_bytecode_cf * cf)
{
	struct r600_bytecode_vtx * vtx;
	struct ast_node * cl, * split;
	boolean first = true;

	LIST_FOR_EACH_ENTRY(vtx,&cf->vtx,list) {
		struct ast_node * tn = create_node(NT_OP);

		tn->subtype = NST_VTX_INST;

		if (first) {
			cl = create_node(NT_LIST);
			node->child=cl;
			cl->parent=node;
			first = false;
		} else {
			cl->rest = create_node(NT_LIST);
			cl->rest->parent = cl;
			cl = cl->rest;
		}

		tn->vtx = vtx;
		tn->ins = vvec_create(1);
		tn->ins->keys[0] = vtx->src_sel_x < 4 ? get_var(info, vtx->src_gpr, vtx->src_sel_x, 0) : NULL;

		tn->outs = vvec_create(4);
		tn->outs->keys[0] = vtx->dst_sel_x < 4 ? get_var(info, vtx->dst_gpr, vtx->dst_sel_x, 0) : NULL;
		tn->outs->keys[1] = vtx->dst_sel_y < 4 ? get_var(info, vtx->dst_gpr, vtx->dst_sel_y, 0) : NULL;
		tn->outs->keys[2] = vtx->dst_sel_z < 4 ? get_var(info, vtx->dst_gpr, vtx->dst_sel_z, 0) : NULL;
		tn->outs->keys[3] = vtx->dst_sel_w < 4 ? get_var(info, vtx->dst_gpr, vtx->dst_sel_w, 0) : NULL;

		tn->flags |= AF_REG_CONSTRAINT;

		tn->flow_dep = get_var(info, REG_AM, 0, 0);

		cl->child = tn;
		tn->parent = cl;

		cl->rest = create_node(NT_LIST);
		cl->rest->parent = cl;
		cl = cl->rest;

		split = build_split(info, tn->outs, true);

		cl->child = split;
		split->parent = cl;
	}
	return true;
}

/* split live intervals for alu 4-slot instruction groups
 * inserting copies before and after each group */
static void build_alu_list_split(struct shader_info * info, struct ast_node * list)
{
	/* maps original var to temp for ins and outs*/
	struct vmap * im = vmap_create(8);
	struct vmap * om = vmap_create(4);

	while (list) {

		/* FIXME: fix parse_cf_alu to get rid of last empty item */
		if (list->child && list->child->subtype == NST_ALU_GROUP) {
			struct ast_node * prev = list->parent, * next = list->rest;
			struct ast_node * g = list->child->child;
			struct ast_node * s_in = prev, * s_out = NULL, *s_out_start = NULL;
			boolean list_start = prev->child == list;
			boolean contains_last = false;
			boolean replicate = false;
			int q;

			while (g) {
				struct var_desc * v, *t;
				struct ast_node * n = g->child;

				if (!replicate && is_alu_replicate_inst(&info->shader->bc, n->alu))
					replicate = true;

				for (q=0; q<n->ins->count; q++) {

					v = n->ins->keys[q];

					if (v) {

						if (!VMAP_GET(im, v, &t)) {
							t = create_temp_var(info);
							t->reg.chan = v->reg.chan;
							VMAP_SET(im, v, t);
						} else {
							n->ins->keys[q] = t;
							continue;
						}

						n->ins->keys[q] = t;

						if (list_start) {
							s_in->child = create_node(NT_LIST);
							s_in->child->parent = s_in;
							s_in = s_in->child;
							list_start = false;
						} else {
							s_in->rest = create_node(NT_LIST);
							s_in->rest->parent = s_in;
							s_in = s_in->rest;
						}
						s_in->child = create_alu_copy(info, t, v);
						s_in->child->parent = s_in;
						s_in->child->alu->last = 1;
					}
				}

				v = n->outs->keys[0];

				if (v) {
					t = create_temp_var(info);
					t->reg.chan = v->reg.chan;
					n->outs->keys[0] = t;
					VMAP_SET(om, v, t);

				}

				if (n->alu->last)
					contains_last = true;

				g = g->rest;
			}

			if (im->count) {
				vmap_clear(im);

				if (s_in != prev)
					s_in->rest = list;
				list->parent = s_in;
			}

			if (om->count) {
				/* insert outs split after the last instruction in the group */

				if (!contains_last) {
					/* if 4-slot group doesn't contain last instruction,
					 * then the next (trans) instruction should be last, insert copies after it */

					list = next;
					next = list->rest;

					assert (list && list->child->alu->last);

					struct var_desc * tw = list->child->outs->keys[0];

					/* trans instruction overwrites the var written by the group */
					VMAP_REMOVE(om, tw);
				}

				if (om->count) {

					for (q=0; q<om->count; q++) {
						struct var_desc * v = om->keys[q*2];
						struct var_desc * t = om->keys[q*2+1];

						if (!s_out) {
							s_out = s_out_start = create_node(NT_LIST);
						} else {
							s_out->rest = create_node(NT_LIST);
							s_out->rest->parent = s_out;
							s_out = s_out->rest;
						}
						s_out->child = create_alu_copy(info, v, t);
						s_out->child->parent = s_out;
						s_out->child->alu->last = 1;
					}

					list->rest = s_out_start;
					s_out_start->parent = list;

					s_out->rest = next;
					if (next)
						next->parent = s_out;

					vmap_clear(om);
				}
			}

			list = next;

		} else
			list = list->rest;
	}

	vmap_destroy(im);
	vmap_destroy(om);
}


/* TODO: predicates and output modifiers */

static int parse_cf_alu(struct shader_info * info, struct ast_node * node, struct r600_bytecode_cf * cf)
{
	struct r600_bytecode * bc = &info->shader->bc;
	struct r600_bytecode_alu * alu;
	struct ast_node * cl, *cg = NULL;
	struct ast_node * slots[2][5];
	int cur_slots = 0;
	int insn = 0;
	boolean has_groups = false;

	memset(slots,0, 5 * 2 * sizeof(void*));

	cl = create_node(NT_LIST);
	node->child=cl;
	cl->parent=node;

	LIST_FOR_EACH_ENTRY(alu,&cf->alu,list) {
		struct ast_node * an = create_node(NT_OP);
		int num_op, i;
		int chan = alu->dst.chan, trans=0;
		boolean grouped;

		an->subtype = NST_ALU_INST;
		grouped = is_alu_four_slots_inst(&info->shader->bc, alu);

		if (grouped) {

			/* we need to keep these instructions together, so we are using
			 * additional level of hierarchy
			 */

			has_groups = true;

			if (!cg ) {
				cg = create_node(NT_GROUP);
				cg->subtype = NST_ALU_GROUP;
				cg->parent = cl;
				cl->child = cg;
				cg->child = create_node(NT_LIST);
				cg->child->parent=cg;
				cg = cg->child;
			} else {
				cg->rest = create_node(NT_LIST);
				cg->rest->parent=cg;
				cg = cg->rest;
			}

			cg->child=an;
			an->parent=cg;

		} else {

			if (cg) {
				cl->rest = create_node(NT_LIST);
				cl->rest->parent = cl;
				cl =  cl->rest;
				cg=NULL;
			}

			cl->child=an;
			an->parent=cl;
		}

		insn++;

		/* FIXME: replace with REG_PR for predicate support */
		an->flow_dep = get_var(info, REG_AM, 0, 0);

		an->alu_allowed_slots_type = AT_ANY;

		if (info->max_slots == 4) {
			trans = 0;
			an->alu_allowed_slots_type = AT_VECTOR;
		}
		else if (is_alu_trans_unit_inst(bc, alu)) {
			trans = 1;
			an->alu_allowed_slots_type = AT_TRANS;
		}
		else if (is_alu_vec_unit_inst(bc, alu)) {
			trans = 0;
			an->alu_allowed_slots_type = AT_VECTOR;
		}
		else if (slots[cur_slots][chan])
			trans = 1; /* Assume ALU_INST_PREFER_VECTOR. */
		else
			trans = 0;

		if (trans) {
			assert(!slots[cur_slots][4]); /* check if ALU.Trans has already been allocated. */
			slots[cur_slots][4] = an;
		} else {
			assert(!slots[cur_slots][chan]); /* check if ALU.chan has already been allocated. */
			slots[cur_slots][chan] = an;
		}

		an->alu = alu;

		if (alu->predicate) {
			an->outs = vvec_create(3);
			an->outs->keys[1] = get_var(info, REG_PR, 0, 0);
			an->outs->keys[2] = get_var(info, REG_AM, 0, 0);
		} else
			an->outs = vvec_create(1);

		boolean write = (alu->dst.write==1) || alu->is_op3;

		if (alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOVA_INT && !alu->is_op3)
			an->outs->keys[0] = get_var(info, REG_AR, 0, 0);
		else
			an->outs->keys[0] = (alu->dst.sel<BC_NUM_REGISTERS && write) ? get_var(info, alu->dst.sel, alu->dst.chan, 0) : NULL;

		if (alu->dst.rel)
			return 0;

		if (alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV && !alu->is_op3)
			an->flags |= AF_COPY_HINT;

		if (alu->dst.clamp)
			an->flags |= AF_ALU_CLAMP_DST;

		if (is_alu_four_slots_inst(&info->shader->bc, alu)) {
			an->flags |= AF_FOUR_SLOTS_INST;
		} else if (is_alu_kill_inst(&info->shader->bc, alu))
			an->flags |= AF_KEEP_ALIVE;

		if (!alu->is_op3 && (alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_XY ||
				alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_ZW ||
				alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_LOAD_P0 ||
				alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_CUBE))
			an->flags |= AF_CHAN_CONSTRAINT;

		num_op = r600_bytecode_get_num_operands(&info->shader->bc, alu);
		an->ins = vvec_create(num_op);

		for (i=0;i<num_op;i++) {
			struct var_desc * v = NULL;

			if (alu->src[i].rel) return 0;

			int sel = alu->src[i].sel;

			if (sel >= BC_KCACHE_FINAL_OFFSET) {

				sel -= BC_KCACHE_FINAL_OFFSET;

				assert((sel>=128 && sel<192) || (sel>=256 && sel<320));

				/* restore kcache addr for r600_bytecode_alloc_kcache_lines */

				/* kcache bank offsets:
				 *
				 *  bank 0 - 128 - 010000000
				 *  bank 1 - 160 - 010100000
				 *  bank 2 - 256 - 100000000
				 *  bank 3 - 288 - 100100000
				 */

				int kc_set = ((sel>>7)&2) + ((sel>>5)&1), kc_addr;

				kc_addr = (cf->kcache[kc_set].addr<<4) + (sel&0x1F);

				alu->src[i].sel = BC_KCACHE_OFFSET + kc_addr;
			}

			if (alu->src[i].sel < BC_NUM_REGISTERS)
				v = get_var(info, alu->src[i].sel, alu->src[i].chan, 0);
			else if (alu->src[i].sel == V_SQ_ALU_SRC_PV || alu->src[i].sel == V_SQ_ALU_SRC_PS) {
				int prev_slot = (alu->src[i].sel == V_SQ_ALU_SRC_PS) ? 4 : alu->src[i].chan;
				struct ast_node * p = slots[!cur_slots][prev_slot];

				assert(p);

				if (p->outs->keys[0])
					v = p->outs->keys[0];
				else {
					v = create_temp_var(info);
					p->outs->keys[0] = v;
				}
			}

			an->ins->keys[i] = v;

			if (!v)
				an->const_ins_count++;
		}

		if (alu->last) {
			info->stats[0].nalugroups++;
			cur_slots = !cur_slots;
			memset(slots[cur_slots],0,5*sizeof(void*));
			cg=NULL;
		}

		if (!grouped || alu->last) {
			cl->rest = create_node(NT_LIST);
			cl->rest->parent = cl;
			cl =  cl->rest;
			cg=NULL;
		}
	}

	if (has_groups)
		build_alu_list_split(info, node->child);

	return insn;
}

static unsigned * get_output_swizzle_ptr(struct r600_bytecode_output * out, int index)
{
	switch (index) {
	case 0: return &out->swizzle_x;
	case 1: return &out->swizzle_y;
	case 2: return &out->swizzle_z;
	case 3: return &out->swizzle_w;
	default: assert(0); return NULL;
	}
}


static struct ast_node * parse_cf(struct shader_info * info, struct ast_node * cfn)
{
	struct r600_bytecode_cf * cf = cfn->cf;
	boolean exp_inst = false;

	switch (cf->inst) {
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT:
	case EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE:
		exp_inst = true;
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
	{

		unsigned count =  cf->output.burst_count, q, w;
		struct ast_node * ln = cfn->parent, * split;

		cfn->cf->output.burst_count = 1;
		cfn->flow_dep = get_var(info, REG_AM, 0, 0);

		for (w = 0; w<count; w++) {

			if (w) {
				cfn = create_node(NT_OP);
				cfn->cf = malloc(sizeof(struct r600_bytecode_cf));
				memcpy(cfn->cf, cf, sizeof(struct r600_bytecode_cf));

				cfn->cf->output.gpr++;
				cfn->cf->output.array_base++;

				ln->rest = create_node(NT_LIST);
				ln->rest->parent = ln;
				ln = ln->rest;
			}

			if (exp_inst)
				cfn->cf->output.inst = EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT;

			cfn->ins = vvec_create(4);
			cfn->op_class = exp_inst ? NOC_CF_EXPORT : NOC_CF_STREAMOUT;

			for (q=0; q<4; q++) {
				int swz;
				if (exp_inst) {
					swz = *get_output_swizzle_ptr(&cf->output,q);
					swz = swz>3 ? -1 : swz;
				} else
					swz = ((cf->output.comp_mask >> q) & 1) ? q : -1;

				struct var_desc * v = swz>=0 ? get_var(info, cfn->cf->output.gpr, swz, 0) : NULL;

				cfn->ins->keys[q] = v;
			}

			cfn->type = NT_OP;
			cfn->flags |= AF_REG_CONSTRAINT;
			if (!exp_inst)
				cfn->flags |= AF_CHAN_CONSTRAINT;

			split = build_split(info, cfn->ins, false);

			if (split) {
				ln->child = split;
				split->parent = ln;

				ln->rest = create_node(NT_LIST);
				ln->rest->parent = ln;
				ln = ln->rest;
			}

			ln->child = cfn;
			cfn->parent = ln;

			cfn = ln->child;
			cfn->subtype = NST_CF_INST;

			cf = cfn->cf;
		}
	}
	break;
	case EG_V_SQ_CF_WORD1_SQ_CF_INST_CALL_FS:
		cfn->outs = vvec_create(1);
		cfn->outs->keys[0] = get_var(info, REG_AM, 0, 0);
		break;

	}

	return cfn;
}

static boolean parse_shader(struct shader_info * info, struct ast_node * root)
{
	struct r600_shader * shader = info->shader;
	struct r600_bytecode_cf *cf;
	struct ast_node * node = root;

	LIST_FOR_EACH_ENTRY(cf,&shader->bc.cf,list) {

		if (cf->inst == EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_EXTENDED)
			continue;

		struct ast_node * cfn = create_node(NT_GROUP);
		cfn->subtype = NST_CF_INST;

		assert(node->type == NT_LIST);

		node->child = cfn;
		cfn->parent = node;

		cfn->cf = cf;
		cfn->label = cf->id;

		cfn->flow_dep = get_var(info, REG_AM, 0, 0);

		cfn = parse_cf(info, cfn);
		node = cfn->parent;

		node->rest = create_node(NT_LIST);
		node->rest->parent = node;

		if (!LIST_IS_EMPTY(&cf->alu)) {

			cfn->subtype = NST_ALU_CLAUSE;

			cfn->insn = parse_cf_alu(info,cfn,cf);

			if (!cfn->insn) {
				info->result_code = SR_SKIP_RELADDR;
				return false;
			}

		} else if (!LIST_IS_EMPTY(&cf->tex)) {
			if (!parse_cf_tex(info,cfn,cf)) return false;
		} else if (!LIST_IS_EMPTY(&cf->vtx)) {
			if (!parse_cf_vtx(info,cfn,cf)) return false;
		} else
			cfn->type = NT_OP;

		node = node->rest;
	}
	return true;
}

static struct ast_node * find_cf_by_addr(struct ast_node * list, int addr)
{
	assert(list->type == NT_LIST);

	while (list) {
		if (list->child && list->child->cf && list->child->label == addr)
			return list->child;
		list=list->rest;
	}
	return NULL;
}

static struct ast_node * convert_cf_if(struct shader_info * info, struct ast_node * root, boolean cnd)
{
	int target = root->cf->cf_addr;
	struct ast_node *list = root->parent, *end = NULL;
	boolean without_else = root->cf->pop_count >0;

	assert(list->type==NT_LIST);
	list = list->rest;
	R600_DUMP("converting if %p @ %d\n", root, root->cf->id);

	end = find_cf_by_addr(list, target);
	assert(end && end->cf);

	if (!without_else && end->cf->inst == EG_V_SQ_CF_WORD1_SQ_CF_INST_ELSE) {

		struct ast_node *n_else = end->parent, * region, *depart, *depart2;

		R600_DUMP("converting if %p : else @ %d\n",root,  end->cf->id);

		end = find_cf_by_addr(end->parent, end->cf->cf_addr);
		assert(end);

		R600_DUMP("converting if %p : end @ %d\n", root, end->cf->id);

		end = end->parent;

		region = create_node(NT_REGION);
		depart = create_node(NT_DEPART);
		depart2 = create_node(NT_DEPART);

		region->label = root->label;

		region->subtype = NST_IF_ELSE_REGION;

		if (root->parent->rest!=n_else) {
			depart2->child = root->parent->rest;
			depart2->child->parent = depart2;
		}

		n_else->parent->rest = NULL;
		end->parent->rest = NULL;

		region->parent = root->parent;
		region->parent->child = region;
		region->parent->rest = end;
		end->parent = region->parent;

		depart2->parent = root;
		root->child = depart2;

		depart->child = create_node(NT_LIST);
		depart->child->parent = depart;

		depart->child->child = root;

		root->parent = depart->child;

		region->child = depart;
		depart->parent = region;

		root->parent->rest = n_else;
		n_else->parent = root->parent;

		depart->depart_number = ++region->depart_count;
		depart2->depart_number = ++region->depart_count;

		depart->target = region->label;
		depart2->target = region->label;

		depart->target_node = region;
		depart2->target_node = region;

		root->type = NT_IF;

		root->outs = vvec_create(1);
		root->outs->keys[0] = get_var(info, REG_AM, 0,0);
		root->flow_dep = get_var(info, REG_AM, 0,0);

		n_else = n_else->child;

		n_else->outs = vvec_create(1);
		n_else->outs->keys[0] = get_var(info, REG_AM, 0,0);
		n_else->flow_dep = get_var(info, REG_AM, 0,0);


		/* FIXME: set the condition correctly */

		root = region;
	} else {
		R600_DUMP("converting if %p: end @ %d\n", root, end->cf->id);

		list=root->parent;
		root->child = list->rest;
		root->child->parent = root;

		end = end->parent;

		end->parent->rest = NULL;

		list->rest = end;
		end->parent = list;

		root->type = NT_IF;

		root->outs = vvec_create(1);
		root->outs->keys[0] = get_var(info, REG_AM, 0,0);

		root->flow_dep = get_var(info, REG_AM, 0,0);
	}
	return root;
}

static int convert_cf_loop(struct shader_info * info, struct ast_node * root)
{
	int target = root->cf->cf_addr;
	struct ast_node *list = root->parent, *end = NULL, *p;

	assert(list->type==NT_LIST);

	list = list->rest;

	end = find_cf_by_addr(list, target);
	assert(end);

	end = end->parent;
	p=end->parent->child;
	assert(p->cf && p->cf->inst == EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_END);

	list=root->parent;

	root->type = NT_REGION;

	root->subtype = NST_LOOP_REGION;

	root->child = create_node(NT_REPEAT);
	root->child->parent = root;

	root->child->repeat_number = ++root->repeat_count;

	p=root->child;

	p->target = p->parent->label;
	p->target_node = p->parent;

	p->child = list->rest;
	p->child->parent = p;

	end->parent->rest = NULL;

	list->rest = end;
	end->parent = list;

	return 0;
}

static struct ast_node * find_block_start(struct ast_node * node)
{
	struct ast_node * prev;
	do {
		prev = node;
		node = node->parent;
	} while (node->type != NT_REGION && node->type != NT_IF && node->type!= NT_DEPART && node->type != NT_REPEAT);
	return prev;
}

static void convert_loop_ops(struct shader_info * info, struct ast_node * node, boolean brk)
{
	struct ast_node * p=node->parent;

	while (p && p->repeat_count==0)
		p=p->parent;

	if (!p)
		return;

	assert(p->type == NT_REGION);

	node->target=p->label;
	node->target_node = p;

	node->outs = vvec_create(1);
	node->outs->keys[0] = get_var(info, REG_AM, 0,0);

	node->flow_dep = get_var(info, REG_AM, 0,0);

	if (brk) {
		node->type = NT_DEPART;
		node->subtype = NST_LOOP_BREAK;
		node->depart_number=++p->depart_count;
	} else {
		node->type = NT_REPEAT;
		node->subtype = NST_LOOP_CONTINUE;
		node->repeat_number=++p->repeat_count;
	}

	p = find_block_start(node);

	if (p == node->parent || p == node)
		return;

	node->child = p;

	node->parent->parent->rest = NULL;

	p->parent->child = node;

	node->parent = p->parent;
	p->parent = node;

}

static int convert_cf(struct shader_info * info, struct ast_node * root)
{
	if (root==NULL)
		return 0;

	if (root->type == NT_LIST)
		convert_cf(info, root->rest);

	if (root->child)
		convert_cf(info, root->child);

	if (root->cf && root->type == NT_OP) {

		switch (root->cf->inst) {
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_START_NO_AL:
			convert_cf_loop(info, root);
			convert_cf(info, root->child);
			break;
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP:
			root = convert_cf_if(info, root, true);
			break;
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_BREAK:
			convert_loop_ops(info, root, true);
			break;
		case EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_CONTINUE:
			convert_loop_ops(info, root, false);
			break;
		}
	}

	return 0;
}

static void variables_defined(struct shader_info * info, struct ast_node * node)
{
	node->vars_defined = vset_create(1, 1);

	if (node->child)
		variables_defined(info, node->child);

	if (node->rest)
		variables_defined(info, node->rest);

	if ((node->type == NT_DEPART || node->type == NT_REPEAT) && node->child)
		vset_addset(node->target_node->vars_defined, node->child->vars_defined);
	else if (node->type == NT_OP) {

		if (node->outs) {
			vset_clear(node->vars_defined);
			vset_addvec(node->vars_defined, node->outs);
		}

	} else {

		if (node->outs) {
			vset_clear(node->vars_defined);
			vset_addvec(node->vars_defined, node->outs);
		}

		if (node->child)
			vset_addset(node->vars_defined, node->child->vars_defined);

		if (node->rest)
			vset_addset(node->vars_defined, node->rest->vars_defined);


		/* don't let alu clause locals to get out of clause */
		if (node->subtype == NST_ALU_CLAUSE) {
			int q;
			for (q=0; q<node->vars_defined->count; q++) {
				struct var_desc * v = node->vars_defined->keys[q];
				if (v && ((v->reg.reg == REG_PR) ||
						(v->reg.reg < BC_NUM_REGISTERS && v->reg.reg >= BC_NUM_REGISTERS-info->temp_gprs))) {
					vset_remove(node->vars_defined, v);
					q--;
				}
			}
		}
	}
}

static struct ast_node * phi_make_trivials(struct ast_node * node, int count)
{
	int i, q;

	struct ast_node *start=NULL, * p, *l=NULL;
	for (i=0; i<node->vars_defined->count; i++) {
		struct var_desc * v = node->vars_defined->keys[i];

		p = create_node(NT_OP);
		p->subtype = NST_PHI;

		p->outs = vvec_create(1);
		p->ins = vvec_create(count);

		p->outs->keys[0] = v;

		for (q=0; q<count; q++)
			p->ins->keys[q] = v;

		if (l) {
			l->rest = create_node(NT_LIST);
			l->rest->parent = l;
			l=l->rest;
		} else
			l = create_node(NT_LIST);

		p->parent = l;
		l->child = p;

		if (!start)
			start = l;
	}

	return start;
}

static void insert_phi(struct ast_node * node)
{
	if (node->child)
		insert_phi(node->child);

	if (node->rest)
		insert_phi(node->rest);

	if (node->type == NT_IF)
		node->phi = phi_make_trivials(node,2);

	else if (node->type == NT_REGION) {
		node->phi = phi_make_trivials(node, node->depart_count);
		if (node->repeat_count)
			node->loop_phi = phi_make_trivials(node, node->repeat_count+1);
	}
}

static struct var_desc * rename_var(struct shader_info * info, struct var_desc * var, int new_index)
{
	struct var_desc * new_var = get_var(info, var->reg.reg, var->reg.chan, new_index);
	new_var->flags = var->flags;
	return new_var;
}

static void add_use(struct var_desc * v, struct ast_node * node)
{

	if (!v->uses)
		v->uses = vset_create(4, 0);

	vset_add(v->uses, node);
}

static void rename_phi_operand(struct shader_info * info, int n, struct ast_node * phi, struct vmap * renames)
{
	uintptr_t i=0;
	struct var_desc * v;
	void *d;
	assert(n-1<phi->ins->count);
	v = phi->ins->keys[n-1];

	if (VMAP_GET(renames, v, &d))
		i = (uintptr_t)d;

	v = rename_var(info, v, i);
	add_use(v, phi);
	phi->ins->keys[n-1] = v;
}

static struct var_desc * rename_def(struct shader_info * info, struct var_desc * var, struct vmap * renames, struct ast_node * node)
{
	uintptr_t  i;
	void * d;
	if (vmap_get(info->def_count, var, &d))
		i=((uintptr_t)d)+1;
	else
		i=1;

	VMAP_SET(info->def_count, var, i);
	VMAP_SET(renames, var, i);

	var = rename_var(info, var, i);
	var->def = node;
	return var;
}

static struct var_desc * ssa_rename_use(struct shader_info * info, struct var_desc * vi, struct vmap * renames)
{
	uintptr_t new_index = 0;
	void * d;

	if (vmap_get(renames, vi, &d))
		new_index = (uintptr_t)d;

	vi = rename_var(info, vi, new_index);
	return vi;
}

static void ssa_ins(struct shader_info * info, struct ast_node * node, struct vmap * renames)
{
	int i;

	if (node->ins) {
		for (i=0;i<node->ins->count;i++) {
			struct var_desc * vi = node->ins->keys[i];
			if (vi) {
				vi = ssa_rename_use(info, vi, renames);
				add_use(vi, node);
				node->ins->keys[i] = vi;
			}
		}
	}
}

static void ssa_outs(struct shader_info * info, struct ast_node * node, struct vmap * renames, struct vset * processed)
{
	int i;

	if (node->outs)
	{
		for (i=0;i<node->outs->count;i++) {
			struct var_desc * v = node->outs->keys[i];

			if (v) {
				assert(v->index == 0);

				if (processed && vset_contains(processed, v)) {
					uintptr_t ni = 0;
					void *d;

					if (VMAP_GET(renames, v, &d))
						ni = (uintptr_t)d;

					assert(ni);
					v = rename_var(info, v, ni);
				} else {
					if (processed)
						vset_add(processed,v);
					v = rename_def(info, v, renames, node);
				}

				node->outs->keys[i] = v;
			}
		}
	}
}


/* build ssa form - renaming variables */
/* top-down code traversal */
static void ssa(struct shader_info * info, struct ast_node * node, struct vmap * renames)
{
	if (!node)
		return;

	if (node->flow_dep)
		node->flow_dep = ssa_rename_use(info, node->flow_dep, renames);

	switch (node->type) {
	case NT_REGION:
		if (node->loop_phi) {
			struct ast_node * p = node->loop_phi;
			while (p && p->child) {
				rename_phi_operand(info, 1, p->child, renames);
				p->child->outs->keys[0] = rename_def(info, p->child->outs->keys[0], renames, p->child);
				p = p->rest;
			}
		}

		ssa(info, node->child, renames);

		if (node->phi) {
			struct ast_node * p = node->phi;
			while (p && p->child) {
				p->child->outs->keys[0] = rename_def(info, p->child->outs->keys[0], renames, p->child);
				p = p->rest;
			}
		}
		break;

	case NT_IF:
		if (node->phi) {
			struct ast_node * p = node->phi;
			while (p && p->child) {
				rename_phi_operand(info, 1, p->child, renames);
				p = p->rest;
			}
		}

		ssa_ins(info, node, renames);
		ssa_outs(info, node, renames, NULL);
		ssa(info, node->child, renames);

		if (node->phi) {
			struct ast_node * p = node->phi;
			while (p && p->child) {
				rename_phi_operand(info, 2, p->child, renames);
				p->child->outs->keys[0] = rename_def(info, p->child->outs->keys[0], renames, p->child);
				p = p->rest;
			}
		}
		break;

	case NT_DEPART:
	{
		struct vmap * new_renames = vmap_createcopy(renames);

		ssa(info, node->child, new_renames);
		ssa_ins(info, node, renames);
		ssa_outs(info, node, renames, NULL);

		if (node->target_node && node->target_node->phi) {
			struct ast_node * p = node->target_node->phi;
			while (p && p->child) {
				rename_phi_operand(info, node->depart_number, p->child, new_renames);
				p = p->rest;
			}
		}
		vmap_destroy(new_renames);
	}
	break;

	case NT_REPEAT:
	{
		struct vmap * new_renames = vmap_createcopy(renames);

		ssa(info, node->child, new_renames);
		ssa_ins(info, node, renames);
		ssa_outs(info, node, renames, NULL);

		if (node->target_node && node->target_node->loop_phi) {
			struct ast_node * p = node->target_node->loop_phi;
			while (p && p->child) {
				rename_phi_operand(info, node->repeat_number+1, p->child, new_renames);
				p = p->rest;
			}
		}

		vmap_destroy(new_renames);
	}
	break;

	case NT_LIST:
		ssa(info, node->child, renames);
		ssa(info, node->rest, renames);
		break;

	case NT_GROUP:
		ssa(info, node->child, renames);
		break;

	case NT_OP:

		if (node->subtype == NST_ALU_INST || node->subtype == NST_COPY) {
			/* take into account parallel execution of alu groups */

			/* every "last" instruction corresponds to alu group */
			if (node->alu->last) {

				/* get all instructions of the alu group, some of them may be
				 * inside group node (multislot instructions), need to handle
				 * this correctly
				 */
				struct ast_node * c = node->parent->parent, *last_group=NULL;
				struct ast_node * ii[5];
				int count = 1, level=0;
				ii[0] = node;

				while (c && c->child && c->subtype!=NST_ALU_CLAUSE) {

					if (c->child->alu) {
						if (c->child->alu->last==0)
							ii[count++] = c->child;
						else
							break;
					} else if (c->child->subtype == NST_ALU_GROUP) {
						if (c!=last_group && level==0) {
							level++;
							last_group = c->child;
							c = last_group->child;
							while (c->rest)
								c=c->rest;
							continue;
						} else
							level--;
					}
					c = c->parent;
				}

				for (int q=0; q<count; q++)
					ssa_ins(info, ii[q], renames);

				for (int q=count-1; q>=0; q--)
					ssa_outs(info, ii[q], renames, NULL);
			}
		} else {
			ssa_ins(info, node, renames);
			ssa_outs(info, node, renames, NULL);

			if (node->flags & AF_REG_CONSTRAINT) {
				set_constraint(node, true);
				set_constraint(node, false);
			}
		}
		break;

	default:
		assert(0);
		break;
	}
}

static void build_ssa(struct shader_info * info)
{
	struct vmap * renames = vmap_create(64);

	info->def_count = vmap_create(64);

	variables_defined(info, info->root);
	insert_phi(info->root);
	ssa(info, info->root, renames);

	vmap_destroy(info->def_count);
	vmap_destroy(renames);
	info->def_count = NULL;
}

static void update_ins_liveness(struct ast_node * node)
{
	int q;

	for(q=0; q<node->ins->count; q++) {
		struct var_desc * v = node->ins->keys[q];
		if (v) {
			if (!(node->flags & AF_DEAD))
				v->flags &= ~VF_DEAD;
		}
	}
}

static void outs_dead(struct ast_node * node, struct vset * live)
{
	if (node->rest)
		outs_dead(node->rest, live);

	if (node->child)
		outs_dead(node->child, live);

	if (node->outs) {
		if (!vset_removevec(live, node->outs))
			node->flags |= AF_DEAD;
		else {
			node->flags &= ~AF_DEAD;
			update_ins_liveness(node);
		}

	}
}

static void live_phi_branch(struct ast_node * node, struct vset * live, int n)
{
	if (node->flags & AF_DEAD)
		return;

	if (node->rest)
		live_phi_branch(node->rest, live, n);

	if (node->child)
		live_phi_branch(node->child, live, n);

	if (node->ins)
		vset_add(live, node->ins->keys[n-1]);
}


static void mark_interferences(struct vset * live)
{
	int q;

	for(q=0; q<live->count; q++) {
		struct var_desc * v = live->keys[q];

		if (!(v->flags & VF_DEAD)) {
			if (v->interferences)
				vset_addset(v->interferences, live);
			else
				v->interferences = vset_createcopy(live);
		}
	}
}

/* computing liveness information */
/* bottom-up code traversal */
static void node_liveness(struct ast_node * node, struct vset * live)
{
	if (node->phi)
		outs_dead(node->phi, live);

	if (node->type!=NT_LIST && ((node->subtype != NST_ALU_INST && node->subtype!=NST_COPY) || node->alu->last))
		mark_interferences(live);

	if (node->vars_live_after)
		vset_copy(node->vars_live_after, live);
	else
		node->vars_live_after = vset_createcopy(live);

	if (node->type == NT_DEPART) {
		vset_copy(live, node->target_node->vars_live_after);
		if (node->target_node->phi)
			live_phi_branch(node->target_node->phi, live, node->depart_number);
	}

	if (node->type == NT_IF && node->phi)
		live_phi_branch(node->phi, live, 2);

	if (node->type == NT_REPEAT && node->target_node->loop_phi) {
		vset_copy(live, node->target_node->vars_live);
		live_phi_branch(node->target_node->loop_phi, live, node->repeat_number+1);
	}

	if (node->rest)
		node_liveness(node->rest, live);

	if (node->child) {
		if (node->child->type == NT_REGION && node->child->vars_live)
			vset_clear(node->child->vars_live);

		node_liveness(node->child, live);
	}

	if (node->type == NT_IF) {
		vset_addset(live, node->vars_live_after);
		if (node->phi)
			live_phi_branch(node->phi, live, 1);
	}

	if (node->type == NT_OP) {
		if (node->outs && !(node->flags & AF_KEEP_ALIVE)) {
			boolean alive = false;

			for (int q=0; q<node->outs->count; q++) {
				struct var_desc * o = node->outs->keys[q];
				if (o) {
					if (!vset_remove(live, o))
						o->flags |= VF_DEAD;
					else {
						alive = true;
						o->flags &= ~VF_DEAD;
					}
				}
			}

			if (!alive)
				node->flags |= AF_DEAD;
			else
				node->flags &=~AF_DEAD;
		}

		if (!(node->flags & AF_DEAD) && node->ins)
			vset_addvec(live,node->ins);

	} else if (node->type == NT_IF && node->ins)
			vset_addvec(live,node->ins);

	if (node->loop_phi) {
		outs_dead(node->loop_phi, live);

		if (node->vars_live)
			vset_copy(node->vars_live, live);
		else
			node->vars_live = vset_createcopy(live);

		if (node->child)
			node_liveness(node->child, live);

		outs_dead(node->loop_phi, live);
		live_phi_branch(node->loop_phi, live, 1);
	}

	if ((node->flags & AF_FOUR_SLOTS_INST) && node->parent->parent->subtype == NST_ALU_GROUP) {
		struct ast_node * n = node->parent;
		boolean alive = false;
		while (n) {
			if (n->child && (n->child->flags & AF_FOUR_SLOTS_INST) && !(n->child->flags & AF_DEAD)) {
				alive = true;
				break;
			}
			n = n->rest;
		}
		if (alive) {
			n = node->parent;
			while (n) {
				if (n->child && (n->child->flags & AF_DEAD) && (n->child->flags & AF_FOUR_SLOTS_INST)) {
					n->child->flags &= ~AF_DEAD;

					n->child->alu->dst.write = 0;
					n->child->outs->keys[0] = NULL;

					vset_removevec(live, n->child->outs);
					vset_addvec(live,n->child->ins);
				}
				n = n->rest;
			}
		} else
			node->parent->parent->flags |= AF_DEAD;
	}

	if (node->flow_dep && !(node->flags & AF_DEAD))
		vset_add(live,node->flow_dep);

	if ((node->type != NT_LIST) && (node->subtype != NST_ALU_INST) && (node->subtype != NST_COPY))
		mark_interferences(live);

	if (node->vars_live)
		vset_copy(node->vars_live, live);
	else
		node->vars_live = vset_createcopy(live);
}

static void liveness(struct shader_info * info)
{
	struct vset * live_vars = vset_create(1, 1);

	node_liveness(info->root, live_vars);

	vset_destroy(live_vars);
	info->liveness_correct = true;
}

static void check_copy(struct shader_info * info, struct var_desc * v)
{
	struct var_desc * src = v->def->ins->keys[0];

	if (src && (src->flags & VF_UNDEFINED))
		v->flags |= VF_UNDEFINED;
	else if ((v->def && v->def->alu && (v->def->alu->src[0].neg || v->def->alu->src[0].abs))) {
		v->def->flags &= ~AF_COPY_HINT;
		v->value_hint = NULL;
		return;
	}

	v->value_hint = src;
}

// for interp_xy output modifiers for y should be set on z channel, same with some others
static struct ast_node * get_real_def_node(struct var_desc * v)
{
	struct ast_node * d = v->def;
	if (!d)
		return NULL;

	if (d->alu) {
		int chan;

		chan = d->alu->dst.chan;

		if (chan==1 && d->alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_XY)
			return d->parent->rest->child;
		else if (chan==3 && d->alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INTERP_ZW)
			return d->parent->parent->parent->parent->child;
	}

	return d;
}

static void propagate_clamp(struct shader_info * info, struct var_desc * v)
{
	struct var_desc * s = v->value_hint;
	struct ast_node *vdef = get_real_def_node(v);
	struct ast_node *sdef = get_real_def_node(s);
	boolean propagate = false;

	R600_DUMP( "propagate_clamp : checking copy ");
	print_var(v);
	R600_DUMP( " from ");
	print_var(s);
	R600_DUMP( "\n");

	if (vdef && (vdef->flags & AF_ALU_CLAMP_DST) && s->def && sdef->alu) {
		int q;

		propagate = true;

		if (!(sdef->flags & AF_ALU_CLAMP_DST)) {

			R600_DUMP( "propagate_clamp: clamp propagation: checking src usage ...\n");

			if (s->uses) {
				for (q=0; q<s->uses->count; q++) {
					struct ast_node * u = s->uses->keys[q];

					if (u != v->def) {
						if (!(u->flags & AF_DEAD) && (!(u->flags & AF_COPY_HINT) || !(u->flags & AF_ALU_CLAMP_DST))) {
							propagate = false;
							R600_DUMP( "propagate_clamp: can't propagate ");
							dump_node(info, u, 0);
							break;
						}
					}
				}
			}

			if (propagate) {
				sdef->flags |= AF_ALU_CLAMP_DST;
				R600_DUMP("  propagated to ");
				print_var(s);
				R600_DUMP(" def\n");
			}

		}
	}

	if (!propagate && (vdef->flags & AF_SPLIT_COPY) && (v->def->flags & AF_ALU_CLAMP_DST)) {
			vdef->flags &= ~AF_ALU_CLAMP_DST;
			R600_DUMP("  reverted clamp for ");
			print_var(v);
			R600_DUMP("\n");
	}

	if (propagate && s->value_hint)
		propagate_clamp(info, s);
}

static void analyze_vars(struct shader_info * info)
{
	int q;

	for (q=0; q<info->vars->count; q++) {
		struct var_desc * v = info->vars->keys[q*2+1];

		if (v->flags & VF_SPECIAL)
			continue;

		if (!v->def) {
			unsigned rc = REGCHAN_KEY(v->reg.reg, v->reg.chan);

			if (!v->uses) {
				vmap_remove(info->vars, info->vars->keys[q--*2]);
				destroy_var(v);

//				v->flags |= VF_ABSOLUTELY_DEAD;

				continue;
			}

			if (v->reg.reg > info->last_input_gpr) {
				R600_DUMP( "undefined var usage : ");
				print_var(v);
				R600_DUMP("\n");

				v->flags |= VF_UNDEFINED;
				continue;
			}

			/* var never defined - probably input */
			v->flags |= VF_PIN_CHAN | VF_PIN_REG;
			v->color = rc;

			if (rc>info->last_color)
				info->last_color = rc;

			v->fixed = true;

			R600_DUMP( "input mapped ");
			print_var(v);
			R600_DUMP("\n");
		} else {

			if ((v->flags & VF_DEAD) || (v->def->flags & AF_DEAD))
				continue;

			if (v->def->flags & AF_CHAN_CONSTRAINT) {
				if (!v->def->alu || !is_alu_replicate_inst(&info->shader->bc,v->def->alu)) {
					v->flags |= VF_PIN_CHAN;
				}
			} else {
				int q;

				if (v->uses) {
					for (q=0; q<v->uses->count; q++) {
						struct ast_node * u = v->uses->keys[q];

						if (u->flags & AF_CHAN_CONSTRAINT) {
							v->flags |= VF_PIN_CHAN;
							break;
						}
					}
				}
			}

			if (v->def->flags & AF_COPY_HINT)
				check_copy(info, v);
		}
	}

	for (q=0; q<info->vars->count; q++) {
		struct var_desc * v = info->vars->keys[q*2+1];

		if (v->def) {
			if ((v->flags & VF_DEAD) || (v->def->flags & AF_DEAD))
				continue;

			if ((v->def->flags & AF_COPY_HINT) && v->value_hint)
				propagate_clamp(info, v);
		}
	}

	for (q=0; q<info->vars->count; q++) {
		struct var_desc * v = info->vars->keys[q*2+1];

		if (v->def) {
			struct var_desc * s = v->value_hint;
			struct ast_node *vdef = get_real_def_node(v);
			struct ast_node *sdef = s ? get_real_def_node(s) : NULL;

			if ((v->flags & VF_DEAD) || (v->def->flags & AF_DEAD))
				continue;

			// for reduction insts check clamps for all 4 instructions
			if (v->def->alu && is_alu_reduction_inst(&info->shader->bc,v->def->alu)) {
				struct ast_node * g = v->def->parent, *p;
				int clamps_count = 0;

				while (g->subtype != NST_ALU_GROUP)
					g = g->parent;

				p = g->child;

				while (p && p->child) {
					struct var_desc * o = p->child->outs->keys[0];
					if ((p->child->flags & AF_ALU_CLAMP_DST) || (o == NULL) || (o->flags & VF_DEAD))
						clamps_count++;

					p = p->rest;
				}

				p = g->child;

				while (p && p->child) {

					if (clamps_count==4)
						p->child->flags |= AF_ALU_CLAMP_DST;
					else
						p->child->flags &= ~AF_ALU_CLAMP_DST;

					p = p->rest;
				}
			}

			if ((v->def->flags & AF_COPY_HINT) && s && ((sdef &&
					(vdef->flags & AF_ALU_CLAMP_DST) != (sdef->flags & AF_ALU_CLAMP_DST))
					|| (!sdef && !(s->flags & VF_UNDEFINED) && (vdef->flags & AF_ALU_CLAMP_DST)))) {

				v->value_hint = NULL;
				v->def->flags &= ~AF_COPY_HINT;
			}
		}
	}

	for (q=0; q<info->vars->count; q++) {
		struct var_desc * v = info->vars->keys[q*2+1];

		if (v->def) {
			if ((v->flags & VF_DEAD) || (v->def->flags & AF_DEAD))
				continue;

			if (v->value_hint) {

				R600_DUMP("checking value_hint: ");
				print_var(v);
				R600_DUMP(" <= ");
				print_var(v->value_hint);
				R600_DUMP("\n");

				if (!v->constraint && !v->value_hint->constraint)
					add_affinity_edge(info, v, v->value_hint, AE_COPY_COST);

				if(v->def->flags & AF_COPY_HINT)
					v->def->subtype = NST_COPY;

				while (v->value_hint->value_hint)
					v->value_hint = v->value_hint->value_hint;
			}
		}
	}
}

static unsigned get_var_alloc(struct shader_info * info, struct var_desc * v)
{
	assert(!(v->flags & VF_DEAD));
	return v->color;
}


void destroy_ast(struct ast_node * n)
{
	if (!n)
		return;

	if (n->flags & AF_ALU_DELETE)
		free(n->alu);

	destroy_ast(n->rest);
	destroy_ast(n->child);

	vvec_destroy(n->ins);
	vvec_destroy(n->outs);

	destroy_ast(n->phi);
	destroy_ast(n->loop_phi);

	vset_destroy(n->vars_defined);
	vset_destroy(n->vars_used);
	vset_destroy(n->vars_live);
	vset_destroy(n->vars_live_after);

	free(n);
}

static void set_alu_regs(struct shader_info * info, struct ast_node * node)
{
	int q;

	for (q=0;q<node->ins->count;q++) {
		struct var_desc * v = node->ins->keys[q];
		if (v) {
			if (v->flags & VF_UNDEFINED) {
				/* FIXME: not sure if it's ok for int operations,
				 * but probably it is if it doesn't lock up the gpu */
				node->alu->src[q].sel = V_SQ_ALU_SRC_0;
				node->alu->src[q].chan = 0;
			} else {
				unsigned rc = get_var_alloc(info, v);
				assert(rc);
				node->alu->src[q].sel = KEY_REG(rc);
				node->alu->src[q].chan = KEY_CHAN(rc);
			}
		}
	}

	struct var_desc * v = node->outs->keys[0];
	if (v && !(v->flags & VF_DEAD)) {
		unsigned rc = get_var_alloc(info, v);
		assert(rc);
		node->alu->dst.sel = KEY_REG(rc);
		node->alu->dst.chan = KEY_CHAN(rc);
		node->alu->dst.write = 1;
	} else {
		node->alu->dst.write = 0;
		node->alu->dst.sel = 0;
	}
}

static void fix_alu_replicate_regs(struct shader_info * info, struct ast_node * group)
{
	struct ast_node * l = group->child;
	struct var_desc * outs[4] = {};
	int q = 0;

	while (l) {
		struct var_desc * o = l->child->outs->keys[0];
		if (o) {
			int chan = KEY_CHAN(o->color);
			assert(q<4);
			assert(chan>=0 && chan<4);
			outs[chan] = o;
		}
		l = l->rest;
		q++;
	}

	l = group->child;
	q = 0;

	while (l) {
		l->child->outs->keys[0] = outs[q++];
		l = l->rest;
	}
}


static void set_regs(struct shader_info * info, struct ast_node * node)
{
	if (node->tex) {
		if (node->ins) {
			int q, gpr = -1;

			for (q=0; q<4; q++) {
				struct var_desc * v = node->ins->keys[q];
				unsigned ssel;
				if (v && !(v->flags & VF_DEAD)) {
					unsigned rc = get_var_alloc(info, v);

					if (gpr==-1)
						gpr = KEY_REG(rc);
					else if (gpr + (q/4) != KEY_REG(rc)) {
						R600_DUMP( "set_reg: tex ins: vars in different gprs\n");
						assert(0);
					}
					ssel = KEY_CHAN(rc);
				} else
					continue;

				switch (q) {
				case 0: node->tex->src_sel_x = ssel; break;
				case 1: node->tex->src_sel_y = ssel; break;
				case 2: node->tex->src_sel_z = ssel; break;
				case 3: node->tex->src_sel_w = ssel; break;
				}
			}
			node->tex->src_gpr = gpr >= 0 ? gpr : 0;
		}

		if (node->outs) {
			int q, gpr = -1;

			node->tex->dst_sel_x = 7;
			node->tex->dst_sel_y = 7;
			node->tex->dst_sel_z = 7;
			node->tex->dst_sel_w = 7;

			for (q=0; q<4; q++) {
				struct var_desc * v = node->outs->keys[q];
				unsigned dsel;

				if (v && !(v->flags & VF_DEAD)) {
					unsigned rc = get_var_alloc(info, v);

					if (gpr==-1)
						gpr = KEY_REG(rc);
					else if (gpr + (q/4) != KEY_REG(rc)) {
						R600_DUMP( "set_reg: tex outs: vars in different gprs\n");
						assert(0);
					}

					dsel = KEY_CHAN(rc);
					assert(dsel<4);

					switch (dsel) {
					case 0: node->tex->dst_sel_x = q; break;
					case 1: node->tex->dst_sel_y = q; break;
					case 2: node->tex->dst_sel_z = q; break;
					case 3: node->tex->dst_sel_w = q; break;
					}
				}
			}

			node->tex->dst_gpr = gpr >= 0 ? gpr : 0;
		}
	} else if (node->vtx) {
		if (node->ins) {
			int q, gpr = -1;

			for (q=0; q<node->ins->count; q++) {
				struct var_desc * v = node->ins->keys[q];
				unsigned ssel = 0;

				assert(!v || !(v->flags & VF_SPECIAL));

				if (v && !(v->flags & VF_DEAD)) {
					unsigned rc = get_var_alloc(info, v);
					assert(rc);

					if (gpr==-1)
						gpr = KEY_REG(rc);
					else if (gpr + (q/4) != KEY_REG(rc)) {
						R600_DUMP( "set_reg: vtx ins: vars in different gprs\n");
						assert(0);
					}
					ssel = KEY_CHAN(rc);
				} else
					assert(0);

				switch (q) {
				case 0: node->vtx->src_sel_x = ssel; break;
				default: assert(0);
				}

			}

			node->vtx->src_gpr = gpr;
		}
		if (node->outs) {
			int q, gpr = -1;

			node->vtx->dst_sel_x = 7;
			node->vtx->dst_sel_y = 7;
			node->vtx->dst_sel_z = 7;
			node->vtx->dst_sel_w = 7;

			for (q=0; q<node->outs->count; q++) {
				struct var_desc * v = node->outs->keys[q];
				unsigned dsel;

				if (v && !(v->flags & VF_DEAD)) {
					unsigned rc = get_var_alloc(info, v);

					assert(rc);

					if (gpr==-1)
						gpr = KEY_REG(rc);
					else if (gpr + (q/4) != KEY_REG(rc)) {
						R600_DUMP( "set_reg: vtx outs: vars in different gprs\n");
						assert(0);

					}

					dsel = KEY_CHAN(rc);
					assert(dsel<4);
					switch (dsel) {
					case 0: node->vtx->dst_sel_x = q; break;
					case 1: node->vtx->dst_sel_y = q; break;
					case 2: node->vtx->dst_sel_z = q; break;
					case 3: node->vtx->dst_sel_w = q; break;
					}
				}
			}

			node->vtx->dst_gpr = gpr >= 0 ? gpr : 0;
		}
	}
}

static void fix_loop_ops(struct shader_info * info, struct ast_node * node)
{
	if (node->subtype == NST_LOOP_BREAK || node->subtype == NST_LOOP_CONTINUE) {
		struct ast_node * p = node->parent;

		while (p && p->subtype!=NST_LOOP_REGION)
			p = p->parent;

		assert(p);
		node->new_cf->cf_addr = p->new_cf->id;

	}

	if (node->child)
		fix_loop_ops(info, node->child);
	if (node->rest)
		fix_loop_ops(info, node->rest);
}

static void destroy_constraint(struct rc_constraint * c, boolean bs)
{
	if (c) {
		int w;
		for (w=0; w<c->comps->count; w++) {
			struct var_desc * vv = c->comps->keys[w];
			if (vv) {
				if (bs)
					vv->bs_constraint = NULL;
				else
					vv->constraint = NULL;
			}
		}
		vvec_destroy(c->comps);
		free(c);
	}
}


static void destroy_shader_info(struct shader_info * info)
{
	int q;

	destroy_ast(info->root);

	for (q=0; q<info->vars->count; q++) {
		struct var_desc * v = info->vars->keys[2*q+1];

		destroy_constraint(v->constraint, false);
		destroy_constraint(v->bs_constraint, true);
		destroy_var(v);
	}

	vmap_destroy(info->vars);

	if (info->chunk_queue) {
		struct affinity_chunk * c;

		vque_destroy(info->chunk_groups);

		while (VQUE_DEQUEUE(info->chunk_queue,&c)) {

			struct chunk_group * g = c->group;
			if (g) {
				for (q=0; q<g->chunks->count; q++) {
					struct affinity_chunk * c2 = g->chunks->keys[q];
					c2->group = NULL;
				}
				vvec_destroy(g->chunks);
				free(g);
			}

			vset_destroy(c->vars);
			free(c);
		}

		vque_destroy(info->chunk_queue);

		struct affinity_edge * e;
		while (VQUE_DEQUEUE(info->edge_queue, &e))
			free(e);

		vque_destroy(info->edge_queue);
	}
}

/* we need to clear interference data if need to recalculate liveness information */
static void reset_interferences(struct shader_info * info)
{
	int q;
	for (q=0; q<info->vars->count; q++) {
		struct var_desc * v = info->vars->keys[q*2+1];

		if (v->interferences)
			vset_clear(v->interferences);
	}
}

static void push_stack(struct shader_info * info)
{
	if (++info->stack_level > info->bc->nstack)
		info->bc->nstack = info->stack_level;
}

static void pop_stack(struct shader_info * info)
{
	if (--info->stack_level<0)
		assert(0);
}

static void build_shader_node(struct shader_info * info, struct ast_node * node);

static void emit_if_else(struct shader_info * info, struct ast_node * node)
{
	struct ast_node * depart = node->child->child;
	struct ast_node * n_if;
	struct ast_node * n_else;
	struct r600_bytecode_cf * cf_if_jump, * cf_if_else, * cf_if_pop;

	if (!depart || !depart->child)
		return;

	n_if = depart->child->child->child;
	n_else = depart->child->rest;

	r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP);
	cf_if_jump = info->bc->cf_last;

	push_stack(info);
	build_shader_node(info, n_if);

	r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_ELSE);
	cf_if_else = info->bc->cf_last;

	build_shader_node(info, n_else);
	pop_stack(info);

	r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_POP);
	cf_if_pop = info->bc->cf_last;

	cf_if_jump->cf_addr = cf_if_else->id;
	cf_if_else->cf_addr = cf_if_pop->id + 2;
	cf_if_pop->cf_addr = cf_if_pop->id + 2;

	cf_if_else->pop_count = 1;
	cf_if_pop->pop_count = 1;
}


static void build_cf_node(struct shader_info * info, struct ast_node * node)
{
	if (node->op_class == NOC_CF_EXPORT) {

		int q, w, gpr, burst_count = node->ins->count >> 2;
		struct r600_bytecode_output out = node->cf->output;

		out.end_of_program = 0;
		out.inst = EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT;
		out.burst_count = 1;

		for (q=0; q<burst_count; q++) {
			gpr = -1;

			for (w=0; w<4; w++) {
				struct var_desc * v = node->ins->keys[q*4+w];
				if (v) {
					if (!(v->flags & VF_UNDEFINED))	{
						unsigned rc = get_var_alloc(info, v);
						assert(rc);

						if (gpr==-1)
							gpr = KEY_REG(rc);
						else if (gpr != KEY_REG(rc)) {
							R600_DUMP( "export: vars in different gprs\n");
							assert(0);
						}

						*get_output_swizzle_ptr(&out, w) = KEY_CHAN(rc);
					} else
						*get_output_swizzle_ptr(&out, w) = 4;
				} else
					*get_output_swizzle_ptr(&out, w) = *get_output_swizzle_ptr(&node->cf->output, w);
			}
			if (q == burst_count-1)
				out.inst = node->cf->output.inst;

			out.gpr = (gpr == -1) ? 0 : gpr;;
			r600_bytecode_add_output(info->bc,&out);

			info->last_export[out.type] = info->bc->cf_last;

			out.array_base++;
			if (gpr>=0)
				r600_bytecode_count_gprs(info->bc, gpr, 0);
		}
	} else if (node->op_class == NOC_CF_STREAMOUT) {
		int q, w, gpr, burst_count = node->ins->count >> 2;
		struct r600_bytecode_output out = node->cf->output;

		out.end_of_program = 0;
		out.burst_count = 1;
		out.comp_mask = 0;

		for (q=0; q<burst_count; q++) {
			gpr = -1;

			for (w=0; w<4; w++) {
				struct var_desc * v = node->ins->keys[q*4+w];
				int comp_write = 0;
				if (v) {
//					if (!(v->flags & VF_UNDEFINED))	{
						unsigned rc = get_var_alloc(info, v);
						assert(rc);

						if (gpr==-1)
							gpr = KEY_REG(rc);
						else if (gpr != KEY_REG(rc)) {
							R600_DUMP( "mem_stream: vars in different gprs\n");
							assert(0);
						}

						if (KEY_CHAN(rc)!=w) {
							dump_node(info, node, 0);
							R600_DUMP( "mem_stream: channel constraint broken\n");
							assert(0);
						}

						comp_write = 1;
//					}
				}

				out.comp_mask |= (comp_write<<w);
			}

			out.gpr = (gpr == -1) ? 0 : gpr;;
			r600_bytecode_add_output(info->bc,&out);

			out.array_base++;
			if (gpr>=0)
				r600_bytecode_count_gprs(info->bc, gpr, 0);
		}
	} else if (node->cf->inst == EG_V_SQ_CF_WORD1_SQ_CF_INST_CALL_FS)
			r600_bytecode_add_cfinst(info->bc,node->cf->inst);
/*	else
		assert(!"unknown cf instruction");
*/
}

static void build_shader_node(struct shader_info * info, struct ast_node * node)
{
#ifdef DEBUG
	static int iii = 0;
#endif

	if (!node || (node->flags & AF_DEAD))
		return;

	if (node->subtype == NST_ALU_GROUP) {
		if (is_alu_replicate_inst(&info->shader->bc,node->child->child->alu))
			fix_alu_replicate_regs(info, node);
	} else if (node->alu) {
		int r;
		unsigned alu_type;
		struct r600_bytecode_alu * p;

		node->alu->dst.clamp = (node->flags & AF_ALU_CLAMP_DST) ? 1 : 0;
		set_alu_regs(info, node);

		R600_DUMP("building alu: %d ", ++iii);
		dump_node(info,node,0);

		if (info->bc->cf_last && !LIST_IS_EMPTY(&info->bc->cf_last->alu))
			alu_type = info->bc->cf_last->inst;
		else
			alu_type = EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU;

		if (node->alu->predicate)
			alu_type =  EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU_PUSH_BEFORE;

		r = r600_bytecode_add_alu_type(info->bc, node->alu, alu_type);
		p = LIST_ENTRY(struct r600_bytecode_alu, info->bc->cf_last->alu.prev, list);

		R600_DUMP_CALL(dump_alu(info, 0, p));

		if (r) {
			R600_DUMP("################### building alu failed:\n");
			dump_node(info,node,0);
			R600_DUMP("###################\n");
			assert(0);
		}

		if ((node->flags & AF_ALU_CLAUSE_SPLIT) || is_alu_kill_inst(info->bc, node->alu))
			info->force_cf = true;

		if (info->force_cf && node->alu->last) {
			info->bc->force_add_cf = 1;
			info->force_cf = false;
		}

		if (p->last)
			info->stats[1].nalugroups++;

	} else if (node->tex) {
		set_regs(info, node);
		r600_bytecode_add_tex(info->bc, node->tex);
	} else if (node->vtx) {
		set_regs(info, node);
		r600_bytecode_add_vtx(info->bc, node->vtx);
	}

	if (node->subtype == NST_LOOP_REGION) {
		struct r600_bytecode_cf * cfl_start, * cfl_end;

		r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_START_NO_AL);
		cfl_start = info->bc->cf_last;

		push_stack(info);

		node->stack_level = info->stack_level;

		if (node->child)
			build_shader_node(info, node->child);

		pop_stack(info);

		r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_END);
		cfl_end = info->bc->cf_last;

		cfl_end->cf_addr = cfl_start->id + 2;
		cfl_start->cf_addr = cfl_end->id + 2;

		node->new_cf = cfl_end;

		fix_loop_ops(info, node->child);

		return;

	} else if (node->subtype == NST_LOOP_BREAK) {

		if (node->child)
			build_shader_node(info, node->child);

		r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_BREAK);
		node->new_cf = info->bc->cf_last;
		node->stack_level = info->stack_level;

		return;

	} else if (node->subtype == NST_LOOP_CONTINUE) {

		if (node->child)
			build_shader_node(info, node->child);

		r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_LOOP_CONTINUE);
		node->new_cf = info->bc->cf_last;
		node->stack_level = info->stack_level;
		return;

	} else if (node->subtype == NST_IF_ELSE_REGION) {
		emit_if_else(info, node);
		return;
	} else if (node->type == NT_IF) {
		struct r600_bytecode_cf * cf_if_jump, * cf_if_pop;

		r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_JUMP);
		cf_if_jump = info->bc->cf_last;

		push_stack(info);
		build_shader_node(info, node->child);
		pop_stack(info);

		r600_bytecode_add_cfinst(info->bc, EG_V_SQ_CF_WORD1_SQ_CF_INST_POP);
		cf_if_pop = info->bc->cf_last;

		cf_if_jump->cf_addr = cf_if_pop->id + 2;
		cf_if_pop->cf_addr = cf_if_pop->id + 2;

		cf_if_jump->pop_count = 1;
		cf_if_pop->pop_count = 1;
		return;

	} else if (node->cf) {

		build_cf_node(info, node);

	}

	if (node->child)
		build_shader_node(info, node->child);

	if (node->rest)
		build_shader_node(info, node->rest);
}

static int build_shader(struct shader_info * info)
{
	int r, q;

	info->bc = calloc(1, sizeof(struct r600_bytecode));
	r600_bytecode_init(info->bc, info->rctx->chip_class, info->rctx->family);
	info->bc->type = info->shader->processor_type;

	info->bc->opt_build = 1;

	build_shader_node(info, info->root);

	for (q = 0; q<3; q++) {
		if (info->last_export[q]) {
			info->last_export[q]->output.inst = EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE;
			info->last_export[q]->inst = EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE;
		}
	}


	info->bc->cf_last->output.end_of_program = 1;

	r = r600_bytecode_build(info->bc);
	if (r) {
		R600_ERR("building optimized bytecode failed !\n");
		return r;
	}

	info->result_code = SR_SUCCESS;

	/* replace original bytecode with optimized */

	info->bc->cf.next->prev = &info->shader->bc.cf;
	info->bc->cf.prev->next = &info->shader->bc.cf;
	r600_bytecode_clear(&info->shader->bc);
	memcpy(&info->shader->bc, info->bc, sizeof(struct r600_bytecode));
	free(info->bc);
	return 0;
}

static float clamp(float f)
{
	return f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
}

static float get_const_val(struct r600_bytecode_alu_src * s)
{
	const uint32_t db = 0xdeadbeef;
	float val;

	switch (s->sel) {
	case V_SQ_ALU_SRC_LITERAL:
		val = *(float*)&s->value;
		break;
	case V_SQ_ALU_SRC_0:
		val = 0.0f;
		break;
	case V_SQ_ALU_SRC_0_5:
		val = 0.5f;
		break;
	case V_SQ_ALU_SRC_1:
		val = 1.0f;
		break;
	default:
		val = *(float*)&db;
	}

	if (s->abs)
		val = val<0? -val : val;

	if (s->neg)
		val = -val;

	return val;
}

static void get_group_instructions(struct ast_node * n, struct ast_node *  gg[4])
{
	struct ast_node * p;
	int q = 0;

	assert(n->flags & AF_FOUR_SLOTS_INST);

	p = n->parent->parent;

	while (p->subtype != NST_ALU_GROUP)
		p = p->parent;

	p = p->child;

	while (p) {
		gg[q++] = p->child;
		p = p->rest;
	}
}

static boolean propagate_copy_input(struct shader_info * info, struct ast_node * node,
		int index, struct ast_node * m)
{


	if (node->subtype == NST_ALU_INST || node->subtype == NST_COPY) {
		struct r600_bytecode_alu_src * d = &node->alu->src[index], *src = &m->alu->src[0];
		unsigned nneg = src->neg, nabs = src->abs;
		struct var_desc * sv = m->ins->keys[0];

/*		R600_DUMP("prop copy %d ", index);
		if (sv)
			print_var(sv);
		dump_node(info, node, 0);
*/
		if (!sv && src->sel>=BC_KCACHE_OFFSET && node->const_ins_count == 2) {
			int t, k=0;

			for (t=0; t<node->ins->count; t++) {
				if (t==index)
					continue;

				if (node->alu->src[t].sel>=BC_KCACHE_OFFSET)
					if (++k == 2)
						return false;
			}
		}

		if (d->abs) {
			nneg = 0;
			nabs = 1;
		}

		if (d->neg)
			nneg = !nneg;

		if (nabs && node->alu->is_op3)
			return false;

		if (sv) {
			if ((m->flags & AF_ALU_CLAMP_DST) || (node->flags & AF_FOUR_SLOTS_INST))
				return false;

			node->ins->keys[index] = sv;
			add_use(sv, node);
		} else {

			d->value = src->value;

			if (m->flags & AF_ALU_CLAMP_DST) {
				if (src->sel>=BC_KCACHE_OFFSET)
					return false;
				else if (src->sel == V_SQ_ALU_SRC_LITERAL) {
					d->value = clamp(d->value);
				}
			}

			if (node->flags & AF_FOUR_SLOTS_INST && src->sel>=BC_KCACHE_OFFSET) {
				struct ast_node * g[4];
				struct vset * csel = vset_create(3, 0);
				int w,e;

				VSET_ADD(csel, CPAIR_KEY(src->sel,src->chan));

				get_group_instructions(node, g);

				for (w=0; w<4; w++) {
					struct ast_node * r = g[w];
					for (e=0; e<r->ins->count; e++) {
						if (r->alu->src[e].sel>=BC_KCACHE_OFFSET) {
							VSET_ADD(csel, CPAIR_KEY(r->alu->src[e].sel,r->alu->src[e].chan));

							if (csel->count>2) {
								vset_destroy(csel);
								return false;
							}
						}
					}
				}

				vset_destroy(csel);
			}


			d->sel = src->sel;
			d->chan = (src->sel>=BC_KCACHE_OFFSET) ? src->chan : 0;
			node->ins->keys[index] = NULL;
			node->const_ins_count++;
		}

                d->neg = nneg;
                d->abs = nabs;

		return true;
	} else if (node->op_class == NOC_CF_EXPORT && node->cf->output.burst_count==1) {
		struct r600_bytecode_alu_src * src = &m->alu->src[0];

		if (src->sel >= V_SQ_ALU_SRC_0 && src->sel <= V_SQ_ALU_SRC_LITERAL) {
			union fui val, zero, one;
			val.f = get_const_val(src);
			zero.f = 0.0f;
			one.f = 1.0f;

			if (val.u == zero.u) {
				node->ins->keys[index] = NULL;
				*get_output_swizzle_ptr(&node->cf->output, index) = 4;
			} else if (val.u == one.u) {
				node->ins->keys[index] = NULL;
				*get_output_swizzle_ptr(&node->cf->output, index) = 5;
			}
		}

		return node->ins->keys[index] == NULL;

	} else if (node->subtype == NST_TEX_INST) {
		struct r600_bytecode_alu_src * src = &m->alu->src[0];

		if (src->sel >= V_SQ_ALU_SRC_0 && src->sel <= V_SQ_ALU_SRC_LITERAL) {
			union fui val, zero, one;
			val.f = get_const_val(src);
			zero.f = 0.0f;
			one.f = 1.0f;

			if (val.u == zero.u) {
				node->ins->keys[index] = NULL;

				switch(index) {
				case 0: node->tex->src_sel_x = 4; break;
				case 1: node->tex->src_sel_y = 4; break;
				case 2: node->tex->src_sel_z = 4; break;
				case 3: node->tex->src_sel_w = 4; break;
				}
			} else if (val.u == one.u) {
				node->ins->keys[index] = NULL;
				switch(index) {
				case 0: node->tex->src_sel_x = 5; break;
				case 1: node->tex->src_sel_y = 5; break;
				case 2: node->tex->src_sel_z = 5; break;
				case 3: node->tex->src_sel_w = 5; break;
				}
			}
		}

		return node->ins->keys[index] == NULL;
	}

	return false;
}


static void propagate_copy_node(struct shader_info * info, struct ast_node * node)
{
	if (node->ins) {
		int q;

		if (node->flags & AF_SPLIT_COPY)
			return;

		for (q=0; q<node->ins->count; q++) {
			struct var_desc * v = node->ins->keys[q], *vv = v;

			if (!v || (v->flags & VF_DEAD))
				continue;

			/* jump through the split copy nodes */
			while (vv->def && (vv->def->flags & AF_SPLIT_COPY))
				vv = vv->def->ins->keys[0];

			if (vv->def && vv->def->alu && !vv->def->alu->is_op3 &&
					vv->def->alu->inst == EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV &&
					!(vv->def->flags & AF_SPLIT_COPY)) {

				if (propagate_copy_input(info, node, q, vv->def))
					vset_remove(vv->uses, node);
			}
		}
	}

	if (node->child)
		propagate_copy_node(info, node->child);

	if (node->rest)
		propagate_copy_node(info, node->rest);

}

static void propagate_copy(struct shader_info * info)
{
	propagate_copy_node(info, info->root);
}

static boolean insert_copies_phi(struct shader_info * info,
		struct ast_node * node)
{
	int q;
	boolean r = true;
	struct var_desc * o = node->outs->keys[0];

	if (!o || (o->flags & VF_DEAD))
		return r;

	for (q=0; q<node->ins->count; q++) {
		struct var_desc * i = node->ins->keys[q];

		if (o->color != i->color) {
			r = false;
			R600_DUMP("uncoalesced phi: ");
			print_var(o);
			R600_DUMP(" <= ");
			print_var(i);
			R600_DUMP("\n");
		}
	}

	return r;
}

static boolean insert_copies_node(struct shader_info * info,
		struct ast_node * node)
{
	boolean r = true;

	if (node->flags & AF_DEAD)
		return true;

/*	if (node->loop_phi)
		r &= insert_copies_phi(info, node, false);
	if (node->phi)
		r &= insert_copies_phi(info, node, true);
*/

	if (node->child)
		r &= insert_copies_node(info, node->child);
	if (node->rest)
		r &= insert_copies_node(info, node->rest);

	return r;
}

/* we need to insert the copies when some vars in the live intervals split
 * nodes or phi nodes weren't coalesced */
static boolean insert_copies(struct shader_info * info)
{
	return insert_copies_node(info, info->root);
}

static boolean create_shader_tree(struct shader_info * info)
{
	info->vars = vmap_create(32);
	info->root = create_node(NT_LIST);
	info->root->subtype = NST_ROOT;

	info->root->child = create_node(NT_LIST);

	/* parse source r600_bytecode */

	if (!parse_shader(info, info->root->child)) {
		return false;
	}

	convert_cf(info, info->root);

	info->edge_queue = vque_create(32);

	/* construct SSA form */

	build_ssa(info);
	liveness(info);

	propagate_copy(info);
	reset_interferences(info);
	liveness(info);

	analyze_vars(info);

	/* global scheduling (initial support - fetch combining etc) */

	gs_schedule(info);


	reset_interferences(info);
	liveness(info);

	/* initial register allocation */

	color(info);
	coalesce(info);

	dump_shader_tree(info);
	dump_var_table(info);

	/* check for not coalesced phi and psplit (parallel copy for live
	 * intervals splitting) vars, and insert copies if needed */
	if (!insert_copies(info)) {
		info->result_code = SR_FAIL_INSERT_COPIES;
		return false;
	}

	info->liveness_correct = false;

	/* alu scheduling */

	if (!post_schedule(info)) {
		info->result_code = SR_FAIL_SCHEDULE;
		return false;
	}

	info->built = true;

	dump_shader_tree(info);

	return true;
}


static void shader_stats_print_diff(struct shader_stats * s0, struct shader_stats * s1)
{
	printf("size %+.1f%% ( %u -> %u dw),    gpr %+.1f%% ( %u -> %u ),   alu_groups %+.1f%% ( %u -> %u )\n",
			(-1.0f + ((float)s1->ndw)/s0->ndw)*100, s0->ndw, s1->ndw,
			(-1.0f + ((float)s1->ngpr)/s0->ngpr)*100, s0->ngpr, s1->ngpr,
			(-1.0f + ((float)s1->nalugroups)/s0->nalugroups)*100, s0->nalugroups, s1->nalugroups
	);
}

static void shader_stats_accumulate(struct shader_stats * s0, struct shader_stats * s1)
{
	s0->ndw+=s1->ndw;
	s0->ngpr+=s1->ngpr;
	s0->nalugroups+=s1->nalugroups;
}

int r600_shader_optimize(struct r600_context * rctx, struct r600_pipe_shader *pipeshader, int dump)
{
	boolean print_info;
	static int dump_global_stats = -1;
	static int shaders_opt = -1;
	static struct shader_stats global_stats[2] = {};
	static int shader_num = 0;
	int r = 0;
	struct shader_info info = {};

	if (dump_global_stats == -1)
		dump_global_stats = debug_get_bool_option("R600_OPT_DUMP_STATS", false);

	if (shaders_opt == -1)
		shaders_opt = debug_get_bool_option("R600_SHADERS_OPT", true);

	// only EVERGREEN is supported currently
	if (rctx->chip_class != EVERGREEN) {
		R600_ERR("shader optimization: unsupported chip\n");
		return -1;
	}

	++shader_num;

	get_dump_level();
	print_info = check_dump_level(R600_OPT_DUMP_LEVEL_INFO);

#ifdef DEBUG

	/* with debug build: skip optimization for selected shaders
	 *  skip_mode:
	 *  	0 - disable,
	 *  	1 - skip opt for skip_cnt shaders starting from skip_num (1-based),
	 *  	2 - inverted 1 (skip for all others)
	 */
	static int skip_mode = 0;
	static int skip_num = 5;
	static int skip_cnt = 1;

	if (skip_mode) {
		if (skip_num==-1)
			skip_num = debug_get_num_option("R600_OPT_SKIP",0);

		if ((skip_mode==1) == ((shader_num >= skip_num) && (shader_num < (skip_num + skip_cnt)))) {
			printf("skipping shader %d\n", shader_num);
			return -1;
		}
	}
#endif

	info.rctx = rctx;

	info.temp_gprs = 4;
	info.next_temp = 1;
	info.dump = dump;

	info.shader_index = shader_num;
	info.shader = &pipeshader->shader;

	if (!shaders_opt) {
		dump_bytecode(&info);
		return 0;
	}

	if (info.shader->ninput)
		info.last_input_gpr = info.shader->input[info.shader->ninput-1].gpr;
	else
		info.last_input_gpr = -1;

	info.enable_last_color_update = true;

	if (print_info) {
		info.stats[0].ngpr = info.shader->bc.ngpr;
		info.stats[0].ndw = info.shader->bc.ndw;
		dump_bytecode(&info);
		printf("optimizing shader %d\n", shader_num);
	}

	info.max_slots = info.shader->bc.chip_class == CAYMAN ? 4 : 5;

	if (create_shader_tree(&info))
		r = build_shader(&info);
	else
		r = -1;

	destroy_shader_info(&info);

	if (print_info) {
		if (r) {
			printf("WARNING: optimization failed : ");
			switch (info.result_code) {
			case SR_FAIL_SCHEDULE:
				printf("scheduler failure\n");
				assert(0);
				break;
			case SR_FAIL_INSERT_COPIES:
				printf("need insert_copies pass (not implemented yet)\n");
				break;
			case SR_SKIP_RELADDR:
				printf("relative addressing\n");
				break;
			default:
				printf("unknown error\n");
				assert(0);
			}
		} else {
			assert(info.result_code == SR_SUCCESS);

			info.stats[1].ngpr = info.shader->bc.ngpr;
			info.stats[1].ndw = info.shader->bc.ndw;

			printf("INFO: shader optimized : ");
			shader_stats_print_diff(&info.stats[0],&info.stats[1]);

			if (dump_global_stats) {
				shader_stats_accumulate(&global_stats[0],&info.stats[0]);
				shader_stats_accumulate(&global_stats[1],&info.stats[1]);
				printf("INFO: global stats : ");
				shader_stats_print_diff(&global_stats[0],&global_stats[1]);
			}

			if (check_dump_level(R600_OPT_DUMP_LEVEL_SHADERS)) {
				fprintf(stderr,"optimized ");
				dump_bytecode(&info);
			}
		}
	}

	return r;
}
