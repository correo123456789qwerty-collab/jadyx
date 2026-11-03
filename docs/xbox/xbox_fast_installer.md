# Instaladores rápidos sin recompilar un build válido

## Iteraciones con código nuevo: estado LTCG

El candidato 0.3.9.3 añade `JADYX_INCREMENTAL_LTCG` a la configuración MSVC:
`/LTCG:INCREMENTAL` y `/LTCGOUT:.../ltcg/jadyx.iobj`, conservando `/GL` y las
optimizaciones Release. Es distinto de `/INCREMENTAL`, incompatible con LTCG.
CMake usa `/LTCG` completo por defecto: no basta activar IPO para conservar
el trabajo del optimizador entre enlaces.

Actions guarda `.iobj/.ipdb`, EXE y PDB tras pasar los gates. La clave separa
imagen/toolchain y todos los archivos de configuración/dependencias del build.
Cada ejecución usa una clave inmutable distinta y restaura la última compatible.
Solo es una semilla del optimizador: Ninja sigue compilando y enlazando el
checkout actual; no se restaura su grafo ni sus timestamps, ni se salta un gate.
El EXE restaurado nunca se empaqueta si falla la compilación o las pruebas.

La primera ejecución no tiene semilla; la ganancia con una edición posterior
debe medirse. El workflow escribe duración y clave restaurada en su resumen.
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
| Árbol de build en caché | Reutilizar objetos, bibliotecas y estado del enlace entre runners | Requiere validar toolchain, rutas y timestamps; no habilitado como una caché ciega |
| LTO incremental | Reducir trabajo del enlace al cambiar código | Microsoft lo soporta; requiere conservar .iobj/.ipdb y medir con este emulador |

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
