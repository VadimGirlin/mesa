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

#ifndef OPT_DUMP_H_
#define OPT_DUMP_H_

extern int _r600_opt_dump_level;

#define R600_OPT_DUMP_LEVEL_INFO	1
#define R600_OPT_DUMP_LEVEL_SHADERS	2
#define R600_OPT_DUMP_LEVEL_DEBUG	3

static inline void get_dump_level()
{
	if (_r600_opt_dump_level == -1)
#ifdef DEBUG
		_r600_opt_dump_level = debug_get_num_option("R600_OPT_DUMP",1);
#else
	_r600_opt_dump_level = debug_get_num_option("R600_OPT_DUMP",0);

	if (_r600_opt_dump_level > R600_OPT_DUMP_LEVEL_SHADERS)
		_r600_opt_dump_level = R600_OPT_DUMP_LEVEL_SHADERS;
#endif
}

static inline boolean check_dump_level(int loglevel)
{
	return loglevel <= _r600_opt_dump_level;
};

#ifdef DEBUG
#define R600_DUMP(fmt, args...) \
		do { \
			if (check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG)) \
			fprintf(stderr, fmt, ##args); \
		} while(0)
#define R600_DUMP_CALL(f) \
		do { \
			if (check_dump_level(R600_OPT_DUMP_LEVEL_DEBUG)) \
			f; \
		} while(0)
#else
#define R600_DUMP(fmt, args...)
#define R600_DUMP_CALL(f)
#endif

void indent(int level);

void print_var(struct var_desc * v);
void print_reg( unsigned k);
void fprint_reg(FILE *f, unsigned k);
void fprint_var(FILE *f, struct var_desc * v);

void dump_bytecode(struct shader_info * info);


void dump_vars(void ** vars, unsigned count);

void dump_vset(struct vset * s);
void dump_vvec(struct vvec * s);

void dump_vset_colors(struct vset * s);

void dump_alu(struct shader_info * info, int level, struct r600_bytecode_alu * alu);
void dump_cf(struct shader_info * info, int level, struct r600_bytecode_cf * cf);
void dump_tex(struct shader_info * info, int level, struct r600_bytecode_tex * tex);
void dump_vtx(struct shader_info * info, int level, struct r600_bytecode_vtx * vtx);

void dump_node(struct shader_info * info, struct ast_node * node, int level);
void dump_shader_tree(struct shader_info * info);


void dump_var_desc(struct var_desc * v);
void dump_var_table(struct shader_info * info);

void dump_chunk(struct affinity_chunk * c);
void dump_chunk_group(struct chunk_group * g);
void dump_chunks_queue(struct shader_info * info);
void dump_chunk_group_queue(struct shader_info * info);

void dump_reg_map(struct vmap * map);
void dump_color_set(struct vset * c);

#define PRINT_REG(k) print_reg((unsigned)(k))

void dot_create_file(struct shader_info * info);

#endif /* OPT_DUMP_H_ */

