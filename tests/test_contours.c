// Pruebas del trazador de contornos con figuras sintéticas.
// Compilar: cl /nologo /W4 /O2 tests\test_contours.c src\contours.c /Fe:test_contours.exe
#include "../src/contours.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, ...)                         \
	do {                                     \
		if (!(cond)) {                   \
			printf("  FALLA: " __VA_ARGS__); \
			printf("\n");            \
			failures++;              \
		}                                \
	} while (0)

// Disco suavizado (valor 0..1 según la cobertura aproximada del píxel)
static float disk(float x, float y, float cx, float cy, float r)
{
	float d = hypotf(x + 0.5f - cx, y + 0.5f - cy) - r;
	float v = 0.5f - d;
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static void check_common(const char *name, const struct lt_contours *c)
{
	CHECK(c->open_chains == 0, "%s: %zu cadenas abiertas", name, c->open_chains);
	for (size_t i = 0; i < c->num; i++) {
		const struct lt_contour *k = &c->list[i];
		const struct lt_point *p = c->points + k->first;
		CHECK(k->count >= 4, "%s: contorno %zu con %zu puntos", name, i, k->count);
		CHECK(p[0].x == p[k->count - 1].x && p[0].y == p[k->count - 1].y, "%s: contorno %zu no cierra", name,
		      i);
		CHECK(p[0].s == 0.0f && p[k->count - 1].s == 1.0f, "%s: contorno %zu s de %g a %g", name, i, p[0].s,
		      p[k->count - 1].s);
		for (size_t j = 1; j < k->count; j++) {
			if (p[j].s < p[j - 1].s) {
				CHECK(0, "%s: contorno %zu s no crece en %zu", name, i, j);
				break;
			}
		}
		// Arranca en el punto más alto
		for (size_t j = 0; j < k->count; j++) {
			if (p[j].y < p[0].y - 1e-3f) {
				CHECK(0, "%s: contorno %zu no arranca arriba", name, i);
				break;
			}
		}
	}
}

static void report(const char *name, const struct lt_contours *c)
{
	printf("%s: %zu contornos, %zu puntos\n", name, c->num, c->num_points);
	for (size_t i = 0; i < c->num && i < 6; i++) {
		const struct lt_contour *k = &c->list[i];
		const struct lt_point *p = c->points + k->first;
		// sentido: signo del área (pantalla, y hacia abajo)
		double a = 0;
		for (size_t j = 0; j + 1 < k->count; j++)
			a += (double)p[j].x * p[j + 1].y - (double)p[j + 1].x * p[j].y;
		printf("   #%zu %-8s largo %7.1f  x %.1f..%.1f  y %.1f..%.1f  area %+.0f  arranque (%.1f, %.1f)\n", i,
		       k->hole ? "hueco" : "exterior", k->length, k->min_x, k->max_x, k->min_y, k->max_y, a / 2, p[0].x,
		       p[0].y);
	}
}

int main(void)
{
	const int W = 200, H = 120;
	float *img = malloc(sizeof(float) * W * H);
	struct lt_contours c;

	// 1. Disco de radio 40
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			img[y * W + x] = disk((float)x, (float)y, 60, 60, 40);
	lt_trace(img, W, H, 0.5f, 4.0f, &c);
	report("disco", &c);
	check_common("disco", &c);
	CHECK(c.num == 1, "disco: %zu contornos", c.num);
	if (c.num == 1) {
		CHECK(fabsf(c.list[0].length - 2 * 3.14159f * 40) < 3, "disco: largo %g (esperado ~251)",
		      c.list[0].length);
		CHECK(!c.list[0].hole, "disco: marcado como hueco");
	}
	lt_contours_free(&c);

	// 2. Anillo: disco 40 menos disco 20 (un exterior y un hueco)
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			img[y * W + x] = fminf(disk((float)x, (float)y, 60, 60, 40),
					       1.0f - disk((float)x, (float)y, 60, 60, 20));
	lt_trace(img, W, H, 0.5f, 4.0f, &c);
	report("anillo", &c);
	check_common("anillo", &c);
	CHECK(c.num == 2, "anillo: %zu contornos", c.num);
	if (c.num == 2) {
		int holes = c.list[0].hole + c.list[1].hole;
		CHECK(holes == 1, "anillo: %d huecos", holes);
	}
	lt_contours_free(&c);

	// 3. Dos cuadrados: deben salir de izquierda a derecha
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			img[y * W + x] = ((x >= 120 && x < 160) || (x >= 20 && x < 50)) && y >= 30 && y < 70 ? 1.0f
													     : 0.0f;
	lt_trace(img, W, H, 0.5f, 4.0f, &c);
	report("cuadrados", &c);
	check_common("cuadrados", &c);
	CHECK(c.num == 2, "cuadrados: %zu contornos", c.num);
	if (c.num == 2) {
		CHECK(c.list[0].min_x < c.list[1].min_x, "cuadrados: orden");
		// 30 × 40 px: perímetro 140, menos 4 esquinas biseladas de (1 - 0.707) px
		CHECK(fabsf(c.list[0].length - 138.83f) < 0.1f, "cuadrados: largo %g (esperado ~138.8)",
		      c.list[0].length);
		CHECK(!c.list[0].hole && !c.list[1].hole, "cuadrados: marcados como hueco");
	}
	lt_contours_free(&c);

	// 4. Imagen llena: un solo contorno pegado al borde
	for (int i = 0; i < W * H; i++)
		img[i] = 1.0f;
	lt_trace(img, W, H, 0.5f, 4.0f, &c);
	report("llena", &c);
	check_common("llena", &c);
	CHECK(c.num == 1, "llena: %zu contornos", c.num);
	lt_contours_free(&c);

	// 5. Tablero de ajedrez de 1 px (todas las celdas son sillas)
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			img[y * W + x] = (float)((x + y) & 1);
	lt_trace(img, W, H, 0.5f, 0.0f, &c);
	report("ajedrez", &c);
	check_common("ajedrez", &c);
	CHECK(c.num > 0, "ajedrez: sin contornos");
	lt_contours_free(&c);

	// 6. Ruido: todo debe cerrar, sin cadenas abiertas
	srand(7);
	for (int i = 0; i < W * H; i++)
		img[i] = (float)rand() / RAND_MAX;
	lt_trace(img, W, H, 0.5f, 0.0f, &c);
	report("ruido", &c);
	check_common("ruido", &c);
	lt_contours_free(&c);

	// 7. Vacía
	for (int i = 0; i < W * H; i++)
		img[i] = 0.0f;
	lt_trace(img, W, H, 0.5f, 0.0f, &c);
	CHECK(c.num == 0, "vacia: %zu contornos", c.num);
	lt_contours_free(&c);

	free(img);
	printf(failures ? "\n%d FALLAS\n" : "\nTodo bien\n", failures);
	return failures ? 1 : 0;
}
