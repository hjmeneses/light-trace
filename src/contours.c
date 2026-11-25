/*
Light Trace
Copyright (C) 2026 hjmeneses

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Marching squares orientado:
 *
 * 1. La imagen se rodea con un marco de ceros, así todos los contornos cierran.
 * 2. Cada celda de 2×2 muestras produce 0, 1 o 2 segmentos entre puntos de sus aristas
 *    (interpolados donde el valor cruza el umbral). Las sillas (dos esquinas opuestas
 *    adentro) se resuelven con el promedio de la celda.
 * 3. Cada segmento se orienta para dejar "adentro" a su izquierda (en coordenadas de
 *    pantalla, con y hacia abajo, eso es girar en un sentido fijo). Así cada arista del
 *    contorno tiene exactamente un segmento que sale de ella, y encadenarlos es buscar
 *    "el segmento que empieza donde terminó este" en una tabla hash.
 * 4. Se recorren las cadenas, se mide el largo, se descartan las muy cortas y se calcula la
 *    posición en el recorrido de cada punto.
 */

#include "contours.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { E_TOP, E_RIGHT, E_BOTTOM, E_LEFT };

struct seg {
	uint32_t a, b; // aristas de salida y de llegada
	float ax, ay, bx, by;
};

struct grid {
	const float *v;
	int w, h;   // imagen
	int gw, gh; // con el marco
	float thr;
};

static inline float sample(const struct grid *g, int x, int y)
{
	if (x < 1 || y < 1 || x > g->w || y > g->h)
		return 0.0f;
	return g->v[(size_t)(y - 1) * (size_t)g->w + (size_t)(x - 1)];
}

static inline uint32_t edge_id(const struct grid *g, int x, int y, int e)
{
	switch (e) {
	case E_TOP:
		return 2u * (uint32_t)(y * g->gw + x);
	case E_BOTTOM:
		return 2u * (uint32_t)((y + 1) * g->gw + x);
	case E_LEFT:
		return 2u * (uint32_t)(y * g->gw + x) + 1u;
	default: // E_RIGHT
		return 2u * (uint32_t)(y * g->gw + x + 1) + 1u;
	}
}

static inline float cross_t(float va, float vb, float thr)
{
	float t = (thr - va) / (vb - va);
	// Lejos de las esquinas, para que la orientación nunca quede en 0
	if (t < 1e-4f)
		t = 1e-4f;
	if (t > 1.0f - 1e-4f)
		t = 1.0f - 1e-4f;
	return t;
}

// Punto donde el valor cruza el umbral sobre la arista e de la celda (x, y)
static void edge_point(const float c[4], int x, int y, int e, float thr, float *px, float *py)
{
	// c: tl, tr, br, bl
	switch (e) {
	case E_TOP:
		*px = (float)x + cross_t(c[0], c[1], thr);
		*py = (float)y;
		break;
	case E_RIGHT:
		*px = (float)(x + 1);
		*py = (float)y + cross_t(c[1], c[2], thr);
		break;
	case E_BOTTOM:
		*px = (float)x + cross_t(c[3], c[2], thr);
		*py = (float)(y + 1);
		break;
	default: // E_LEFT
		*px = (float)x;
		*py = (float)y + cross_t(c[0], c[3], thr);
		break;
	}
}

// Esquina que comparten dos aristas contiguas (-1 si son opuestas). 0 tl, 1 tr, 2 br, 3 bl
static int shared_corner(int e1, int e2)
{
	int lo = e1 < e2 ? e1 : e2, hi = e1 < e2 ? e2 : e1;
	if (lo == E_TOP && hi == E_LEFT)
		return 0;
	if (lo == E_TOP && hi == E_RIGHT)
		return 1;
	if (lo == E_RIGHT && hi == E_BOTTOM)
		return 2;
	if (lo == E_BOTTOM && hi == E_LEFT)
		return 3;
	return -1;
}

/* ------------------------------------------------------------------ tabla hash arista → segmento */

struct map {
	uint32_t *keys;
	int32_t *vals;
	size_t mask;
};

static bool map_init(struct map *m, size_t n)
{
	size_t cap = 16;
	while (cap < n * 2)
		cap <<= 1;
	m->keys = malloc(cap * sizeof(uint32_t));
	m->vals = malloc(cap * sizeof(int32_t));
	if (!m->keys || !m->vals)
		return false;
	memset(m->vals, 0xff, cap * sizeof(int32_t)); // -1 = vacío
	m->mask = cap - 1;
	return true;
}

static inline size_t map_slot(const struct map *m, uint32_t key)
{
	return (size_t)((key * 2654435761u) ^ (key >> 16)) & m->mask;
}

static void map_put(struct map *m, uint32_t key, int32_t val)
{
	size_t i = map_slot(m, key);
	while (m->vals[i] >= 0 && m->keys[i] != key)
		i = (i + 1) & m->mask;
	m->keys[i] = key;
	m->vals[i] = val;
}

static int32_t map_get(const struct map *m, uint32_t key)
{
	size_t i = map_slot(m, key);
	while (m->vals[i] >= 0) {
		if (m->keys[i] == key)
			return m->vals[i];
		i = (i + 1) & m->mask;
	}
	return -1;
}

static void map_free(struct map *m)
{
	free(m->keys);
	free(m->vals);
}

/* ------------------------------------------------------------------ segmentos */

struct segs {
	struct seg *a;
	size_t n, cap;
};

static bool segs_push(struct segs *s, const struct seg *v)
{
	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 1024;
		struct seg *p = realloc(s->a, cap * sizeof(struct seg));
		if (!p)
			return false;
		s->a = p;
		s->cap = cap;
	}
	s->a[s->n++] = *v;
	return true;
}

static bool emit(struct segs *out, const struct grid *g, const float c[4], int x, int y, int e1, int e2)
{
	struct seg s;
	edge_point(c, x, y, e1, g->thr, &s.ax, &s.ay);
	edge_point(c, x, y, e2, g->thr, &s.bx, &s.by);
	s.a = edge_id(g, x, y, e1);
	s.b = edge_id(g, x, y, e2);

	// Esquina de referencia: la que comparten las aristas, o cualquiera de adentro
	static const float cx[4] = {0, 1, 1, 0}, cy[4] = {0, 0, 1, 1};
	int k = shared_corner(e1, e2);
	if (k < 0) {
		for (k = 0; k < 4; k++)
			if (c[k] >= g->thr)
				break;
	}
	float px = (float)x + cx[k], py = (float)y + cy[k];
	float cr = (s.bx - s.ax) * (py - s.ay) - (s.by - s.ay) * (px - s.ax);
	bool inside = c[k] >= g->thr;
	if ((cr > 0.0f) != inside) {
		uint32_t t = s.a;
		s.a = s.b;
		s.b = t;
		float fx = s.ax, fy = s.ay;
		s.ax = s.bx;
		s.ay = s.by;
		s.bx = fx;
		s.by = fy;
	}
	return segs_push(out, &s);
}

static bool march(const struct grid *g, struct segs *out)
{
	for (int y = 0; y < g->gh - 1; y++) {
		for (int x = 0; x < g->gw - 1; x++) {
			float c[4] = {sample(g, x, y), sample(g, x + 1, y), sample(g, x + 1, y + 1),
				      sample(g, x, y + 1)};
			int k = (c[0] >= g->thr ? 1 : 0) | (c[1] >= g->thr ? 2 : 0) | (c[2] >= g->thr ? 4 : 0) |
				(c[3] >= g->thr ? 8 : 0);
			bool ok = true;
			switch (k) {
			case 0:
			case 15:
				break;
			case 1:
			case 14:
				ok = emit(out, g, c, x, y, E_TOP, E_LEFT);
				break;
			case 2:
			case 13:
				ok = emit(out, g, c, x, y, E_TOP, E_RIGHT);
				break;
			case 4:
			case 11:
				ok = emit(out, g, c, x, y, E_RIGHT, E_BOTTOM);
				break;
			case 8:
			case 7:
				ok = emit(out, g, c, x, y, E_BOTTOM, E_LEFT);
				break;
			case 3:
			case 12:
				ok = emit(out, g, c, x, y, E_LEFT, E_RIGHT);
				break;
			case 6:
			case 9:
				ok = emit(out, g, c, x, y, E_TOP, E_BOTTOM);
				break;
			case 5:
			case 10: {
				// Silla: si el centro está adentro, las esquinas de adentro quedan unidas y
				// se recortan las de afuera; si no, se recortan las de adentro
				bool center = (c[0] + c[1] + c[2] + c[3]) * 0.25f >= g->thr;
				bool cut_tl_br = (k == 5) != center; // recortar tl y br (si no, tr y bl)
				if (cut_tl_br)
					ok = emit(out, g, c, x, y, E_TOP, E_LEFT) &&
					     emit(out, g, c, x, y, E_RIGHT, E_BOTTOM);
				else
					ok = emit(out, g, c, x, y, E_TOP, E_RIGHT) &&
					     emit(out, g, c, x, y, E_BOTTOM, E_LEFT);
				break;
			}
			}
			if (!ok)
				return false;
		}
	}
	return true;
}

/* ------------------------------------------------------------------ armado de contornos */

struct pts {
	struct lt_point *a;
	size_t n, cap;
};

static bool pts_push(struct pts *p, float x, float y)
{
	if (p->n == p->cap) {
		size_t cap = p->cap ? p->cap * 2 : 1024;
		struct lt_point *q = realloc(p->a, cap * sizeof(struct lt_point));
		if (!q)
			return false;
		p->a = q;
		p->cap = cap;
	}
	p->a[p->n].x = x;
	p->a[p->n].y = y;
	p->a[p->n].s = 0.0f;
	p->n++;
	return true;
}

static int cmp_contour(const void *pa, const void *pb)
{
	const struct lt_contour *a = pa, *b = pb;
	if (a->min_x != b->min_x)
		return a->min_x < b->min_x ? -1 : 1;
	if (a->min_y != b->min_y)
		return a->min_y < b->min_y ? -1 : 1;
	return 0;
}

// Rota el anillo pts[0..n) para que empiece en el punto más alto (y luego el de más a la izquierda)
static void rotate_to_top(struct lt_point *p, size_t n)
{
	size_t best = 0;
	for (size_t i = 1; i < n; i++) {
		if (p[i].y < p[best].y - 1e-4f || (fabsf(p[i].y - p[best].y) <= 1e-4f && p[i].x < p[best].x))
			best = i;
	}
	if (!best)
		return;
	struct lt_point *tmp = malloc(best * sizeof(struct lt_point));
	if (!tmp)
		return;
	memcpy(tmp, p, best * sizeof(struct lt_point));
	memmove(p, p + best, (n - best) * sizeof(struct lt_point));
	memcpy(p + (n - best), tmp, best * sizeof(struct lt_point));
	free(tmp);
}

bool lt_trace(const float *values, int w, int h, float threshold, float min_length, struct lt_contours *out)
{
	memset(out, 0, sizeof(*out));
	if (!values || w < 1 || h < 1)
		return true;

	struct grid g = {values, w, h, w + 2, h + 2, threshold};
	struct segs segs = {0};
	struct map map = {0};
	struct pts pts = {0};
	struct lt_contour *list = NULL;
	size_t num = 0, cap = 0, open_chains = 0;
	unsigned char *used = NULL;
	bool ok = false;

	if (!march(&g, &segs))
		goto done;
	if (!segs.n) {
		ok = true;
		goto done;
	}
	if (!map_init(&map, segs.n))
		goto done;
	for (size_t i = 0; i < segs.n; i++)
		map_put(&map, segs.a[i].a, (int32_t)i);

	used = calloc(segs.n, 1);
	if (!used)
		goto done;

	for (size_t i = 0; i < segs.n; i++) {
		if (used[i])
			continue;
		size_t first = pts.n;
		size_t cur = i;
		bool closed = false;
		for (;;) {
			used[cur] = 1;
			// Del marco a píxeles de la imagen: la muestra (gx, gy) es el centro del píxel (gx-1, gy-1)
			if (!pts_push(&pts, segs.a[cur].ax - 0.5f, segs.a[cur].ay - 0.5f))
				goto done;
			int32_t nx = map_get(&map, segs.a[cur].b);
			if (nx < 0)
				break;
			if ((size_t)nx == i) {
				closed = true;
				break;
			}
			if (used[nx])
				break;
			cur = (size_t)nx;
		}
		size_t n = pts.n - first;
		if (!closed) {
			open_chains++;
			pts.n = first;
			continue;
		}
		if (n < 3) {
			pts.n = first;
			continue;
		}

		struct lt_point *p = pts.a + first;
		float len = 0.0f, area = 0.0f;
		for (size_t k = 0; k < n; k++) {
			const struct lt_point *q0 = &p[k], *q1 = &p[(k + 1) % n];
			len += hypotf(q1->x - q0->x, q1->y - q0->y);
			area += q0->x * q1->y - q1->x * q0->y;
		}
		if (len < min_length) {
			pts.n = first;
			continue;
		}

		rotate_to_top(p, n);
		// Punto de cierre (igual al primero) y posición en el recorrido
		if (!pts_push(&pts, pts.a[first].x, pts.a[first].y))
			goto done;
		p = pts.a + first;
		float acc = 0.0f;
		float mnx = FLT_MAX, mny = FLT_MAX, mxx = -FLT_MAX, mxy = -FLT_MAX;
		for (size_t k = 0; k <= n; k++) {
			if (k)
				acc += hypotf(p[k].x - p[k - 1].x, p[k].y - p[k - 1].y);
			p[k].s = k == n ? 1.0f : acc / len;
			mnx = fminf(mnx, p[k].x);
			mny = fminf(mny, p[k].y);
			mxx = fmaxf(mxx, p[k].x);
			mxy = fmaxf(mxy, p[k].y);
		}

		if (num == cap) {
			size_t nc = cap ? cap * 2 : 32;
			struct lt_contour *q = realloc(list, nc * sizeof(struct lt_contour));
			if (!q)
				goto done;
			list = q;
			cap = nc;
		}
		struct lt_contour *c = &list[num++];
		c->first = first;
		c->count = n + 1;
		c->length = len;
		// Con la orientación elegida (y hacia abajo), los exteriores tienen área positiva
		c->hole = area < 0.0f;
		c->min_x = mnx;
		c->min_y = mny;
		c->max_x = mxx;
		c->max_y = mxy;
	}

	if (num > 1)
		qsort(list, num, sizeof(struct lt_contour), cmp_contour);
	ok = true;

done:
	free(segs.a);
	map_free(&map);
	free(used);
	if (ok) {
		out->points = pts.a;
		out->num_points = pts.n;
		out->list = list;
		out->num = num;
		out->open_chains = open_chains;
	} else {
		free(pts.a);
		free(list);
	}
	return ok;
}

void lt_contours_free(struct lt_contours *c)
{
	free(c->points);
	free(c->list);
	memset(c, 0, sizeof(*c));
}
