# Final Fantasy Resonance: primera revisión de 0.3.9.1

2 de noviembre de 2026. Rama `feature/series-s-video-stability`.
El usuario prioriza este juego; Mario Kart y Qliphah quedan para después.
Hay cambios locales, sin commit, paquete nuevo ni validación de este candidato en Series S.

## Evidencia recibida

Archivo privado `Downloads/jadyx-logs-20261102-222416.tar.zst`, extraído en
`test-results/final-fantasy-20261102/log/`. El JSON del juego identifica
`0100606026F12000`, fuente `a915a1e247`, 14.129 frames y cero informes descartados.
El diagnóstico acumula sesiones anteriores: usar la selección final del juego,
no mezclar sus tiempos con Mario Kart ni con otro arranque del proceso.

El usuario observa unos 23 FPS al comenzar, tambaleo y luces intermitentes en
escenas en tiempo real y gameplay; primer combate cerca de 30 FPS, apariciones
negras breves en el fondo y tirones al estrenar movimientos. Al terminar el
combate permanece en negro. Son observaciones manuales, sin captura ni perfil
que permita medir todavía el coste por subsistema.

| Tiempo del resumen | Frame inicial | Hecho registrado |
| --- | --- | --- |
| 95,049 s | 1.548 | Primer aviso SparseLargeVector y lectura no mapeada; 102 avisos y 210.574 lecturas en total |
| 413,278 s | 9.392 | Empiezan escrituras no mapeadas; 26.630.352 en total |
| 744,969 s | 14.129 | `PANIC` del guest por `svcBreak`, buffer de cuatro bytes |

La primera dirección de lectura registrada es `0x104` y la de escritura `0x10`.
El resumen solo guarda la primera muestra por sitio: no afirmar que todos los
accesos tienen esa misma dirección. El aviso de SparseLargeVector también se
produce cuando el JIT consulta entradas vacías de una tabla reservada; su texto
no demuestra por sí solo un defecto de Dynarmic.

La pantalla negra no se explica únicamente por compilación: existe un aborto
explícito del programa emulado. No hay evidencia suficiente para atribuirlo a
FFmpeg, shaders, corrupción del JIT o falta de memoria. El código exacto del
aborto no quedó en los archivos recibidos; la traza del JSON acaba truncada tras
480 bytes. Los dos logs generales y el log de bugs actual tienen tamaño cero
en el archivo exportado, aunque el resumen contiene errores.

La caché de shaders de esta sesión de Final Fantasy tarda 4,703 s según el diag.
En otra sesión de Mario Kart del mismo diagnóstico son 36,297 s de shaders y
12,531 s de prewarm CPU: no llamar «compilación de shaders» a la suma de ambos.
No hay una medición de ahorro ni se ha modificado su política de precarga.

## Cambios y límites

- `SparseLargeVector::ResizeAndClear` no limpiaba nada si el tamaño coincidía.
  Ahora limpia las páginas comprometidas al reutilizar la reserva. Afecta a las
  entradas de metadata y a los punteros absolutos de la tabla del JIT; no añade
  comprobaciones a las lecturas/escrituras normales. La prueba con PageTable de
  producción falla antes (`FAIL: reset metadata page type`) y pasa después.
  Esto prueba el defecto de reinicialización, **no su relación causal con este juego**.
- El exportador consulta `FileStandardInfo.EndOfFile` sobre un handle validado,
  en lugar de usar el tamaño de la enumeración del directorio. Las consultas de
  directorio pueden devolver información desactualizada: es una explicación
  compatible con los logs abiertos de tamaño cero, pendiente de confirmar en
  Series. Conserva el límite del snapshot, las comprobaciones de reparse points
  y la lectura por bloques. El gate del portal mantiene un escritor abierto y
  vaciado durante la exportación, y el verificador PAX compara sus bytes.
- El código de cuatro bytes de un `svcBreak` se registra explícitamente en el
  resumen, incluso dentro del ámbito que silencia el tap del logger. Las trazas
  ARM se dividen en hasta 16 detalles de 400 bytes; mantienen UTF-8 válido y
  declaran el truncamiento cuando no caben. No se aumenta la cola del tracker.

Referencia para el exportador: Microsoft documenta que las
[consultas de directorio pueden devolver metadata antigua](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/fa8194e0-53ec-413b-8315-e8fa85396fd8).
Se utiliza [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex)
con [FILE_STANDARD_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_standard_info),
sin introducir `GetFileSizeEx`, que Microsoft desaconseja para apps Store.

## Pruebas y siguiente gate

Linux, desde la raíz, con C++20 y headers de fmt:

```bash
g++ -std=c++20 -O2 -pthread -DFMT_HEADER_ONLY -Isrc tools/xbox/tests/page-table-reset.cpp src/common/page_table.cpp src/common/sparse_large_vector.cpp -o /tmp/jadyx-page-table-reset
/tmp/jadyx-page-table-reset
g++ -std=c++20 -O2 -pthread -DFMT_HEADER_ONLY -Isrc tools/xbox/tests/bug-tracker.cpp src/common/bug_tracker.cpp -o /tmp/jadyx-guest-diagnostics
/tmp/jadyx-guest-diagnostics
```

Para ASan/UBSan, sustituir `-O2` por
`-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined`.
Ejecutar el tracker con `--no-benchmark`: los sanitizadores alteran el coste de
instrucción y no permiten aplicar su umbral de cinco nanosegundos. Las
comprobaciones funcionales siguen activas; el build optimizado conserva el benchmark.

El gate de reinicialización cubre ocho reutilizaciones, tablas metadata/JIT,
crecimiento/reducción y una última página parcial. El del tracker cubre código
de aborto bajo TapMute, texto largo, límites UTF-8, cola acotada, deduplicación
y persistencia del código en JSON/log. Ninguno ejecuta un juego ni mide FPS.

Los dos gates pasan localmente en build optimizado y con ASan/UBSan. El
benchmark optimizado del tracker conserva el umbral de coste; se observaron
2,04 ns adicionales por hit repetido en este host, sin inferir FPS de la Series.
Se incorporan al workflow reutilizable de regresiones, con el fmt fijado del
proyecto. Su ejecución remota queda pendiente; no hay commit ni push.

Pendiente en Windows: build UWP y `tools/xbox/tests/run-network-portal.ps1`,
incluida comparación PAX del log con escritor abierto. Después preparar un
paquete distinto de 0.3.9.1 y repetir manualmente la transición del primer
combate en Series S, exportando los logs. El candidato aún no certifica resolver
la pantalla negra, el tambaleo, las luces ni mejorar el promedio de FPS.

## Ampliación de estabilidad antes del instalador 0.3.9.2

El usuario autoriza continuar las mejoras generales y preparar después el
instalador. Se mantienen las correcciones verificables; no se cambian shaders,
precisión de CPU/GPU, presupuestos ni número de workers sin una comparación válida.

En Windows, el registro de SparseLargeVector almacenaba números de página pero
al liberar comparaba contra una dirección en bytes: no retiraba la reserva.
Además incluía la página del extremo final, consideraba los fallos de ejecución
como lecturas y podía cambiar una página ya comprometida de RW a RO.
Ahora retira por número de página, usa intervalo semiabierto y atiende únicamente
lecturas recuperables dentro de una reserva viva. Compromete solo la página que
falló y conserva permisos si otro hilo la comprometió mientras llegaba la excepción.
Un lock mantiene estable la propiedad frente a la liberación de la reserva.

La clasificación sigue el contrato de Microsoft para
[EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record):
verificar NumberParameters, distinguir lectura/escritura/DEP y no reanudar
excepciones no continuables. Las lecturas iniciales de una tabla JIT reservada
ya no se presentan automáticamente como un error de Dynarmic.

Los Read/Write no mapeados conservan contadores y hasta 16 muestras de dirección
en el tracker. Su log síncrono imprime los primeros 16 y después potencias de
dos. El caso de 26.630.352 escrituras produce 36 líneas de progreso en la prueba,
en vez de decenas de millones de formatos/escrituras/vaciados. Los accesos
válidos no pasan por el limitador; `extended_logging` conserva la opción de
registrarlos todos. Esto reduce trabajo de diagnóstico durante un fallo,
**no corrige el acceso inválido ni prueba más FPS**.

El gate de límite pasa optimizado y con ASan/UBSan, incluye cuatro productores
y reinicio de instancia. La tabla de producción vuelve a pasar ASan/UBSan.
El gate Windows usa el handler real, permisos y VirtualAlloc/VirtualQuery:
comprueba límites, registros cortos, ejecución/escritura, permisos concurrentes,
reutilización de una dirección liberada, 1.000 ciclos sin crecimiento del registro
y un fallo hardware de lectura atendido por el VEH instalado.

`run-stability-gates.ps1` se añade al build Windows junto al gate TCP/PAX de
exportación con escritor abierto. Estos gates nativos y el build UWP se deben
completar antes de entregar 0.3.9.2. La firma y distribución siguen en el fork
personal; sin release pública ni cambios en los repositorios originales.
