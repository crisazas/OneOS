#!/bin/sh
# linux-x400: builds the Vulkan layer in x400/venus/layer into out/mesa25/root/x400layer (the stage's /tmp/mesa25/x400layer in the guest).
set -eu
cd "$(dirname "$0")"; D=../../../out/mesa25/root/x400layer; mkdir -p "$D"
aarch64-linux-gnu-gcc -O2 -shared -fPIC -fvisibility=hidden -o "$D/libVkLayer_x400_drmid.so" x400_drmid.c
cat > "$D/VkLayer_x400_drmid.json" <<J
{"file_format_version":"1.1.2","layer":{"name":"VK_LAYER_x400_drmid","type":"GLOBAL","library_path":"./libVkLayer_x400_drmid.so","api_version":"1.3.0","implementation_version":"1","description":"linux-x400: DRM node ids for Zink","functions":{"vkNegotiateLoaderLayerInterfaceVersion":"vkNegotiateLoaderLayerInterfaceVersion"}}}
J
echo "layer built in $D"
