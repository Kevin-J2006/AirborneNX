# AirborneNX

Port nativo de **Asphalt 8: Airborne (Android, v4.0.0l)** para **Nintendo Switch**, por **ItsDroidy06**.

Versión del port: **1.0.1**

AirborneNX no incluye el juego. Carga las bibliotecas originales `arm64-v8a` del APK y les da en la consola
el entorno Android que esperan. Necesitas tu propia copia del juego (APK v4.0.0l y sus datos).

> ## ⚠️ Aviso importante
>
> **AirborneNX NO es un producto oficial de Gameloft.** No está afiliado, patrocinado ni respaldado por
> Gameloft de ninguna forma. Es un proyecto hecho por un fan y desarrollador independiente que
> simplemente quiso llevar la experiencia del juego a la consola.
>
> **AirborneNX es gratuito y siempre lo será.** Su autor lo publica gratis y nadie tiene por qué cobrarte
> por él. **Si pagaste por este port, lamentablemente te estafaron:** pide tu dinero de vuelta.
>
> Está **estrictamente prohibido** vender o monetizar los assets propietarios de Gameloft (APK, datos,
> texturas, sonidos, música, marcas). No son tuyos ni míos.

---

## Estado

Probado en consola real (Atmosphère) y en el emulador Eden v0.2.1.

Esta es la **primera versión** del port, así que puede tener bugs. Si encuentras uno, repórtalo con los
archivos que se indican en [Si algo falla](#si-algo-falla).

Funciona:
* Menús, garaje y carreras completas.
* Pantalla táctil y mando (Joy-Con / Pro Controller).
* Guardado de la partida en la SD.

### Rendimiento

El port **apunta a 60 FPS**, pero no esperes que se mantengan todo el tiempo: habrá momentos en que la
tasa baje, sobre todo en escenas cargadas y mientras el juego carga texturas. Eso no significa que el
juego vaya mal ni que sea injugable. Si prefieres una tasa más estable, puedes limitarlo a 30 FPS desde
`config.ini` (ver [Configuración](#configuración)).

### Limitaciones conocidas
* Sin conexión: el juego corre sin red (multijugador, eventos en línea y tareas diarias no están disponibles).
* Los menús pueden dar tirones mientras cargan imágenes, y las pantallas de carga son largas: el driver de
  vídeo de la consola no usa las texturas ASTC del juego directamente y hay que descomprimirlas por CPU (próximamente trataré de implementar una solución para este problema).
* El stick izquierdo actúa como la cruceta (giro todo o nada), igual que en un móvil Android.
* Sin giroscopio ni teclado en pantalla.

---

## Instalación

### Qué necesitas

* Una Nintendo Switch con **Atmosphère** y el Homebrew Menu.
* Una tarjeta SD con unos **2 GB libres**, mejor en **FAT32** (exFAT es propenso a corromper archivos con homebrew).
* Tu propia copia de **Asphalt 8: Airborne para Android, versión `4.0.0l`**, obtenida legalmente:
  * el **APK**, y
  * los **datos** que el juego descarga en el teléfono (la carpeta `files`).
* Un programa que abra archivos ZIP, como [7-Zip](https://www.7-zip.org/).

### La versión exacta del juego

El port **solo funciona con la versión `4.0.0l`** y sus bibliotecas de 64 bits (`arm64-v8a`). Con cualquier
otra versión se detiene al arrancar con un mensaje de error, a propósito, en lugar de fallar más adelante.

| Dato | Valor |
| :--- | :--- |
| Versión | `4.0.0l` |
| Paquete | `com.gameloft.android.ANMP.GloftA8HM` |
| Arquitectura | `arm64-v8a` (el APK debe contener `lib/arm64-v8a/`) |
| Tamaño del APK | 76 711 514 bytes (73,2 MB) |


Los **datos** deben ser los de texturas **ASTC**: se reconocen porque los paquetes de pistas se llaman
`dlc_v2_astc_track_...`. Los datos de otras variantes (`etc1`) no sirven.

### Paso a paso

**1. Crea la carpeta del port.** En la tarjeta SD, crea `switch/AirborneNX/`.

**2. Copia el port.** Pon `AirborneNX.nro` dentro de esa carpeta.

**3. Abre el APK como si fuera un ZIP.** Un APK es un archivo ZIP con otra extensión. Con 7-Zip: clic
derecho sobre el APK → *7-Zip* → *Abrir comprimido*. (También puedes hacer una copia y cambiarle la
extensión a `.zip`.)

**4. Saca dos bibliotecas del APK.** Dentro del APK, entra en `lib/arm64-v8a/` y extrae a
`switch/AirborneNX/` estos dos archivos:
* `libmyAndroid.so`
* `libc++_shared.so`

Los otros `.so` de esa carpeta no hacen falta.

**5. Saca la carpeta `assets` del APK.** Extrae la carpeta `assets` entera a `switch/AirborneNX/`, de modo
que quede `switch/AirborneNX/assets/`.

**6. Copia el APK completo.** Además de lo anterior, copia el APK entero a `switch/AirborneNX/` y
**renómbralo a `base.apk`**. El juego lo abre por su cuenta al arrancar.

**7. Copia los datos del juego.** En el teléfono, los datos están en
`Android/data/com.gameloft.android.ANMP.GloftA8HM/files`. Copia esa carpeta `files` completa a
`switch/AirborneNX/`, de modo que quede `switch/AirborneNX/files/`.

**8. Comprueba el resultado.** La carpeta debe quedar así:

```text
sdmc:/switch/AirborneNX/
├── AirborneNX.nro        # El port
├── libmyAndroid.so       # Del APK: lib/arm64-v8a/
├── libc++_shared.so      # Del APK: lib/arm64-v8a/
├── base.apk              # El APK 4.0.0l completo, renombrado
├── assets/               # La carpeta assets/ del APK
└── files/                # Los datos del juego
    ├── levels.jpk, pvs.jpk, track_*.jpk
    ├── dlcs/             # Paquetes de pistas (dlc_v2_astc_track_*.jpk)
    └── gui/ models/ shaders/ textures_android/ sounds/ music/ ...
```

**9. Arranca el juego.** En la consola, abre el Homebrew Menu **manteniendo R mientras abres un juego
instalado** (modo aplicación) y lanza AirborneNX. Desde el Álbum no hay memoria suficiente y el juego no
arrancará.

El arranque puede tardar alrededor de un minuto. Las carpetas `internal/` (tu partida guardada), `cache/`,
`sdcard/`, `filesupdate/` y `sysroot/` se crean solas.

### Problemas frecuentes

Si el port se cierra solo, abre `switch/AirborneNX/airbornenx.log`: la última línea dice por qué.

| Qué pasa | Causa probable |
| :--- | :--- |
| Se cierra a los pocos segundos; el log dice `Failed to load ...` | Falta `libmyAndroid.so` o `libc++_shared.so`, o la carpeta no se llama `switch/AirborneNX`. |
| Se cierra a los pocos segundos; el log dice `patch preimage mismatch` | El juego no es la versión `4.0.0l` de 64 bits. |
| Se queda en la pantalla de carga | Falta `base.apk` o la carpeta `assets/`. |
| No arranca o da error al abrirlo | Comprueba que lo lanzaste manteniendo R sobre un juego, no desde el Álbum. |
| Faltan pistas | Las pistas adicionales están en `files/dlcs/` (`dlc_v2_astc_track_*.jpk`). La versión 1.0.0 del port dejaba que el juego las borrara al arrancar; desde la 1.0.1 se conservan. Si las perdiste, vuelve a copiar esa carpeta. |

---

## Controles

| Switch | En carrera |
| :--- | :--- |
| Stick izquierdo / cruceta | Girar |
| ZR, R, A, stick derecho arriba | Acelerar |
| ZL, L, X, stick derecho abajo | Frenar |
| B | Nitro |
| Y | Cambiar vista |
| Pulsar stick izquierdo | Reaparición |
| + | Pausa |
| Pantalla táctil | Menús y controles táctiles del juego |

Al arrancar, el juego muestra su propio esquema de mando; pulsa cualquier botón para continuar.

---

## Configuración

Opcional. Crea `sdmc:/switch/AirborneNX/config.ini`:

```ini
fps=30
```

| Clave | Valores | Por defecto |
| :--- | :--- | :--- |
| `fps` | `30` o `60` | `60` |
| `shadercache` | `0` (desactivada) o `1` | `1` |
| `tokencars` | `0` (precios originales) o `1` | `1` |
| `tokenrate` | créditos por ficha | `100` |
| `economy` | `0` (precios del juego) o `1` | `1` |

`shadercache` guarda los shaders ya compilados en `cache/shaders.bin` para no recompilarlos en cada arranque. Se puede borrar ese archivo sin problema: se vuelve a generar.

`tokencars` añade un precio en créditos a los vehículos que solo se venden por fichas (que sin
conexión no se pueden conseguir): fichas × `tokenrate`. Los vehículos que se fabrican con planos
se venden igual que los demás, y los vinilos que costaban fichas pasan a costar créditos.

`economy` ajusta los precios a un juego sin compras: los vehículos bajan por tramos (hasta 50 000
no cambian; hasta 250 000, −30 %; hasta 1 000 000, −45 %; por encima, −60 %), las mejoras cuestan
el 40 % y el Audi R8 e-tron cuesta 2500 créditos para que sea un primer coche asequible.

El archivo original no se modifica; la
versión convertida se guarda en `cache/asphaltshop_credits.xtea`.

---

## Si algo falla

Cada arranque escribe `sdmc:/switch/AirborneNX/airbornenx.log`. Si la consola muestra un error de
Atmosphère, el informe queda en `sdmc:/atmosphere/crash_reports/`. Adjunta ambos al reportar un problema.

---

## Compilación

Requisitos: [devkitPro](https://devkitpro.org/) con `devkitA64`, `libnx` y los portlibs `switch-mesa`,
`switch-sdl2` y `switch-zlib`.

```sh
make              # versión normal
make DEBUG=1      # registro detallado y comprobación de cada llamada GL
make PROFILE=1    # registra en qué se fue el tiempo de cada frame lento
make clean
```

`-DGLTRACE_FRAME=<n>` en `CFLAGS` vuelca todas las llamadas GL del frame `n` a `gltrace.txt`.

Los parches al binario del juego (`source/patch.c`) están fijados a la v4.0.0l `arm64-v8a` y comprueban las
instrucciones originales antes de escribir; con otra versión el port se detiene en lugar de parchear a ciegas.

### Cómo funciona, brevemente explicado

* **Cargador ELF64** (`lib/so_util`): arma la biblioteca en memoria, la enlaza y la mapea como código.
* **Capa bionic** (`source/reimpl`): hilos, archivos, relojes y `errno` traducidos entre Android y libnx.
* **Java simulado** (`lib/falso_jni`, `source/java.c`): responde a las llamadas JNI del juego.
* **Gráficos**: el juego usa OpenGL ES 2.0 sobre Mesa/nouveau. Todos sus contextos se respaldan con uno
  solo real, compartido entre hilos bajo un candado, porque Mesa en Switch no comparte objetos entre
  contextos; las texturas ASTC se descomprimen fuera de ese candado.
* **Audio**: OpenSL ES sobre SDL2.

---

## Créditos

**AirborneNX**: desarrollado por **ItsDroidy06**.

### El port de PS Vita

AirborneNX está basado e inspirado en **[Asphalt 8 Vita](https://gitlab.com/sexcurrybeats/asphalt8-vita)**,
de **sexcurrybeats**. Ese port demostró que el juego podía correr fuera de Android y parte de la lógica
usada aquí fue posible gracias a él: la forma de cargar la biblioteca del juego, de simular su entorno
Java y de saltar la pantalla de edad parten de su trabajo. Sin ese proyecto, este no existiría.

### Código de terceros incluido

* **so_util** (cargador de bibliotecas `.so`), de Andy Nguyen (TheOfficialFloW) y Rinnegatamante.
  Licencia MIT ([lib/so_util/LICENSE](lib/so_util/LICENSE)). Reescrito aquí para ELF64 / AArch64 y Horizon.
* **FalsoJNI** (entorno JNI simulado), de Volodymyr Atamanenko. Licencia MIT
  ([lib/falso_jni/LICENSE](lib/falso_jni/LICENSE)). Incluye partes de la implementación JNI de Dalvik,
  © The Android Open Source Project (Apache 2.0).

### Bibliotecas y herramientas

* **devkitPro, devkitA64 y libnx**: la base para hacer homebrew de Nintendo Switch.
* **Mesa y nouveau** (`switch-mesa`): OpenGL ES y EGL en la consola.
* **SDL2** (`switch-sdl2`): salida de audio.
* **zlib** (`switch-zlib`).
* **Atmosphère**: el entorno donde corre el homebrew.
* **Eden**: el emulador usado durante el desarrollo.
* **baksmali**, **Capstone** y **GNU binutils**: para estudiar cómo funciona el juego.

### El juego

**Asphalt 8: Airborne** es obra y propiedad de **Gameloft**.

---

## Aviso legal

* AirborneNX es un proyecto de fan, **no oficial**, sin relación con Gameloft ni con Nintendo.
* *Asphalt*, *Asphalt 8: Airborne* y *Gameloft* son marcas de Gameloft. *Nintendo Switch* es una marca de
  Nintendo. Todas las marcas pertenecen a sus dueños.
* Este repositorio **no contiene ni distribuye** archivos del juego. Para usar el port necesitas una copia
  del juego obtenida legalmente.
* El port se ofrece **gratis** y tal cual, sin garantía de ningún tipo. Si alguien te lo vende, es una
  estafa. La venta o monetización de los assets de Gameloft está prohibida.
