# Elgato HD60 Pro — Estado del soporte experimental

> **Resumen rápido**  
> Ya podemos hablar con el hardware de forma segura.  
> Todavía no podemos capturar imagen.  
> Estamos aproximadamente al 18 % de un driver usable.

---

## ¿Qué estamos intentando?

La Elgato HD60 Pro (PCI `12ab:0380`, subsystem `1cfa:0006`) no tiene soporte oficial en Linux.  
Este trabajo consiste en ir construyendo, paso a paso y con mucho cuidado, un backend propio dentro del driver `sc0710` (el mismo que ya funciona con la 4K60 Pro MK.2 y la 4K Pro).

La diferencia importante es que **no estamos reutilizando el código de las tarjetas más nuevas a ciegas**.  
La HD60 Pro se trata como un dispositivo distinto, con su propio backend, para no romper lo que ya funciona ni activar por accidente cosas peligrosas (DMA, interrupciones, etc.).

---

## ¿Dónde estamos ahora?

Hoy el driver es capaz de:

- Detectar la tarjeta correctamente
- Cargarla y descargarla de forma limpia
- Mapear sus registros (BARs)
- Hablar con el *mailbox* del hardware de forma controlada
- Enviar un comando real (`SIGNAL_READ`) y recibir la respuesta
- Limpiar el estado de completion sin dejar basura

Todo esto se hace **sin** activar:

- Bus Master
- Interrupciones (IRQ/MSI)
- DMA
- Nodos de vídeo o audio

Es decir: podemos observar y hablar con el hardware, pero aún no le pedimos que haga nada peligroso.

---

## El hito más importante hasta ahora

Logramos enviar un comando real al dispositivo desde Linux y recibir su respuesta de forma limpia.

La secuencia (reconstruida a partir del driver de Windows y validada en hardware real) es:

1. Limpiar el bit de completion
2. Escribir el opcode `SIGNAL_READ` (0x14)
3. Indicar qué señal queremos leer (en este caso HDMI Hot Plug Detect)
4. Disparar el comando
5. Esperar (por polling) a que el hardware responda
6. Leer el resultado
7. Limpiar de nuevo el completion para no dejar estado viejo

Funcionó.  
La latencia observada fue de aproximadamente 1.5 ms (2 polls).  
El valor que devolvió la señal era 0, lo cual es correcto: no había cable HDMI conectado en ese momento. El transporte funcionó; el hardware simplemente reportó “no hay señal”.

Esto puede parecer un detalle pequeño, pero es la primera vez que el driver y el hardware se entienden de verdad.

---

## Por qué vamos tan despacio (y por qué está bien)

Cuando se reverse-enginea un dispositivo de captura, el peligro no es solo “que no funcione”.  
El peligro real es:

- Corromper memoria del kernel
- Activar un motor de DMA que no controlamos
- Dejar el sistema congelado
- Confundir un estado viejo del hardware con uno nuevo

Por eso el enfoque actual es deliberadamente restrictivo:

- Todo está apagado por defecto
- Hace falta un opt-in explícito (parámetro de módulo + escritura manual en debugfs)
- Solo se permite un intento por carga del módulo
- Se toman snapshots del estado antes y después de cada experimento
- No hay reintentos automáticos ni “recuperaciones” improvisadas

Preferimos avanzar lento y saber exactamente qué está pasando, a avanzar rápido y no saber por qué algo se rompió.

---

## ¿Qué falta todavía?

Bastante, y es mejor ser honestos:

| Área                      | Estado aproximado | Comentario                              |
|---------------------------|-------------------|-----------------------------------------|
| Arquitectura e integración| ~80 %             | La base ya está bien planteada          |
| Acceso seguro a registros | ~90 %             | Muy sólido                              |
| Mailbox (polling)         | ~70 %             | Funciona, pero aún es experimental      |
| Inicialización HDMI       | ~10 %             | Casi todo por hacer                     |
| IRQ / MSI                 | ~5 %              | Todavía no tocado                       |
| DMA                       | ~5 %              | Todavía no tocado                       |
| Captura V4L2              | 0 %               | —                                       |
| Audio                     | 0 %               | —                                       |
| Robustez / productización | ~10 %             | —                                       |

**Estimación global: ≈ 18 % de un driver de captura usable.**

La parte más arriesgada (la que puede romper el sistema) ya está bastante avanzada.  
Lo que queda es más trabajo, pero también más predecible.

---

## Camino propuesto

1. **Cerrar G1**  
   Dejar el transporte de mailbox limpio y reutilizable (sin duplicar validaciones).

2. **Validar la señal HDMI real**  
   Conectar y desconectar un cable y confirmar que el hardware reporta correctamente el Hot Plug Detect.

3. **I²C + frontend**  
   Empezar a hablar con el chip de entrada HDMI. Aquí es donde realmente nos acercamos a poder ver una imagen.

4. **Después**  
   Interrupciones, DMA, V4L2 y audio.

La decisión actual es clara: **priorizar el frontend HDMI antes que las interrupciones**.  
El polling ya nos permite seguir investigando. Tener imagen (aunque sea por polling) es más valioso en este momento que tener eventos eficientes.

---

## Cómo se está trabajando

- Todo el código experimental está aislado.
- El backend de la HD60 Pro solo se activa para este PCI ID + subsystem exacto.
- Los experimentos requieren doble confirmación deliberada.
- Se guarda evidencia (snapshots, logs, hashes) de cada hito importante.

El objetivo no es solo “hacer que funcione algún día”.  
El objetivo es entender el dispositivo lo suficiente como para construir un soporte que no sea un castillo de naipes.

---

## Estado actual en una frase

> Tenemos un canal de control seguro y real con el hardware.  
> Todavía no tenemos imagen.  
> La base es sólida. Lo que falta es construible.