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
 * Trazado de contornos (marching squares) sobre una imagen de valores 0..1. Devuelve cada
 * contorno cerrado como una lista ordenada de puntos, con su posición en el recorrido.
 * C puro, sin dependencias de OBS, para poder probarlo por separado.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lt_point {
	float x, y; // en píxeles de la imagen (0,0 = esquina superior izquierda)
	float s;    // posición en el recorrido: 0 en el arranque, 1 al cerrar
};

struct lt_contour {
	size_t first; // índice del primer punto en lt_contours.points
	size_t count; // puntos, incluido el de cierre (igual al primero, con s = 1)
	float length; // largo en píxeles
	bool hole;    // hueco (por ejemplo, el de la A) en lugar de contorno exterior
	float min_x, min_y, max_x, max_y;
};

struct lt_contours {
	struct lt_point *points;
	size_t num_points;
	struct lt_contour *list; // ordenados de izquierda a derecha
	size_t num;
	size_t open_chains; // cadenas que no cerraron (no debería pasar; se descartan)
};

/*
 * values: w × h valores por filas. Un punto está "adentro" si su valor es >= threshold.
 * Descarta los contornos más cortos que min_length (px). Los exteriores giran en un sentido
 * y los huecos en el contrario; cada uno arranca en su punto más alto (y, a la par, el de
 * más a la izquierda). Devuelve false si no hubo memoria.
 */
bool lt_trace(const float *values, int w, int h, float threshold, float min_length, struct lt_contours *out);

void lt_contours_free(struct lt_contours *c);

#ifdef __cplusplus
}
#endif
