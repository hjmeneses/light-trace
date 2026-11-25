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
 * Filtro "Trazo de luz" para imágenes y textos.
 *
 * Cuando la fuente cambia (señal "update", cambio de tamaño o el botón "Volver a trazar"),
 * se dibuja una vez en una textura, se copia a la CPU y se trazan sus contornos cerrados
 * (contours.c). Con eso se arma una cinta de triángulos por contorno (vertex buffer estático).
 *
 * Cada cuadro:
 *   1. Se dibujan las cintas en una textura (núcleo en rgb, aporte al resplandor en a), con
 *      mezcla MAX para que los dobleces de la cinta en las esquinas no sumen dos veces.
 *   2. Se desenfoca el canal a (gaussiano separable): el resplandor.
 *   3. Se dibuja la fuente y encima, sumando, el núcleo más el resplandor.
 * Si "Ampliar el lienzo" está activo, el filtro crece unos px por lado para que el resplandor
 * no se recorte en el borde de la fuente.
 */

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/threading.h>
#include <plugin-support.h>

#include <math.h>
#include <string.h>

#include "contours.h"

#define S_CONTOUR_FROM "contour_from"
#define S_THRESHOLD "threshold"
#define S_MIN_LENGTH "min_length"
#define S_SHOW_CONTOURS "show_contours"
#define S_RETRACE "retrace"
#define S_UNSUPPORTED "unsupported"

#define S_AUTO_PLAY "auto_play"
#define S_DURATION "duration"
#define S_PROGRESS "progress"
#define S_STAGGER "stagger"
#define S_TRAIL "trail"
#define S_RESIDUAL "residual"
#define S_LINE_WIDTH "line_width"
#define S_GLOW_SIZE "glow_size"
#define S_GLOW_STRENGTH "glow_strength"
#define S_CORE_COLOR "core_color"
#define S_GLOW_COLOR "glow_color"
#define S_EXPAND "expand"

#define BLUR_TAPS 32 // muestras por lado en el shader

enum { FROM_ALPHA = 0, FROM_BRIGHTNESS = 1 };

struct light_trace {
	obs_source_t *context;
	obs_source_t *parent; // fuente a cuya señal "update" estamos conectados (sin referencia)
	volatile bool dirty;  // hay que volver a trazar
	bool ribbons_dirty;   // hay que rearmar las cintas (cambió el grosor)
	bool supported;

	// Trazado
	int contour_from;
	float threshold;
	float min_length;
	bool show_contours;

	// Animación
	bool auto_play;
	float duration;
	float progress;
	float stagger;
	float clock; // 0..1 en bucle

	// Luz
	float trail;
	float residual;
	float line_width;
	float glow_size;
	float glow_strength;
	struct vec4 core_color;
	struct vec4 glow_color;
	bool expand;

	gs_effect_t *effect;
	gs_texrender_t *capture;
	gs_stagesurf_t *stage;
	gs_texrender_t *core_tex, *blur_a, *blur_b;
	uint32_t traced_w, traced_h;

	struct lt_contours contours;
	gs_vertbuffer_t *lines;   // contornos como líneas (prueba)
	gs_vertbuffer_t *ribbons; // contornos como cintas
};

static const char *lt_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LightTrace");
}

/* ------------------------------------------------------------------ fuente de origen */

static bool kind_supported(obs_source_t *parent)
{
	if (!parent)
		return false;
	const char *id = obs_source_get_unversioned_id(parent);
	return id && (strcmp(id, "image_source") == 0 || strcmp(id, "text_gdiplus") == 0 ||
		      strcmp(id, "text_ft2_source") == 0);
}

static void parent_updated(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct light_trace *lt = data;
	os_atomic_set_bool(&lt->dirty, true);
}

static void attach_parent(struct light_trace *lt, obs_source_t *parent)
{
	if (lt->parent == parent)
		return;
	if (lt->parent)
		signal_handler_disconnect(obs_source_get_signal_handler(lt->parent), "update", parent_updated, lt);
	lt->parent = parent;
	if (parent)
		signal_handler_connect(obs_source_get_signal_handler(parent), "update", parent_updated, lt);
	lt->supported = kind_supported(parent);
	os_atomic_set_bool(&lt->dirty, true);
}

/* ------------------------------------------------------------------ medidas */

// Medio ancho de la cinta: el núcleo y un px de suavizado
static float ribbon_half_width(const struct light_trace *lt)
{
	return lt->line_width * 0.5f + 1.5f;
}

static float glow_sigma(const struct light_trace *lt)
{
	return lt->glow_size * 0.5f;
}

// Margen por lado para que el resplandor no se recorte
static uint32_t padding(const struct light_trace *lt)
{
	if (!lt->expand || !lt->supported)
		return 0;
	return (uint32_t)ceilf(ribbon_half_width(lt) + 3.0f * glow_sigma(lt));
}

static uint32_t lt_get_width(void *data)
{
	struct light_trace *lt = data;
	obs_source_t *target = obs_filter_get_target(lt->context);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	return w ? w + 2 * padding(lt) : 0;
}

static uint32_t lt_get_height(void *data)
{
	struct light_trace *lt = data;
	obs_source_t *target = obs_filter_get_target(lt->context);
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	return h ? h + 2 * padding(lt) : 0;
}

/* ------------------------------------------------------------------ ciclo de vida */

static void lt_update(void *data, obs_data_t *settings)
{
	struct light_trace *lt = data;

	int contour_from = (int)obs_data_get_int(settings, S_CONTOUR_FROM);
	float threshold = (float)obs_data_get_double(settings, S_THRESHOLD);
	float min_length = (float)obs_data_get_double(settings, S_MIN_LENGTH);
	if (contour_from != lt->contour_from || threshold != lt->threshold || min_length != lt->min_length)
		os_atomic_set_bool(&lt->dirty, true);
	lt->contour_from = contour_from;
	lt->threshold = threshold;
	lt->min_length = min_length;
	lt->show_contours = obs_data_get_bool(settings, S_SHOW_CONTOURS);

	lt->auto_play = obs_data_get_bool(settings, S_AUTO_PLAY);
	lt->duration = (float)obs_data_get_double(settings, S_DURATION);
	lt->progress = (float)obs_data_get_double(settings, S_PROGRESS);
	lt->stagger = (float)obs_data_get_double(settings, S_STAGGER);

	float line_width = (float)obs_data_get_double(settings, S_LINE_WIDTH);
	if (line_width != lt->line_width)
		lt->ribbons_dirty = true;
	lt->line_width = line_width;
	lt->trail = (float)obs_data_get_double(settings, S_TRAIL);
	lt->residual = (float)obs_data_get_double(settings, S_RESIDUAL);
	lt->glow_size = (float)obs_data_get_double(settings, S_GLOW_SIZE);
	lt->glow_strength = (float)obs_data_get_double(settings, S_GLOW_STRENGTH);
	vec4_from_rgba(&lt->core_color, (uint32_t)obs_data_get_int(settings, S_CORE_COLOR));
	vec4_from_rgba(&lt->glow_color, (uint32_t)obs_data_get_int(settings, S_GLOW_COLOR));
	lt->expand = obs_data_get_bool(settings, S_EXPAND);
}

static void *lt_create(obs_data_t *settings, obs_source_t *source)
{
	struct light_trace *lt = bzalloc(sizeof(struct light_trace));
	lt->context = source;
	lt->dirty = true;
	lt->contour_from = -1;

	char *path = obs_module_file("effects/light-trace.effect");
	obs_enter_graphics();
	lt->effect = gs_effect_create_from_file(path, NULL);
	obs_leave_graphics();
	if (!lt->effect)
		obs_log(LOG_ERROR, "could not load %s", path ? path : "effects/light-trace.effect");
	bfree(path);

	lt_update(lt, settings);
	return lt;
}

static void lt_destroy(void *data)
{
	struct light_trace *lt = data;
	attach_parent(lt, NULL);
	obs_enter_graphics();
	gs_effect_destroy(lt->effect);
	gs_texrender_destroy(lt->capture);
	gs_texrender_destroy(lt->core_tex);
	gs_texrender_destroy(lt->blur_a);
	gs_texrender_destroy(lt->blur_b);
	gs_stagesurface_destroy(lt->stage);
	gs_vertexbuffer_destroy(lt->lines);
	gs_vertexbuffer_destroy(lt->ribbons);
	obs_leave_graphics();
	lt_contours_free(&lt->contours);
	bfree(lt);
}

static void lt_filter_add(void *data, obs_source_t *parent)
{
	attach_parent(data, parent);
}

static void lt_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	attach_parent(data, NULL);
}

static void lt_tick(void *data, float seconds)
{
	struct light_trace *lt = data;
	attach_parent(lt, obs_filter_get_parent(lt->context));

	obs_source_t *target = obs_filter_get_target(lt->context);
	if (target) {
		uint32_t w = obs_source_get_base_width(target), h = obs_source_get_base_height(target);
		if (w != lt->traced_w || h != lt->traced_h)
			os_atomic_set_bool(&lt->dirty, true);
	}

	if (lt->auto_play && lt->duration > 0.0f) {
		lt->clock += seconds / lt->duration;
		lt->clock -= floorf(lt->clock);
	}
}

/* ------------------------------------------------------------------ geometría */

static uint32_t hue_color(float hue, float value)
{
	// HSV con saturación 0.8 a 0xAABBGGRR
	float h = (hue - floorf(hue)) * 6.0f;
	float c = value * 0.8f, x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f)), m = value - c;
	float r, g, b;
	if (h < 1)
		r = c, g = x, b = 0;
	else if (h < 2)
		r = x, g = c, b = 0;
	else if (h < 3)
		r = 0, g = c, b = x;
	else if (h < 4)
		r = 0, g = x, b = c;
	else if (h < 5)
		r = x, g = 0, b = c;
	else
		r = c, g = 0, b = x;
	uint32_t R = (uint32_t)((r + m) * 255.0f + 0.5f), G = (uint32_t)((g + m) * 255.0f + 0.5f),
		 B = (uint32_t)((b + m) * 255.0f + 0.5f);
	return 0xFF000000u | (B << 16) | (G << 8) | R;
}

// Líneas de prueba: cada contorno de un color, de oscuro a claro según el recorrido
static void build_lines(struct light_trace *lt)
{
	gs_vertexbuffer_destroy(lt->lines);
	lt->lines = NULL;
	if (!lt->contours.num_points)
		return;

	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = lt->contours.num_points;
	vbd->points = bmalloc(sizeof(struct vec3) * vbd->num);
	vbd->colors = bmalloc(sizeof(uint32_t) * vbd->num);
	for (size_t i = 0; i < lt->contours.num; i++) {
		const struct lt_contour *c = &lt->contours.list[i];
		float hue = (float)i * 0.618034f;
		for (size_t k = 0; k < c->count; k++) {
			const struct lt_point *p = &lt->contours.points[c->first + k];
			vec3_set(&vbd->points[c->first + k], p->x, p->y, 0.0f);
			vbd->colors[c->first + k] = hue_color(hue, 0.25f + 0.75f * p->s);
		}
	}
	lt->lines = gs_vertexbuffer_create(vbd, 0);
}

/*
 * Cintas: por cada punto, dos vértices a ±r sobre la normal. La normal sale de los vecinos a
 * dos puntos de distancia (el contorno es cerrado), para suavizar los escalones del trazado.
 */
static void build_ribbons(struct light_trace *lt)
{
	gs_vertexbuffer_destroy(lt->ribbons);
	lt->ribbons = NULL;
	lt->ribbons_dirty = false;
	if (!lt->contours.num_points)
		return;

	const float r = ribbon_half_width(lt);
	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = lt->contours.num_points * 2;
	vbd->points = bmalloc(sizeof(struct vec3) * vbd->num);
	vbd->num_tex = 1;
	vbd->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	vbd->tvarray[0].width = 4;
	vbd->tvarray[0].array = bmalloc(sizeof(struct vec4) * vbd->num);
	struct vec4 *tv = vbd->tvarray[0].array;

	for (size_t i = 0; i < lt->contours.num; i++) {
		const struct lt_contour *c = &lt->contours.list[i];
		const struct lt_point *p = lt->contours.points + c->first;
		const size_t n = c->count - 1; // puntos distintos (el último repite el primero)
		for (size_t k = 0; k < c->count; k++) {
			size_t kk = k % n;
			const struct lt_point *a = &p[(kk + n - 2) % n], *b = &p[(kk + 2) % n];
			float tx = b->x - a->x, ty = b->y - a->y;
			float l = sqrtf(tx * tx + ty * ty);
			float nx = l > 0.0f ? -ty / l : 0.0f, ny = l > 0.0f ? tx / l : 0.0f;

			size_t v = (c->first + k) * 2;
			vec3_set(&vbd->points[v], p[k].x + nx * r, p[k].y + ny * r, 0.0f);
			vec3_set(&vbd->points[v + 1], p[k].x - nx * r, p[k].y - ny * r, 0.0f);
			vec4_set(&tv[v], p[k].s, r, c->length, (float)i);
			vec4_set(&tv[v + 1], p[k].s, -r, c->length, (float)i);
		}
	}
	lt->ribbons = gs_vertexbuffer_create(vbd, 0);
}

/* ------------------------------------------------------------------ trazado */

// Dibuja la fuente en una textura, la copia a la CPU y traza los contornos
static void retrace(struct light_trace *lt, uint32_t w, uint32_t h)
{
	uint64_t t0 = os_gettime_ns();

	if (!lt->capture)
		lt->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_texrender_reset(lt->capture);

	if (!obs_source_process_filter_begin(lt->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;
	if (!gs_texrender_begin(lt->capture, w, h)) {
		obs_source_process_filter_end(lt->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
		return;
	}
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_process_filter_end(lt->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
	gs_blend_state_pop();
	gs_texrender_end(lt->capture);

	if (lt->stage && (gs_stagesurface_get_width(lt->stage) != w || gs_stagesurface_get_height(lt->stage) != h)) {
		gs_stagesurface_destroy(lt->stage);
		lt->stage = NULL;
	}
	if (!lt->stage)
		lt->stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (!lt->stage)
		return;
	gs_stage_texture(lt->stage, gs_texrender_get_texture(lt->capture));

	uint8_t *px;
	uint32_t linesize;
	if (!gs_stagesurface_map(lt->stage, &px, &linesize))
		return;
	float *values = bmalloc(sizeof(float) * w * h);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = px + (size_t)y * linesize;
		float *out = values + (size_t)y * w;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *p = row + x * 4;
			float a = p[3] / 255.0f;
			if (lt->contour_from == FROM_BRIGHTNESS)
				out[x] = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f * a;
			else
				out[x] = a;
		}
	}
	gs_stagesurface_unmap(lt->stage);

	lt_contours_free(&lt->contours);
	if (!lt_trace(values, (int)w, (int)h, lt->threshold, lt->min_length, &lt->contours))
		obs_log(LOG_WARNING, "out of memory tracing %ux%u", w, h);
	bfree(values);
	build_lines(lt);
	build_ribbons(lt);

	lt->traced_w = w;
	lt->traced_h = h;
	os_atomic_set_bool(&lt->dirty, false);

	size_t holes = 0;
	for (size_t i = 0; i < lt->contours.num; i++)
		holes += lt->contours.list[i].hole;
	obs_log(LOG_INFO, "'%s': %zu contours (%zu holes), %zu points, %ux%u in %.1f ms%s",
		obs_source_get_name(lt->parent), lt->contours.num, holes, lt->contours.num_points, w, h,
		(double)(os_gettime_ns() - t0) / 1e6, lt->contours.open_chains ? " (open chains dropped)" : "");
}

/* ------------------------------------------------------------------ dibujo */

static void set_float(gs_effect_t *e, const char *name, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(e, name), v);
}

static gs_texrender_t *ensure_texrender(gs_texrender_t **tr)
{
	if (!*tr)
		*tr = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	gs_texrender_reset(*tr);
	return *tr;
}

// Posición de la cabeza que entiende el shader
static float head_position(const struct light_trace *lt)
{
	if (lt->auto_play)
		return lt->clock;
	// Una pasada: con progreso 1 todas las cabezas y sus estelas ya terminaron
	float n = lt->contours.num ? (float)(lt->contours.num - 1) : 0.0f;
	return lt->progress * (1.0f + lt->trail + lt->stagger * n);
}

// Paso 1: cintas a una textura (núcleo en rgb, aporte al resplandor en a)
static bool render_core(struct light_trace *lt, uint32_t cw, uint32_t ch, float pad)
{
	gs_texrender_t *tr = ensure_texrender(&lt->core_tex);
	if (!gs_texrender_begin(tr, cw, ch))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
	gs_matrix_push();
	gs_matrix_translate3f(pad, pad, 0.0f);

	gs_effect_t *e = lt->effect;
	set_float(e, "head", head_position(lt));
	set_float(e, "looping", lt->auto_play ? 1.0f : 0.0f);
	set_float(e, "stagger", lt->stagger);
	set_float(e, "trail", lt->trail);
	set_float(e, "residual", lt->residual);
	set_float(e, "line_width", lt->line_width);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, "core_color"), &lt->core_color);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	gs_blend_op(GS_BLEND_OP_MAX);
	gs_load_vertexbuffer(lt->ribbons);
	gs_load_indexbuffer(NULL);
	while (gs_effect_loop(e, "Ribbon")) {
		for (size_t i = 0; i < lt->contours.num; i++) {
			const struct lt_contour *c = &lt->contours.list[i];
			gs_draw(GS_TRISTRIP, (uint32_t)(c->first * 2), (uint32_t)(c->count * 2));
		}
	}
	gs_load_vertexbuffer(NULL);
	gs_blend_op(GS_BLEND_OP_ADD);
	gs_blend_state_pop();

	gs_matrix_pop();
	gs_texrender_end(tr);
	return true;
}

// Paso 2: una pasada del desenfoque de src a dst
static gs_texture_t *blur_pass(struct light_trace *lt, gs_texture_t *src, gs_texrender_t **dst, uint32_t cw,
			       uint32_t ch, float dx, float dy)
{
	gs_texrender_t *tr = ensure_texrender(dst);
	if (!gs_texrender_begin(tr, cw, ch))
		return NULL;
	gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);

	gs_effect_t *e = lt->effect;
	float sigma = glow_sigma(lt);
	struct vec2 texel, dir;
	vec2_set(&texel, 1.0f / (float)cw, 1.0f / (float)ch);
	vec2_set(&dir, dx, dy);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), src);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "texel"), &texel);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "blur_dir"), &dir);
	set_float(e, "sigma", sigma);
	set_float(e, "blur_step", fmaxf(3.0f * sigma / BLUR_TAPS, 0.25f));

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	while (gs_effect_loop(e, "Blur"))
		gs_draw_sprite(src, 0, cw, ch);
	gs_blend_state_pop();

	gs_texrender_end(tr);
	return gs_texrender_get_texture(tr);
}

static void draw_lines(struct light_trace *lt)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &white);

	gs_load_vertexbuffer(lt->lines);
	gs_load_indexbuffer(NULL);
	while (gs_effect_loop(solid, "SolidColored")) {
		for (size_t i = 0; i < lt->contours.num; i++) {
			const struct lt_contour *c = &lt->contours.list[i];
			gs_draw(GS_LINESTRIP, (uint32_t)c->first, (uint32_t)c->count);
		}
	}
	gs_load_vertexbuffer(NULL);
}

static void lt_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct light_trace *lt = data;
	obs_source_t *target = obs_filter_get_target(lt->context);
	if (!lt->supported || !target || !lt->effect) {
		obs_source_skip_video_filter(lt->context);
		return;
	}
	uint32_t w = obs_source_get_base_width(target), h = obs_source_get_base_height(target);
	if (!w || !h) {
		obs_source_skip_video_filter(lt->context);
		return;
	}

	if (os_atomic_load_bool(&lt->dirty))
		retrace(lt, w, h);
	if (lt->ribbons_dirty)
		build_ribbons(lt);

	uint32_t pad = padding(lt);
	uint32_t cw = w + 2 * pad, ch = h + 2 * pad;

	// Luz: núcleo y resplandor, antes de dibujar la fuente (cambian el destino de dibujo)
	gs_texture_t *core = NULL, *glow = NULL;
	if (lt->ribbons && render_core(lt, cw, ch, (float)pad)) {
		core = gs_texrender_get_texture(lt->core_tex);
		glow = core;
		if (lt->glow_size > 0.0f && lt->glow_strength > 0.0f) {
			gs_texture_t *t = blur_pass(lt, core, &lt->blur_a, cw, ch, 1.0f, 0.0f);
			if (t)
				t = blur_pass(lt, t, &lt->blur_b, cw, ch, 0.0f, 1.0f);
			if (t)
				glow = t;
		}
	}

	// La fuente tal cual (si ya se dibujó para trazar, OBS reutiliza esa textura)
	gs_matrix_push();
	gs_matrix_translate3f((float)pad, (float)pad, 0.0f);
	if (obs_source_process_filter_begin(lt->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		obs_source_process_filter_end(lt->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
	gs_matrix_pop();

	if (core) {
		gs_effect_t *e = lt->effect;
		float sigma = glow_sigma(lt);
		// Un trazo de ancho lw desenfocado baja su pico a lw / (sqrt(2π)·σ); se compensa
		float gain = glow == core ? 0.0f : lt->glow_strength * 2.5066f * sigma / (lt->line_width + 1.0f);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), core);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "glow"), glow);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, "glow_color"), &lt->glow_color);
		set_float(e, "glow_gain", gain);

		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(e, "Composite"))
			gs_draw_sprite(core, 0, cw, ch);
		gs_blend_state_pop();
	}

	if (lt->show_contours && lt->lines) {
		gs_matrix_push();
		gs_matrix_translate3f((float)pad, (float)pad, 0.0f);
		draw_lines(lt);
		gs_matrix_pop();
	}
}

/* ------------------------------------------------------------------ propiedades */

static bool retrace_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct light_trace *lt = data;
	os_atomic_set_bool(&lt->dirty, true);
	return false;
}

static bool auto_play_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool loop = obs_data_get_bool(settings, S_AUTO_PLAY);
	obs_property_set_visible(obs_properties_get(props, S_DURATION), loop);
	obs_property_set_visible(obs_properties_get(props, S_PROGRESS), !loop);
	return true;
}

static obs_properties_t *lt_properties(void *data)
{
	struct light_trace *lt = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	if (lt && !lt->supported) {
		p = obs_properties_add_text(props, S_UNSUPPORTED, obs_module_text("Unsupported"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
	}

	// Animación
	obs_properties_t *anim = obs_properties_create();
	p = obs_properties_add_bool(anim, S_AUTO_PLAY, obs_module_text("AutoPlay"));
	obs_property_set_modified_callback(p, auto_play_changed);
	p = obs_properties_add_float_slider(anim, S_DURATION, obs_module_text("Duration"), 0.5, 30.0, 0.1);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_float_slider(anim, S_PROGRESS, obs_module_text("Progress"), 0.0, 1.0, 0.001);
	obs_property_set_long_description(p, obs_module_text("Progress.Description"));
	p = obs_properties_add_float_slider(anim, S_STAGGER, obs_module_text("Stagger"), 0.0, 0.5, 0.01);
	obs_property_set_long_description(p, obs_module_text("Stagger.Description"));
	obs_properties_add_group(props, "group_anim", obs_module_text("Group.Animation"), OBS_GROUP_NORMAL, anim);

	// Luz
	obs_properties_t *light = obs_properties_create();
	p = obs_properties_add_float_slider(light, S_TRAIL, obs_module_text("Trail"), 0.02, 1.0, 0.01);
	obs_property_set_long_description(p, obs_module_text("Trail.Description"));
	obs_properties_add_float_slider(light, S_RESIDUAL, obs_module_text("Residual"), 0.0, 1.0, 0.01);
	p = obs_properties_add_float_slider(light, S_LINE_WIDTH, obs_module_text("LineWidth"), 0.5, 12.0, 0.1);
	obs_property_float_set_suffix(p, " px");
	p = obs_properties_add_float_slider(light, S_GLOW_SIZE, obs_module_text("GlowSize"), 0.0, 60.0, 0.5);
	obs_property_float_set_suffix(p, " px");
	obs_properties_add_float_slider(light, S_GLOW_STRENGTH, obs_module_text("GlowStrength"), 0.0, 4.0, 0.05);
	obs_properties_add_color(light, S_CORE_COLOR, obs_module_text("CoreColor"));
	obs_properties_add_color(light, S_GLOW_COLOR, obs_module_text("GlowColor"));
	p = obs_properties_add_bool(light, S_EXPAND, obs_module_text("Expand"));
	obs_property_set_long_description(p, obs_module_text("Expand.Description"));
	obs_properties_add_group(props, "group_light", obs_module_text("Group.Light"), OBS_GROUP_NORMAL, light);

	// Contornos
	obs_properties_t *trace = obs_properties_create();
	p = obs_properties_add_list(trace, S_CONTOUR_FROM, obs_module_text("ContourFrom"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("ContourFrom.Alpha"), FROM_ALPHA);
	obs_property_list_add_int(p, obs_module_text("ContourFrom.Brightness"), FROM_BRIGHTNESS);
	obs_properties_add_float_slider(trace, S_THRESHOLD, obs_module_text("Threshold"), 0.05, 0.95, 0.01);
	p = obs_properties_add_float_slider(trace, S_MIN_LENGTH, obs_module_text("MinLength"), 0.0, 500.0, 1.0);
	obs_property_float_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("MinLength.Description"));
	obs_properties_add_bool(trace, S_SHOW_CONTOURS, obs_module_text("ShowContours"));
	if (lt)
		obs_properties_add_button2(trace, S_RETRACE, obs_module_text("Retrace"), retrace_clicked, lt);
	obs_properties_add_group(props, "group_trace", obs_module_text("Group.Trace"), OBS_GROUP_NORMAL, trace);

	return props;
}

static void lt_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_AUTO_PLAY, true);
	obs_data_set_default_double(settings, S_DURATION, 3.0);
	obs_data_set_default_double(settings, S_PROGRESS, 0.5);
	obs_data_set_default_double(settings, S_STAGGER, 0.0);

	obs_data_set_default_double(settings, S_TRAIL, 0.3);
	obs_data_set_default_double(settings, S_RESIDUAL, 0.15);
	obs_data_set_default_double(settings, S_LINE_WIDTH, 2.5);
	obs_data_set_default_double(settings, S_GLOW_SIZE, 22.0);
	obs_data_set_default_double(settings, S_GLOW_STRENGTH, 1.5);
	obs_data_set_default_int(settings, S_CORE_COLOR, 0xFFFFFFFF);
	obs_data_set_default_int(settings, S_GLOW_COLOR, 0xFF388CFF); // naranja (1, 0.55, 0.22)
	obs_data_set_default_bool(settings, S_EXPAND, true);

	obs_data_set_default_int(settings, S_CONTOUR_FROM, FROM_ALPHA);
	obs_data_set_default_double(settings, S_THRESHOLD, 0.5);
	obs_data_set_default_double(settings, S_MIN_LENGTH, 8.0);
	obs_data_set_default_bool(settings, S_SHOW_CONTOURS, false);
}

struct obs_source_info light_trace_filter = {
	.id = "light_trace_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = lt_get_name,
	.create = lt_create,
	.destroy = lt_destroy,
	.update = lt_update,
	.get_defaults = lt_defaults,
	.get_properties = lt_properties,
	.get_width = lt_get_width,
	.get_height = lt_get_height,
	.video_tick = lt_tick,
	.video_render = lt_render,
	.filter_add = lt_filter_add,
	.filter_remove = lt_filter_remove,
};
