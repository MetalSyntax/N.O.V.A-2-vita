# Plan de Port — N.O.V.A 2 (PS Vita)

> Generado por psvita-port-toolkit el 2026-09-05. Punto de partida con lo detectado automáticamente --
confirmar todo con objdump/Ghidra/jadx a mano antes de asumirlo como cierto.

## 0. Contexto

- **Juego:** N.O.V.A 2
- **Paquete Java:** com.gameloft.android.GAND.GloftN2HP
- **APK original:** `NOVA-2-HD-v1-0-5.apk`
- **TITLEID asignado:** `PSVNOVA02`

**¿Motor conocido?** Revisar si algún port hermano (bajo la misma BASE_DIR) comparte motor antes de
reusar su código -- confirmar con símbolos JNI reales, no por analogía superficial.

## 1. Detección automática

- **ABI(s):** armeabi-v7a
- **ABI elegida:** armeabi-v7a
- **Nota de arquitectura:** armeabi-v7a presente -> ARMv7 (hard-float/NEON disponible). El CPU de Vita (Cortex-A9) corre esto sin traducción.
- **Versión de GLES:** `0x20000` = GLES 2.0 (constante estándar `GL_ES_VERSION_2_0`,
  el "no estándar" del aviso automático era erróneo). Confirmado por NEEDED:
  `libGLESv1_CM + libGLESv2` (nova2), `+ libEGL` (Storm).

## 2. .so encontrados (ABI armeabi-v7a)

- `nova2_extract/lib/armeabi-v7a/libStormGLOFT.so` (880 KB)
- `nova2_extract/lib/armeabi-v7a/libmthook.so` (26 KB, repack — ignorar, ver §3)
- `nova2_extract/lib/armeabi-v7a/libnova2.so` (7354 KB)

(Los `._lib*.so` de 4 KB que listaba el aviso automático son metadatos AppleDouble
de macOS, no librerías.)


## 3. Exports JNI (convención `Java_*`)

**Corrección 2026-09-24 (verificado contra el .so real con strings + índice disasm):**
el aviso automático ("no se encontraron exports") era falso. Confirmado:

- `libnova2.so` (7.2 MB): **36 exports `Java_*`**, sin `RegisterNatives` (0 ocurrencias).
  `JNI_OnLoad` presente (log `"JNI_OnLoad called"`, usa `GetEnv`+`FindClass` 1 vez c/u).
- `libStormGLOFT.so` (880 KB): `JNI_OnLoad` propio + **2 exports** que **colisionan**
  con los de libnova2: `GameRenderer_nativeInit`, `GameRenderer_nativeResize`.
  Contiene además el string `/data/data/com.gameloft.android.GAND.GloftN2HP/lib/libnova2.so`
  → probable `dlopen` interno. Orden de carga Java: `nova2` primero, `StormGLOFT` después
  (`GLGame.java:91-92`).
- `libmthook.so` (26 KB): 1 export `Java_cc_binmt_signature_Hook_hookOpen`.
  Es inyectado por la herramienta "APK Signature Killer" (`HookApplication` en manifest,
  `mthook/hook.dex` en el APK) — **no es del juego original, ignorar y no embarcar**.

Lista completa de los 36 en `port_progress.md` Fase 3. Detalle de direcciones en
`decompiled/disasm/full_libnova2.so.md` (índice, ej. `nativeInit @0x002239bc`,
`GameRenderer_nativeInit @0x00225534`).


## 4. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (ciclo de vida nativo, reuso de otro port o boilerplate genérico).
  Verificado 2026-09-24: motor **Gameloft Glitch** (no Cocos/Unity; sin hermano confirmado).
  Ciclo driver = `GameRenderer.onSurfaceCreated/onSurfaceChanged/onDrawFrame` (detalle en §6).
- [x] Bootstrap del loader (2026-09-24, sin compilar — ver §7): carga dual, dlopen
  interceptado, remapeo de rutas, FalsoJNI del boot-path, secuencia nativeInit/Render en `main.c`.
- [x] Tabla JNI (FalsoJNI) — cableada 2026-09-24: 177 métodos + valores escalares reales
  desde el Java (wifi/network=1, DRM=1 fiel, volúmenes=1.0f, idioma=eng, sin demo/cheats/roaming;
  `RegisterNatives` ausente en ambos .so → nada que registrar). Objetos (`byte[]`, emitters)
  aún NULL: primer crash esperado en `getTextureCompression`/`getResource*` (ver §7).
- [x] Gráficos (2026-09-24, estático): 95/95 imports gl/egl de ambos .so mapeados en `dynlib`
  (vitaGL + `glShaderSource_soloader`; 3 ex-no-op reactivados — si el link falla, revertir).
  EGL en `reimpl/egl.c`; sin imports `egl*`. Riesgo abierto: compresión `atc` forzada.
- [x] Input (2026-09-24, `main.c`): touch frontal con 5 slots estables, sampling explícito,
  12 botones → keycodes DPAD+BUTTON_* con flancos. Coordenadas /2 a 960x544.
- [x] Audio (diseño, sin código): VoxEngine nativo + stb_vorbis → sink Java `AudioTrack`;
  arranque mudo hasta que el log muestre la secuencia PCM (backend SceAudio entonces).
- [x] Assets (layout definido en §8): nativo lee `DATA_PATH/assets/`; `GameInstaller` saltado.
- [x] LiveArea/VPK (2026-09-24): PNGs correctos en modo-P sin metadatos; VPK con lista
  explícita. Falta solo el build.
- [ ] Primer arranque en consola real (runbook mapeado en §8 — ejecutar en la
  máquina con VITASDK + Vita).
- [ ] Pruebas en hardware real (idem §8: un bug a la vez con `so-crash-triage`).

## 5. Herramientas

Este port se gestiona con **psvita-port-toolkit** (standalone, fuera de este repo). Desde el
toolkit: `Continuar con un port existente` → elegí esta carpeta (ya tiene `.psvita-toolkit.json`).

## 6. Motor real confirmado (2026-09-24, evidencia en `port_progress.md` Fase 3)

- **Motor:** Gameloft Glitch (`GLITCH_OPENGLES_2`, `glitch::IDevice`, `Application::Init`).
  GLES 2.0 (`glEsVersion 0x20000`); `libnova2` linka `libGLESv1_CM + libGLESv2`,
  `libStormGLOFT` linka `libEGL + libGLESv1_CM + libGLESv2` (Mesa/glcpp fork interno).
- **Driver de arranque (único camino):** `GameRenderer.onSurfaceCreated` →
  `GLResLoader.init()` → `GLMediaPlayer.init()` → `GLGame.nativeInit()` →
  `GameRenderer.nativeInit(w, h, GLGame.a)`; `onSurfaceChanged` → `nativeResize`;
  `onDrawFrame` → `nativeRender` (solo si `MyVideoView.isVideoCompleted()!=0`).
  Video intro: `/sdcard/gameloft/games/GloftN2HP/GLVideo.mp4` (si no completado, lanza
  `MyVideoView` y `finish()` — el loader debe devolver "completado" o saltarlo).
- **Riesgos abiertos:** colisión `GameRenderer_nativeInit/Resize` entre los dos .so
  (resuelta por preferencia+fallback en `main.c`, a confirmar en log);
  `getTextureCompression` fuerza `"atc"` (VitaGL sin ATC); stubs objeto aún NULL
  (crash #1/#2 previstos en §7).
- **Estado actual:** `main.c` con secuencia de arranque + input; `java.c` con 177 métodos;
  `dynlib.c` con dlopen/dlsym dual + GL 95/95; `io.c` con remapeo; todo sin compilar
  (falta VITASDK + submódulos). Detalle en §7, runbook en §8.

## 7. Bootstrap implementado (2026-09-24 — LÍMITE SOFTWARE, falta hardware)

Decisiones tomadas en código (todo verificable por lectura; **sin compilar**: no hay
VITASDK ni submódulos en esta máquina):

- **Layout de datos en Vita:** `SO_PATH` = `ux0:data/nova2/main.so` ← `libnova2.so`;
  `ux0:data/nova2/libStormGLOFT.so` ← renderer (opcional: sin él se usan los símbolos
  GameRenderer de libnova2). `libmthook.so` no se embarca.
- **Carga dual** (`source/utils/init.c`): `so_mod` en `0x98000000`, `storm_mod` en
  `+0x2000000`, cada uno con relocate+resolve+flush+initialize. Colisión GameRenderer:
  se prefiere Storm (último `loadLibrary` en Java) con fallback a nova2 (`main.c`).
- **dlopen/dlsym** (`source/dynlib.c`): `dlopen_soloader` rutea `libnova2`/`libStormGLOFT`
  a los módulos ya cargados (Storm referencia a nova2 por ruta absoluta) e ignora `mthook`;
  `dlsym_soloader` busca en ambos módulos (antes: dummy/NULL siempre).
- **Rutas** (`source/reimpl/io.c`): `/data/data/<pkg>/…`, `/sdcard/gameloft/games/GloftN2HP/…`
  y `/sdcard/…` → `ux0:data/nova2/…` en fopen/open/opendir/stat + nuevos
  `access_soloader`/`mkdir_soloader` (el engine chequea la carpeta de datos al arrancar).
- **FalsoJNI** (`source/java.c`): 177 métodos del boot-path cableados (IDs = generated,
  menos Hook 0-11); stubs aún no-op. `CMakeLists.txt` ahora compila `generated_jni_stubs.c`
  (antes faltaba → link roto).
- **Arranque** (`source/main.c`): `JNI_OnLoad` de ambos .so → `GLResLoader/Device/GLMediaPlayer/
  GLGame.nativeInit` → `GameRenderer.nativeInit(960,544,0)` → `nativeResize` → loop
  `nativeRender`+`gl_swap`. Hitos con `BOOT:` en el log; input (touch/teclas) pendiente
  del primer frame.
- **Puerta inevitable del hardware:** `git submodule update --init` (FalsoJNI vacío aquí),
  build con VITASDK, desplegar `main.so`+`libStormGLOFT.so`+datos vía psvita-toolkit y
  leer el primer log en consola real (un bug a la vez, skill `so-crash-triage`).
- **Bonus 2026-09-24:** typo `cost char` en `asset_manager.cpp` (rompía el build) corregido;
  verificación de balanceo en todos los editados OK (falso positivo conocido: llaves
  partidas por `#ifdef` en `AAsset_read`).
- **Crash esperados #1/#2:** `GLGame.getTextureCompression()` y `GLResLoader.getResource*`
  devuelven NULL (FalsoJNI sin API de alloc sin submódulo); el log dirá cuál pega primero,
  fix = construir el `jbyteArray` real entonces.

## 8. Runbook hardware (mapeado 2026-09-24 — ejecutar en la máquina con VITASDK + Vita)

### 8.1 Datos en la Vita (`/ux0:/data/nova2/`, cf. `.psvita-toolkit.json`)

| Destino | Origen en este repo / APK |
|---|---|
| `libnova2.so` | `nova2_extract/lib/armeabi-v7a/libnova2.so` |
| `GloftN2HP/*` (`*.gla`, `sounds_hi.glza`, `oconf.bar`, ...) | carpeta sdcard `/sdcard/gameloft/games/GloftN2HP` tal cual |
| `saves/` | `/data/data/<pkg>/*.dat` (remapeo `io.c`) |
| `logs/nova2_NNNN.log` | lo escribe el loader, uno nuevo por arranque |
| ~~`libStormGLOFT.so`~~ | NO se carga (hooker de terceros, ver port_progress Fase 5) |
| `assets/data.save` | `NOVA-2-HD-v1-0-5.apk :: assets/data.save` |
| `res_raw/igli.bin, data.txt, gi_settings.xml, serialkey.txt, infoversion.txt, raw_000.ogg` | `APK :: res/raw/*` (los deja el loader vía stubs cuando sean reales) |
| `*.dat` (game_save, interface, level_save, levels_save, settings, trophy, upgrades) | se crean solos; el remapeo `io.c` los lleva aquí |

`libmthook.so` / `mthook/hook.dex` NO se copian. Saves y settings viven bajo el mismo
dir gracias al remapeo (sin él el engine escribe a `/data/data/...` y falla).

### 8.2 Build

```bash
git submodule update --init   # FalsoJNI vacío en esta máquina; sin esto no compila
export VITASDK=/ruta/al/vitasdk
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build   # Debug: log + BOOT: visibles
```

Si el link protesta por `glBlendColor`, `glCompressedTexSubImage2D` o `glSampleCoverage`,
revertir esas 3 líneas a `ret0` en `dynlib.c` (ver §4 Gráficos).

### 8.3 Deploy

Con psvita-toolkit (`Continuar con un port existente`, Vita en `192.168.3.15`):
VPK a `sce_sys/` ya empaquetado por CMake; `eboot.bin` + `/ux0:/data/nova2/*` por FTP.
Logs en `/ux0:/data/nova2/logs`.

### 8.4 Lectura del primer log (orden esperado de hitos `BOOT:`)

1. `FIOS initialized / kubridge check / SO relocated / imports resolved / patched /
   caches flushed / initialized` — si falla acá: `.so` ausente o relocate (anotar dirección).
2. `calling libnova2 JNI_OnLoad` → debe seguir `calling libStormGLOFT JNI_OnLoad`
   (o `Storm not found`, boot degradado con símbolos nova2).
3. `sounds=N same=M` — totales del engine de audio (si crashea acá: firma getter).
4. `GLResLoader → GLMediaPlayer → Device → GLGame nativeInit` — el primer callback
   `.so→Java` que pida objeto = **crash #1 (`getTextureCompression`) o #2
   (`getResource*`)**: sale NULL hoy. Fix = construir el `jbyteArray` real con la API
   FalsoJNI (disponible tras §8.2) y reintentar. Un bug a la vez (`so-crash-triage`).
5. `GameRenderer.nativeInit(960,544,1) → nativeResize → entering render loop` —
   pantalla negra inicial esperable (`atc` sin VitaGL); input ya activo para probar a ciegas.

### 8.5 Diferidos a propósito (no son olvidos)

- `GLiveMain.nativeInit` / `IGPActivity.nativeInit` (lista auto-detectada): activities
  separadas que solo se lanzan vía `launchGLLive/launchIGP` (stubs no-op) → inalcanzables
  en este boot; se cablean cuando el menú las pida.
- `MyVideoView`/intro `GLVideo.mp4`: saltado por construcción (`main.c` llama a
  `nativeRender` directo, sin la puerta `isVideoCompleted` del Java).
- `nativePause/Resume/SafePause/Destroy`, sensores y `nativeEnd`: con el primer frame
  en pantalla.

## Apéndice: lifecycle methods auto-detectados (psvita-toolkit)

Native methods whose name matches a well-known Android/GL app lifecycle hook --
these are the ones `main.c`/the loader most likely needs to call directly to
drive the game (there's no real Android `Activity`/`GLSurfaceView` calling them
for you). Estado: los 4 del boot-path ya llamados en `main.c`; `GLiveMain`/`IGPActivity`
diferidos (§8.5).

- `com.gameloft.android.GAND.GloftN2HP.GLGame.nativeInit(void)`
- `com.gameloft.android.GAND.GloftN2HP.GLMediaPlayer.nativeInit(void)`
- `com.gameloft.android.GAND.GloftN2HP.GLResLoader.nativeInit(void)`
- `com.gameloft.android.GAND.GloftN2HP.GLUtils.Device.nativeInit(void)`
- `com.gameloft.android.GAND.GloftN2HP.GLiveMain.nativeInit(void)`
- `com.gameloft.android.GAND.GloftN2HP.GameRenderer.nativeInit(int, int, int)`
- `com.gameloft.android.GAND.GloftN2HP.GameRenderer.nativeRender(void)`
- `com.gameloft.android.GAND.GloftN2HP.GameRenderer.nativeResize(int, int)`
- `com.gameloft.android.GAND.GloftN2HP.IGPActivity.nativeInit(void)`
