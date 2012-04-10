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

#include "opt_core.h"

//#define CLR_DUMP

static void update_last_color(struct shader_info * info, int color)
{
	if (info->enable_last_color_update && color>info->last_color)
		info->last_color = color;
}

static boolean value_equal_cb(struct var_desc * v1, struct var_desc * v2)
{
	assert(v1 != NULL && v2 != NULL);

	if (!(v1==v2 || v1->value_hint == v2 || v1 == v2->value_hint || (v1->value_hint == v2->value_hint && v1->value_hint!=NULL)))
		return false;

	return true;
}

static boolean value_equal(struct var_desc * v1, struct var_desc * v2)
{
	assert(v1 != NULL && v2 != NULL);

	if ((v1->fixed || v2->fixed) && v1->chunk != v2->chunk)
		return false;

	if (!(v1==v2 || v1->value_hint == v2 || v1 == v2->value_hint || (v1->value_hint == v2->value_hint && v1->value_hint!=NULL)))
		return false;

	return true;
}

static boolean interference(struct var_desc * v1, struct var_desc * v2)
{
	boolean result = !value_equal_cb(v1, v2) &&
			(v1->interferences && vset_contains(v1->interferences,v2));

	if (result) {
		R600_DUMP("intf_cb: ");
		print_var(v1);
		print_var(v2);
		R600_DUMP("\n");
	}

	return result;
}

static int vec_vars_count(struct vvec * vv)
{
	int q, r=0;
	for (q=0;q<vv->count;q++) {
		struct var_desc * v = vv->keys[q];
		if (v && !(v->flags & VF_DEAD))
			r++;
	}
	return r;
}

static void add_var_edge(struct var_desc * v, struct affinity_edge * e)
{
	if (!v->aff_edges)
		v->aff_edges = vset_create(1);

	vset_add(v->aff_edges, e);
}


void add_affinity_edge(struct shader_info * info, struct var_desc * v1, struct var_desc * v2, int cost)
{
	struct affinity_edge * e;

	if (v1 == NULL || v2 == NULL || (v1->flags & VF_DEAD) /*|| (v2->flags & VF_DEAD)*/)
		return;

	e = calloc(1, sizeof(struct affinity_edge));
	e->cost = cost;

	if (v1->constraint)
		e->cost += AE_CONSTRAINT_COST * vec_vars_count(v1->constraint->comps);

	if (v2->constraint)
		e->cost += AE_CONSTRAINT_COST * vec_vars_count(v2->constraint->comps);

	if (v1->fixed)
		e->cost += AE_INPUT_COST;

	if (v2->fixed)
		e->cost += AE_INPUT_COST;

	e->v = v1;
	e->v2 = v2;
	vque_enqueue(info->edge_queue, e->cost, e);

	add_var_edge(v1, e);
	add_var_edge(v2, e);
}

static void build_a_edges_phi_copy(struct shader_info * info, struct ast_node * node, boolean alu_split)
{
	boolean phi = (node->subtype == NST_PHI);
	int q;

	if (node->flags & AF_DEAD)
		return;

	for (q=0; q<node->ins->count; q++) {
		struct var_desc * v = node->ins->keys[q];

		if (v && !(v->flags & VF_DEAD)) {

			if (v->flags & VF_SPECIAL)
				break;

			if (phi)
				add_affinity_edge(info, v, node->outs->keys[0], AE_PHI_COST);
			else if (alu_split)
				add_affinity_edge(info, v, node->outs->keys[q], AE_CSPLIT_COST);
			else
				add_affinity_edge(info, v, node->outs->keys[q], AE_SPLIT_COST);
		}
	}
}

static void build_affinity_edges(struct shader_info * info, struct ast_node * node)
{
	boolean alu_gr = node->subtype == NST_ALU_GROUP;

	if (node->flags & AF_DEAD)
		return;

	if (node->child)
		build_affinity_edges(info, node->child);

	if (node->rest)
		build_affinity_edges(info, node->rest);

	if (node->phi)
		build_affinity_edges(info, node->phi);

	if (node->loop_phi)
		build_affinity_edges(info, node->loop_phi);

	if (node->p_split)
		build_a_edges_phi_copy(info, node->p_split, alu_gr);

	if (node->p_split_outs)
		build_a_edges_phi_copy(info, node->p_split_outs, alu_gr);

	if (node->subtype == NST_PHI)
		build_a_edges_phi_copy(info, node, false);

}
static boolean chunks_vars_interference(struct affinity_chunk * c1, struct affinity_chunk * c2)
{
	int q,w;

	for (q=0; q<c1->vars->count; q++) {
		struct var_desc *v1 = c1->vars->keys[q];
		for (w=0; w<c2->vars->count; w++) {
			struct var_desc  *v2 = c2->vars->keys[w];
			if (interference(v1,v2))
				return true;
		}
	}

	return false;
}

/* checks if two chunk groups are compatible (that is, can be unified and coalesced) */
static boolean chunk_sets_mappable(struct vset * s1, struct vset * s2, int ncomp)
{
	int q, w;
	int max_v = 0;
	int max_mask = 0;
	int max_q = -1;

	assert (s1->count <= ncomp && s2->count <= ncomp);

	if (s1->count + s2->count <= ncomp)
		return true;

	if (s1->count > s2->count) {
		struct vset *t = s1;
		s1 = s2;
		s2 = t;
	}

	for (q=0; q<s1->count; q++) {
		struct affinity_chunk * c = s1->keys[q];

		if (vset_contains(s2, c)) {
			vset_remove(s1, c);
			vset_remove(s2, c);
			q--;
			ncomp--;
		}
	}

	if (s1->count == 0)
		return true;

	for (q=0; q<s1->count; q++) {
		struct affinity_chunk * c = s1->keys[q];

		int mv=0;
		int mask = 0;

		for (w=0; w<s2->count; w++) {
			struct affinity_chunk * c2 = s2->keys[w];
			if (chunks_vars_interference(c,c2)) {
				mask |= 1<<w;
				mv++;
			}
		}

		if (mv == 0) {
			vset_remove(s1, c);
			q--;
		} else if (mv == s2->count) {
			if (ncomp > s2->count) {
				vset_remove(s1, c);
				q--;
				ncomp--;
			} else
				return false;
		} else if (mv > max_v) {
			max_v = mv;
			max_mask = mask;
			max_q = q;
		}
	}

	if (max_v) {
		struct affinity_chunk * c = s1->keys[max_q];

		for (q=0; q<s2->count; q++) {
			if (!(max_mask & (1<<q))) {
				struct affinity_chunk * c2 = s2->keys[q];
				struct vset * sn1, * sn2;
				boolean r;

				/* assume that we've mapped c <-> c2, check remaining recursively
				 */

				if (s1->count + s2->count -1 <= ncomp)
					return true;

				/* FIXME: inefficient, but this path is expected to be hit really very rarely
				 */

				sn1 = vset_createcopy(s1);
				sn2 = vset_createcopy(s2);
				vset_remove(sn1, c);
				vset_remove(sn2, c2);

				r = chunk_sets_mappable(sn1,sn2, ncomp-1);

				vset_destroy(sn1);
				vset_destroy(sn2);

				if (r)
					return true;
			}
		}
	}
	return true;
}

static boolean constraints_compatible(struct var_desc * v1, struct var_desc * v2)
{
	int q;
	struct vset * s1 = vset_create(4);
	struct vset * s2 = vset_create(4);
	boolean result = false;

	for (q=0; q<v1->constraint->comps->count; q++) {
		struct var_desc * v = v1->constraint->comps->keys[q];
		if (v && v->chunk && v->chunk != v1->chunk)
			vset_add(s1, v->chunk);
	}

	for (q=0; q<v2->constraint->comps->count; q++) {
		struct var_desc * v = v2->constraint->comps->keys[q];
		if (v && v->chunk && v->chunk != v2->chunk)
			vset_add(s2, v->chunk);
	}

	result = chunk_sets_mappable(s1,s2,3);

	vset_destroy(s1);
	vset_destroy(s2);
	return result;
}

static boolean chunks_interference(struct shader_info * info, struct affinity_chunk * c1, struct affinity_chunk * c2)
{
	int q, w;

	assert (c1!=c2);

	if (chunks_vars_interference(c1, c2))
		return true;

	for (q=0; q<c1->vars->count; q++) {
		struct var_desc *v1 = c1->vars->keys[q];

		if (v1->constraint) {
			for (w=0; w<c2->vars->count; w++) {
				struct var_desc  *v2 = c2->vars->keys[w];
				if (v2->constraint)
					if (!constraints_compatible(v1,v2))
						return true;
			}
		}
	}

	return false;
}

static struct affinity_chunk * create_chunk()
{
	struct affinity_chunk * c = calloc(1, sizeof(struct affinity_chunk));
	return c;
}


static struct affinity_chunk * create_var_chunk(struct var_desc *v)
{
	struct affinity_chunk * c = create_chunk();

	assert(v);

	c->cost = 0;
	c->vars = vset_create(1);
	vset_add(c->vars, v);

	return c;
}

static void delete_chunk(struct affinity_chunk * c)
{
	vset_destroy(c->vars);
	free(c);
}

static void unify_chunks(struct shader_info * info, struct affinity_edge * e)
{
	int q;
	struct affinity_chunk * c = e->v2->chunk;

	if (c!=e->v->chunk) {

		for (q=0; q<c->vars->count; q++) {
			struct var_desc * v =  c->vars->keys[q];
			v->chunk = e->v->chunk;
		}

		vset_addset(e->v->chunk->vars, c->vars);
		e->v->chunk->cost += c->cost + e->cost;
		delete_chunk(c);
	} else
		c->cost += e->cost;
}

static void build_chunks(struct shader_info * info)
{
	int q;

	for (q=info->edge_queue->count-1; q>=0; q--) {
		struct affinity_edge * e = info->edge_queue->keys[2*q+1];

		if (e->v->chunk == NULL)
			e->v->chunk = create_var_chunk(e->v);

		if (e->v2->chunk == NULL)
			e->v2->chunk = create_var_chunk(e->v2);
	}

	for (q=info->edge_queue->count-1; q>=0; q--) {
		struct affinity_edge * e = info->edge_queue->keys[2*q+1];

		if (e->v->chunk == e->v2->chunk)
			e->v->chunk->cost += e->cost;
		else if (!chunks_interference(info, e->v->chunk, e->v2->chunk))
			unify_chunks(info, e);
	}
}

static void build_chunks_queue(struct shader_info * info)
{
	int q;
	struct vset * chunks = vset_create(16);
	struct vset * group = vset_create(4);

	info->chunk_queue = vque_create(chunks->count);

	for (q=0; q<info->vars->count;q++) {
		struct var_desc * v = info->vars->keys[q*2+1];
		struct affinity_chunk * c = v->chunk;

		if (c && vset_add(chunks, c))
			vque_enqueue(info->chunk_queue, c->cost, c);
	}

	dump_chunks_queue(info);

	/* building chunk groups */

	info->chunk_groups = vque_create(chunks->count);

	for (q=info->chunk_queue->count-1; q>=0; q--) {
		struct affinity_chunk * c = info->chunk_queue->keys[2*q+1];
		struct chunk_group * g;
		int w;
		struct var_desc * max_v = NULL;
		int max_constraint_cost = -1;

		if (c->group != NULL)
			continue;

		g = calloc(1, sizeof(struct chunk_group));

		for (w=0; w<c->vars->count; w++) {
			struct var_desc * v = c->vars->keys[w];

			if (v->constraint) {
				int e;
				int constraint_cost = 0;
				int comps_count = 0;

				for (e=0; e<v->constraint->comps->count; e++) {
					struct var_desc * v2 = v->constraint->comps->keys[e];

					if (v2 && v2->chunk && v2->chunk->group == NULL) {
						constraint_cost += v2->chunk->cost;
						comps_count++;
					}
				}

				if (constraint_cost > max_constraint_cost) {
					max_constraint_cost = constraint_cost;
					max_v = v;
				}
			}
		}

		if (max_v) {
			int i = 0;

			vset_clear(group);

			for (w = 0; w<max_v->constraint->comps->count; w++) {
				struct var_desc * v2 = max_v->constraint->comps->keys[w];

				if (v2 && v2->chunk && vset_remove(chunks, v2->chunk))
					vset_add(group, v2->chunk);
			}

			g->chunks = vvec_create_clean(group->count);
			g->cost = 0;

			for (w = 0; w<max_v->constraint->comps->count; w++) {
				struct var_desc * v2 = max_v->constraint->comps->keys[w];

				if (v2 && v2->chunk && VSET_CONTAINS(group, v2->chunk)) {
					g->chunks->keys[i++] = v2->chunk;
					v2->chunk->group = g;
					g->cost += v2->chunk->cost;
					vset_remove(group, v2->chunk);
				}
			}

			assert(group->count == 0);

		} else {
			g->chunks = vvec_create(1);
			g->chunks->keys[0] = c;
			c->group = g;
			g->cost = c->cost;
			vset_remove(chunks, c);
		}

		vque_enqueue(info->chunk_groups, g->cost, g);
	}

	vset_destroy(group);
	vset_destroy(chunks);
}

static void build_affinity_chunks(struct shader_info * info)
{
	build_affinity_edges(info, info->root);

	for(int q=0;q<info->edge_queue->count; q++) {
		struct affinity_edge * e = info->edge_queue->keys[2*q+1];

		R600_DUMP("aff (%d) ", e->cost);
		print_var(e->v);
		R600_DUMP(" <=> ");
		print_var(e->v2);
		R600_DUMP("\n");
	}

	build_chunks(info);
	build_chunks_queue(info);
	dump_chunk_group_queue(info);
}

static void fix_var_color(struct var_desc * v, boolean fixed)
{
	if (v->fixed == fixed)
		return;

	if (v->constraint) {
		if (fixed) {
			if (v->constraint->fixed == 0)
				v->constraint->r_color = (v->color-1)/4 + 1;
			v->constraint->fixed++;
		}
		else
			v->constraint->fixed--;
	}

	v->fixed = fixed;
}


static void set_color(struct var_desc * v, int color, struct vset * recolored)
{
	v->saved_color = v->color;
	vset_add(recolored, v);
	v->color = color;
	fix_var_color(v, true);
}

static int get_unique_color(struct shader_info * info, struct var_desc * v)
{
	int new_color;
	int color_start=1, color_step=1;
	int last_color = (128-info->temp_gprs)*4;

	if (v->flags & VF_PIN_CHAN) {
		color_start += v->reg.chan;
		color_step = 4;
	}

	new_color = color_start;

	if (v->interferences) {
		typedef unsigned long long bst;
		const int bs = sizeof(bst)<<3;
		const int fo = __builtin_ctz(bs);
		const int block_count = 511/bs+1;
		bst avail[block_count];
		int q, block = 0, bitofs, used_count, skip_steps, shift;
		boolean found = false;

		memset(avail, 0xFF, block_count * sizeof(bst));

		for (q=0; q<v->interferences->count; q++) {
			struct var_desc *n = v->interferences->keys[q];

			if (n->color>0 && (color_step==1 || KEY_CHAN(n->color)==KEY_CHAN(color_start)) && !value_equal(v,n)) {
				int bit = n->color-1;
				avail[bit>>fo] &= ~(((bst)1)<<(bit&(bs-1)));
			}
		}

		while (!found && block < block_count && new_color<last_color) {

			bst cblock = avail[block++];
			bitofs = (new_color-1)&(bs-1);

			if (bitofs)
				cblock>>=bitofs;

			while (bitofs<bs) {

				used_count = cblock ? __builtin_ctzll(cblock) : bs-bitofs;

				if (used_count==0) {
					found = true;
					break;
				}

				if (color_step==1)
					shift = used_count;
				else {
					skip_steps = ((used_count-1)>>2) + 1;
					shift = skip_steps<<2;
				}

				new_color += shift;
				bitofs += shift;

				if (bitofs<bs)
					cblock >>= shift;
			}
		}
	}

	return new_color;
}

static int choose_color_constrained(struct shader_info * info, struct var_desc * v)
{
	struct rc_constraint * rc = v->constraint;
	int q,w, color;
	boolean used;

	for (q = 0; q<4;q++) {
		color = (rc->r_color-1)*4 + 1 + q;

		if ((v->flags & VF_PIN_CHAN) && (KEY_CHAN(color) != v->reg.chan))
			continue;

		used = false;

		for (w = 0; w<4; w++) {
			struct var_desc * c = rc->comps->keys[w];

			if (c!=NULL && c->fixed && c->color==color && !value_equal(c,v)) {
				used = true;
				color = 0;
				break;
			}
		}

		if (!used) {
			for (w=0; w<v->interferences->count; w++) {
				struct var_desc * c = v->interferences->keys[w];

				if (c->fixed && c->color == color && !value_equal(v,c)) {
#ifdef CLR_DUMP
					R600_DUMP("skip %d : fixed ",color);
					print_var(c);
#endif
					used = true;
					color = 0;
					break;
				}
			}
		}

		if (!used)
			break;
	}

#ifdef CLR_DUMP
	R600_DUMP("\tchoosing constrained color %d for ", color);
	print_var(v);
	R600_DUMP("\n");
#endif

	return color;
}

static void rollback_colors(struct vset * recolored)
{
	int q;

	for (q=0; q<recolored->count; q++) {
		struct var_desc * n = recolored->keys[q];
		n->color = n->saved_color;
		fix_var_color(n, false);
	}

	vset_clear(recolored);
}


static boolean avoid_color(struct shader_info * info, struct var_desc * v, int color, struct vset * recolored, boolean unfix)
{
	boolean result;
	int new_color,q;
	boolean selected = true;

	if (v->fixed)
		return false;

	result = false;

	selected = true;

	if (v->constraint && v->constraint->fixed)
		new_color = choose_color_constrained(info, v);
	else
		new_color = get_unique_color(info, v);

	selected = (new_color != 0);

	if (!selected)
		return false;

#ifdef CLR_DUMP
	R600_DUMP("  av: recoloring ");
	print_var(v);
	R600_DUMP(" to ");
	print_reg(new_color);
#endif

	set_color(v, new_color, recolored);

	result = true;

	for (q=0; q<v->interferences->count; q++) {
		struct var_desc *n = v->interferences->keys[q];

		if ((n->color == new_color) && (n->constraint==NULL || n->constraint!=v->constraint) && !value_equal(n,v)) {
			if (!avoid_color(info, n, new_color, recolored, true)) {
#ifdef CLR_DUMP
				R600_DUMP(" !! interference with ");
				print_var(n);
#endif
				result = false;
				break;
			}
		}
	}

#ifdef CLR_DUMP

	R600_DUMP(": %s\n", result? "OK":"FAIL");
#endif

	if (result)
		update_last_color(info, color);

	if (unfix)
		fix_var_color(v, false);

	return result;
}

static boolean recolor_var(struct shader_info * info, struct var_desc * v, int color, boolean dump)
{
	struct vset * recolored = vset_create(1);
	boolean result = true;
	int q;

#ifdef CLR_DUMP
	R600_DUMP("\t\t RECOLOR %d ", color);
	print_var(v);
	R600_DUMP("\n");
#endif

	if (!v->fixed) {
		if ((v->flags & VF_PIN_CHAN) && (color%4 != v->color%4)) {
			result=false;
#ifdef CLR_DUMP
			R600_DUMP("\t unable to recolor ");
			print_var(v);
			R600_DUMP(": chan constraint\n");
#endif
		}

		if (result && v->constraint && v->constraint->fixed && (v->constraint->r_color != ((color-1)/4 +1) )) {
			result = false;
#ifdef CLR_DUMP
			R600_DUMP("\t unable to recolor ");
			print_var(v);
			R600_DUMP(": block constraint\n");
#endif
		}

		if (result && v->bs_constraint) {
			int w, cc = 0;
			for (w=0; w<v->bs_constraint->comps->count; w++) {
				struct var_desc * v2 = v->bs_constraint->comps->keys[w];

				if (v2!=v && KEY_CHAN(v2->color) == KEY_CHAN(color) && v2->fixed && !value_equal(v,v2)) {
					if (cc<2)
						cc++;
					else {
						result=false;
						break;
					}
				}
			}
		}

		if (result) {

			set_color(v, color, recolored);

			if (v->interferences) {
				if (v->constraint) {
					for (q=0; q<v->constraint->comps->count; q++) {
						struct var_desc * n = v->constraint->comps->keys[q];
						if (n && !(n->flags & VF_DEAD) && !n->fixed && !value_equal(n,v)) {

#ifdef CLR_DUMP
							R600_DUMP("\trecoloring constraint neighbour ");
							print_var(n);
							R600_DUMP(" for ");
							print_var(v);
							R600_DUMP("\n");
#endif

							if (!avoid_color(info, n, color, recolored, false)) {

#ifdef CLR_DUMP
								R600_DUMP("recoloring ");
								print_var(n);
								R600_DUMP("failed\n");
#endif
								rollback_colors(recolored);
								result = false;
								break;
							}
						}
					}
				}

				if (result) {
					for (q=0; q<v->interferences->count; q++) {
						struct var_desc * n = v->interferences->keys[q];
						if (!value_equal(n,v) && n->color == color &&
								!avoid_color(info, n, color, recolored, true)) {

#ifdef CLR_DUMP
							if (dump) {

								R600_DUMP("\trecoloring ");
								print_var(n);
								R600_DUMP("failed\n");
							}

#endif
							rollback_colors(recolored);
							result = false;
							break;
						}
					}
				}
			}
		}
	} else if (v->color != color)
		result = false;

	// unfix
	for (q=0; q<recolored->count; q++) {
		struct var_desc * n = recolored->keys[q];
		fix_var_color(n, false);
	}

	vset_destroy(recolored);

#ifdef CLR_DUMP
	R600_DUMP("\t\tRECOLOR %s\n", result? "OK" : "FAILED");
#endif

	if (result)
		update_last_color(info, color);

	return result;
}

static void get_affine_subset(struct shader_info * info, struct vset * colored, struct var_desc *v, struct vset * cset, int * cur_cost)
{
	int cost = 0, q, w;

	struct vset * new_vars, * new_vars2, * edges, *tmp;

	new_vars = vset_create(1);
	new_vars2 = vset_create(1);
	edges = vset_create(1);

	vset_clear(cset);

	vset_add(new_vars, v);
	vset_remove(colored, v);

	do {

		for (q = 0; q < new_vars->count; q++) {
			struct var_desc * nv = new_vars->keys[q];

			for (w = 0; w < nv->aff_edges->count; w++) {
				struct affinity_edge * e = nv->aff_edges->keys[w];

				if (e->v == nv) {
					if (vset_remove(colored, e->v2)) {
						vset_add(new_vars2, e->v2);
						if (vset_add(edges, e))
							cost += e->cost;
					}
				} else {
					if (vset_remove(colored, e->v)) {
						vset_add(new_vars2, e->v);
						if (vset_add(edges, e))
							cost += e->cost;
					}
				}
			}
		}

		tmp = new_vars;
		new_vars = new_vars2;

		vset_addset(cset, tmp);
		vset_clear(tmp);

		new_vars2 = tmp;

	} while (new_vars->count>0);


	vset_destroy(new_vars);
	vset_destroy(new_vars2);
	vset_destroy(edges);

	*cur_cost = cost;
}

static void get_best_affine_subset(struct shader_info * info, struct vset * colored, struct vset * clr_best, int * cur_cost)
{
	int best_cost = -1, cost;
	struct vset * cset = vset_create(1);

	vset_clear(clr_best);

	while (colored->count > 0) {
		struct var_desc * v = colored->keys[0];

		get_affine_subset(info, colored, v, cset, &cost);

		if (cost>best_cost) {
			best_cost = cost;
			vset_copy(clr_best, cset);
		}
	}

	*cur_cost = best_cost;
	vset_destroy(cset);
}

static void recalc_chunk_cost(struct shader_info * info, struct affinity_chunk * chunk)
{
	chunk->cost = 0;

	if (chunk->vars->count>1) {
		int q;

		for (q=0; q<info->edge_queue->count; q++) {
			struct affinity_edge * e = info->edge_queue->keys[q*2+1];

			if (vset_contains(chunk->vars, e->v) && vset_contains(chunk->vars, e->v2))
				chunk->cost += e->cost;
		}
	}
}

#define MAX_GROUP_CHUNKS 4

struct rcg_ctx {
	struct shader_info * info;
	struct chunk_group * group;
	int chan[4];
	int new_chan[4];
	int ccost[4];
	int base_reg;

};

// get next combination of channels for count components
static boolean rcg_next_channels(struct rcg_ctx * x)
{
	unsigned free_chans = ~0;
	int count = x->group->chunks->count;
	int q;

	if (count<4) {
		for (q=0; q<count; q++)
			free_chans &= ~(1 << x->new_chan[q]);
	} else
		free_chans &= ~0b1111;

	for (q=count-1; q>=0; q--) {
		int cur_chan = x->new_chan[q];
		int new_chan = __builtin_ctz(free_chans>>(cur_chan+1)) + cur_chan+1;

		free_chans |= 1 << cur_chan;

		if (new_chan<4) {
			free_chans &= ~(1 << new_chan);
			x->new_chan[q] = new_chan;
			break;
		}

		if (q==0)
			return false;
	}

	for (q++; q<count; q++) {
		int new_chan = __builtin_ctz(free_chans);
		assert(new_chan<4);

		free_chans &= ~(1 << new_chan);
		x->new_chan[q] = new_chan;
	}

	return true;
}


static void rcg_color_chunk(struct rcg_ctx * x, int chunk_index, boolean final)
{
	struct affinity_chunk * chunk = x->group->chunks->keys[chunk_index];
	struct shader_info * info = x->info;
	struct var_desc * v;
	int q, cur_cost=-1;
	struct vset * colored = vset_create(chunk->vars->count);
	struct vset * clr_best = vset_create(1);
	int color = REGCHAN_KEY(x->base_reg, x->new_chan[chunk_index]);

#ifdef CLR_DUMP
	if (final)
		R600_DUMP("########## RCG recoloring chunk with %d\n", color);
#endif
	for (q=0;q<chunk->vars->count; q++) {
		v = chunk->vars->keys[q];
		if (!(v->flags & VF_PIN_REG))
			fix_var_color(v, false);
	}

	for (q=0;q<chunk->vars->count; q++) {
		v = chunk->vars->keys[q];

		if (recolor_var(info, v, color, final))
			vset_add(colored, v);
	}

	if (colored->count == chunk->vars->count) {
#ifdef CLR_DUMP
		if (final)
			R600_DUMP("fully colored\n");
#endif
		cur_cost = chunk->cost;
		vset_copy(clr_best, colored);
	} else {
		get_best_affine_subset(info, colored, clr_best, &cur_cost);

#ifdef CLR_DUMP
		if (final) {
			R600_DUMP("curr subset cost = %d : ", cur_cost);
			dump_vset(clr_best);
			R600_DUMP("\n");
		}
#endif

	}

	x->ccost[chunk_index] = cur_cost;

	if (final) {
#ifdef CLR_DUMP
		R600_DUMP("  best set ( clr = %d,  cost = %d ) : ", color, cur_cost);
		dump_vset(clr_best);
		R600_DUMP("\n");
#endif
		// recolor clr_best to best_color and fix

		if (clr_best->count == 0) {
			R600_DUMP("### UNABLE TO RECOLOR CHUNK, DISCARDING\n");
		} else {
			struct vset * rest;

			for (q=0; q<clr_best->count; q++) {
				struct var_desc * v = clr_best->keys[q];

				recolor_var(info, v, color, final);
				fix_var_color(v, true);
			}

			// make a new chunk out of the rest

			rest = vset_createcopy(chunk->vars);
			vset_removeset(rest, clr_best);

			if (rest->count) {

				struct affinity_chunk * rest_chunk = create_chunk();
				struct chunk_group * g = calloc(1, sizeof(struct chunk_group));

				rest_chunk->vars = rest;

				chunk->cost = cur_cost;
				vset_copy(chunk->vars, clr_best);

				// sync chunk pointers for vars
				for (q=0; q<rest->count; q++) {
					struct var_desc * v = rest->keys[q];
					v->chunk = rest_chunk;
				}

				recalc_chunk_cost(info, rest_chunk);

				R600_DUMP("rest_chunk: ");
				dump_chunk(rest_chunk);

				vque_enqueue(info->chunk_queue, rest_chunk->cost, rest_chunk);

				g->chunks = vvec_create(1);
				g->chunks->keys[0] = rest_chunk;
				rest_chunk->group = g;

				g->cost = rest_chunk->cost;

				vque_enqueue(info->chunk_groups, g->cost, g);

			} else
				vset_destroy(rest);
		}
	}

	vset_destroy(clr_best);
	vset_destroy(colored);
}

static void recolor_chunk_group(struct shader_info * info, struct chunk_group * group)
{
	int count = group->chunks->count;
	int base_reg;
	int best_total_cost = -1;
	int best_color[4] = {-1,-1,-1,-1};
	int q;
	boolean completed = false;
	struct rcg_ctx rctx = {};
	int last_reg = MIN2(info->last_color/4+2, 127-info->temp_gprs);

	assert(count<=4);

	rctx.group = group;
	rctx.info = info;

	info->enable_last_color_update = false;

	for (base_reg = 0; base_reg<last_reg && !completed; base_reg++) {

		rctx.base_reg = base_reg;

		R600_DUMP("trying base_reg %d\n", base_reg);

		for (q=0; q<count; q++) {
			rctx.chan[q] = -1;
			rctx.new_chan[q] = q;
		}

		do {
			int total_cost = 0;

			R600_DUMP("####### trying channels : ");
			for (q=0; q<count; q++) {
				R600_DUMP("%d ", rctx.new_chan[q]);
			}
			R600_DUMP("\n");

			for (q=0; q<count; q++) {
				if (rctx.new_chan[q]!=rctx.chan[q]) {
					rcg_color_chunk(&rctx, q, false);
					rctx.chan[q]=rctx.new_chan[q];
				}
				total_cost += rctx.ccost[q];
			}

			if (total_cost == group->cost) {
				// all chunks fully colored
				R600_DUMP("ALL CHUNKS FULLY COLORED\n");
				completed = true;
			}

			if (total_cost > best_total_cost) {
				for (q=0; q<count; q++)
					best_color[q] = REGCHAN_KEY(base_reg, rctx.new_chan[q]);
				best_total_cost = total_cost;
			}

		} while(!completed && rcg_next_channels(&rctx));
	}

	rctx.base_reg = KEY_REG(best_color[0]);

	R600_DUMP("FINAL GROUP COLORING\n");

	info->enable_last_color_update = true;

	for (q=0; q<count; q++) {
		rctx.new_chan[q] = best_color[q] - 1 -(rctx.base_reg<<2);
		rcg_color_chunk(&rctx, q, true);
	}
}

static void recolor_chunk_groups(struct shader_info * info)
{
	struct chunk_group * g;

	while (VQUE_DEQUEUE(info->chunk_groups, &g)) {
		R600_DUMP("###########################################\n");
		dump_chunk_group(g);
		recolor_chunk_group(info, g);
	}
}


void coalesce(struct shader_info * info)
{
	build_affinity_chunks(info);

	recolor_chunk_groups(info);

	R600_DUMP("## coalesce done\n");
}

/* initial coloring (modified later by coalescing, and then by scheduler) */
static void color_node(struct shader_info * info, struct ast_node * node)
{
	if (node->flags & AF_DEAD)
		return;

	if (node->loop_phi)
		color_node(info, node->loop_phi);

	if (node->p_split)
		color_node(info, node->p_split);

	if (node->outs) {
		int q;

		for (q=0; q< node->outs->count; q++) {
			struct var_desc * v = node->outs->keys[q];

			if (v && !(v->flags & (VF_DEAD | VF_SPECIAL))) {
				if (v->color == 0)
					v->color = get_unique_color(info, v);
				update_last_color(info, v->color);
			}
		}
	}

	if (node->p_split_outs)
		color_node(info, node->p_split_outs);

	if (node->child)
		color_node(info, node->child);

	if (node->rest)
		color_node(info, node->rest);

	if (node->phi)
		color_node(info, node->phi);
}

void color(struct shader_info * info)
{
	color_node(info, info->root);
}

static void add_neighbour_colors(struct vset * colors, struct vset * vars, struct var_desc * v)
{
	int q;

	if (vars) {
		for (q=0;q<vars->count; q++) {
			struct var_desc * v2 = vars->keys[q];
			if (v2->fixed && !value_equal(v2,v))
				VSET_ADD(colors, v2->color);
		}
	}
}

/* used from scheduler to recolor local variables */
boolean recolor_local(struct shader_info * info, struct var_desc * v)
{
	boolean result = false;
	int color = 1, color_step = 1;
	struct vset * colors;
	int q;

	if (v->flags & VF_DEAD) {
		v->color = 0;
		return true;
	}

	colors = vset_create(16);

	color += KEY_CHAN(v->color);
	color_step = 4;

	R600_DUMP("recoloring ");
	print_var(v);

	if (v->chunk) {
		assert(v->chunk->flags & ACF_LOCAL);

		for (q=0; q < v->chunk->vars->count; q++) {
			struct var_desc * n = v->chunk->vars->keys[q];

			dump_vset(n->interferences);
			add_neighbour_colors(colors, n->interferences, v);
		}
	} else {
		dump_vset(v->interferences);
		add_neighbour_colors(colors, v->interferences, v);
	}

	R600_DUMP(" : neighbours :");
	dump_color_set(colors);
	R600_DUMP("\n");

	q=0;

	do {
		while (q<colors->count && (uintptr_t)colors->keys[q]<color) q++;

		if (q<colors->count && (uintptr_t)colors->keys[q] == color)
			color+=color_step;
		else {
			result = true;
			break;
		}
	} while (color<=REGCHAN_KEY_MAX);

	if (result) {
		assert(KEY_CHAN(v->color) == KEY_CHAN(color));

		if (v->chunk) {
			for (q=0; q<v->chunk->vars->count; q++) {
				struct var_desc * n = v->chunk->vars->keys[q];
				n->color = color;
				n->fixed = true;
				R600_DUMP("recolored local ");
				print_var(n);
				R600_DUMP(" @ ");
				print_reg(color);
				R600_DUMP("\n");
			}
		} else {
			v->color = color;
			v->fixed = true;
			R600_DUMP("recolored local ");
			print_var(v);
			R600_DUMP(" @ ");
			print_reg(color);
			R600_DUMP("\n");
		}
	}

	vset_destroy(colors);
	return result;
}
