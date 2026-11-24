// LD_PRELOAD shim (borrador): VirtualBackend::createInputBackend() -> new LibinputBackend(kwinApp()->session())
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
void x400_create_input(void);
__attribute__((visibility("hidden"), used)) void *x400_qapp_instance, *x400_libinput_ctor;
__asm__(
".text\n.balign 4\n.globl x400_create_input\n.type x400_create_input,%function\n"
"x400_create_input:\n"
"  bti c\n"
"  stp x29, x30, [sp, #-32]!\n  mov x29, sp\n  stp x19, x20, [sp, #16]\n"
"  mov x19, x8\n"                                  // sret: std::unique_ptr<InputBackend>*
"  adrp x9, x400_qapp_instance\n  ldr x9, [x9, :lo12:x400_qapp_instance]\n  blr x9\n"   // QCoreApplication::instance(), found with dlsym: the preload is loaded before Qt
"  ldr x20, [x0, #136]\n"                          // Application::m_session (setSession: str x3,[x2,#136])
"  mov x0, #0x30\n  bl malloc\n"   // the object is freed with operator delete, which is free() in libstdc++
"  mov x1, x20\n  mov x2, xzr\n  mov x20, x0\n"
"  adrp x9, x400_libinput_ctor\n  ldr x9, [x9, :lo12:x400_libinput_ctor]\n  blr x9\n"
"  str x20, [x19]\n"
"  ldp x19, x20, [sp, #16]\n  ldp x29, x30, [sp], #32\n  ret\n");
__attribute__((constructor)) static void patch(void) {
  x400_qapp_instance = dlsym(RTLD_DEFAULT, "_ZN15QCoreApplication8instanceEv");
  x400_libinput_ctor = dlsym(RTLD_DEFAULT, "_ZN4KWin14LibinputBackendC1EPNS_7SessionEP7QObject");
  void **vt = dlsym(RTLD_DEFAULT, "_ZTVN4KWin14VirtualBackendE");
  void *base = dlsym(RTLD_DEFAULT, "_ZN4KWin13OutputBackend18createInputBackendEv");
  if (!vt || !base || !x400_qapp_instance || !x400_libinput_ctor) return;
  long pg = sysconf(_SC_PAGESIZE);
  for (int i = 2; i < 26; i++) if (vt[i] == base) {
    void *p = (void *)((uintptr_t)&vt[i] & ~(pg - 1));
    mprotect(p, pg, PROT_READ | PROT_WRITE); vt[i] = (void *)x400_create_input; mprotect(p, pg, PROT_READ); return;
  }
}
