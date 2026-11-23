// linux-x400: registers android.frameworks.sensorservice@1.0::ISensorManager/default (HIDL), which the camera provider's HAL waits for
// ("getService: Trying again for ...ISensorManager/default" forever, and no request is ever completed) and which on Android is
// registered by system_server. It reuses the real class of libsensorservicehidl.so: SensorManager(JavaVM*) ... registerAsService(),
// like frameworks/base/services/core/jni/com_android_server_SystemServer.cpp does. Needs /system/bin/sensorservice running (libsensor
// talks to it). The JavaVM is a stand-in: it is only used to attach the thread that createEventQueue starts.
// No Android headers for libhidl exist in the NDK, so everything goes through dlsym with the mangled names of this build.
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jint JNICALL vm_ok(JavaVM *) { return JNI_OK; }
static jint JNICALL vm_attach(JavaVM *, void **env, void *) { static char dummy[256]; *env = dummy; return JNI_OK; }
static jint JNICALL vm_getenv(JavaVM *, void **env, jint) { static char dummy[256]; *env = dummy; return JNI_OK; }

static JNIInvokeInterface iface = { nullptr, nullptr, nullptr, (jint (JNICALL *)(JavaVM *))vm_ok, (jint (JNICALL *)(JavaVM *, JNIEnv **, void *))vm_attach,
				    (jint (JNICALL *)(JavaVM *))vm_ok, (jint (JNICALL *)(JavaVM *, void **, jint))vm_getenv,
				    (jint (JNICALL *)(JavaVM *, JNIEnv **, void *))vm_attach };
static struct { const JNIInvokeInterface *functions; } fake_vm = { &iface };

// libc++ (platform, std::__1) std::string, short form, enough for "default"
struct fake_string { unsigned char b[24]; };

int main()
{
	void *hidl = dlopen("libsensorservicehidl.so", RTLD_NOW);
	void *base = dlopen("libhidlbase.so", RTLD_NOW);
	void *fw = dlopen("android.frameworks.sensorservice@1.0.so", RTLD_NOW);

	if (!hidl || !base || !fw) { fprintf(stderr, "sensormgr: dlopen: %s\n", dlerror()); return 1; }
	typedef void (*ctor_t)(void *self, void *vm);
	typedef int (*reg_t)(void *self, const fake_string *name);
	typedef void (*inc_t)(const void *refbase, const void *id);
	typedef void (*conf_t)(unsigned long, bool);
	typedef void (*join_t)(void);
	auto ctor = (ctor_t)dlsym(hidl, "_ZN7android10frameworks13sensorservice4V1_014implementation13SensorManagerC1EP7_JavaVM");
	auto reg = (reg_t)dlsym(fw, "_ZN7android10frameworks13sensorservice4V1_014ISensorManager17registerAsServiceERKNSt3__112basic_stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEE");
	auto inc = (inc_t)dlsym(dlopen("libutils.so", RTLD_NOW), "_ZNK7android7RefBase9incStrongEPKv");
	auto conf = (conf_t)dlsym(base, "_ZN7android8hardware22configureRpcThreadpoolEmb");
	auto join = (join_t)dlsym(base, "_ZN7android8hardware17joinRpcThreadpoolEv");

	if (!ctor || !reg || !inc || !conf || !join) { fprintf(stderr, "sensormgr: missing symbols %p %p %p %p %p\n", (void *)ctor, (void *)reg, (void *)inc, (void *)conf, (void *)join); return 1; }
	void *obj = calloc(1, 0x200);	// the class is < 0x100 bytes; never freed

	ctor(obj, &fake_vm);
	inc((char *)obj + 0x50, obj);	// the virtual RefBase base sits at +0x50 (see the constructor); keep one strong ref
	fake_string name;
	memset(&name, 0, sizeof name);
	name.b[0] = 7 << 1;
	memcpy(name.b + 1, "default", 8);
	int r = reg(obj, &name);

	printf("sensormgr: registerAsService(default) = %d\n", r);
	fflush(stdout);
	if (r != 0) return 1;
	conf(1, true);
	join();
	return 0;
}
