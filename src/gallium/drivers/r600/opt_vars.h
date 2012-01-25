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

#ifndef OPT_VARS_H_
#define OPT_VARS_H_

struct shader_info;

#define REGCHAN_KEY_MAX (123<<2)

#define REG_SPECIAL (1<<20)
#define REG_TEMP (1<<30)

/* pseudoregisters: */
/* active mask */
#define REG_AM (REG_SPECIAL + 0)

/* predicate (alu clause local)*/
#define REG_PR (REG_SPECIAL + 1)

#define REG_AL (REG_SPECIAL + 2)
#define REG_AR (REG_SPECIAL + 3)

/* tex gradients (chan 0 - V, 1 - H */
#define REG_GR (REG_SPECIAL + 10)

#define REGCHAN_KEY(reg,chan) (((reg)<<2)+((chan)&3)+1)

#define CPAIR_KEY(idx,chan) (((idx)<<1)+((chan>>1)+1))

#define KEY_REG(key) (((key)-1)>>2)
#define KEY_CHAN(key) (((key)-1)&3)

/* affinity costs (for coalescing, see opt_color.[ch] */
#define AE_SPLIT_COST		1000000
#define AE_INPUT_COST		10000
#define AE_CSPLIT_COST		10000
#define AE_CONSTRAINT_COST	1000
#define AE_PHI_COST			100000
#define AE_COPY_COST		1

/* priorities (see comments for prio_get_node_subtype, calc_prio) */
#define ANP_PRIO_STEP 1
#define ANP_PRIO_BLOCKSTEP (1<<16)

/* internal representation is based on the idea of CORTL AST
 * (Carl D. McConnell, "Tree-based Code Optimization") */

enum node_type
{
	NT_NONE,

	NT_REGION,
	NT_DEPART,
	NT_REPEAT,
	NT_IF,
	NT_LIST,
	NT_OP,
	NT_GROUP,

	NT_LAST
};

enum node_subtype
{
	NST_NONE,

	NST_ROOT, /* subtype for the root list node */

	NST_PHI,

	NST_PARALLEL_GROUP,
	NST_COPY,
	NST_PCOPY,

	NST_ALU_CLAUSE,
	NST_ALU_GROUP,

	NST_ALU_INST,
	NST_TEX_INST,
	NST_VTX_INST,
	NST_CF_INST,

	NST_LOOP_REGION,
	NST_IF_ELSE_REGION,

	NST_LOOP_BREAK,
	NST_LOOP_CONTINUE,

	NST_BLOCK,


	NST_LAST
};

enum node_op_class {
	NOC_GENERIC,

	NOC_CF_EXPORT,
	NOC_CF_STREAMOUT
};


struct reg_desc
{
	int reg;
	int chan;
};

enum var_flags
{
	VF_NONE = 0,

	/* channel can't be modified */
	VF_PIN_CHAN	= (1<<0),

	/* gpr index can't be modified */
	VF_PIN_REG	= (1<<1),

	/* for special registers (REG_AM etc) */
	VF_SPECIAL	= (1<<2),

	/* variable is dead (not used) */
	VF_DEAD = (1<<8),

	/* temporary variable created during optimization,
	 * as opposed to variable corresponding to gpr from original shader code */
	VF_TEMP = (1<<9),

	VF_UNDEFINED = (1<<10)

};

/* following structure is used for two types of constraints :
 * 1) reg constraint (var_desc.constraint):
 * 		all constrained variables should be allocated
 * 		to the components of the same gpr (e.g. inputs for EXPORT, SAMPLE, etc)
 *
 * 2) bank swizzle constraint (var_desc.bs_constraint):
 * 		for all groups of (4-slots) instructions, we should avoid using more than 3
 * 		different input values (variables) allocated to the same channel,
 * 		otherwise it will break bank swizzle check.
 */
struct rc_constraint
{
	struct vvec * comps;
	int fixed;
	int r_color;
};


struct var_desc
{
	struct reg_desc reg;

	enum var_flags flags;

	/* SSA value index */
	int index;

	/* use/def data */
	struct ast_node * def;
	struct vset * uses;

	/* points to the value source (e.g for MOV a,b - a->value_hint==b */
	struct var_desc * value_hint;

	/* constraints - see comments for struct rc_constraint above */
	struct rc_constraint * constraint;
	struct rc_constraint * bs_constraint;

	/* set of interfering variables (i.e. which are live simultaneously with this variable) */
	struct vset * interferences;

	/* coalescing data (see opt_color.[hc]) */

	struct affinity_chunk * chunk;
	int color;
	int saved_color;
	boolean fixed;

	/* for priority calculation (see comments for prio_get_node_subtype, calc_prio) */
	unsigned prio;
//	enum node_subtype last_use_type;
	boolean fetch_dep;
};


/* FIXME: vector, set, map, queue are implemented in the most simple and
 * straightforward way, probably makes sense to optimize
 * (e.g. using bitfields for storing interferences instead of sets, or not storing them)
 */

// set (sorted)

struct vset
{
	void **keys;
	unsigned count;
	unsigned size;
};

// map (sorted by key)

struct vmap
{
	void **keys;
	unsigned count;
	unsigned size;
};

// vector

struct vvec
{
	void **keys;
	unsigned count;
};

struct affinity_edge
{
	int cost;
	struct var_desc *v, *v2;
};

enum chunk_flags {

	/* local chunk - all variables in the chunk are local for some alu clause */
	ACF_LOCAL = 1,

	ACF_GLOBAL = 2,
};

/* up to four chunks, to handle coalescing for registers/components better */
struct chunk_group {
	struct vvec * chunks;
	int cost;
};

struct affinity_chunk
{
	struct vset * vars;
	struct chunk_group * group;
	int cost;
	enum chunk_flags flags;
};


// priority queue (sorted by priority ascending),
// dequeue gets last item (with max priority)

struct vque {
	void **keys;
	unsigned count;
	unsigned size;
};


struct vvec * vvec_create(unsigned initial_size);
struct vvec * vvec_create_clean(unsigned initial_size);
void vvec_set_size(struct vvec * s, unsigned new_size);
void vvec_append(struct vvec * s, void * key);
void vvec_destroy(struct vvec * s);
boolean vvec_contains(struct vvec * s, void * key);
struct vvec * vvec_createcopy(struct vvec * s);
void vvec_clear(struct vvec * s);


struct vset* vset_create(unsigned initial_size);
void vset_destroy(struct vset * s);
int vset_get_pos(struct vset * s, void * key);
boolean vset_contains(struct vset * s, void * key);
boolean vset_containsvec(struct vset * s, struct vvec * v);
boolean vset_containsset(struct vset * s, struct vset * c);
boolean vset_intersects(struct vset * s, struct vset * c);
void vset_resize(struct vset * s);
boolean vset_add(struct vset * s, void * key);
boolean vset_remove(struct vset * s, void * key);
void vset_addset(struct vset * s, struct vset * from);
void vset_addvec(struct vset * s, struct vvec * from);
boolean vset_removeset(struct vset * s, struct vset * from);
boolean vset_removevec(struct vset * s, struct vvec * from);
void vset_clear(struct vset * s);
void vset_copy(struct vset * s, struct vset * from);
struct vset * vset_createcopy(struct vset * from);

struct vmap* vmap_create(unsigned initial_size);
void vmap_destroy(struct vmap * s);
void vmap_copy(struct vmap * m, struct vmap * from);
int vmap_get_pos(struct vmap * s, void * key);
boolean vmap_contains(struct vmap * s, void * key);
void vmap_resize(struct vmap * s);
boolean vmap_set(struct vmap * s, void * key, void * data);
boolean vmap_get(struct vmap * s, void * key, void ** data);
boolean vmap_remove(struct vmap * s, void * key);
struct vmap * vmap_createcopy(struct vmap * from);
void vmap_clear(struct vmap * s);


struct vque* vque_create(unsigned initial_size);
void vque_destroy(struct vque * s);
int vque_get_pos(struct vque * s,  uintptr_t pri);
void vque_resize(struct vque * s);
void vque_enqueue(struct vque * s,  uintptr_t pri, void * data);
boolean vque_dequeue(struct vque * s, void ** data);

#define VSET_ADD(a,b) vset_add(a,(void*)(uintptr_t)b)
#define VSET_REMOVE(a,b) vset_remove(a,(void*)(uintptr_t)b)
#define VSET_CONTAINS(a,b) vset_contains(a,(void*)(uintptr_t)b)

#define VMAP_GET(a,b,c) vmap_get(a,(void*)(uintptr_t)b,(void**)(c))
#define VMAP_SET(a,b,c) vmap_set(a,(void*)(uintptr_t)b,(void*)(uintptr_t)(c))
#define VMAP_REMOVE(a,b) vmap_remove(a,(void*)(uintptr_t)b)
#define VMAP_CONTAINS(a,b) vmap_contains(a,(void*)(uintptr_t)b)

#define VQUE_ENQUEUE(a,b,c) vque_enqueue(a,(uintptr_t)b,(void*)c)
#define VQUE_DEQUEUE(a,b) vque_dequeue(a,(void**)b)

#endif /* OPT_VARS_H_ */
