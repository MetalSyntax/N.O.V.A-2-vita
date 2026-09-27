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

## Fase 13: vuelta a 960x544 1:1 + fondos estáticos estirados por sprite (2026-09-25, build OK sin consola)
- Log 0010: el mundo 960x640 de la Fase 12 rompía el juego 3D (otra cámara/
  desborde) y el 960x640 resultó ser un FBO del juego, no la pantalla.
- Revertido: engine reportado 960x544 real, touch 1:1, `glViewport`/`glScissor`
  passthrough total (el zoom por viewport tampoco servía: el fondo se pinta
  bajo viewport 960x544).
- Causa real de las barras (capturas kd/hd 2026-09-24): `GS_Loading::Render`/
  `GS_Splash::Render` pintan el fondo con `CSprite::PaintFrame(bg,f,0,0,0..)`
  a tamaño nativo 800x480; la UI ya usa g_ScreenW/H 960x544.
- `patch.c` `patch_bg_stretch`: trampolín en `PaintFrame` (0x273038); si el
  frame mide ~800x480 y se pinta en (0,0), los 2 `bl draw2DRectangle` de
  `PaintModule` (0x272df8/0x272e2c) escalan el rect destino a 960x544.
  HUD/textos/3D no se tocan. Log `[bg] stretch sprite ... frame ...` una vez
  por fondo.
- Probar: carga/título a pantalla completa, juego 3D igual que antes.

## Fase 14: stick izq. camina + recorte de log por frame (2026-09-26, build OK sin consola)
- Log 0014 + captura kb 2026-09-24-230504: el stick izq. giraba la cámara.
  Causa: el ancla adaptativa de move se había ido a (180,293) por los toques
  de MENÚ en la mitad izquierda; el press sintético caía fuera del joystick
  (zona de FullScreenRotateJoystick). Los press reales del dedo caminando
  están en ~(80,465), fire en ~(840,420).
- `main.c`: anclas re-medidas 1:1 — joystick (84,462), fire (840,420),
  acción (915,497), STICK_RADIUS 60 (aro ~70px). El ancla solo adapta con
  press a <60px de la semilla.
- FPS: el logger escribe cada línea con sceIoWrite síncrono. En juego había
  ~6 líneas/frame (glBlendColor x2, [glvp] x3-4) + fopen/fclose de
  sounds_hi.glza en cada sonido. `glBlendColor` avisa 1 vez, `[glvp]` solo
  los primeros 24 cambios, fopen/fclose sin log en éxito (fallos sí).
- Probar: ¿stick izq. camina?, ¿R1/Cross aciertan?, contador FPS arriba a la
  izq. (captura mostraba 16) antes/después. Si sigue bajo: probar build
  `--preset release` para medir sin logs.
- Resultado en consola: stick izq. camina, 60 FPS, Cross salta. R no disparaba.

## Fase 15: mapeo completo de botones (2026-09-26, build OK sin consola)
- R no disparaba: en la Vita física L/R son `SCE_CTRL_LTRIGGER/RTRIGGER`
  (0x100/0x200); `SCE_CTRL_L1/R1` (0x400/0x800) solo existen en PSTV/DS4.
  La coord del disparo ya era correcta: `CButtonShoot(W-119,H-123)` =
  (841,421) ±53 (ControlScheme4/8).
- `main.c` (slots 6 = gestos HUD, 7 = rayo; ids <= 7):
  - R (RTRIGGER|R1) = hold disparo (840,420).
  - L (LTRIGGER|L1) = hold rayo/poder especial `CButtonShoot(W-105,H-225,
    90x90)` = (855,319), evento 0x1d.
  - Circle = tap en el ícono de arma (790,40): `CButtonHudSweep` sin
    deslizar -> `HudPressedNoSweepCallback` -> `ReloadWeapon`.
  - Triangle = swipe (8 frames x -16px) en el mismo ícono ->
    `HudSweepCallback` -> `SetCurrentWeapon`.
  - START = tap en pausa (120,25), arriba a la izquierda.
  - Circle/Triangle/START siguen además como keys (menús).
- Pausa/arma son nodos del HUD 3D sin coords en código: medidas en captura.
  Si fallan, el log `[syntouch] gesture ...` muestra dónde se tocó.

## Fase 16: todos los botones del HUD + controles virtuales al 1% + video intro (2026-09-26, build OK sin consola)
- Captura hc 2026-09-26-214009 = todos los botones visibles. Cruce con pseudo-C:
  - Rayo = `CButtonSweep(W-116,H-224)` de `CLevelInterfaceNormal` (rect =
    frame del sprite, dibujado en ~(905,322)); tap -> `PressedNoSweepCallback`
    (usar poder), swipe > ancho/4 -> `SweepCallback` (cambiar poder). El
    `CButtonShoot` 0x1d de (855,319) de la Fase 15 NO era el rayo.
  - Arma = `CButtonHudSweep` (tap recarga, swipe cambia). Granada/pausa =
    nodos HUD 3D (`grenade_button`, `pause_idle`), medidos en captura.
- Mapeo (`main.c`): R disparo (hold), Cross salto, L usar poder (tap
  905,322), D-pad izq/der cambiar poder (swipe 50px), Circle recargar (tap
  790,40), Triangle/D-pad arriba arma siguiente, D-pad abajo arma anterior
  (swipe 128px), Square granada (tap 835,105), START pausa (tap 118,25). Las
  keys Android se siguen mandando para menús.
- Opacidad 1% (`patch.c` `patch_dim_controls`): `Draw(int alpha)` de cada
  `ControlSchemeN` (joystick, disparo, salto) llega al byte alfa del SColor
  de `CSprite::PaintModule`; se cambia el slot Draw en la vtable y se llama
  al original con alpha*1%. El sweep del rayo se atenúa solo en la instancia
  con frame 0xa5 (+0x58). HUD 3D (munición/granadas/pausa) intacto.
- Video intro `GLVideo.mp4` (H.264 Baseline 640x360 60fps + AAC) SIN
  convertir: `video.cpp` de Dungeon-Hunter-2-vita (SceAvPlayer por hardware,
  YUV->RGB en GPU, audio por sceAudioOut; mismo motor y vitaGL). GT Racing
  no sirve de base: sus videos son MPEG-4 Part 2 (decodifica por software).
  Se llama tras `nativeInit/Resize` como `GameRenderer.onSurfaceCreated`
  (MyVideoView), Cross/START lo saltan. `GLMediaPlayer.loadMovie` también
  reproduce vía `video_play`. Busca en `GloftN2HP/`, raíz y `files/`.
  CMake: `video.cpp`, `SceAvPlayer_stub`, `SceSysmodule_stub`.

## Fase 17: video intro (memoria) + cámara por velocidad (2026-09-26, build OK sin consola)
- Log 0016: `patch_dim` OK (8 esquemas + sweep). Video abre, SceAvPlayer lee,
  pero `av_alloc_texture` falla: CDRAM 0x80024309 (vglInitExtended se quedó
  con toda la CDRAM) y PHYCONT 0x80020005 (invalid flags: opt.alignment no
  vale para PHYCONT) -> STATE_STOP, 0 frames.
- `video.cpp`: PHYCONT sin opt en múltiplos de 1 MB; último recurso
  `vglMemalign` (pool vitaGL, ya mapeado), liberado con `vglFree`.
- Cámara (stick der.) como Shadow-Guardian-vita: velocidad px/frame con zona
  muerta 15% + curva cuadrática (X 14, Y 9 px/frame máx), acumulador float y
  wrap (suelta/re-aprieta en el ancla al salir de ±130px alrededor de
  (600,260)) -> giro continuo sin tope. Antes: posición fija ancla+90px, la
  cámara frenaba al llegar.
- Probar: ¿se ve el intro? (log `video: texture memblock ok`/`uso pool
  vitaGL`, `first video frame decoded`); velocidad/suavidad de cámara
  (ajustar LOOK_SPEED_X/Y).

## Fase 18: PHYCONT reservada para el intro (2026-09-26, build OK sin consola)
- Log 0017: CDRAM llena (0x80024309), PHYCONT también (0x80024302, vitaGL
  tomó todo el pool PHYCONT en init); el fallback a pool vitaGL (RAM común,
  0x82b4...) no sirve: el decoder hardware se detiene, 0 frames.
- `glutil.c`: `vglInitWithCustomThreshold(0,960,544, 12MB, 0, 0x7FFFFFFF,
  cdlg)` = mismo reparto que vglInitExtended pero SIN pool PHYCONT.
  `gl_provide_phycont()` (tras `video_play` en main.c) entrega la PHYCONT
  libre - 1 MB con `vglPhycontMemLazyInit` (API de vitaGL para "SceAvPlayer
  solo en el intro").
- `video.cpp`: se quitó el fallback a vglMemalign; PHYCONT sin opt, 1 MB.
- Probar: log `texture memblock ok (PHYCONT)`, `first video frame decoded`,
  `vitaGL: PHYCONT pool ... KB entregado`.

## Fase 19: intro por sceAvcdec directo (2026-09-26, build OK sin consola)
- Log 0018: memoria OK (2 bloques PHYCONT), pero SceAvPlayer igual STATE_STOP
  con 0 frames. Comparación: DH2 RE-ENCODEÓ su intro (intro_orig_1280x720.mp4
  -> intro.mp4, H.264 Main 24 fps / AAC 48 kHz); GLVideo.mp4 es Constrained
  Baseline 640x360 60 fps / AAC 44.1 kHz. Asphalt 5 / GT Racing usan FFmpeg
  software, pero el libavcodec del vitasdk NO trae decoder H.264 (22
  decoders: mpeg4, aac...).
- Nuevo `source/video_avc.c`: demux MP4 propio (moov en memoria; validado en
  host contra GLVideo.mp4 = ffprobe), avcC -> Annex-B con SPS/PPS, decoder
  H.264 HARDWARE `sceAvcdec` (el de Moonlight-Vita, sin límite de fps) que
  escribe RGBA8888 directo en la memoria de 3 texturas vitaGL (sin copia);
  AAC con libavcodec -> s16 -> ring -> hilo sceAudioOut BGM 44.1 kHz. Sync por
  reloj; si se atrasa >2 frames decodifica sin dibujar. Cross/START saltan.
- `video.cpp` `video_play` prueba primero `video_play_avc`; SceAvPlayer queda
  de fallback. CMake: `video_avc.c`, SceVideodec_stub, avcodec, avutil,
  swresample, mp3lame.
- Probar: log `video_avc: 640x360, 5544 frames...`, `primer frame en
  pantalla`, `fin (N dibujados, M salteados)`. Si falla, el log trae el código
  de sceVideodecInitLibrary / CreateDecoder / Decode.

## Fase 20: intro con audio + estado GL restaurado (2026-09-26, build OK sin consola)
- Log 0019: ¡video_avc funciona! 356 frames dibujados, 0 atrasados, 0 errores
  hasta el skip. Pero:
  - Sin audio: `sceAudioOutOpenPort(BGM)` -> 0x80260005: el único puerto BGM
    ya lo tiene el AudioTrack del juego (port 256). Ahora VOICE (44.1 kHz) con
    fallback a BGM y volumen 0 dB.
  - Juego negro tras el skip: el motor glitch cachea el estado GL; el intro
    dejaba otro programa, textura 0 (borrada), atributos 0/1, depth/blend...
    Ahora `gl_save`/`gl_restore` (programa, texturas, array buffer, FBO,
    viewport, clear color, blend/depth/cull/scissor) y atributos 14/15 que el
    motor no usa.
  - "Se ve enorme": el contenido de GLVideo.mp4 llena el cuadro 640x360 y
    Android lo muestra match_parent (fit) igual que acá (960x540). Pendiente
    captura si en la Vita aparece recortado.

## Fase 21: sensibilidad de cámara regulable + menos I/O por sonido (2026-09-26, build OK sin consola)
- Log 0020: intro OK (sin errores). Cámara "muy rápida".
- `main.c`: velocidad = máx (14/9 px/frame, la de Fase 17) x nivel/10.
  Niveles 1..10, default 4, SELECT + D-pad arriba/abajo (sin disparar los
  gestos de arma), guardado en `ux0:data/nova2/camera_sens.txt`. OSD: barra
  de 10 bloques arriba al centro 1.5 s (glScissor+glClear, estado GL
  restaurado). Log `[cam] sensibilidad N/10`.
- FPS (log 0020): en cada sonido el motor prueba un .wav suelto inexistente
  (sfx_fs_run_stone_2.wav x201, ..._3 x189, jeep, disparos...) antes del
  .glza: fopen fallido síncrono en ux0 + línea de log síncrona -> tirón por
  cada paso/disparo. `io.c`: caché negativo de fopen de solo lectura (NULL
  inmediato, sin tocar ux0 ni loguear); se vacía ante cualquier escritura,
  creación, O_CREAT, mkdir, rename, remove/unlink.
- `log.c`: ALOG INFO/DEBUG repetidos del juego ("== ATC ==" x341...) solo
  las primeras 3 veces de cada texto.
