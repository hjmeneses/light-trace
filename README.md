# Light Trace

*An OBS filter for images and text: a line of light runs along every outline of the source.*

Filtro para **OBS Studio** que traza los contornos de una imagen o un texto y hace correr una línea de luz por cada uno, como el efecto Vegas de After Effects: cada letra tiene su propia luz, y los huecos (el de la A, la R o la O) también.

*[English below](#english)*

> **En desarrollo.** Esta primera versión traza los contornos y los muestra como líneas de colores para revisarlos. La luz (núcleo, resplandor, estela y temblor) viene en la siguiente etapa.

## Cómo funciona

Un shader no puede recorrer un contorno en orden: cada píxel se calcula solo, sin saber de qué contorno es ni en qué punto de su recorrido está. Este filtro hace esa parte en la CPU:

1. Cuando la fuente cambia (otro texto, otra fuente, otro archivo, otro tamaño), la dibuja una vez en una textura y la copia a la memoria.
2. Traza los contornos cerrados con *marching squares*: cada letra, cada hueco, cada mancha.
3. Guarda cada contorno como una línea ordenada, con la posición de cada punto en su recorrido.
4. En cada cuadro solo dibuja esas líneas, lo que cuesta casi nada.

Funciona con fuentes de **imagen** y **texto** (GDI+ y FreeType). En otras fuentes deja pasar la imagen sin cambios, porque trazar un video en cada cuadro sería mucho más pesado.

## Propiedades

| Propiedad | Qué hace |
|---|---|
| Contorno de | Transparencia (alfa) o brillo, para logos sobre fondo negro |
| Umbral del contorno | Dónde se corta: más bajo engorda el contorno, más alto lo adelgaza |
| Largo mínimo de un contorno | Ignora manchitas y ruido del suavizado |
| Mostrar contornos trazados | Dibuja los contornos: cada uno de un color, de oscuro a claro en el sentido del recorrido |
| Volver a trazar | Por si la fuente cambió y el filtro no se enteró |

## Compilar

Igual que la [plantilla oficial de plugins de OBS](https://github.com/obsproject/obs-plugintemplate): `cmake --preset windows-x64` y `cmake --build build_x64 --config RelWithDebInfo`. Las pruebas del trazador no necesitan OBS: `tests/test_contours.c` se compila solo con `src/contours.c`.

---

## English

An OBS Studio filter that traces the outlines of an image or text source and runs a line of light along each one, like After Effects' Vegas: every letter gets its own light, and so do the holes in letters such as A, R or O.

**Work in progress.** This first version traces the outlines and draws them as coloured lines for checking. The light itself comes next.

A pixel shader can't follow an outline in order, so the filter does that part on the CPU: whenever the source changes it renders it once, reads it back, traces the closed outlines with marching squares and keeps each one as an ordered line with its position along the path. Each frame only draws those lines. It works on image and text sources (GDI+ and FreeType); other sources pass through unchanged.

## License

GPL-2.0-or-later
