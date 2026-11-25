# Light Trace

*An OBS filter for images and text: a line of light runs along every outline of the source.*

Filtro para **OBS Studio** que traza los contornos de una imagen o un texto y hace correr una línea de luz por cada uno, como el efecto Vegas de After Effects: cada letra tiene su propia luz, y los huecos (el de la A, la R o la O) también.

*[English below](#english)*

> **En desarrollo.** Ya corre la luz (núcleo, estela y resplandor) por cada contorno. Faltan el temblor eléctrico, el sentido y punto de arranque, y el control desde el dock.

## Cómo funciona

Un shader no puede recorrer un contorno en orden: cada píxel se calcula solo, sin saber de qué contorno es ni en qué punto de su recorrido está. Este filtro hace esa parte en la CPU:

1. Cuando la fuente cambia (otro texto, otra fuente, otro archivo, otro tamaño), la dibuja una vez en una textura y la copia a la memoria.
2. Traza los contornos cerrados con *marching squares*: cada letra, cada hueco, cada mancha.
3. Guarda cada contorno como una línea ordenada, con la posición de cada punto en su recorrido.
4. En cada cuadro dibuja cada contorno como una cinta fina (el núcleo) en una textura, la desenfoca para el resplandor y lo suma sobre la fuente. Cuesta poco: no se vuelve a trazar nada.

Funciona con fuentes de **imagen** y **texto** (GDI+ y FreeType). En otras fuentes deja pasar la imagen sin cambios, porque trazar un video en cada cuadro sería mucho más pesado.

## Propiedades

| Grupo | Propiedad | Qué hace |
|---|---|---|
| Animación | En bucle / Duración | La luz da vueltas sin parar; una vuelta dura lo indicado |
| Animación | Progreso | Sin bucle: de 0 (no salió) a 1 (ya recorrió todo), para animarlo desde fuera |
| Animación | Desfase entre contornos | Cada letra arranca un poco después que la anterior, de izquierda a derecha |
| Luz | Largo de la estela | Qué fracción del contorno queda encendida detrás de la cabeza |
| Luz | Contorno ya recorrido | Brillo tenue que deja la luz al pasar |
| Luz | Grosor del núcleo, color del núcleo | La línea brillante |
| Luz | Tamaño, fuerza y color del resplandor | El halo alrededor |
| Luz | Ampliar el lienzo | Agranda la fuente unos px por lado para que el halo no se corte |
| Contornos | Contorno de | Transparencia (alfa) o brillo, para logos sobre fondo negro |
| Contornos | Umbral del contorno | Dónde se corta: más bajo engorda el contorno, más alto lo adelgaza |
| Contornos | Largo mínimo de un contorno | Ignora manchitas y ruido del suavizado |
| Contornos | Mostrar contornos trazados | Prueba: cada contorno de un color, de oscuro a claro en el sentido del recorrido |
| Contornos | Volver a trazar | Por si la fuente cambió y el filtro no se enteró |

## Compilar

Igual que la [plantilla oficial de plugins de OBS](https://github.com/obsproject/obs-plugintemplate): `cmake --preset windows-x64` y `cmake --build build_x64 --config RelWithDebInfo`. Las pruebas del trazador no necesitan OBS: `tests/test_contours.c` se compila solo con `src/contours.c`.

---

## English

An OBS Studio filter that traces the outlines of an image or text source and runs a line of light along each one, like After Effects' Vegas: every letter gets its own light, and so do the holes in letters such as A, R or O.

**Work in progress.** The light (core, trail and glow) already runs along every outline; the electric flicker, direction/start options and dock control are still to come.

A pixel shader can't follow an outline in order, so the filter does that part on the CPU: whenever the source changes it renders it once, reads it back, traces the closed outlines with marching squares and keeps each one as an ordered line with its position along the path. Each frame draws every outline as a thin ribbon (the core) into a texture, blurs it for the glow and adds both over the source. It works on image and text sources (GDI+ and FreeType); other sources pass through unchanged.

## License

GPL-2.0-or-later
