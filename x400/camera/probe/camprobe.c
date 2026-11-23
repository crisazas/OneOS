// Read-only probe: lists camera ids and characteristics through the NDK
// camera2 API (libcamera2ndk). Never opens a camera device.
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <stdio.h>
#include <string.h>

static const char *facing(uint8_t f)
{
	switch (f) {
	case ACAMERA_LENS_FACING_FRONT: return "FRONT";
	case ACAMERA_LENS_FACING_BACK: return "BACK";
	case ACAMERA_LENS_FACING_EXTERNAL: return "EXTERNAL";
	}
	return "?";
}

static void dump(const char *id, ACameraMetadata *md)
{
	ACameraMetadata_const_entry e;

	if (ACameraMetadata_getConstEntry(md, ACAMERA_LENS_FACING, &e) == ACAMERA_OK)
		printf("  facing: %s\n", facing(e.data.u8[0]));
	if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK)
		printf("  sensor orientation: %d\n", e.data.i32[0]);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &e) == ACAMERA_OK)
		printf("  pixel array: %dx%d\n", e.data.i32[0], e.data.i32[1]);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &e) == ACAMERA_OK)
		printf("  physical size mm: %.3fx%.3f\n", e.data.f[0], e.data.f[1]);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e) == ACAMERA_OK)
		printf("  focal length mm: %.2f\n", e.data.f[0]);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &e) == ACAMERA_OK)
		printf("  hw level: %d (0 limited,1 full,2 legacy,3 level3,4 external)\n", e.data.u8[0]);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e) == ACAMERA_OK) {
		printf("  fps ranges:");
		for (uint32_t i = 0; i + 1 < e.count; i += 2)
			printf(" [%d,%d]", e.data.i32[i], e.data.i32[i + 1]);
		printf("\n");
	}
	if (ACameraMetadata_getConstEntry(md, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
		// format, w, h, isInput
		printf("  output streams (fmt WxH), %u entries:\n", e.count / 4);
		for (uint32_t i = 0; i + 3 < e.count; i += 4) {
			if (e.data.i32[i + 3])
				continue;
			printf("    0x%x %dx%d\n", e.data.i32[i], e.data.i32[i + 1], e.data.i32[i + 2]);
		}
	}
	if (ACameraMetadata_getConstEntry(md, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK) {
		printf("  capabilities:");
		for (uint32_t i = 0; i < e.count; i++)
			printf(" %d", e.data.u8[i]);
		printf("\n");
	}
	if (ACameraMetadata_getConstEntry(md, ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS, &e) == ACAMERA_OK)
		printf("  logical multi-camera, physical ids: %.*s\n", (int)e.count, (const char *)e.data.u8);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_FLASH_INFO_AVAILABLE, &e) == ACAMERA_OK)
		printf("  flash: %d\n", e.data.u8[0]);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) == ACAMERA_OK)
		printf("  min focus distance: %.2f (0 = fixed focus)\n", e.data.f[0]);
	if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK)
		printf("  cfa: %d\n", e.data.u8[0]);
	int32_t n;
	const uint32_t *tags;
	if (ACameraMetadata_getAllTags(md, &n, &tags) == ACAMERA_OK)
		printf("  total metadata tags: %d\n", n);
	(void)id;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	ACameraManager *mgr = ACameraManager_create();
	ACameraIdList *list = NULL;
	camera_status_t st;

	if (!mgr) {
		fprintf(stderr, "ACameraManager_create failed\n");
		return 1;
	}
	st = ACameraManager_getCameraIdList(mgr, &list);
	printf("getCameraIdList: status=%d\n", st);
	if (st != ACAMERA_OK || !list) {
		ACameraManager_delete(mgr);
		return 2;
	}
	printf("cameras: %d\n", list->numCameras);
	for (int i = 0; i < list->numCameras; i++) {
		const char *id = list->cameraIds[i];
		ACameraMetadata *md = NULL;

		printf("camera id \"%s\"\n", id);
		st = ACameraManager_getCameraCharacteristics(mgr, id, &md);
		printf("  getCameraCharacteristics: status=%d\n", st);
		if (st == ACAMERA_OK && md) {
			dump(id, md);
			ACameraMetadata_free(md);
		}
	}
	ACameraManager_deleteCameraIdList(list);
	ACameraManager_delete(mgr);
	return 0;
}
