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

#include "util/u_memory.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_math.h"

#include "pipe/p_compiler.h"

#include "r600_pipe.h"
#include "r600_asm.h"
#include "r600_opcodes.h"

#include "opt_core.h"

struct vvec * vvec_create(unsigned initial_size)
{
	struct vvec * s = calloc(1, sizeof(struct vvec));

	if (!s)
		return NULL;

	vvec_set_size(s, initial_size);

	if (!s->keys) {
		free(s);
		return NULL;
	}
	return s;
}

struct vvec * vvec_create_clean(unsigned initial_size)
{
	struct vvec * s = calloc(1, sizeof(struct vvec));

	if (!s)
		return NULL;

	s->count =  initial_size;
	s->keys = calloc(s->count, sizeof(void*));

	if (!s->keys) {
		free(s);
		return NULL;
	}
	return s;
}

void vvec_append(struct vvec * s, void * key)
{
	vvec_set_size(s, s->count+1);
	s->keys[s->count-1] = key;
}

void vvec_set_size(struct vvec * s, unsigned new_size)
{
	if (new_size != s->count) {
		s->count = new_size;
		s->keys = realloc(s->keys, s->count * sizeof(void *));
	}
}

void vvec_clear(struct vvec * s)
{
	memset(s->keys, 0, sizeof(void*) * s->count);
}

boolean vvec_contains(struct vvec * s, void * key)
{
	int q;

	for(q=0; q<s->count;q++)
		if (s->keys[q]==key)
			return true;
	return false;
}

void vvec_destroy(struct vvec * s)
{
	if (s) {
		free(s->keys);
		free(s);
	}
}

struct vvec * vvec_createcopy(struct vvec * s)
{
	struct vvec * v = vvec_create(s->count);

	memcpy(v->keys, s->keys, s->count * sizeof(void*));
	return v;
}

struct vset* vset_create(unsigned initial_size)
{
	struct vset * s = malloc(sizeof(struct vset));

	if (!s)
		return NULL;
	s->size=initial_size > 0 ? initial_size : 1;
	s->count=0;
	s->keys = malloc(s->size * sizeof(void *));
	if (!s->keys) {
		free(s);
		return NULL;
	}
	return s;
}

void vset_destroy(struct vset * s)
{
	if (s) {
		free(s->keys);
		free(s);
	}
}

int vset_get_pos(struct vset * s, void * key)
{
	int a = 0, b = s->count, c;

	if (s->count<1)
		return 0;
	do {
		c = (a + b)>>1;
		if (c == s->count || s->keys[c] == key)
			return c;
		else if (s->keys[c] < key)
			a = c+1;
		else
			b = c;
	} while (b!=a);
	return a;
}

boolean vset_contains(struct vset * s, void * key)
{
	int p;

	if (!s->count)
		return false;
	p = vset_get_pos(s, key);
	return p < s->count && s->keys[p] == key;
}

boolean vset_containsvec(struct vset * s, struct vvec * v)
{
	int q;

	if (!v)
		return true;
	for(q=0;q<v->count;q++)
		if (v->keys[q]!=NULL && !vset_contains(s,v->keys[q]))
			return false;
	return true;
}

boolean vset_containsset(struct vset * s, struct vset * c)
{
	int q;

	if (!c)
		return true;
	for(q=0;q<c->count;q++)
		if (!vset_contains(s,c->keys[q]))
			return false;

	return true;
}

boolean vset_intersects(struct vset * s, struct vset * c)
{
	int q;

	if (!c)
		return true;
	for(q=0;q<c->count;q++)
		if (vset_contains(s,c->keys[q]))
			return true;

	return false;
}

void vset_resize(struct vset * s)
{
	int new_size=s->size;

	while (s->count > new_size)
		new_size *= 2;

	if (new_size!=s->size) {
		s->size = new_size;
		s->keys = realloc(s->keys, s->size * sizeof(void *));
		assert(s->keys);
	}
}

boolean vset_add(struct vset * s, void * key)
{
	int p = vset_get_pos(s, key);

	if (p == s->count || s->keys[p] != key) {
		s->count++;
		vset_resize(s);
		if (s->count > p+1)
			memmove(&s->keys[p+1], &s->keys[p], (s->count-1-p)*sizeof(void *));
		s->keys[p] = key;
		return true;
	}
	return false;
}

boolean vset_remove(struct vset * s, void * key)
{
	int p;

	if (!s->count)
		return false;
	p = vset_get_pos(s, key);

	if (p == s->count)
		return false;

	if (s->keys[p] == key) {
		if (s->count > p+1)
			memmove(&s->keys[p], &s->keys[p+1], (s->count-1-p)*sizeof(void *));
		s->count--;
		return true;
	}
	return false;
}

void vset_addset(struct vset * s, struct vset * from)
{
	int p1 = 0, p2 = 0;
	uintptr_t k1,k2;
	int need_insert = 1;

	while (p2 < from->count && p1 < s->count ) {
		k1 = (uintptr_t)s->keys[p1];
		k2 = (uintptr_t)from->keys[p2];

		if (k1<k2)
			p1++;
		else if (k1 == k2) {
			p1++;
			p2++;
			if (p2>=from->count)
				need_insert = 0;
		} else if (k1>k2) {
			break;
		}
	}

	if (need_insert) {

		int maxsize = from->count + s->count;
		void ** nkeys = malloc(maxsize * sizeof(void*));
		int p = p1;

		if (p)
			memcpy(nkeys, s->keys, p * sizeof(void*));

		while (p2 < from->count && p1 < s->count) {
			k1 = (uintptr_t)s->keys[p1];
			k2 = (uintptr_t)from->keys[p2];

			if (k1<k2) {
				nkeys[p] = (void*)k1;
				p1++;
			} else if (k1 == k2) {
				nkeys[p] = (void*)k1;
				p1++;
				p2++;
			} else if (k1>k2) {
				nkeys[p] = (void*)k2;
				p2++;
			}

			p++;
		}

		while (p1 < s->count) {
			nkeys[p++] = s->keys[p1++];
		}
		while (p2 < from->count) {
			nkeys[p++] = from->keys[p2++];
		}

		free(s->keys);
		s->keys = nkeys;
		s->count = p;
		s->size = maxsize;
		assert(p<=maxsize);

	}
}


void vset_addvec(struct vset * s, struct vvec * from)
{
	int q;
	for (q=0; q<from->count; q++)
		if (from->keys[q] != NULL)
			vset_add(s,from->keys[q]);
}

boolean vset_removeset(struct vset * s, struct vset * from)
{
	int q;
	boolean result = false;

	for (q=0; q<from->count; q++)
		result |= vset_remove(s,from->keys[q]);

	return result;
}

boolean vset_removevec(struct vset * s, struct vvec * from)
{
	int q;
	boolean result = false;

	for (q=0; q<from->count; q++)
		if (from->keys[q]!=NULL)
			result |= vset_remove(s,from->keys[q]);

	return result;
}


void vset_clear(struct vset * s)
{
	s->count=0;
}

void vset_copy(struct vset * s, struct vset * from)
{
	if (!from) {
		vset_clear(s);
		return;
	}

	s->count=from->count;
	vset_resize(s);
	memcpy(s->keys, from->keys, s->count * sizeof(void *));
}

struct vset * vset_createcopy(struct vset * from)
{
	struct vset *s = vset_create(from->count);
	vset_copy(s, from);
	return s;
}

struct vmap* vmap_create(unsigned initial_size)
{
	struct vmap * s = malloc(sizeof(struct vmap));

	if (!s)
		return NULL;

	s->size=initial_size > 0 ? initial_size : 1;
	s->count=0;
	s->keys = malloc(s->size * sizeof(void *)*2);

	if (!s->keys) {
		free(s);
		return NULL;
	}
	return s;
}

void vmap_clear(struct vmap * s)
{
	s->count = 0;
}


void vmap_destroy(struct vmap * s)
{
	if (s) {
		free(s->keys);
		free(s);
	}
}

void vmap_copy(struct vmap * m, struct vmap * from)
{
	m->count=from->count;
	vmap_resize(m);
	memcpy(m->keys,from->keys, 2 * m->count * sizeof(void*));
}


int vmap_get_pos(struct vmap * s, void * key)
{
	int a = 0, b = s->count, c;

	if (s->count<1)
		return 0;
	do {
		c = (a + b)>>1;
		if (c == s->count || s->keys[c*2] == key)
			return c;
		else if (s->keys[c*2] < key)
			a = c+1;
		else
			b = c;
	} while (b!=a);
	return a;
}

boolean vmap_contains(struct vmap * s, void * key)
{
	int p;
	if (!s->count)
		return false;
	p = vmap_get_pos(s, key);

	return p < s->count && s->keys[p*2] == key;
}

void vmap_resize(struct vmap * s)
{
	int new_size=s->size;
	while (s->count > new_size)
		new_size *= 2;

	if (new_size!=s->size) {
		s->size = new_size;
		s->keys = realloc(s->keys, 2 * s->size * sizeof(void *));
		assert(s->keys);
	}
}

boolean vmap_set(struct vmap * s, void * key, void * data)
{
	boolean result = false;
	int p = vmap_get_pos(s, key);

	if (p == s->count || s->keys[p*2] != key) {
		s->count++;
		vmap_resize(s);
		if (s->count > p+1)
			memmove(&s->keys[(p+1)*2], &s->keys[p*2], (s->count-1-p)*sizeof(void *)*2);
		s->keys[p*2] = key;
		result = true;
	}
	s->keys[p*2+1] = data;
	return result;
}

boolean vmap_get(struct vmap * s, void * key, void ** data)
{
	int p;

	if (!s->count)
		return false;

	p = vmap_get_pos(s, key);

	if (p < s->count && s->keys[p*2] == key) {
		*data = s->keys[p*2+1];
		return true;
	}
	return false;
}

boolean vmap_remove(struct vmap * s, void * key)
{
	int p;

	if (!s->count)
		return false;

	p = vmap_get_pos(s, key);

	if (p == s->count)
		return false;

	if (s->keys[p*2] == key) {
		if (s->count > p+1)
			memmove(&s->keys[p*2], &s->keys[(p+1)*2], (s->count-1-p)*sizeof(void *)*2);
		s->count--;
		return true;
	}
	return false;
}

struct vmap * vmap_createcopy(struct vmap * s) {
	struct vmap * n = vmap_create(s->count);
	n->count = s->count;
	memcpy(n->keys,s->keys,n->count * sizeof(void *) * 2);
	return n;
}


struct vque* vque_create(unsigned initial_size)
{
	struct vque * s = malloc(sizeof(struct vque));

	if (!s)
		return NULL;
	s->size=initial_size > 0 ? initial_size : 1;
	s->count=0;
	s->keys = malloc(s->size * sizeof(void *)*2);
	if (!s->keys) {
		free(s);
		return NULL;
	}
	return s;
}

void vque_destroy(struct vque * s)
{
	if (s) {
		free(s->keys);
		free(s);
	}
}

int vque_get_pos(struct vque * s, uintptr_t pri)
{
	void * key = (void*)pri;
	int a = 0, b = s->count, c;

	if (s->count<1)
		return 0;
	do {
		c = (a + b)>>1;
		if (c == s->count)
			return c;
		else if (s->keys[c*2] == key && (c==s->count-1 || s->keys[(c+1)*2] > key))
			return c+1;
		else if (s->keys[c*2] <= key)
			a = c+1;
		else
			b = c;
	} while (b!=a);
	return a;
}

void vque_resize(struct vque * s)
{
	int new_size=s->size;
	while (s->count > new_size)
		new_size *= 2;

	if (new_size!=s->size) {
		s->size = new_size;
		s->keys = realloc(s->keys, s->size * sizeof(void *)*2);
		assert(s->keys);
	}
}

void vque_enqueue(struct vque * s,  uintptr_t  pri, void * data)
{
	void * key = (void*)pri;
	int p = vque_get_pos(s, pri);

	s->count++;
	vque_resize(s);

	if (s->count > p+1)
		memmove(&s->keys[(p+1)*2], &s->keys[p*2], (s->count-1-p)*sizeof(void *)*2);

	s->keys[p*2] = key;
	s->keys[p*2+1] = data;
}

boolean vque_dequeue(struct vque * s, void ** data)
{
	int p;

	if (!s->count)
		return false;

	p = --s->count;
	*data = s->keys[p*2+1];
	return true;
}

