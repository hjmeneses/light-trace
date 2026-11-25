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
 * (contours.c). Con eso se arma un vertex buffer estático; dibujarlo cada cuadro cuesta casi
 * nada.
 *
 * Etapa actual: dibuja la fuente y encima los contornos como líneas, cada uno de un color y
 * de oscuro a claro según su recorrido, para revisar el trazado.
 */

#include <obs-module.h>
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

enum { FROM_ALPHA = 0, FROM_BRIGHTNESS = 1 };

struct light_trace {
	obs_source_t *context;
	obs_source_t *parent; // fuente a cuya señal "update" estamos conectados (sin referencia)
	volatile bool dirty;
	bool supported;

	int contour_from;
	float threshold;
	float min_length;
	bool show_contours;

	gs_texrender_t *capture;
	gs_stagesurf_t *stage;
	uint32_t traced_w, traced_h;

	struct lt_contours contours;
	gs_vertbuffer_t *vb;
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

/* ------------------------------------------------------------------ ciclo de vida */

static void lt_update(void *data, obs_data_t *settings)
{
	struct light_trace *lt = data;
	lt->contour_from = (int)obs_data_get_int(settings, S_CONTOUR_FROM);
	lt->threshold = (float)obs_data_get_double(settings, S_THRESHOLD);
	lt->min_length = (float)obs_data_get_double(settings, S_MIN_LENGTH);
	lt->show_contours = obs_data_get_bool(settings, S_SHOW_CONTOURS);
	os_atomic_set_bool(&lt->dirty, true);
}

static void *lt_create(obs_data_t *settings, obs_source_t *source)
{
	struct light_trace *lt = bzalloc(sizeof(struct light_trace));
	lt->context = source;
	lt->dirty = true;
	lt_update(lt, settings);
	return lt;
}

static void lt_destroy(void *data)
{
	struct light_trace *lt = data;
	attach_parent(lt, NULL);
	obs_enter_graphics();
	gs_texrender_destroy(lt->capture);
	gs_stagesurface_destroy(lt->stage);
	gs_vertexbuffer_destroy(lt->vb);
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
	UNUSED_PARAMETER(seconds);
	struct light_trace *lt = data;
	attach_parent(lt, obs_filter_get_parent(lt->context));

	obs_source_t *target = obs_filter_get_target(lt->context);
	if (target) {
		uint32_t w = obs_source_get_base_width(target), h = obs_source_get_base_height(target);
		if (w != lt->traced_w || h != lt->traced_h)
			os_atomic_set_bool(&lt->dirty, true);
	}
}

/* ------------------------------------------------------------------ trazado */

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

static void build_vertex_buffer(struct light_trace *lt)
{
	gs_vertexbuffer_destroy(lt->vb);
	lt->vb = NULL;
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
	lt->vb = gs_vertexbuffer_create(vbd, 0);
}

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
	build_vertex_buffer(lt);

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

static void lt_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct light_trace *lt = data;
	obs_source_t *target = obs_filter_get_target(lt->context);
	if (!lt->supported || !target) {
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

	// La fuente tal cual (si ya se dibujó para trazar, OBS reutiliza esa textura)
	if (obs_source_process_filter_begin(lt->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		obs_source_process_filter_end(lt->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);

	if (!lt->show_contours || !lt->vb)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &white);

	gs_load_vertexbuffer(lt->vb);
	gs_load_indexbuffer(NULL);
	while (gs_effect_loop(solid, "SolidColored")) {
		for (size_t i = 0; i < lt->contours.num; i++) {
			const struct lt_contour *c = &lt->contours.list[i];
			gs_draw(GS_LINESTRIP, (uint32_t)c->first, (uint32_t)c->count);
		}
	}
	gs_load_vertexbuffer(NULL);
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

static obs_properties_t *lt_properties(void *data)
{
	struct light_trace *lt = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	if (lt && !lt->supported) {
		p = obs_properties_add_text(props, S_UNSUPPORTED, obs_module_text("Unsupported"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
	}

	p = obs_properties_add_list(props, S_CONTOUR_FROM, obs_module_text("ContourFrom"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("ContourFrom.Alpha"), FROM_ALPHA);
	obs_property_list_add_int(p, obs_module_text("ContourFrom.Brightness"), FROM_BRIGHTNESS);

	obs_properties_add_float_slider(props, S_THRESHOLD, obs_module_text("Threshold"), 0.05, 0.95, 0.01);

	p = obs_properties_add_float_slider(props, S_MIN_LENGTH, obs_module_text("MinLength"), 0.0, 500.0, 1.0);
	obs_property_float_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("MinLength.Description"));

	obs_properties_add_bool(props, S_SHOW_CONTOURS, obs_module_text("ShowContours"));
	if (lt)
		obs_properties_add_button2(props, S_RETRACE, obs_module_text("Retrace"), retrace_clicked, lt);
	return props;
}

static void lt_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_CONTOUR_FROM, FROM_ALPHA);
	obs_data_set_default_double(settings, S_THRESHOLD, 0.5);
	obs_data_set_default_double(settings, S_MIN_LENGTH, 8.0);
	obs_data_set_default_bool(settings, S_SHOW_CONTOURS, true);
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
	.video_tick = lt_tick,
	.video_render = lt_render,
	.filter_add = lt_filter_add,
	.filter_remove = lt_filter_remove,
};
