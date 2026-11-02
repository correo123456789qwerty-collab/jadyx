# Instaladores rápidos sin recompilar un build válido

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
  --ref feature/fast-installer -f reuse_build_run=RUN_CORRECTO \
  -f package_version=0.3.9.2 -f windows_runner=windows-2025
```

Para cambios de código se deja `reuse_build_run` vacío. El nuevo preflight
compila el exportador aislado y ejecuta los gates de memoria antes del build
completo: una llamada de Windows inválida ya no espera al final para fallar.
Los tests TCP/PAX que requieren las bibliotecas completas siguen después.

Referencias: [artefactos de workflows](https://docs.github.com/en/actions/tutorials/store-and-share-data),
[comportamiento de las cachés](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)
y [LTCG incremental](https://learn.microsoft.com/en-us/cpp/build/reference/ltcg-link-time-code-generation?view=msvc-170).
El tiempo real de reempaquetado se registra al completar el gate remoto.
