# Instaladores rápidos: builds incrementales y reempaquetado

## Iteraciones con código nuevo: estado LTCG

### Corrección del modo efectivo (3 nov)

La edición real del renderer en el run 41573230511 confirma el ahorro de las
fases de construcción (mismo UWP x64 Release, `/O2`, `/GL` y validaciones):

| Fase | Sin árbol, 41563797787 | Árbol + LTCG corregido, 41573230511 |
| --- | ---: | ---: |
| Bibliotecas | 6 min 48 s | 22 s |
| Gate frontend, compilación/enlace/ejecución | 4 min 46 s | 3 min 16 s, con segundo enlace y ejecución |
| Ejecutable | 6 min 57 s | 2 min 39 s |
| Suma de esas fases | 18 min 31 s | 6 min 17 s |

El segundo run modifica código de la clave de shaders, no solo documentación
o versión. Antes de construir valida las fuentes por contenido y vuelve a
compilar los objetos afectados. La tabla excluye setup, restauración/guardado
de caché, los otros gates y empaquetado: no presentarla como tiempo total de
entrega ni como promesa para cualquier tamaño de edición. El árbol comprimido
del primer modo corregido ocupa ~4,94 GB en CI; no aumenta el APPX entregado.
El CI completo final tardó **17 min 5 s**, incluyendo cachés y demás gates;
su job de empaquetado y firma, unos **47 s**. Fuente `466c0d3`, entrega 0.3.9.3,
[todos los jobs requeridos correctos](https://github.com/correo123456789qwerty-collab/jadyx/actions/runs/41573230511).

La inspección del árbol guardado confirmó que CMake colocaba `/LTCG` después de
`/LTCG:INCREMENTAL`. Conservaba un `.iobj` de 3.196.031.901 bytes pero el enlace
seguía rehaciendo el programa. Ahora el ejecutable recibe `/GL` explícito y
desactiva solo la adición automática de flags IPO de CMake; las bibliotecas
conservan su IPO y el enlace tiene **un único `/LTCG:INCREMENTAL`**. Un gate lee
las reglas Ninja y exige ese modo y `/GL` en los objetos del ejecutable.

El harness también conserva su estado LTCG. En Windows, run 41570736391:
compilar/enlazar 239,13 s y volver a compilar/enlazar con el estado retenido
102,66 s; ambas ejecuciones funcionales pasan. Esa segunda prueba recompila el
mismo harness, no mide una edición de un shader ni FPS. En el run final ambos
enlaces ya calientes tardaron 94,54 y 98,34 s: esa repetición no produjo ahorro
adicional. La edición real del renderer es la registrada en la tabla anterior.

La migración controlada del enlace admite `seed_build_cache` explícito: reutiliza
objetos `/GL` de la configuración anterior, con la misma imagen, arquitectura,
modo LTO y workspace; CMake vuelve a configurar y Ninja compara comandos y
dependencias. El valor por defecto sigue exigiendo la clave de configuración
exacta. `inspect_cache` en CI permite comprobar flags y archivos optimizados
de un árbol sin compilar ni empaquetar.

Los gates del portal revelaron dos carreras de su propio harness: un exportador
puede terminar entre dos POST, o entre conceder una URL y su GET. La prueba
reintenta hasta observar la exclusión de un exportador activo; acepta tanto
chunks como una respuesta completa con ETag y longitud exacta. Conserva la
comparación íntegra de bytes, PAX, rangos, cancelación y el escritor abierto.
No se modifica el comportamiento del exportador para acomodar las pruebas.

El primer run del candidato, 41558482021, confirmó que el problema no es solo
el enlace: 1.550 hits/17 misses (98,92 %), pero ~8 min recorriendo los objetos y
bibliotecas y ~6 min 41 s hasta terminar el enlace. El ejecutable compiló; el
harness falló por exigir `vma_impl.cpp.obj` en un build sin Vulkan. La ruta del
test ahora es condicional. No se entregó ese artefacto ni se omitió el gate.

Se añade caché del árbol `build-uwp`, con Ninja, objetos, bibliotecas y estado
LTCG. Antes de configurar se compara SHA256 de cada fuente con un manifiesto
del build anterior. **Solo los bytes idénticos recuperan su timestamp**; las
fuentes nuevas/cambiadas se fechan después de todos los outputs cacheados,
también si conservan tamaño o traen una fecha antigua. No se sustituyen fuentes.
La clave fija imagen/toolchain, LTO, configuración, dependencias y el script;
el manifiesto exige el mismo workspace y valida su formato/rutas antes de actuar.
CMake y Ninja siguen configurando, comprobando dependencias y construyendo.

La prueba local con g++/make pasa: checkout sin cambios no recompila; header del
mismo tamaño con timestamp antiguo recompila y cambia el resultado 21→42;
entrada nueva y manifiestos con workspace/formato/rutas inválidos se detectan.
El run final valida 5.364 fuentes idénticas y cuatro nuevas/cambiadas, conserva
los objetos vigentes y compila `scm_rev.cpp` y `d3d12_pipeline_cache.cpp`, además
de enlazar las bibliotecas afectadas y el ejecutable. No es un reempaquetado.

La caché de compilación se guarda tras enlazar, antes de los tests posteriores.
Es material de compilador, como sccache; no aprueba un instalador. El empaquetado
sigue exigiendo todos los gates. La prueba de frontend ahora se ejecuta tras
compilar las bibliotecas, **antes del enlace del emulador**, para fallar antes.

El candidato 0.3.9.3 añade `JADYX_INCREMENTAL_LTCG` a la configuración MSVC:
`/LTCG:INCREMENTAL` y `/LTCGOUT:.../ltcg/jadyx.iobj`, conservando `/GL` y las
optimizaciones Release. Es distinto de `/INCREMENTAL`, incompatible con LTCG.
CMake usa `/LTCG` completo por defecto: no basta activar IPO para conservar
el trabajo del optimizador entre enlaces.

Actions guarda `.iobj/.ipdb`, EXE y PDB con el árbol tras enlazar. La clave separa
imagen/toolchain y todos los archivos de configuración/dependencias del build.
Cada ejecución usa una clave inmutable distinta y restaura la última compatible.
Ninja sigue compilando y enlazando el checkout actual: se restaura su estado,
pero las fuentes solo recuperan fechas tras comprobar identidad por SHA256.
No se salta ningún gate.
El EXE restaurado nunca se empaqueta si falla la compilación o las pruebas.

La primera ejecución sin semilla sigue necesitando un build completo; la
ganancia con una edición posterior quedó medida arriba. El workflow escribe
duración y clave restaurada en su resumen.
Se mantienen el firmante/pin y las verificaciones de entrega existentes.
El preflight nuevo compila primero el objeto del renderer cambiado y comprueba
la sintaxis del harness, antes del enlace completo.

Fuentes: [Microsoft /LTCG](https://learn.microsoft.com/en-us/cpp/build/reference/ltcg-link-time-code-generation?view=msvc-170),
[/LTCGOUT](https://learn.microsoft.com/en-us/cpp/build/reference/ltcgout?view=msvc-170),
[configuración IPO MSVC de CMake](https://github.com/Kitware/CMake/blob/master/Modules/Platform/Windows-MSVC.cmake).

2 de noviembre de 2026. Aplicable al fork personal.

## Medición del cuello de botella

El run 41546366579 tuvo 1.565 aciertos de caché de 1.567 compilaciones (99,87 %).
Aun así volvió a recorrer 1.772 tareas y enlazar el emulador entero. El último
enlace de bibliotecas terminó hacia 23:41:10 UTC y el ejecutable a 23:46:35:
unos 5 minutos y 25 segundos de enlace optimizado. La caché de objetos no evita
ese enlace ni conserva el estado de Ninja del equipo anterior.

Los cuatro gates nativos de memoria y diagnósticos pasaron. El portal falló
porque el test seguía esperando tres archivos después de añadir el log abierto:
la expectativa correcta es cuatro. El pipeline subía el binario al final y lo
perdió al fallar ese test. La corrección conserva el ejecutable justo después
de enlazarlo; el empaquetado normal sigue exigiendo que todos los gates pasen.

## Rutas disponibles

| Ruta | Uso | Estado |
| --- | --- | --- |
| Reempaquetar un build correcto | Cambiar versión o repetir firma/entrega del mismo código | Implementado, sin compilador ni configuración CMake |
| Preflight Windows | Detectar fallos de los módulos modificados antes del build grande | Implementado: exportador y cuatro gates nativos |
| Windows persistente + Ninja incremental | Iteraciones que sí cambian código | Preferible para desarrollo local; aquí no hay un equipo Windows configurado |
| Árbol de build en caché | Reutilizar objetos, bibliotecas y estado del enlace entre runners | Verificado con Ninja/MSVC y edición real; fases medidas arriba |
| LTO incremental | Reducir trabajo del enlace al cambiar código | Modo efectivo y /GL verificados; .iobj/.ipdb retenidos, enlace del EXE 6 min 57 s→2 min 39 s junto con el árbol |

Esta entrega conserva el perfil UWP Release con LTO. Las rutas rápidas no
prometen conservar binarios cuando cambia el código: reempaquetar reproduce
el commit ya compilado y validado, que queda registrado en build-info.json.

## Ruta de empaquetado

`ci.yml` acepta `reuse_build_run`. Con un run completo y correcto, omite
compilación y tests repetidos, descarga los artefactos inmutables de ese run y
ejecuta empaquetado, firma, pin, CRC, digests y bloques como la ruta completa.
Comprueba que el origen sea CI del mismo repositorio y que los jobs UWP/Mesa
hayan pasado; un run de solo empaquetado no sirve de origen de compilación.
El checkout para empaquetar usa el SHA del build original y la metadata conserva
tanto ese SHA como el enlace al run original. Los secretos de firma siguen en
Actions; no se guardan en cachés ni artefactos.

```bash
gh workflow run ci.yml --repo correo123456789qwerty-collab/jadyx \
  --ref feature/fast-installer -f reuse_build_run=41548652658 \
  -f package_version=0.3.9.2 -f windows_runner=windows-2025
```

Para cambios de código se deja `reuse_build_run` vacío. El nuevo preflight
compila el exportador aislado y ejecuta los gates de memoria antes del build
completo: una llamada de Windows inválida ya no espera al final para fallar.
Los tests TCP/PAX que requieren las bibliotecas completas siguen después.

Referencias: [artefactos de workflows](https://docs.github.com/en/actions/tutorials/store-and-share-data),
[comportamiento de las cachés](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)
y [LTCG incremental](https://learn.microsoft.com/en-us/cpp/build/reference/ltcg-link-time-code-generation?view=msvc-170).
## Gate remoto de la ruta rápida

[Run 41549119630](https://github.com/correo123456789qwerty-collab/jadyx/actions/runs/41549119630)
correcto: **67 s en el job Windows y 72 s desde el despacho hasta completarlo**.
Se reempaquetó 0.3.9.1 desde el build válido 41523693476, sin compilar ni
ejecutar CMake. El EXE del APPX resultante es byteidéntico al de la entrega
anterior. OpenSSL verificó de nuevo PKCS#7 y el pin personal; también se
comprobaron digests firmados, CRC, bloques y modo biblioteca.

La prueba se conserva en `test-results/stability-0.3.9.2/fast-route-proof.json`
y `fast-proof-verification.json`. Su artefacto de prueba no reemplaza la entrega
de 0.3.9.2. El tiempo medido corresponde a reempaquetar código ya validado;
un cambio de código necesita un nuevo build, que luego puede reutilizarse.
La entrega de código nuevo 0.3.9.2 ya completó su CI 41548652658 con todos los
gates correctos; el ejemplo anterior reutiliza ese build, no el de la prueba 0.3.9.1.
