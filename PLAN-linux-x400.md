# Plan: Linux completo en la SM-X400 sobre el kernel de One UI

Carpeta: `~/linux-x400` (clon limpio de `fahadalijaved/OneOS`, `bc10b21` = 0.5.34). No se escribe nada en `~/SM-X400/oneios`.
Estado: **solo plan; no se ha tocado la tablet.**

## Idea
OneOS arranca un segundo Android como contenedor sobre el kernel de Samsung y vuelve a One UI sin flashear.
Aquí el huésped sería un rootfs Linux (Debian/Ubuntu arm64) en vez de un Android: no hay emulación, es un
`chroot`/contenedor sobre el kernel 5.15 que ya corre. Si el chroot no saca imagen, no se sigue.

## Hechos medidos (2026-11-22, tablet SM-X400, Exynos 1380 `s5e8835`, Mali-G68 `0x9204`, kernel 5.15)
- Kernel: `CGROUPS NAMESPACES NET_NS SECCOMP BINFMT_MISC TUN DRM EXT4 F2FS FUSE OVERLAY_FS` = y.
  `USER_NS`, `PID_NS`, `DEVTMPFS` no salieron en la lectura: **sin comprobar**.
- `/dev/dri/card0` y `/dev/mali0` existen. `MemAvailable` ≈ 4311 MiB con One UI parada (de 5704).
- `flash.locked=1`, root DFroot (en RAM, se pierde al reiniciar). No hace falta desbloquear nada.

## Lo que OneOS ya resuelve (reutilizable, en `switcher/assets/`)
`oneos_switch.sh` (4701 líneas), `return_to_oneui.sh` (vuelta en el sitio sin reiniciar, con failsafe), guardas que
impiden tocar los datos de One UI, parada de `zygote`, tecla de emergencia Vol+ y Vol- 3 s.
No reutilizable tal cual: `guest_prepare.sh` / `guest_start.sh` / `guest_props.sh` asumen un Android (zygote,
system_server, servicemanager), y la app rechaza todo lo que no esté en su tabla de compatibilidad.

## Fases (una por sesión; cada una acaba con la vuelta a One UI comprobada)
0. **Lectura, sin tablet.** Leer `oneos_switch.sh` y `return_to_oneui.sh`: qué pasos son del huésped Android y cuáles
   del cambio/retorno. Salida: lista de qué se corta y qué se deja. Mirar si `docs/exynos*.md` cubren la X400.
1. **Sonda de kernel (solo lectura).** `USER_NS`, `PID_NS`, `devtmpfs`, `/dev/dri/card0` (¿modo KMS usable sin
   `surfaceflinger`?), `/dev/input`, `/dev/mali0`. Nodos con nombre; sin recorrer árboles.
2. **Rootfs en el PC.** `debootstrap` arm64 mínimo + `weston`/`kmscube`/`mesa` (≥ 26.3 por Panfrost G68) en una
   imagen ext4 en `~/linux-x400/out/` (git-ignored). Sin KDE todavía.
3. **Chroot sin pantalla en la tablet.** Montar la imagen, `chroot`, ping de red, `ls`, volver. Comprueba el
   mecanismo de montaje y retorno sin pelearse con la pantalla.
4. **Pantalla.** Parar `surfaceflinger` solo si hace falta, `kmscube` sobre `card0`. Primer punto de decisión:
   si no sale imagen, se para aquí. Pregunta abierta: `mali_kbase` de Samsung (Android) vs Panfrost de Mesa
   necesitan drivers de kernel distintos; con el kernel de Samsung puede que solo sirva el blob/Vulkan de Android
   o software (llvmpipe).
5. **Entrada y red.** Táctil por `/dev/input`, Wi-Fi heredado del Android parado.
6. **Escritorio.** Primero ligero (LXQt/XFCE); KDE Plasma solo si la RAM lo permite (~1–1,5 GB en reposo).

## Riesgos
- El root se pierde al reiniciar; un fallo de retorno obliga a rehacerlo.
- La GPU puede quedar en llvmpipe (lenta) si Panfrost no engancha con el kernel de Samsung.
- OneOS avisa de pérdida de datos si un huésped alcanza las particiones de One UI: las guardas hay que conservarlas.
- Una tarea de OneOS exige quitar el bloqueo de pantalla antes de cambiar.

## Primer paso propuesto
Fase 0 (solo leer scripts) y fase 1 (sonda de solo lectura). No monta nada ni toca la sesión.

---
## Resultados de las fases 0 y 1 (2026-11-22, solo lectura; la tablet no cambió, `session: none`)

### Fase 1: kernel de la tablet (`/proc/config.gz`, `/proc/filesystems`)
| Opción | Valor | Consecuencia |
|---|---|---|
| `USER_NS`, `PID_NS` | **no están** | sin contenedores sin privilegios ni PID propio: el Linux huésped verá los procesos de Android |
| `DEVTMPFS` | **no está** | `/dev` hay que montarlo a mano (tmpfs + `mknod`, o bind de `/dev` del host) |
| `SYSVIPC`, `CGROUP_DEVICE`, `SQUASHFS` | no están | rootfs en ext4 (no squashfs); sin memoria compartida SysV |
| `MEMCG`, `UTS_NS`, `NET_NS`, `OVERLAY_FS`, `FUSE`, `F2FS`, `EXT4`, `UNIX98_PTYS`, `INPUT_EVDEV`, `INPUT_UINPUT` | sí | suficiente para chroot, pty y entrada |
| `DRM_PANFROST` | **no está** | Panfrost de Mesa no tiene driver de kernel: la GPU es `MALI_MIDGARD=m` de Samsung (`/dev/mali0`) |
| `/dev/dri/card0`, `/dev/mali0` | existen, 0666 | |
| SELinux | Enforcing | el huésped necesita lo que OneOS hace (`setenforce 0` en el cambio) |

### Fase 0: cómo aísla OneOS al huésped
- Solo usa **`unshare -m` (espacio de montajes) + `pivot_root`** (`guest_start.sh:14`, lanzado con `unshare -m ... guest_start.sh`
  en `oneos_switch.sh:2073`). No usa PID ni user namespace. Lo que necesita el kernel de la tablet **ya lo tiene**.
- El resto de la entrada al huésped es `nsenter -t <pid> -m` y `chroot "$STAGE/rootfs"`.
- `docs/devices.md` lista la SM-X400 (Tab S10 Lite) como "Not yet"; no hay perfil para ella. El script `oneos_switch.sh` rechaza todo
  lo que no cumpla sus comprobaciones previas (`refuse`, líneas 17–180), entre ellas `system.raw` en el stage; todas ocurren antes
  del primer cambio en el teléfono.

### Qué cambia en el plan
1. **GPU:** Panfrost queda descartado con este kernel. Opciones reales: (a) `llvmpipe` por software, (b) el Vulkan/GLES de Samsung
   en `/vendor` (bionic) con `libhybris`, mucho trabajo, (c) lo que ya hicimos en iOS (Venus sobre el Vulkan del host), que es otra vía.
   La fase 4 queda como **"¿llvmpipe basta?"**; es lo primero que probar.
2. **Fase 3** sin PID namespace: `systemd` no puede ser PID 1 en un chroot; hay que arrancar el escritorio sin `init` (script que
   lance `dbus` + compositor) o con un init mínimo.
3. **Fase 2:** rootfs en ext4 (no squashfs).

---
## Decisión (2026-11-22): la GPU será Venus
El huésped Linux usa el driver Vulkan `virtio` de Mesa (cliente Venus) y un `virgl_renderer` en el lado Android sobre el Vulkan de Samsung (`/dev/mali0`).
Reutiliza lo ya hecho en `~/SM-X400/oneios/runtime/venus` (solo leer de allí; no se escribe en esa carpeta). Sin `llvmpipe` como plan principal.
Pendiente de medir, sin supuestos: cómo se conecta el cliente (socket vtest o equivalente) y si el rootfs sin PID/user namespace lo permite.

## Fase 2 en curso
- `x400/build-rootfs.sh`: Ubuntu Base 24.04.4 arm64 (sha256 comprobado) + `weston`, `mesa-vulkan-drivers`, `vulkan-tools`, `kmscube`...
  en `out/rootfs` con `qemu-user-static` (instalado en el PC con apt). Log: `out/rootfs-build.log`. Falta empaquetar `out/rootfs.ext4`.

---
## Resultados de la lectura de Venus (2026-11-22, solo leer; sin tablet)
- **Cliente en el rootfs:** `libvulkan_virtio.so` (Mesa 24.04) contiene el modo vtest: cadenas `VTEST_SOCKET_NAME`, `/tmp/.virgl_test`,
  `VN_DEBUG`, "vtest protocol version (%d) too old". Se activa con `VN_DEBUG=vtest` (a confirmar al probarlo). Por socket Unix: no hace falta
  `virtio-gpu` en el kernel ni DRM en el chroot.
- **Servidor en el proyecto de iOS:** `external/virglrenderer` (`7e17c8e`) trae `vtest/vtest_server.c` con la opción `--venus`
  (si se compila con `venus=true`). Pero lo que corre en la tablet es `runtime/venus/build/venus-host` (aarch64 bionic, 2,9 MB, no estático;
  `usage: venus-host --fd N`, usa un fd ya abierto, no un socket con nombre) más `venus-client`, que usa `socketpair`.
  Es el protocolo del propio proyecto iOS, no vtest: **no sirve tal cual para Mesa**.
- **Fuentes (leídas con `git show f95de4b:...`, sin tocar ese repo):** `runtime/venus/{venus-host.c,venus_wire.h,build-host.sh,build-probe.sh,
  virglrenderer-dma-heap.patch,stub/}`. `build-probe.sh` compila con el NDK 28.2 (aarch64-linux-android30) el núcleo `src/venus/vkr_*`
  sin meson ni epoxy.
- **`venus-host` no habla vtest.** Su protocolo (`venus_wire.h`) es un `AF_UNIX SOCK_SEQPACKET` con mensajes `VW_*` y un `memfd` con la RAM
  entera del huésped (`VW_HELLO`), pensado para `xnuhost`. Un Linux con Mesa no puede usarlo tal cual.
- **Camino elegido a probar primero:** compilar `virgl_test_server --venus` (`vtest/vtest_server.c`, opción `--venus` ya en el fork) para
  Android con el mismo NDK y el parche de dma-heap, y exponer su socket `/tmp/.virgl_test` en el chroot. Mesa usa `VN_DEBUG=vtest`.
  Sin esto no hay GPU; es el primer riesgo real. Alternativa: escribir un `vtest`→`VW_*` pequeño (más trabajo, reutiliza `venus-host`).
- **Aviso:** `~/SM-X400/oneios` está a mitad de un `git rebase -i` (32 hechos, 395 pendientes, 24 ficheros `AA`). No es de este fork; no se toca.
  Las fuentes se leen del commit, no del árbol de trabajo.

### Coste de los dos caminos de Venus (leído, sin compilar nada)
- `vtest_renderer.c` usa 22 funciones del API completo `virgl_renderer_*` (init, context, resource, fence, poll, transfer...). Esas viven en
  `src/virglrenderer.c`, que el proyecto de iOS **no compila**: `build-probe.sh` compila solo `src/venus/vkr_*` y evita meson y epoxy.
  Camino (a) = compilar todo `libvirglrenderer` para bionic sin GL (meson con epoxy fuera): no sé si el fork lo permite.
- `venus-host.c` (188 líneas) ya usa solo `vkr_renderer_*` (8 llamadas) y compila con el NDK. Camino (b) = escribir un `vtest-venus.c` pequeño
  (~300 líneas: socket `/tmp/.virgl_test`, el protocolo `vtest_protocol.h` de las llamadas que usa Mesa con Venus, y `vkr_renderer_*` debajo).
  Reutiliza lo ya probado (`venus-host`, dma-heap, ventanas). **Recomendado**; hay que medir qué subconjunto de vtest pide Mesa.

### Corrección (misma fecha): ni (a) ni (b) tal cual; mejor (c)
- El rootfs trae **Mesa 25.2.8** (no 24.04). Su `vn_renderer_vtest.c` usa 12 comandos vtest: protocolo, capset, `CONTEXT_INIT`,
  `RESOURCE_CREATE_BLOB` (shmem por `SCM_RIGHTS`), `RESOURCE_UNREF`, `BUSY_WAIT`, `SUBMIT_CMD2` (por anillo) y los 5 de `SYNC_*` (timelines).
  Lo de "~300 líneas" para (b) era optimista: las syncs y el multi-anillo son lo delicado.
- (a) choca con GL: `meson.build:71` exige epoxy sin condición y `src/virglrenderer.c` incluye `epoxy/gl.h` y llama 73 veces a `vrend_`.
- **(c):** compilar el `vtest/vtest_server.c` + `vtest_renderer.c` (2141 líneas, ya implementa syncs con eventfd y blobs) del fork con el NDK,
  y en vez de `src/virglrenderer.c` darle una capa fina `virgl_renderer_*` → `vkr_renderer_*` solo para Venus (22 funciones; las de GL,
  transferencias y caps de virgl devuelven error). Usa el protocolo probado upstream con Mesa y el núcleo `vkr_*` que ya compila y corre en la tablet.

### Opción (c) compilada (2026-11-22)
- `x400/venus/build.sh` compila con el NDK 28.2 `out/venus-build/virgl_test_server` (aarch64 bionic, 2,9 MB; solo `liblog libdl libm libc`).
  Fuentes: `out/virgl` = `virglrenderer` `7e17c8e` (de `git archive`) + `virglrenderer-dma-heap.patch` (de `f95de4b`) + `x400/venus/vtest-x400.patch`.
- El fork ya traía `src/venus/vkr_virgl_adapter.c` (el puente `virgl_context` → `vkr_renderer_*`): **la capa de (c) ya existía**.
  Lo único añadido: `x400/venus/stub/epoxy/{gl,egl}.h` (solo tipos), `x400/venus/vrend_stub.c` (37 funciones de GL/DRM que fallan con un mensaje)
  y `--venus` = `VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL` (sin EGL).
- Probado: arranca en la tablet (`./x400vt --help` imprime el uso) y se borró. **No probado:** que `--venus` inicie Vulkan ni que un cliente conecte.
- Para probar con un cliente hace falta el rootfs en la tablet (fase 3): el `vulkaninfo` de Mesa es glibc y solo corre dentro del chroot.

---
## Fase 3 hecha (2026-11-22): Mesa en el chroot ve la Mali-G68 a través de Venus
Con One UI en marcha, sin sesión, sin parar nada. Todo dentro de `busybox unshare -m` (los montajes no los ve One UI).
- Rootfs: `/data/local/tmp/x400-rootfs.ext4` montado `loop,ro` en `/data/local/tmp/x400root`, más tmpfs en `/tmp` y `/run`, bind de `/dev`, proc y sysfs.
- Servidor fuera del chroot: `ANDROID_EMU_VK_LOADER_PATH=/system/lib64/libvulkan.so x400vt --venus --no-fork --socket-path=<root>/tmp/.virgl_test`.
- Cliente dentro: `VN_DEBUG=vtest VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json vulkaninfo --summary` →
  `deviceName = Virtio-GPU Venus (Mali-G68)`, `driverName = venus`, `apiVersion 1.3.219`, `rc=0`. Repetido con el binario sin trazas: igual.
- Tras cada prueba: 0 montajes, 0 loops, 0 procesos, 0 mensajes de reset de la Mali en `dmesg`.
Tres fallos, en este orden, cada uno con su causa:
1. **SELinux:** el hilo `kworker` del loop (`u:r:kernel:s0`) no puede leer `shell_data_file`. Arreglo: `chcon u:object_r:apex_data_file:s0` sobre la imagen.
2. **`vkr_renderer_init` exige `ASYNC_FENCE_CB`** (y `vtest_server` no lo pasaba): "failed to initialise renderer". Parche de 1 línea.
3. **Cuelgue en `SYNC_WAIT`:** con `ring 0`, Venus retira el fence *dentro* de `virgl_renderer_context_create_fence`, pero `vtest_submit_cmd2_batch`
   añadía el envío a la lista *después* de llamarla: la señal se perdía y el cliente esperaba para siempre. Arreglo: añadir a la lista antes.
`x400/venus/vtest-x400.patch` recoge 2 y 3 (sobre `7e17c8e` + dma-heap). Sin `VN_DEBUG=vtest`, Mesa prueba antes el DRM real (`exynos-drmdpu`) y falla:
la variable es obligatoria (o enlazar `VK_ICD`/entorno del chroot).
Medido y no medido: **no hay ningún dato de rendimiento**; solo se enumeró el dispositivo. No se ha dibujado nada.

---
## Fase 4a hecha (2026-11-22): Venus RENDERIZA en la Mali-G68 desde el chroot (sin pantalla)
`x400/test/vk-offscreen.c` (compilado en el PC con `aarch64-linux-gnu-gcc` contra `libvulkan.so.1` del rootfs): dibuja un triángulo en una imagen
de 512x512, la copia a un buffer visible y comprueba dos píxeles. Resultado en la tablet, One UI en marcha:
`center 255 128 0 255`, `corner 26 51 230 255`, **PIXELS OK**; 1 fotograma 10 ms (incluye crear todo), **500 fotogramas en 0,45 s = 1119 fps** (≈0,9 ms/fotograma
de una escena trivial 512x512 con una espera por fotograma). Con 1000 y 300: 1099 y 1148 fps. Sin resets de la Mali (`dmesg`).
Scripts en `x400/phone/` (`run.sh` = todo dentro de `busybox unshare -m`; `chroot-venus.sh`; `check-clean.sh`; `force-clean.sh`). Uso: `NF=500 sh run.sh`.
### Cuatro cosas que importan
1. **Hay que fijar `VN_PERF=no_fence_feedback`.** Con el *fence feedback* de Mesa (por defecto) `vkWaitForFences` se cuelga ("stuck in fence wait") y el
   cliente cierra la conexión. **La causa no está encontrada**: el feedback hace que el cliente lea el estado de la fence en memoria compartida, y lo que
   escribe la GPU ahí no le llega (sospecha: la memoria `host visible` viene del dma-heap con el parche, sin coherencia; sin medir). Es un rodeo, no un arreglo.
2. **Sin una dependencia de subpass a `TRANSFER` los píxeles salen a 0:** era un fallo de mi prueba (Mali/Venus no inserta la barrera sola). `vkCmdFillBuffer` y
   `vkCmdClearColorImage` + `CopyImageToBuffer` funcionaban; el render pass sin la dependencia, no. No es un fallo de Venus.
3. **Los montajes se acumulan si no se usa `unshare -m`:** en el espacio de montajes de init, mis 18 pruebas dejaron 81 montajes y 12 loops, y varios
   `umount` fallaban con EINVAL (`/vk-offscreen//deleted`, bind de un fichero sustituido). Los limpié con `umount` uno a uno (el último primero) y
   `losetup -d`; init vuelve a 175 montajes. **Esto lo causé yo** al lanzar `chr.sh` sin `unshare`: hay que usar siempre `run.sh`. Compruébese con `check-clean.sh`.
4. **No se ha probado `weston`/`kmscube` sobre la pantalla**: lo hecho es solo cómputo y copia a memoria; no se ha visto un fotograma en la tablet.

---
## Fase 4b (2026-11-22): la pantalla, arquitectura (b) elegida y capa probada
Informe del subagente (fuentes: KWin 6.6, Mesa main): **A) (b)** compositor sin cabeza en el chroot + ayudante Android con capa SurfaceControl (no parar
surfaceflinger: KMS directo obliga a parar HWC y arriesga el panel DSI y el root); **B) KWin 6.6.x `--virtual`** (sesión Noop, sin logind), QPainter si no hay render
node; Zink solo si Venus pasa los requisitos (no fijar Plasma 6.8: quita QPainter); salida por ScreenCast/PipeWire; **C) entrada**: `--virtual` no crea libinput,
demonio propio que lee event8/event10 y los inyecta con `org_kde_kwin_fake_input` (`KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`).
Medido: Venus expone `VK_KHR_maintenance4`, `dynamic_rendering`, `synchronization2`, `extended_dynamic_state{,2}`, `line_rasterization`, `custom_border_color`,
`provoking_vertex`, `transform_feedback`, `physical_device_drm`, `image_drm_format_modifier`; **faltan `VK_KHR_maintenance5` y `VK_EXT_robustness2`** (requisitos de Zink GL 2.1). apiVersion 1.3.219.
Ayudante propio `x400/display/` (X400Display.java + jni.c + `fb.h`; build con `build.sh`; `display.sh`): lee un fichero compartido (cabecera + BGRA, `seq` al final) y lo
copia a una capa BLAST. **Probado en la tablet con One UI en marcha**: patrón en movimiento a 2112x1320, captura con `screencap` = degradado + barra blanca visibles, 34 fps
(300 fotogramas en 8,9 s; la copia por CPU y el `pattern` con 16 ms de `usleep` limitan, no se ha afinado), capa retirada al terminar (SIGTERM).
Tres hallazgos: (1) la pantalla estaba en `Dozing` y salía negra: hay que despertarla (`KEYCODE_WAKEUP`, `svc power stayon true` durante la prueba y devolverlo a `false`);
(2) la pantalla está en **horizontal, 2112x1320** (`wm size` 1320x2112 con rotación 1), la capa debe medir 2112x1320; (3) **el keyguard está activo** (`isKeyguardShowing=true`).

---
## Hito (2026-11-22): KDE Plasma 6.6.7 VISIBLE en la pantalla de la tablet
Prueba solo en la tablet, One UI en marcha. `x400/phone/run-desktop.sh` → `plasma.sh` (monta `x400-plasma.ext4` `loop,ro` en un `unshare -m`, tmpfs en `/tmp` y `/run`,
servidor Venus fuera, y `session-inner.sh` dentro) + `display.sh` (capa SurfaceControl 2112x1320). Captura con `screencap` (`out/display/desktop-first.png`):
fondo de Plasma, barra de tareas, reloj 18:46 y fecha. Primer fotograma a los 30 s del arranque. Sesión cerrada limpia: 0 procesos con raíz x400root, 0 montajes, `MemAvailable` ~3009 MiB.
Cadena: KWin 6.6.7 `--virtual` con **llvmpipe** (`KWIN_COMPOSE=O2`, `LIBGL_ALWAYS_SOFTWARE=1`) → `zkde_screencast` → PipeWire → `capture` → fichero compartido → capa.
Lo que desbloqueó la captura: **`wireplumber`** (sin él el flujo quedaba en `paused`: nadie enlazaba `kwin_wayland:output_1` con `x400-capture:input_1`) y llvmpipe en vez de QPainter
(KWin 6.6.7 solo permite screencast con `OpenGLCompositing`). Plasma sin systemd: `kglobalacceld`, `kded6`, `kactivitymanagerd` lanzados a mano, `kbuildsycoca6` antes de `plasmashell`.
Limpieza: el chroot comparte el espacio de PIDs con Android; `plasma.sh` mata solo procesos cuya raíz es el chroot (probé antes sin eso y quedaron 3 sesiones vivas).
### Qué NO está
- **GPU en la composición:** KWin usa llvmpipe (CPU). Venus solo sirve a apps Vulkan. Zink exigiría `maintenance5` y `robustness2`, que Venus no expone.
- **Entrada táctil:** `x400/display/inputd.c` escrito y compilado (aarch64); primera prueba en curso (toque inyectado con `sendevent`).
- **Estabilidad y fps:** sin medir en una sesión larga. Faltan fps reales, CPU y calor.

---
## Decisiones del propietario (2026-11-22)
- **Se para One UI**, como hace OneOS: el huésped Linux se arranca y se devuelve con el flujo de OneOS (parada del framework, teclas Vol+/Vol-, vigilante, retorno), no con mis scripts `x400_switch.sh`/`x400_return.sh`, que quedan como andamio.
- **Se queda el parche propio de dma-heap** (`x400/venus/vkr-dma-heap.patch`, MIT), ya medido igual de bueno que el de OneiOS.
- **Prueba con dedo real (primera):** el escritorio se ve, el táctil responde y el toque cae donde se pone el dedo (rotación y escala de `inputd` correctas a 2112x1320).
  One UI seguía respondiendo al tacto aunque `inputd` dijo haber tomado `event8` con `EVIOCGRAB`: **sin explicar** (el log no dio error). Con One UI parado no importa.
### Medido el mismo día (llvmpipe, sin GPU en la composición)
- Escritorio quieto: 4,6 fps de captura en 50 s (solo el arranque). Con un `konsole` desplazando texto: 5,8 fps, `kwin_wayland` 31 % de un núcleo (8 núcleos), `konsole` 3,9 %, `capture` 0,5 %.
  La CPU total está casi libre, así que el límite no es la CPU global: pendiente mirar `Cpus_allowed`/cpuset, el gobernador, los hilos de KWin y `/dev/pts` en el chroot (medición en curso).
- Zink no arranca: el Vulkan de Samsung no ofrece `VK_EXT_robustness2` (102 extensiones) y Zink exige `nullDescriptor` (`zink_screen.c:3458`).
