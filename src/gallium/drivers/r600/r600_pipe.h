/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
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
 *
 * Authors:
 *      Jerome Glisse
 */
#ifndef R600_PIPE_H
#define R600_PIPE_H

#include "util/u_slab.h"
#include "r600.h"
#include "r600_llvm.h"
#include "r600_public.h"
#include "r600_shader.h"
#include "r600_resource.h"
#include "evergreen_compute.h"

#define R600_MAX_CONST_BUFFERS 2
#define R600_MAX_CONST_BUFFER_SIZE 4096

#ifdef PIPE_ARCH_BIG_ENDIAN
#define R600_BIG_ENDIAN 1
#else
#define R600_BIG_ENDIAN 0
#endif

enum r600_atom_flags {
	/* When set, atoms are added at the beginning of the dirty list
	 * instead of the end. */
	EMIT_EARLY = (1 << 0)
};

/* This encapsulates a state or an operation which can emitted into the GPU
 * command stream. It's not limited to states only, it can be used for anything
 * that wants to write commands into the CS (e.g. cache flushes). */
struct r600_atom {
	void (*emit)(struct r600_context *ctx, struct r600_atom *state);

	unsigned		num_dw;
	enum r600_atom_flags	flags;
	bool			dirty;

	struct list_head	head;
};

/* This is an atom containing GPU commands that never change.
 * This is supposed to be copied directly into the CS. */
struct r600_command_buffer {
	struct r600_atom atom;
	uint32_t *buf;
	unsigned max_num_dw;
};

struct r600_surface_sync_cmd {
	struct r600_atom atom;
	unsigned flush_flags; /* CP_COHER_CNTL */
};

struct r600_db_misc_state {
	struct r600_atom atom;
	bool occlusion_query_enabled;
	bool flush_depthstencil_enabled;
};

enum r600_pipe_state_id {
	R600_PIPE_STATE_BLEND = 0,
	R600_PIPE_STATE_BLEND_COLOR,
	R600_PIPE_STATE_CONFIG,
	R600_PIPE_STATE_SEAMLESS_CUBEMAP,
	R600_PIPE_STATE_CLIP,
	R600_PIPE_STATE_SCISSOR,
	R600_PIPE_STATE_VIEWPORT,
	R600_PIPE_STATE_RASTERIZER,
	R600_PIPE_STATE_VGT,
	R600_PIPE_STATE_FRAMEBUFFER,
	R600_PIPE_STATE_DSA,
	R600_PIPE_STATE_STENCIL_REF,
	R600_PIPE_STATE_PS_SHADER,
	R600_PIPE_STATE_VS_SHADER,
	R600_PIPE_STATE_CONSTANT,
	R600_PIPE_STATE_SAMPLER,
	R600_PIPE_STATE_RESOURCE,
	R600_PIPE_STATE_POLYGON_OFFSET,
	R600_PIPE_STATE_FETCH_SHADER,
	R600_PIPE_STATE_SPI,
	R600_PIPE_NSTATES
};

struct compute_memory_pool;
void compute_memory_pool_delete(struct compute_memory_pool* pool);
struct compute_memory_pool* compute_memory_pool_new(
	int64_t initial_size_in_dw,
	struct r600_screen *rscreen);

struct r600_pipe_fences {
	struct r600_resource		*bo;
	unsigned			*data;
	unsigned			next_index;
	/* linked list of preallocated blocks */
	struct list_head		blocks;
	/* linked list of freed fences */
	struct list_head		pool;
	pipe_mutex			mutex;
};

struct r600_screen {
	struct pipe_screen		screen;
	struct radeon_winsys		*ws;
	unsigned			family;
	enum chip_class			chip_class;
	struct radeon_info		info;
	bool				has_streamout;
	struct r600_tiling_info		tiling_info;
	struct r600_pipe_fences		fences;

	bool				use_surface_alloc;
	int 				glsl_feature_level;

	/*for compute global memory binding, we allocate stuff here, instead of
	 * buffers.
	 * XXX: Not sure if this is the best place for global_pool.  Also,
	 * it's not thread safe, so it won't work with multiple contexts. */
	struct compute_memory_pool *global_pool;
};

struct r600_pipe_sampler_view {
	struct pipe_sampler_view	base;
	struct r600_pipe_resource_state		state;
};

struct r600_pipe_rasterizer {
	struct r600_pipe_state		rstate;
	boolean				flatshade;
	boolean				two_side;
	unsigned			sprite_coord_enable;
	unsigned                        clip_plane_enable;
	unsigned			pa_sc_line_stipple;
	unsigned			pa_cl_clip_cntl;
	float				offset_units;
	float				offset_scale;
	bool				scissor_enable;
};

struct r600_pipe_blend {
	struct r600_pipe_state		rstate;
	unsigned			cb_target_mask;
	unsigned			cb_color_control;
	bool				dual_src_blend;
};

struct r600_pipe_dsa {
	struct r600_pipe_state		rstate;
	unsigned			alpha_ref;
	ubyte				valuemask[2];
	ubyte				writemask[2];
	bool				is_flush;
	unsigned                        sx_alpha_test_control;
};

struct r600_vertex_element
{
	unsigned			count;
	struct pipe_vertex_element	elements[PIPE_MAX_ATTRIBS];
	struct r600_resource		*fetch_shader;
	unsigned			fs_size;
	struct r600_pipe_state		rstate;
};

struct r600_pipe_shader {
	struct r600_shader		shader;
	struct r600_pipe_state		rstate;
	struct r600_resource		*bo;
	struct r600_resource		*bo_fetch;
	struct r600_vertex_element	vertex_elements;
	struct tgsi_token		*tokens;
	unsigned	sprite_coord_enable;
	unsigned	flatshade;
	unsigned	pa_cl_vs_out_cntl;
	unsigned        ps_cb_shader_mask;
	unsigned		db_shader_control;
	unsigned		ps_depth_export;
	struct pipe_stream_output_info	so;
};

struct r600_pipe_sampler_state {
	struct r600_pipe_state		rstate;
	boolean seamless_cube_map;
};

/* needed for blitter save */
#define NUM_TEX_UNITS 16

struct r600_textures_info {
	struct r600_pipe_sampler_view	*views[NUM_TEX_UNITS];
	struct r600_pipe_sampler_state	*samplers[NUM_TEX_UNITS];
	unsigned			n_views;
	unsigned			n_samplers;
	bool				samplers_dirty;
	bool				is_array_sampler[NUM_TEX_UNITS];
};

struct r600_fence {
	struct pipe_reference		reference;
	unsigned			index; /* in the shared bo */
	struct r600_resource            *sleep_bo;
	struct list_head		head;
};

#define FENCE_BLOCK_SIZE 16

struct r600_fence_block {
	struct r600_fence		fences[FENCE_BLOCK_SIZE];
	struct list_head		head;
};

#define R600_CONSTANT_ARRAY_SIZE 256
#define R600_RESOURCE_ARRAY_SIZE 160

struct r600_stencil_ref
{
	ubyte ref_value[2];
	ubyte valuemask[2];
	ubyte writemask[2];
};

struct r600_constbuf_state
{
	struct r600_atom		atom;
	struct pipe_constant_buffer	cb[PIPE_MAX_CONSTANT_BUFFERS];
	uint32_t			enabled_mask;
	uint32_t			dirty_mask;
};

struct r600_context {
	struct pipe_context		context;
	struct blitter_context		*blitter;
	enum radeon_family		family;
	enum chip_class			chip_class;
	boolean				has_vertex_cache;
	unsigned			r6xx_num_clause_temp_gprs;
	void				*custom_dsa_flush;
	struct r600_screen		*screen;
	struct radeon_winsys		*ws;
	struct r600_pipe_state		*states[R600_PIPE_NSTATES];
	struct r600_vertex_element	*vertex_elements;
	struct pipe_framebuffer_state	framebuffer;
	unsigned			cb_target_mask;
	unsigned			fb_cb_shader_mask;
	unsigned			sx_alpha_test_control;
	unsigned			cb_shader_mask;
	unsigned			db_shader_control;
	unsigned			cb_color_control;
	unsigned			pa_sc_line_stipple;
	unsigned			pa_cl_clip_cntl;
	/* for saving when using blitter */
	struct pipe_stencil_ref		stencil_ref;
	struct pipe_viewport_state	viewport;
	struct pipe_clip_state		clip;
	struct r600_pipe_shader 	*ps_shader;
	struct r600_pipe_shader 	*vs_shader;
	struct r600_pipe_compute	*cs_shader;
	struct r600_pipe_rasterizer	*rasterizer;
	struct r600_pipe_state          vgt;
	struct r600_pipe_state          spi;
	struct pipe_query		*current_render_cond;
	unsigned			current_render_cond_mode;
	struct pipe_query		*saved_render_cond;
	unsigned			saved_render_cond_mode;
	/* shader information */
	boolean				two_side;
	boolean				spi_dirty;
	unsigned			sprite_coord_enable;
	boolean				flatshade;
	boolean				export_16bpc;
	unsigned			alpha_ref;
	boolean				alpha_ref_dirty;
	unsigned			nr_cbufs;
	struct r600_textures_info	vs_samplers;
	struct r600_textures_info	ps_samplers;

	struct u_upload_mgr	        *uploader;
	struct util_slab_mempool	pool_transfers;
	boolean				have_depth_texture, have_depth_fb;

	unsigned default_ps_gprs, default_vs_gprs;

	/* States based on r600_atom. */
	struct list_head		dirty_states;
	struct r600_command_buffer	start_cs_cmd; /* invariant state mostly */
	struct r600_surface_sync_cmd	surface_sync_cmd;
	struct r600_atom		r6xx_flush_and_inv_cmd;
	struct r600_db_misc_state	db_misc_state;
	struct r600_atom		vertex_buffer_state;
	struct r600_constbuf_state	vs_constbuf_state;
	struct r600_constbuf_state	ps_constbuf_state;

	struct radeon_winsys_cs	*cs;

	struct r600_range	*range;
	unsigned		nblocks;
	struct r600_block	**blocks;
	struct list_head	dirty;
	struct list_head	resource_dirty;
	struct list_head	enable_list;
	unsigned		pm4_dirty_cdwords;
	unsigned		ctx_pm4_ndwords;

	/* The list of active queries. Only one query of each type can be active. */
	int			num_occlusion_queries;

	/* Manage queries in two separate groups:
	 * The timer ones and the others (streamout, occlusion).
	 *
	 * We do this because we should only suspend non-timer queries for u_blitter,
	 * and later if the non-timer queries are suspended, the context flush should
	 * only suspend and resume the timer queries. */
	struct list_head	active_timer_queries;
	unsigned		num_cs_dw_timer_queries_suspend;
	struct list_head	active_nontimer_queries;
	unsigned		num_cs_dw_nontimer_queries_suspend;

	unsigned		num_cs_dw_streamout_end;

	unsigned		backend_mask;
	unsigned                max_db; /* for OQ */
	unsigned		flags;
	boolean                 predicate_drawing;
	struct r600_range	ps_resources;
	struct r600_range	vs_resources;
	int			num_ps_resources, num_vs_resources;

	unsigned		num_so_targets;
	struct r600_so_target	*so_targets[PIPE_MAX_SO_BUFFERS];
	boolean			streamout_start;
	unsigned		streamout_append_bitmask;

	/* There is no scissor enable bit on r6xx, so we must use a workaround.
	 * These track the current scissor state. */
	bool			scissor_enable;
	struct pipe_scissor_state scissor_state;

	/* With rasterizer discard, there doesn't have to be a pixel shader.
	 * In that case, we bind this one: */
	void			*dummy_pixel_shader;

	boolean			dual_src_blend;

	/* Vertex and index buffers. */
	bool			vertex_buffers_dirty;
	struct pipe_index_buffer index_buffer;
	struct pipe_vertex_buffer vertex_buffer[PIPE_MAX_ATTRIBS];
	unsigned		nr_vertex_buffers;
};

static INLINE void r600_emit_atom(struct r600_context *rctx, struct r600_atom *atom)
{
	atom->emit(rctx, atom);
	atom->dirty = false;
	if (atom->head.next && atom->head.prev)
		LIST_DELINIT(&atom->head);
}

static INLINE void r600_atom_dirty(struct r600_context *rctx, struct r600_atom *state)
{
	if (!state->dirty) {
		if (state->flags & EMIT_EARLY) {
			LIST_ADD(&state->head, &rctx->dirty_states);
		} else {
			LIST_ADDTAIL(&state->head, &rctx->dirty_states);
		}
		state->dirty = true;
	}
}

/* evergreen_state.c */
void evergreen_init_state_functions(struct r600_context *rctx);
void evergreen_init_atom_start_cs(struct r600_context *rctx);
void evergreen_pipe_shader_ps(struct pipe_context *ctx, struct r600_pipe_shader *shader);
void evergreen_pipe_shader_vs(struct pipe_context *ctx, struct r600_pipe_shader *shader);
void evergreen_fetch_shader(struct pipe_context *ctx, struct r600_vertex_element *ve);
void *evergreen_create_db_flush_dsa(struct r600_context *rctx);
void evergreen_polygon_offset_update(struct r600_context *rctx);
boolean evergreen_is_format_supported(struct pipe_screen *screen,
				      enum pipe_format format,
				      enum pipe_texture_target target,
				      unsigned sample_count,
				      unsigned usage);

void evergreen_update_dual_export_state(struct r600_context * rctx);

/* r600_blit.c */
void r600_init_blit_functions(struct r600_context *rctx);
void r600_blit_uncompress_depth(struct pipe_context *ctx, struct r600_resource_texture *texture);
void r600_blit_push_depth(struct pipe_context *ctx, struct r600_resource_texture *texture);
void r600_flush_depth_textures(struct r600_context *rctx);

/* r600_buffer.c */
bool r600_init_resource(struct r600_screen *rscreen,
			struct r600_resource *res,
			unsigned size, unsigned alignment,
			unsigned bind, unsigned usage);
struct pipe_resource *r600_buffer_create(struct pipe_screen *screen,
					 const struct pipe_resource *templ);

/* r600_pipe.c */
void r600_flush(struct pipe_context *ctx, struct pipe_fence_handle **fence,
		unsigned flags);

/* r600_query.c */
void r600_init_query_functions(struct r600_context *rctx);
void r600_suspend_nontimer_queries(struct r600_context *ctx);
void r600_resume_nontimer_queries(struct r600_context *ctx);
void r600_suspend_timer_queries(struct r600_context *ctx);
void r600_resume_timer_queries(struct r600_context *ctx);

/* r600_resource.c */
void r600_init_context_resource_functions(struct r600_context *r600);

/* r600_shader.c */
int r600_pipe_shader_create(struct pipe_context *ctx, struct r600_pipe_shader *shader);
#ifdef HAVE_OPENCL
int r600_compute_shader_create(struct pipe_context * ctx,
	LLVMModuleRef mod,  struct r600_bytecode * bytecode);
#endif
void r600_pipe_shader_destroy(struct pipe_context *ctx, struct r600_pipe_shader *shader);
int r600_find_vs_semantic_index(struct r600_shader *vs,
				struct r600_shader *ps, int id);

/* r600_state.c */
void r600_set_scissor_state(struct r600_context *rctx,
			    const struct pipe_scissor_state *state);
void r600_update_sampler_states(struct r600_context *rctx);
void r600_init_state_functions(struct r600_context *rctx);
void r600_init_atom_start_cs(struct r600_context *rctx);
void r600_pipe_shader_ps(struct pipe_context *ctx, struct r600_pipe_shader *shader);
void r600_pipe_shader_vs(struct pipe_context *ctx, struct r600_pipe_shader *shader);
void r600_fetch_shader(struct pipe_context *ctx, struct r600_vertex_element *ve);
void *r600_create_db_flush_dsa(struct r600_context *rctx);
void r600_polygon_offset_update(struct r600_context *rctx);
void r600_adjust_gprs(struct r600_context *rctx);
boolean r600_is_format_supported(struct pipe_screen *screen,
				 enum pipe_format format,
				 enum pipe_texture_target target,
				 unsigned sample_count,
				 unsigned usage);

/* r600_texture.c */
void r600_init_screen_texture_functions(struct pipe_screen *screen);
void r600_init_surface_functions(struct r600_context *r600);
uint32_t r600_translate_texformat(struct pipe_screen *screen, enum pipe_format format,
				  const unsigned char *swizzle_view,
				  uint32_t *word4_p, uint32_t *yuv_format_p);
unsigned r600_texture_get_offset(struct r600_resource_texture *rtex,
					unsigned level, unsigned layer);

/* r600_translate.c */
void r600_translate_index_buffer(struct r600_context *r600,
				 struct pipe_index_buffer *ib,
				 unsigned count);

/* r600_state_common.c */
void r600_init_atom(struct r600_atom *atom,
		    void (*emit)(struct r600_context *ctx, struct r600_atom *state),
		    unsigned num_dw, enum r600_atom_flags flags);
void r600_init_common_atoms(struct r600_context *rctx);
unsigned r600_get_cb_flush_flags(struct r600_context *rctx);
void r600_texture_barrier(struct pipe_context *ctx);
void r600_set_index_buffer(struct pipe_context *ctx,
			   const struct pipe_index_buffer *ib);
void r600_set_vertex_buffers(struct pipe_context *ctx, unsigned count,
			     const struct pipe_vertex_buffer *buffers);
void *r600_create_vertex_elements(struct pipe_context *ctx,
				  unsigned count,
				  const struct pipe_vertex_element *elements);
void r600_delete_vertex_element(struct pipe_context *ctx, void *state);
void r600_bind_blend_state(struct pipe_context *ctx, void *state);
void r600_set_blend_color(struct pipe_context *ctx,
			  const struct pipe_blend_color *state);
void r600_bind_dsa_state(struct pipe_context *ctx, void *state);
void r600_set_max_scissor(struct r600_context *rctx);
void r600_bind_rs_state(struct pipe_context *ctx, void *state);
void r600_delete_rs_state(struct pipe_context *ctx, void *state);
void r600_sampler_view_destroy(struct pipe_context *ctx,
			       struct pipe_sampler_view *state);
void r600_delete_state(struct pipe_context *ctx, void *state);
void r600_bind_vertex_elements(struct pipe_context *ctx, void *state);
void *r600_create_shader_state(struct pipe_context *ctx,
			       const struct pipe_shader_state *state);
void r600_bind_ps_shader(struct pipe_context *ctx, void *state);
void r600_bind_vs_shader(struct pipe_context *ctx, void *state);
void r600_delete_ps_shader(struct pipe_context *ctx, void *state);
void r600_delete_vs_shader(struct pipe_context *ctx, void *state);
void r600_constant_buffers_dirty(struct r600_context *rctx, struct r600_constbuf_state *state);
void r600_set_constant_buffer(struct pipe_context *ctx, uint shader, uint index,
			      struct pipe_constant_buffer *cb);
struct pipe_stream_output_target *
r600_create_so_target(struct pipe_context *ctx,
		      struct pipe_resource *buffer,
		      unsigned buffer_offset,
		      unsigned buffer_size);
void r600_so_target_destroy(struct pipe_context *ctx,
			    struct pipe_stream_output_target *target);
void r600_set_so_targets(struct pipe_context *ctx,
			 unsigned num_targets,
			 struct pipe_stream_output_target **targets,
			 unsigned append_bitmask);
void r600_set_pipe_stencil_ref(struct pipe_context *ctx,
			       const struct pipe_stencil_ref *state);
void r600_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info);
uint32_t r600_translate_stencil_op(int s_op);
uint32_t r600_translate_fill(uint32_t func);
unsigned r600_tex_wrap(unsigned wrap);
unsigned r600_tex_filter(unsigned filter);
unsigned r600_tex_mipfilter(unsigned filter);
unsigned r600_tex_compare(unsigned compare);

/*
 * Helpers for building command buffers
 */

#define PKT3_SET_CONFIG_REG	0x68
#define PKT3_SET_CONTEXT_REG	0x69
#define PKT3_SET_CTL_CONST      0x6F
#define PKT3_SET_LOOP_CONST                    0x6C

#define R600_CONFIG_REG_OFFSET	0x08000
#define R600_CONTEXT_REG_OFFSET 0x28000
#define R600_CTL_CONST_OFFSET   0x3CFF0
#define R600_LOOP_CONST_OFFSET                 0X0003E200
#define EG_LOOP_CONST_OFFSET               0x0003A200

#define PKT_TYPE_S(x)                   (((x) & 0x3) << 30)
#define PKT_COUNT_S(x)                  (((x) & 0x3FFF) << 16)
#define PKT3_IT_OPCODE_S(x)             (((x) & 0xFF) << 8)
#define PKT3_PREDICATE(x)               (((x) >> 0) & 0x1)
#define PKT3(op, count, predicate) (PKT_TYPE_S(3) | PKT_COUNT_S(count) | PKT3_IT_OPCODE_S(op) | PKT3_PREDICATE(predicate))

static INLINE void r600_store_value(struct r600_command_buffer *cb, unsigned value)
{
	cb->buf[cb->atom.num_dw++] = value;
}

static INLINE void r600_store_config_reg_seq(struct r600_command_buffer *cb, unsigned reg, unsigned num)
{
	assert(reg < R600_CONTEXT_REG_OFFSET);
	assert(cb->atom.num_dw+2+num <= cb->max_num_dw);
	cb->buf[cb->atom.num_dw++] = PKT3(PKT3_SET_CONFIG_REG, num, 0);
	cb->buf[cb->atom.num_dw++] = (reg - R600_CONFIG_REG_OFFSET) >> 2;
}

static INLINE void r600_store_context_reg_seq(struct r600_command_buffer *cb, unsigned reg, unsigned num)
{
	assert(reg >= R600_CONTEXT_REG_OFFSET && reg < R600_CTL_CONST_OFFSET);
	assert(cb->atom.num_dw+2+num <= cb->max_num_dw);
	cb->buf[cb->atom.num_dw++] = PKT3(PKT3_SET_CONTEXT_REG, num, 0);
	cb->buf[cb->atom.num_dw++] = (reg - R600_CONTEXT_REG_OFFSET) >> 2;
}

static INLINE void r600_store_ctl_const_seq(struct r600_command_buffer *cb, unsigned reg, unsigned num)
{
	assert(reg >= R600_CTL_CONST_OFFSET);
	assert(cb->atom.num_dw+2+num <= cb->max_num_dw);
	cb->buf[cb->atom.num_dw++] = PKT3(PKT3_SET_CTL_CONST, num, 0);
	cb->buf[cb->atom.num_dw++] = (reg - R600_CTL_CONST_OFFSET) >> 2;
}

static INLINE void r600_store_loop_const_seq(struct r600_command_buffer *cb, unsigned reg, unsigned num)
{
	assert(reg >= R600_LOOP_CONST_OFFSET);
	assert(cb->atom.num_dw+2+num <= cb->max_num_dw);
	cb->buf[cb->atom.num_dw++] = PKT3(PKT3_SET_LOOP_CONST, num, 0);
	cb->buf[cb->atom.num_dw++] = (reg - R600_LOOP_CONST_OFFSET) >> 2;
}

static INLINE void eg_store_loop_const_seq(struct r600_command_buffer *cb, unsigned reg, unsigned num)
{
	assert(reg >= EG_LOOP_CONST_OFFSET);
	assert(cb->atom.num_dw+2+num <= cb->max_num_dw);
	cb->buf[cb->atom.num_dw++] = PKT3(PKT3_SET_LOOP_CONST, num, 0);
	cb->buf[cb->atom.num_dw++] = (reg - EG_LOOP_CONST_OFFSET) >> 2;
}

static INLINE void r600_store_config_reg(struct r600_command_buffer *cb, unsigned reg, unsigned value)
{
	r600_store_config_reg_seq(cb, reg, 1);
	r600_store_value(cb, value);
}

static INLINE void r600_store_context_reg(struct r600_command_buffer *cb, unsigned reg, unsigned value)
{
	r600_store_context_reg_seq(cb, reg, 1);
	r600_store_value(cb, value);
}

static INLINE void r600_store_ctl_const(struct r600_command_buffer *cb, unsigned reg, unsigned value)
{
	r600_store_ctl_const_seq(cb, reg, 1);
	r600_store_value(cb, value);
}

static INLINE void r600_store_loop_const(struct r600_command_buffer *cb, unsigned reg, unsigned value)
{
	r600_store_loop_const_seq(cb, reg, 1);
	r600_store_value(cb, value);
}

static INLINE void eg_store_loop_const(struct r600_command_buffer *cb, unsigned reg, unsigned value)
{
	eg_store_loop_const_seq(cb, reg, 1);
	r600_store_value(cb, value);
}

void r600_init_command_buffer(struct r600_command_buffer *cb, unsigned num_dw, enum r600_atom_flags flags);
void r600_release_command_buffer(struct r600_command_buffer *cb);

/*
 * Helpers for emitting state into a command stream directly.
 */

static INLINE unsigned r600_context_bo_reloc(struct r600_context *ctx, struct r600_resource *rbo,
					     enum radeon_bo_usage usage)
{
	assert(usage);
	return ctx->ws->cs_add_reloc(ctx->cs, rbo->cs_buf, usage, rbo->domains) * 4;
}

static INLINE void r600_write_value(struct radeon_winsys_cs *cs, unsigned value)
{
	cs->buf[cs->cdw++] = value;
}

static INLINE void r600_write_config_reg_seq(struct radeon_winsys_cs *cs, unsigned reg, unsigned num)
{
	assert(reg < R600_CONTEXT_REG_OFFSET);
	assert(cs->cdw+2+num <= RADEON_MAX_CMDBUF_DWORDS);
	cs->buf[cs->cdw++] = PKT3(PKT3_SET_CONFIG_REG, num, 0);
	cs->buf[cs->cdw++] = (reg - R600_CONFIG_REG_OFFSET) >> 2;
}

static INLINE void r600_write_context_reg_seq(struct radeon_winsys_cs *cs, unsigned reg, unsigned num)
{
	assert(reg >= R600_CONTEXT_REG_OFFSET && reg < R600_CTL_CONST_OFFSET);
	assert(cs->cdw+2+num <= RADEON_MAX_CMDBUF_DWORDS);
	cs->buf[cs->cdw++] = PKT3(PKT3_SET_CONTEXT_REG, num, 0);
	cs->buf[cs->cdw++] = (reg - R600_CONTEXT_REG_OFFSET) >> 2;
}

static INLINE void r600_write_ctl_const_seq(struct radeon_winsys_cs *cs, unsigned reg, unsigned num)
{
	assert(reg >= R600_CTL_CONST_OFFSET);
	assert(cs->cdw+2+num <= RADEON_MAX_CMDBUF_DWORDS);
	cs->buf[cs->cdw++] = PKT3(PKT3_SET_CTL_CONST, num, 0);
	cs->buf[cs->cdw++] = (reg - R600_CTL_CONST_OFFSET) >> 2;
}

static INLINE void r600_write_config_reg(struct radeon_winsys_cs *cs, unsigned reg, unsigned value)
{
	r600_write_config_reg_seq(cs, reg, 1);
	r600_write_value(cs, value);
}

static INLINE void r600_write_context_reg(struct radeon_winsys_cs *cs, unsigned reg, unsigned value)
{
	r600_write_context_reg_seq(cs, reg, 1);
	r600_write_value(cs, value);
}

static INLINE void r600_write_ctl_const(struct radeon_winsys_cs *cs, unsigned reg, unsigned value)
{
	r600_write_ctl_const_seq(cs, reg, 1);
	r600_write_value(cs, value);
}

/*
 * common helpers
 */
static INLINE uint32_t S_FIXED(float value, uint32_t frac_bits)
{
	return value * (1 << frac_bits);
}
#define ALIGN_DIVUP(x, y) (((x) + (y) - 1) / (y))

static inline unsigned r600_tex_aniso_filter(unsigned filter)
{
	if (filter <= 1)   return 0;
	if (filter <= 2)   return 1;
	if (filter <= 4)   return 2;
	if (filter <= 8)   return 3;
	 /* else */        return 4;
}

/* 12.4 fixed-point */
static INLINE unsigned r600_pack_float_12p4(float x)
{
	return x <= 0    ? 0 :
	       x >= 4096 ? 0xffff : x * 16;
}

static INLINE uint64_t r600_resource_va(struct pipe_screen *screen, struct pipe_resource *resource)
{
	struct r600_screen *rscreen = (struct r600_screen*)screen;
	struct r600_resource *rresource = (struct r600_resource*)resource;

	return rscreen->ws->buffer_get_virtual_address(rresource->cs_buf);
}

#endif
