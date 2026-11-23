// Temporary stand-ins for the system_server services that cameraserver
// waits for (permission_checker, activity, appops, sensor_privacy, ...).
// One UI is stopped, so nobody registers them. Every transaction is logged
// to stdout and answered with "no exception" + 0, which for
// IPermissionChecker means PERMISSION_GRANTED. The services disappear when
// this process exits. Usage: fakesvc [seconds]   (0 = until killed)
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* The NDK stub of libbinder_ndk omits these; the device library exports them. */
static binder_exception_t (*AServiceManager_addService)(AIBinder *, const char *);
static void (*ABinderProcess_startThreadPool)(void);
static void (*ABinderProcess_joinThreadPool)(void);
static int (*ABinderProcess_setThreadPoolMaxThreadCount)(uint32_t);

#define SYM(h, n) do { *(void **)&n = dlsym(h, #n); if (!n) { fprintf(stderr, "no %s\n", #n); return 1; } } while (0)

/* service name -> interface descriptor; AIBinder_Class rejects any other token. */
static const struct { const char *name, *desc; } svcs[] = {
	{ "permission_checker", "android.permission.IPermissionChecker" },
	{ "activity", "android.app.IActivityManager" },
	{ "appops", "com.android.internal.app.IAppOpsService" },
	{ "sensor_privacy", "android.hardware.ISensorPrivacyManager" },
	{ "media.camera.proxy", "android.hardware.ICameraServiceProxy" },
	{ "permission", "android.os.IPermissionController" },
	{ "processinfo", "android.os.IProcessInfoService" },
	{ NULL, NULL },
};

static void *on_create(void *args) { return args; }
static void on_destroy(void *userdata) { (void)userdata; }

static binder_status_t on_transact(AIBinder *binder, transaction_code_t code,
				   const AParcel *in, AParcel *out)
{
	(void)in;
	printf("tx %s uid=%d pid=%d code=%u\n", (const char *)AIBinder_getUserData(binder),
	       AIBinder_getCallingUid(), AIBinder_getCallingPid(), code);
	fflush(stdout);
	AParcel_writeInt32(out, 0);	/* binder exception: none */
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
