# Registro de Progreso — N.O.V.A 2 (PS Vita)

## Fase 1: Configuración y Preparación (Completada — 2026-09-05)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `NOVA-2-HD-v1-0-5.apk` copiado y extraído.
- ABI detectada: armeabi-v7a (elegida: armeabi-v7a).
- GLES detectado: valor no estándar en manifest: 0x20000 (declarado en AndroidManifest.xml)

## Fase 2: Decompilación (Completada — 2026-09-05, verificada 2026-09-24)
- jadx: SÍ disponible en `decompiled/apk_jadx/sources/` (el "NO corrido" anterior era erróneo).
- Ghidra: `decompiled/libnova2_armeabi-v7a/ghidra/`, `libStormGLOFT_*/ghidra/`, `libmthook_*/ghidra/`.
- Disasm indexado: `decompiled/disasm/full_libnova2.so.md` (2.1 M líneas), `full_libStormGLOFT.so.md`.

## Fase 3: Análisis del Motor Real (Completada — 2026-09-24)

- **Motor: Gameloft Glitch, no Cocos/Unity, sin hermano confirmado.** Evidencia en strings de
  `libnova2.so`: `GLITCH_OPENGLES_2`, `glitch::IDevice`, `Application::Init(glitch::IDevice* device)`,
  `Freeing anyways (at Glitch exit)`. `libStormGLOFT.so` es un fork Mesa/glcpp interno
  (`glcpp_lex`, `_mesa_*`, `hash_table_*`).
- **GLES:** manifest `glEsVersion 0x20000` = GLES 2.0. NEEDED reales: `libnova2` →
  `libGLESv1_CM, libGLESv2, libc, libdl, liblog, libm, libstdc++`;
  `libStormGLOFT` → `+ libEGL, libandroid, libz`. Híbrido v1 (query compresión) + v2 (shaders).
- **Roles .so:** `libnova2.so` 7.2 MB = juego+lógica (36 JNI, `JNI_OnLoad` con log
  `"JNI_OnLoad called"`); `libStormGLOFT.so` 880 KB = renderer (`JNI_OnLoad` propio en `0x31fdc`);
  `libmthook.so` 26 KB + `mthook/hook.dex` + `HookApplication` = parche "APK Signature Killer"
  del repack, **ignorar**.
- **36 exports `Java_*` en libnova2 (sin `RegisterNatives`):**
  GLGame: `nativeInit, nativeOrientation, nativeCanInterrupt, nativeGyroscope, nativeAccelerometer, nativeTouchPressed/Released/Moved, nativeSetOnKeyDown/Up`;
  GLMediaPlayer: `nativeInit, nativeGetTotalSounds, nativeGetTotalSoundsOfSameInstance, nativeSetStopOnMusic`;
  GameGLSurfaceView: `nativeResume, nativePause, nativeSafePause`;
  GameRenderer: `nativeInit(int,int,int), nativeRender, nativeResize, nativeDestroy, onKeyboardFinish`;
  GLResLoader: `nativeInit`; GLUtils.Device: `nativeInit`; Musicplayer: `nativeInitplayer`;
  installer.GameInstaller: `initNative, nativeStart, getPublicKey, nativeGetDevID`;
  installer.GDRMPolicy: `setLongConst, setConst, initNativeAP, processServer, nativeAllow, setTime, getConstString`.
- **Colisión entre .so:** `GameRenderer_nativeInit/Resize` existen en AMBOS.
  Carga Java (`GLGame.java:91-92`): `loadLibrary("nova2")` luego `loadLibrary("StormGLOFT")`.
  `libStormGLOFT` referencia `/data/data/.../lib/libnova2.so` (probable dlopen). Decisión pendiente.
- **Ciclo de vida driver (`GameRenderer.java:120-144`):** `onSurfaceCreated` →
  `getTextureCompression` (fuerza `"atc"`, riesgo VitaGL) → `GLResLoader.init()` →
  `GLMediaPlayer.init()` → `GLGame.nativeInit()` → `nativeInit(w,h,GLGame.a)` →
  si video no completado lanza `MyVideoView` (`/sdcard/gameloft/games/GloftN2HP/GLVideo.mp4`) y `finish()`.
  `onSurfaceChanged` → `nativeResize`; `onDrawFrame` → `nativeRender` (puerta `isVideoCompleted`).
- **Assets:** APK sin OBB (109 entradas): `assets/data.save`, `res/raw/{igli.bin, data.txt, gi_settings.xml, serialkey.txt, infoversion.txt, raw_000.ogg}`.
  Rutas duras `/data/data/com.gameloft.android.GAND.GloftN2HP/*` y `/sdcard/gameloft/games/GloftN2HP` → remapear.
- **Estado loader:** `main.c` boilerplate (solo `JNI_OnLoad`, loop vacío, sin `nativeInit/Render`);
  `java.c` con tablas vacías (stubs en `generated_jni_stubs.c/table.h` = candidatos sin cablear);
  `dynlib.c` trae `resolve_imports` + EGL/vitaGL en `reimpl/egl.c`, `gl_init` 960x544 MSAA4x.

## Fase 5: Adelanto sin hardware (2026-09-24)
- JNI con valores reales (leídos del Java): network/wifi=1, `isUseNativeDRM`=1 (fiel),
  vols=1.0f, `getLanguage`=1 eng (=`GLGame.a` → `nativeInit(w,h,1)`), `Autologin`/`needToRemoveFacebook`/
  `isDemo`=FALSE, `isCheatEnable`/`getWifiIP`/`playMusic`/`getEmitter`=0, uniqueCode=1.
  `RegisterNatives`=0 en ambos .so → sin tabla que registrar. Objetos aún NULL.
- GL: 95/95 imports mapeados (verificado por parseo ELF + cruce con `dynlib.c`); 3 ex-`ret0`
  reactivados. Sin imports `egl*` (EGL vive en Java + `reimpl/egl.c`). Riesgo: `atc` + texturas
  comprimidas.
- Input en `main.c`: slots 0-4, sampling on, pad→keycodes. Audio: diseño Vox→AudioTrack,
  arranque mudo hasta el log. Assets: layout `ux0:data/nova2/` definido; installer saltado.
- LiveArea: PNGs 128x128/840x500/960x544/280x158 en modo P, XMP limpiado.
- Fix: `asset_manager.cpp` `(cost char*)` → `(const char*)`.
- Plan: duplicados del template eliminados del checklist (§4), notas obsoletas corregidas
  (GLES 0x20000 = 2.0 estándar, `._` = AppleDouble, §6 al día), runbook hardware en §8
  (layout `ux0:data/nova2`, build Debug, orden de hitos `BOOT:`, crash #1/#2, diferidos).
- Límite: build (submódulo FalsoJNI + VITASDK) y primer arranque = consola real.

## Fase 4: Bootstrap del loader (Implementado 2026-09-24 — sin compilar, falta hardware)
- [x] `main.so` = `libnova2.so`; `libStormGLOFT.so` 2º módulo opcional (`0x98000000` / `+0x2000000`,
  `source/utils/init.c`). Colisión GameRenderer: preferir Storm, fallback nova2 (`main.c`).
- [x] `dlopen`/`dlsym` interceptados a ambos módulos; `mthook` ignorado (`source/dynlib.c`).
- [x] Remapeo `/data/data/<pkg>`, `/sdcard/gameloft/games/GloftN2HP`, `/sdcard` → `ux0:data/nova2`
  en fopen/open/opendir/stat/access/mkdir (`source/reimpl/io.c/io.h`).
- [x] `java.c`: 177 callbacks del boot-path cableados (verificado: todo stub existe en el header,
  todo ID tiene nombre, solo Hook 0-11 excluido). `generated_jni_stubs.c` agregado al build.
- [x] `main.c`: secuencia `JNI_OnLoad×2 → ResLoader/Device/MediaPlayer/GLGame.nativeInit →
  Renderer.nativeInit(960,544,1) → nativeResize → loop nativeRender` con hitos `BOOT:`,
  más input touch (slots 0-4) + 12 botones → keycodes (ver Fase 5).
- Pendiente (requiere VITASDK + consola): `git submodule update --init` (FalsoJNI vacío en
  esta máquina), primer build, deploy de `main.so`+`libStormGLOFT.so`+datos y triage del
  primer log con `so-crash-triage`. Ver PORTING_PLAN.md §7.

## Fase 4b: Primer build OK (2026-09-24 — compila, sin probar en consola)
- [x] FalsoJNI clonado en `lib/falso_jni` (HEAD 083d5a0); upstream nuevo requiere
  `converter.c` en `CMakeLists.txt` (utf8/utf16).
- [x] `SO_PATH` = `ux0:data/nova2/libnova2.so` (antes `main.so`, no coincidía con `ux0_data/`).
- [x] `/data/data/<pkg>` → `ux0:data/nova2/saves/` (g_saveDirPath: game_save/level_save/
  settings/interface.dat). `/sdcard/...` sigue → `ux0:data/nova2/`. remove/rename/unlink remapeados.
- [x] Logger escribe además a `ux0:data/nova2/logs/nova2_NNNN.log` (número incremental,
  sin colores ANSI, sceIo sin buffer para sobrevivir crashes) — `source/utils/logger.c`.
- [x] Build fixes: `glBlendColor`/`glCompressedTexSubImage2D`/`glSampleCoverage` no existen
  en el vitaGL instalado → stubs no-op con warning (`dynlib.c`); el vitaGL instalado trae EGL
  propio → los nuestros renombrados `*_soloader` por macro en `reimpl/egl.h`;
  `generated_jni_stubs.c` usaba `fjni_logv_dbg` sin args → `fjni_log_dbg` + include del logger.

## Fase 5: Crash #1 — JNI_OnLoad con JavaVM nulo (corregido 2026-09-24, falta reprobar)
- Log `logs/nova2_0001.log`: último hito `BOOT: calling libnova2 JNI_OnLoad` + `[ALOG][GLGame] JNI_OnLoad called`.
- Dump: Data abort en `libnova2.so+0x229850` = `JNI_OnLoad+0x3c`, `ldr pc,[r3,#24]`, R3=0.
  Ghidra: `(*vm)->GetEnv(vm, &env, 0x10004)`. `main.c` pasaba `&g_jvm_slot` (puntero a NULL).
- Fix: `JNI_OnLoad(&jvm, NULL)` (JavaVM de FalsoJNI) y `&jni` como `JNIEnv*` en todos los
  entry points de `main.c` (antes NULL; los nativeInit hacen `NewGlobalRef`/`GetStaticMethodID`).
- libStormGLOFT.so: exporta SOLO `JNI_OnLoad`, que es un hooker (inline + GOT sobre libnova2
  vía `dlopen(...)+0x8c` = base de bionic soinfo, `mprotect`, gl*Tex*/glViewport/glShaderSource/
  __android_log_*). Bajo so_util parchearía memoria al azar → ya no se carga
  (`-DLOAD_STORM_HOOKS` para experimentar). La "colisión GameRenderer" no existía.
- Adelantado (crash #1/#2 previstos en PLAN §8.4): `source/jni_impl.c` con los getters reales —
  `getTextureCompression`="atc", `getVersion`="1.0.5", `getHostName`, `GLResLoader.getResource{Full,
  Bytes,Length}` (→ `assets/`), `getSoundRaw`/`getResourceLengthSoundRaw` (→ `res/raw/raw_NNN.ogg`).
  Entrada `getResourceFull(I)` (94) quitada de la tabla: FalsoJNI busca solo por nombre.
- Datos: `/sdcard/gameloft/games/GloftN2HP` → `ux0:data/nova2/GloftN2HP/` (estructura del usuario).
- Nombres pedidos por el .so y aún sin registrar (devuelven methodID NULL, no crashean en
  FalsoJNI): AudioTrack (`<init>`, getMinBufferSize, write, pause, release) → audio mudo;
  reproductor de música (PlayBGMusic, GetNumSongs...), getSystemService, randomUUID.

## Fase 6: Crash #2 — stack overflow en ALicenseCheck (corregido 2026-09-24, falta reprobar)
- Log `nova2_0002.log`: JNI_OnLoad, sounds=635/2, los 4 nativeInit y `ADevice Init` OK; último
  mensaje `[ALC] Release version: 1.4.5`.
- Dump: data abort en el `push` del prólogo de `ALicenseCheck::CallJNIFuncChar` (+0x675748), LR en
  `LoadConfig+0x58`. SP=0x81500ef0, 256 KB debajo del SP del boot 1: `LoadConfig` hace
  `sub sp, #0x40000` (LoadRMS/SaveRMS: 0x80000) → overflow del hilo principal (256 KB).
- Causa de fondo: stub `isUseNativeDRM` devolvía 1 (fiel al Java) → `GLGame.nativeInit` entra al
  DRM Gameloft (`ALicenseCheck_InitLicense`).
- Port de referencia: **Dungeon-Hunter-2-vita** (mismo motor glitch/Vox/ALicenseCheck, misma época).
  De ahí: bypass del DRM y el bug de `SWP`.
- Fixes:
  - `isUseNativeDRM` → 0 (`jni_impl.c`).
  - `patch.c`: `ALicenseCheck::Init`, `InitLicenseVM`, `ValidateServer` → no-op; `LoadRMS`/`SaveRMS` → 1.
    (`ALicenseCheck_InitLicense`/`_ValidateLicense` son `b` de 4 bytes pegados a la siguiente
    función: se hookea el destino para no pisar LoadRMS/ConfigFile.)
  - `sceUserMainThreadStackSize = 4 MB` (`main.c`, igual que Advena/ILLUSIA/Inotia3/PES 2012).
  - Preventivo: 27 `SWP` inlineados (spinlock pool de boost) en `glitch::io::CMemoryReadFile`
    ctor/dtor/clone y `shared_ptr<char>::reset` → cada uno reemplazado por `BL` a un thunk
    LDREX/STREX en la arena RX de so_util (DH2 crasheó con 0x30002 por esto). Verificado offline:
    las 4 funciones guardan LR y a cada SWP le sigue `cmp`; encoding comparado con `as`.
- Ojo FalsoJNI: un método int NO registrado devuelve **-1** (verdadero), no 0.

## Fase 7: "unknown symbol uname" (corregido 2026-09-24, falta reprobar)
- Log `nova2_0003.log`: DRM bypass + `patch_swp: 27/27` OK; llega a `GameRenderer.nativeInit` →
  `app_Android.appInit` → `glitch::CAndroidOSDevice` ctor, que llama `uname()` → dummy "unknown symbol".
- Barrido completo de imports de libnova2 vs `dynlib.c`: faltaban 11 → agregados todos
  (`uname`, `syslog`, `inet_addr`, `wprintf`, `cosh`, `frexpf`, `modff`, `__aeabi_d2iz`,
  `__dso_handle`, `_ZSt7nothrow`, `_ZnajRKSt9nothrow_t`). Ahora 0 imports sin resolver.
  `uname` con layout bionic de 6 campos (valores de Dungeon-Hunter-2-vita); el motor solo loguea `version`.

## Fase 8: Crash #3 — Vox con pack de sonido NULL / DATA FOLDER NOT FOUND (corregido 2026-09-24, falta reprobar)
- Log `nova2_0005.log`: ¡llega a `entering render loop`! Pero `appInit: DATA FOLDER NOT FOUND` y todos
  los packs fallan con rutas relativas (`fopen(sprites.gla)`, `strings.gla`, `effects.gla`, `oconf.bar`,
  `sounds_hi.glza`...).
- Dump: data abort en `VoxSoundManager::Init+0x16c`, `ldr ip,[r9]` con R9=0 → llamada virtual sobre el
  lector de `sounds_hi.glza`, que no se abrió.
- Causa: `appInit` hace `fileSystem->changeWorkingDirectoryTo("/sdcard/gameloft/games/GloftN2HP")`
  → glitch `CFileSystem` → `chdir()` crudo de newlib (sin remapeo) → falla; y aunque anduviera,
  SceLibcBridge (USE_SCELIBC_IO) no usa el cwd de newlib.
- Fix (`reimpl/io.c`): cwd propio en formato Android (`g_cwd`, default la carpeta de datos);
  `chdir_soloader`/`getcwd_soloader`; `soloader_remap_path` resuelve rutas relativas contra `g_cwd`
  antes de remapear (lo usan todos los wrappers). `lstat` estaba mapeado crudo (escribía una
  `struct stat` de newlib en un buffer bionic) → ahora `lstat_soloader` = `stat_soloader`.
- Mejora: `source/audiotrack.c` — `android.media.AudioTrack` sobre `sceAudioOut` (puerto BGM,
  grano 1024 frames; `write()` bloqueante = ritmo de MODE_STREAM). Vox: 44100 Hz estéreo PCM16,
  `getMinBufferSize` = 8192 → escribe exactamente un grano por llamada. IDs 200-206 en `java.c`.
  Antes: "AudioTrack driver could not initialize" (sin sonido).

## Fase 9: Pantalla negra en el render loop (en curso 2026-09-24, falta reprobar)
- Log `nova2_0006.log`: sin crash. Carga todos los `.gla`, `AudioTrack` abre el puerto 256, compila
  shaders; loop de render corriendo pero pantalla negra. Texturas: `[ATC] ATC Texture Loading`,
  `[IMAGE-LOADER] == ATC ==` → vitaGL no conoce ATC → texturas rechazadas.
- Referencias: los ports Gameloft terminados (GT Racing Motor Academy HD — mismo motor glitch/GLES2 —,
  Asphalt 5/6, BiA2, DH2) compilan **vitaGL desde fuente** con
  `SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2` y sin MSAA.
  Ninguno decodifica ATC.
- Cambios:
  - `lib/vitagl` vendorizado desde GT Racing (ya parchado para este vitasdk); `Makefile` →
    `vitagl.mk` (la copia temporal del toolkit descarta archivos llamados "Makefile").
    Flags de arriba + `LOG_ERRORS=1`; errores de vitaGL → log vía `vgl_log()`.
  - `reimpl/egl.c/.h` = los de GT Racing (el `lookup.c` de vitaGL necesita todos los `egl*` con su
    nombre real → fuera el renombrado por macro); + `SceRtc_stub`.
  - `gl_init()` como GT Racing: `vglSetupRenderTargetScenesNum(8,8)`, shader cache en
    `ux0:data/nova2/shader_cache`, `vglUseCachedMem`, 12 MB, **MSAA 4X → NONE**, idempotente
    (antes `main.c` y `eglInitialize` llamaban dos veces a `vglInitExtended`).
  - `glCompileShader`/`glLinkProgram`: se loguea el info-log si fallan; el `.gxp` se guarda solo si
    compiló y se crea el directorio (antes 52 `file_save` fallidos por falta de `gxp/`).
  - `reimpl/atc.c`: decodificador ATC por software en `glCompressedTexImage2D` — ATC RGB → RGB565,
    ATC explicit/interpolated alpha → RGBA8888 (bloques iguales a DXT3/DXT5 alfa + color ATC).
  - Audio: logs en play/pause/stop y en write() descartado (no apareció "first write" en 0006).

## Fase 10: Crash #4 — serialize_shader con programa NULL (corregido 2026-09-24, falta reprobar)
- Log `nova2_0007.log` (80 líneas, regresión vs 0006 con 706): se corta en
  `effects.gla`/`glsl.config` durante `appInit`, antes de ATC/menús. Sin crash en el `.so`.
- Dump `...3387...psp2dmp`: Data abort con PC en `SceLibKernel+0x16` (memcpy del
  kernel) → frame relevante = LR en `serialize_shader+0x76` de NUESTRO loader:
  `ldrd r1,r2,[r6,#796]` carga `s->prog=NULL` (R1=0) + `s->size`=basura enorme
  (R2=0x826d67e0) → `memcpy(buf, NULL, gigante)`. Verificado por objdump
  (`serialize_shader @0x81037c9c`, crash en el `blx sceClibMemcpy` de
  `vgl_fast_memcpy(buf, s->prog, s->size)`).
- Causa: `compile_shader()` (cache `HAVE_SHADER_CACHE`) llamaba a
  `serialize_shader()` + `sceIoWrite()` INCONDICIONALMENTE, incluso cuando
  `shark_compile_shader_extended()` fallaba (`s->prog=NULL`, `s->size` sin setear).
  El primer shader de efectos falla → crash antes de cualquier render. En 0006 el
  vitaGL era el del sistema (sin este path de caché) por eso llegaba a menús.
- Fixes (`lib/vitagl/source/custom_shaders.c`, vendored — rebuild verificado, build OK):
  - `serialize_shader`: guard `!s->prog || !s->size` → `*sz=0`, `NULL` (no toca memcpy).
  - `compile_shader`: solo escribe caché si `s->prog && s->size`, con `f>=0` y `buf&&sz`.
  - Lectores de caché (`glCompileShader` + 2 en `glLinkProgram`): ignoran entradas
    envenenadas (`sz<=4`, read truncado) y conservan `s->source` para recompilar si
    `unserialize` no produce `prog` (las corridas con el bug pueden haber dejado
    `.gxp` de 0 bytes en `shader_cache/`).
  - `unserialize_shader`: hardening completo — tope `matrix_uniforms_num<=64`,
    cheques de límites en binds/samplers, `size` en `(0, 2 MB]`, `malloc!=NULL`,
    índices de matriz `<=1024` validados ANTES de registrar en el patcher (si falla,
    `prog=NULL/size=0`, el llamante recompila).
- Mejora port (`source/utils/glutil.c`): `gl_init()` re-crea `shader_cache/v` y `/f`
  tras `vglInitExtended` (el init ya los crea, esto es seguro contra reordenamientos).
- Nota build: el ELF fresco es `build/nova2` (`serialize_shader` movido a `0x81037d64`
  tras el fix); `build/nova2.elf` quedó stale (22:03) de un layout anterior — para el
  próximo triage usar `build/nova2` (es lo que `psvita-toolkit analyze` ya hace).
- Antes de reprobar en consola: **borrar `ux0:data/nova2/shader_cache/`** (posibles
  `.gxp` envenenados de 0007) y desplegar el `eboot.bin` nuevo. Esperado: vuelve el
  recorrido de 0006 (ATC → menús) con `glCompileShader FAILED` logueado en vez de crash
  si algún shader no compila con ShaRK.

## Fase 11: Pantalla / velocidad / sticks (2026-09-25, build OK sin probar en consola)
- Reporte: pantallas de carga/título arriba-izquierda con negro; el resto jugable con touch.
- Análisis: el motor conoce 960x544 reales (scale=1, `appInitW[960]H[544]`, touch
  alineado) → un remap global tipo DH2 (960x640→544) ROMPERÍA lo que ya funciona.
  Las imágenes chicas son layout, no viewport: se instrumenta antes de tocar nada.
- Diagnóstico (passthrough, cero cambio de conducta, quitar al confirmar):
  `glViewport_soloader`/`glScissor_soloader` (`glutil.c`, cableados en `dynlib.c`)
  loguean cada rect DISTINTO como `[glvp]`; cada press real se loguea como
  `[touch] press(x,y) slot`. El próximo log dice qué rect usan las pantallas
  estáticas y dónde están los botones/joystick.
- Velocidad (set SEGURO de DH2, probado en hardware en la misma familia
  glitch/GLES2, todo CPU-only): `HAVE_GLSL_UBOS=1 SAMPLERS_SPEEDHACK=1
  MATH_SPEEDHACK=1 CIRCULAR_POOL_SPEEDHACK=1 NO_TEX_COMBINER=1` (se mantiene
  `DRAW_SPEEDHACK=2`; excluidos a propósito: stencil/primitivas/GPU-path/OC>444).
  Verificado: `libnova2` es shader-path (`glUseProgram`, solo `glLoadIdentity`/
  `glMatrixMode` como FFP) → los hacks FFP son no-ops para él. Relojes siguen
  en 444/222/222/166 (stock seguro).
- Sticks (el motor no tiene path analógico de keys: mover = `AnalogJoystick`,
  mirar = `FullScreenRotateJoystick`, ambos toman el punto de touch como origen):
  `main.c` ahora fija `SCE_CTRL_MODE_ANALOG` (en DIGITAL lx/ly siempre dan 128 —
  los sticks estaban muertos), y cada stick maneja un touch sintético en slot
  reservado (MOVE=3 ancla 140,400; LOOK=4 ancla 720,272, radio 90, histéresis
  0.30/0.15). Los dedos reales solo usan slots 0-2 (nunca pisan un slot de
  stick). DPAD/botones → keycodes como antes. Engages se loguean `[syntouch]`.
- Gotcha build: `set(... CACHE STRING ...)` no actualiza flags ya cacheadas —
  tras cambiar `VITAGL_MAKE_FLAGS` hay que `cmake -B build -UVITAGL_MAKE_FLAGS`
  (si no, vitaGL no se reconstruye aunque el stamp exista). Ya aplicado;
  `libvitaGL.a` + `eboot.bin` frescos con el set seguro.
- Para la próxima ronda pedir: `nova2_0008.log` (líneas `[glvp]`/`[touch]`/
  `[syntouch]`), una foto de la pantalla de carga/título, y si el stick
  izquierdo mueve o rota la vista (calibra `MOVE_ANCHOR_*`).

## Fase 12: Mundo 960x640 + sticks que caminan + fire (2026-09-25, build OK sin consola)
- Datos del reporte (log 0009 de 8967 líneas + 3 capturas medidas a píxel):
  - Viewports: SOLO `(0,0,960,544)`, `(0,0,960,640)` (alternados cada frame) y
    `(0,0,1024,1024)` (FBO/sombras). El 960x640 = canvas lógico "retina/HD" del
    engine (igual que DH2); con superficie de 544 se pierden 96 filas (texto
    "DROPPING BY" cortado en la captura).
  - Contenido 2D (carga/título): 800px exactos de ancho, pegado a la izquierda
    (kd 0..799, hd 0..800; gameplay kb full 960). Causa: buckets `Is*Res()` —
    con 960x544 ningún bucket matchea y el 2D cae al default 800x480.
  - Touches reales: joystick en x 74-229/y 395-484 (centro ~140,440, r~80);
    fire en ~(844,411); look-drags al centro. El ancla vieja (140,400) quedaba
    al borde del círculo → el stick izquierdo caía a FullScreenRotateJoystick
    (ambos sticks movían cámara, nada caminaba). Sensibilidad brutal = salto
    directo de 90px en un frame.
  - Medición HUD en kb: joystick~(140,440), fire~(886,428), botón
    acción~(921,482) en coords físicas.
- Cambios:
  - `main.c`: engine reportado como 960x640 (`nativeInit/Resize(960,640)` →
    `Is960x640Res` TRUE, layouts de 960; EGL sigue en 960x544 reales como DH2).
    Touch real convertido a lógico (`TOUCH_LOGICAL_Y = y*640/544`).
  - `glutil.c`/`dynlib.c`: remap `glViewport`/`glScissor` ×(1.0, 0.85) SOLO con
    FBO 0 (nuevo `glBindFramebuffer_soloader` rastrea el bind; pases 1024 a FBO
    pasan intactos). Log `[glvp]` ahora muestra lógico->físico.
  - Sticks: drag por pasos (`SYN_STEP`=12px/frame → giro proporcional, sin
    saltos); ancla de move ADAPTATIVA (media móvil de press reales en mitad
    izquierda, semilla 140,440); slots extendidos a 6 (dedos 0-2, move 3,
    look 4, fire/acción 5; ids ≤7 válidos en TouchScreenBase).
  - Botones con coords lógicas medidas: R1 = FIRE hold (886,503),
    Cross = tap acción 6 frames (921,567); Cross/R1 salen del pad_map de keys
    (el engine las ignora en juego). Resto de botones/DPAD por keys (menús).
  - NOTA: compresión vertical 15% global (círculos levemente elípticos) — mismo
    trade-off aceptado en DH2 a cambio de fullscreen real.
- Probar: captura de carga/título (¿fullscreen?), ¿stick izq. camina?,
  ¿velocidad de cámara sana?, ¿R1 dispara?, ¿qué hace Cross (jump o arma?).
  Traer log + capturas. Pendiente si algo falla: eglGetProcAddress devuelve las
  reales de vitaGL (bypass a wrappers) — cablear a la tabla soloader si el
  `[glvp]` mostrara valores sin remapear.
