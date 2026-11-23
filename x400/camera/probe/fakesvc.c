// Temporary stand-ins for the system_server services that cameraserver
// waits for (permission_checker, activity, appops, sensor_privacy, ...).
// One UI is stopped, so nobody registers them. Every transaction is logged
// to stdout and answered with "no exception" + 0, which for
// IPermissionChecker means PERMISSION_GRANTED. The services disappear when
// this process exits. Usage: fakesvc [seconds]   (0 = until killed); FAKESVC_TRACE=1 logs every transaction
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The NDK stub of libbinder_ndk omits these; the device library exports them. */
static binder_exception_t (*AServiceManager_addService)(AIBinder *, const char *);
static void (*ABinderProcess_startThreadPool)(void);
static void (*ABinderProcess_joinThreadPool)(void);
static int (*ABinderProcess_setThreadPoolMaxThreadCount)(uint32_t);

#define SYM(h, n) do { *(void **)&n = dlsym(h, #n); if (!n) { fprintf(stderr, "no %s\n", #n); return 1; } } while (0)

/* service name -> interface descriptor; AIBinder_Class rejects any other token. */
/* "edmnativehelper" is deliberately NOT registered: a stand-in with an unknown descriptor makes cameraserver answer "Camera is disabled",
 * while with no service it waits 5 s and then treats the EDM policy as absent (= allowed). */
static const struct { const char *name, *desc; } svcs[] = {
	{ "permission_checker", "android.permission.IPermissionChecker" },
	{ "activity", "android.app.IActivityManager" },
	{ "appops", "com.android.internal.app.IAppOpsService" },
	{ "sensor_privacy", "android.hardware.ISensorPrivacyManager" },
	{ "media.camera.proxy", "android.hardware.ICameraServiceProxy" },
	{ "permission", "android.os.IPermissionController" },
	{ "processinfo", "android.os.IProcessInfoService" },
	{ "batterystats", "com.android.internal.app.IBatteryStats" },
	{ "package_native", "android.content.pm.IPackageManagerNative" },
	{ "virtualdevice_native", "android.companion.virtualdevice.IVirtualDeviceManagerNative" },
	{ NULL, NULL },
};

static void *on_create(void *args) { return args; }
static void on_destroy(void *userdata) { (void)userdata; }

/* Replies by service. The wire formats are the ones libprocessinfo / the camera service clients use. */
static void dump_in(const char *svc, transaction_code_t code, const AParcel *in)
{
	if (!getenv("FAKESVC_TRACE"))
		return;
	int32_t size = AParcel_getDataSize(in), pos = AParcel_getDataPosition(in);

	printf("tx %s code=%u uid=%d datasize=%d pos=%d ints:", svc, code, AIBinder_getCallingUid(), size, pos);
	for (int i = 0; i < 6 && pos + 4 * (i + 1) <= size; i++) {
		int32_t v = 0;

		AParcel_setDataPosition(in, pos + 4 * i);
		AParcel_readInt32(in, &v);
		printf(" %d", v);
	}
	printf("\n");
	fflush(stdout);
	AParcel_setDataPosition(in, pos);
}

static binder_status_t on_transact(AIBinder *binder, transaction_code_t code,
				   const AParcel *in, AParcel *out)
{
	const char *svc = AIBinder_getUserData(binder);

	dump_in(svc, code, in);
	AParcel_writeInt32(out, 0);	/* binder exception: none */
	if (!strcmp(svc, "processinfo") && code == 2) {
		/* getProcessStatesAndOomScoresFromPids(int[] pids, out int[] states, out int[] scores):
		 * the request is the pid array (length, values) then two output lengths; answer with
		 * PROCESS_STATE_TOP (2) and OOM score 0 for every pid. */
		int32_t n = 0;

		AParcel_readInt32(in, &n);
		if (n < 0 || n > 64) n = 0;
		for (int pass = 0; pass < 2; pass++) {
			AParcel_writeInt32(out, n);
			for (int i = 0; i < n; i++)
				AParcel_writeInt32(out, pass == 0 ? 2 : 0);
		}
		return STATUS_OK;
	}
	AParcel_writeInt32(out, 0);	/* return value: 0 / false / GRANTED */
	return STATUS_OK;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 0;
	void *h = dlopen("libbinder_ndk.so", RTLD_NOW);

	if (!h) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	SYM(h, AServiceManager_addService);
	SYM(h, ABinderProcess_startThreadPool);
	SYM(h, ABinderProcess_joinThreadPool);
	SYM(h, ABinderProcess_setThreadPoolMaxThreadCount);
	for (int i = 0; svcs[i].name; i++) {
		AIBinder_Class *cls = AIBinder_Class_define(svcs[i].desc, on_create,
							     on_destroy, on_transact);
		AIBinder *b = AIBinder_new(cls, (void *)svcs[i].name);
		binder_exception_t r = AServiceManager_addService(b, svcs[i].name);

		printf("addService %s (%s): %d\n", svcs[i].name, svcs[i].desc, r);
	}
	fflush(stdout);
	ABinderProcess_setThreadPoolMaxThreadCount(4);
	ABinderProcess_startThreadPool();
	if (secs)
		sleep(secs);
	else
		ABinderProcess_joinThreadPool();
	return 0;
}
