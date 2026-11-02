# Series S: estabilidad del vídeo y prioridades de compatibilidad

2 de noviembre de 2026. Rama `feature/series-s-video-stability`, basada en
`52c95fae381fbc30a0aac3d13dba0850fa774b84` (main remoto comprobado al iniciar).
Estado: candidato de código fuente; todavía no es una entrega validada en Xbox.

## Objetivo y alcance

| Prioridad | Juego | Síntoma comunicado por el usuario | Validación pendiente |
| --- | --- | --- | --- |
| 1 | Final Fantasy Resonance | Arranca, cinemáticas lentas y errores gráficos; posible cierre sin confirmar | Log, escenas reproducibles y comparación visual en Series S |
| 2 | Qliphah | Jugable hasta una cinemática; después se cierra toda la aplicación | Log del cierre y repetir la transición con el candidato |
| 3 | Mario Kart 8 Deluxe | Casi a velocidad completa; algunas pistas bajan a 70–80 % | Identificar pistas y medir cuando estén resueltos los anteriores |

Los tres reportes corresponden a Jadyx 0.3.9.0. No hay volcados, registros de estos
juegos ni conexión a la consola en este entorno. No se atribuyen los síntomas a
NVDEC sin esas pruebas, ni se declara corregido ningún juego por pasar un test CPU.

## Correcciones del candidato

- `FFmpeg::Packet` posee una copia de los datos, con el padding cero exigido por
  los lectores de FFmpeg. El `span` prestado anterior no lo garantizaba. También
  comprueba fallos de asignación y tamaños antes de convertirlos a `int`.
- `avcodec_send_packet` acepta un paquete únicamente si devuelve éxito. Ante
  `EAGAIN`, se conservan los fotogramas pendientes y se reintenta el mismo paquete
  después de recibir la salida. EOF y los errores no se convierten en éxito.
  El contrato está descrito en la [API send/receive de FFmpeg](https://www.ffmpeg.org/doxygen/8.1/avcodec_8h_source.html).
- Los offsets de destino y el contador PTS avanzan después de aceptar el paquete.
  La entrega conserva la política FIFO existente; no se introduce una política
  distinta de reordenación H.264 ni de asociación de superficies.
- La negociación GPU usa el tipo del dispositivo realmente creado y devuelve
  solamente un formato ofrecido por el decodificador. El callback conserva la
  referencia de dispositivo y la recepción distingue fotogramas hardware de
  fotogramas software. Véase el [contrato de get_format y hw_device_ctx](https://ffmpeg.org/doxygen/7.1/structAVCodecContext.html).
- Se contienen asignaciones fallidas de contextos/AVFrame y accesos al API sin
  inicializar. Un envío rechazado permite entregar salida anterior que ya se
  hubiera recibido. La ruta CPU evita crear un segundo AVFrame que no utilizaba.

Se conservaron y completaron los cambios de Packet que ya estaban en el árbol
al iniciar. No se modifican shaders de juegos, el renderer D3D12, presupuestos de
memoria, el JIT ni ajustes específicos de Mario Kart. El código de vídeo es
compartido, por lo que los juegos existentes necesitan también una prueba de
regresión en consola.

## Pruebas reproducibles

Requisitos Linux: compilador C++20, Python 3, pkg-config, FFmpeg con libx264/libvpx
y headers de libavcodec, libavformat, libavutil y fmt. Las entradas se generan
con `testsrc2`; no se descargan ni usan archivos de juegos.

```bash
python3 tools/xbox/tests/run-video-gates.py --output test-results/series-s-video/optimized.json
python3 tools/xbox/tests/run-video-gates.py --sanitize --output test-results/series-s-video/sanitized.json
```

El gate compila la implementación de producción de `ffmpeg.cpp`, sustituyendo
solo el logger y la política de assertions. Descarta por linker las secciones de
inicialización que requieren Settings y el resto del emulador. No sustituye a
una compilación completa de `video_core`.

Resultados locales:

- Propiedad y padding en siete tamaños, retención con `av_packet_ref`, rechazo
  de entrada vacía y fallos de asignación: correcto. Además, 128 fotogramas
  rawvideo conservan todos sus píxeles después de destruir el origen.
- H.264 con B-frames, VP8 y VP9: 66 paquetes/66 fotogramas por codec, comparados
  byte a byte con otro contexto de FFmpeg, incluyendo PTS, formato y dimensiones.
  Los envíos en lotes de once fuerzan backpressure sin perder salida.
- Se repiten los tres streams con un dispositivo de metadatos sin acelerador
  compatible: salida software idéntica y sin intentar transferencias GPU.
  Total de esta matriz: 396 fotogramas comprimidos.
- Se verifican EOF, formatos ofrecidos, identidad del dispositivo, conservación
  de referencias, fallo al asignar contexto/AVFrame y API sin inicializar.
- ASan y UBSan pasan en el código del gate y del wrapper; no se recompila FFmpeg
  con sanitizers. Se excluye `vptr` porque su metadata retendría RTTI de Settings
  que el gate aislado no enlaza. Los JSON incluyen versiones y SHA256 de fuentes.
- El test de streams contra el código original de HEAD falla comparando VP9
  bajo backpressure; el candidato pasa. Para compilar ese control aislado solo
  se retiraron dos includes no utilizados que arrastraban Boost.

Se añadió el job `video decoder regressions` a CI. Su ejecución remota y el build
UWP siguen pendientes; las pruebas anteriores son ejecuciones locales.

## Compilación y prueba en Xbox

Este equipo tiene Linux y carece del toolchain Windows/UWP, por lo que no se ha
generado, firmado ni instalado un APPX. El manifiesto permanece en 0.3.9.0.
En Windows, con los requisitos de [uwp_build.md](uwp_build.md) y Mesa disponibles:

```powershell
tools\xbox\build-env.bat cmake --preset uwp-x64
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library -Version 0.3.9.1
```

Ese último comando prepara `build-uwp/package/jadyx.appx` con una versión de
prueba superior a la del usuario. No implica publicar una release. Antes de
instalar, verificar el paquete y archivar los símbolos como indica
[xbox_deploy.md](xbox_deploy.md).

### Instalador mediante GitHub Actions

El despacho manual de CI acepta `package_version` y `lto`. Para esta entrega se
creó el fork `correo123456789qwerty-collab/jadyx`, puesto que la cuenta del usuario
no tiene permisos de escritura en el original. Una vez subida esta rama:

```bash
gh workflow run ci.yml --repo correo123456789qwerty-collab/jadyx --ref feature/series-s-video-stability -f package=true -f package_version=0.3.9.1 -f lto=true
```

El job de empaquetado comprueba CMS, clave de firma fijada, digests del contenedor
y bloques del payload antes de subir `jadyx-package`. Ese artefacto incluye APPX,
dependencia VCLibs, símbolos, identidad del commit y SHA256SUMS. Tras descargarlo,
el APPX se entrega como `build-uwp/package/jadyx.appx`.

La sesión GH quedó disponible durante el trabajo. El fork tiene su propio
certificado de compilación personal, guardado en secretos cifrados; el material
privado local se conserva fuera del repositorio con permisos restringidos. El
verificador acepta un `PublicKeyPath` explícito para ese certificado y conserva
el pin del proyecto original por defecto. Actions obtiene el pin personal de la
variable pública `JADYX_PACKAGE_PUBLIC_KEY`.

La firma personal no es la firma de distribución de JulianDr14. Se mantiene la
identidad del paquete y se entrega el certificado público junto al APPX; la
instalación/actualización y conservación de datos deben validarse en consola.
Todavía no se ha producido un APPX ni publicado una release.

Para continuar, recoger primero el log 0.3.9.0 de Final Fantasy y Qliphah mediante
«Comprimir y descargar logs» en el portal. Anotar actualización del juego, escena
y punto exacto del fallo. Con el candidato compilado, repetir manualmente esas
escenas y exportar sus logs. Final Fantasy necesita además capturas comparables
de sus errores gráficos: esta corrección del vídeo no demuestra resolver shaders
ni cinemáticas renderizadas en tiempo real. Los FPS y el paso de la transición
de Qliphah deben confirmarse en la Series S antes de cerrar el trabajo.
