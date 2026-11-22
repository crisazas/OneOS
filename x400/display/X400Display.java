// linux-x400: shows the frames a Linux guest writes into a shared file on a SurfaceControl layer above One UI.
// Run by app_process (display.sh); only surfaceflinger has to be running. Every android.* class is reached by
// reflection (the pattern of the OneOS fork's OneOSBootStatus and of OneiOS's display helper), so it builds with a JDK
// and d8 alone. The copy is native (jni.c). Usage: X400Display LIB FILE WIDTH HEIGHT [Z]
import java.lang.reflect.Method;

public final class X400Display {
    private static native int nRun(Object surface, String file, int width, int height);
    private static Class<?> SC, TX;

    private static void log(String s) { System.err.println("x400-display: " + s); }

    public static void main(String[] a) {
        int code;
        try { code = run(a); } catch (Throwable t) { t.printStackTrace(System.err); code = 5; }
        System.err.flush();
        System.exit(code);
    }

    private static int run(String[] a) throws Exception {
        if (a.length < 4) { log("usage: X400Display LIB FILE WIDTH HEIGHT [Z]"); return 2; }
        System.load(a[0]);
        String file = a[1];
        int w = Integer.parseInt(a[2]), h = Integer.parseInt(a[3]);
        int z = a.length > 4 ? Integer.parseInt(a[4]) : Integer.MAX_VALUE - 2;
        SC = Class.forName("android.view.SurfaceControl");
        TX = Class.forName("android.view.SurfaceControl$Transaction");
        Object tx = TX.getConstructor().newInstance();
        Object layer = null, queue = null, surface = null;
        int result;
        try {
            Object builder = Class.forName("android.view.SurfaceControl$Builder").getConstructor().newInstance();
            builder.getClass().getMethod("setName", String.class).invoke(builder, "x400 display");
            builder.getClass().getMethod("setBLASTLayer").invoke(builder);
            layer = builder.getClass().getMethod("build").invoke(builder);
            Class<?> blast = Class.forName("android.graphics.BLASTBufferQueue");
            try {
                queue = blast.getConstructor(String.class, SC, int.class, int.class, int.class).newInstance("x400 display", layer, w, h, 5);
            } catch (NoSuchMethodException changed) {
                queue = blast.getConstructor(String.class, boolean.class).newInstance("x400 display", false);
                blast.getMethod("update", SC, int.class, int.class, int.class).invoke(queue, layer, w, h, 5);
            }
            surface = queue.getClass().getMethod("createSurface").invoke(queue);
            if (!call(tx, "setLayer", new Class<?>[]{SC, int.class}, layer, z)) throw new IllegalStateException("setLayer is gone");
            call(tx, "setLayerStack", new Class<?>[]{SC, int.class}, layer, 0);
            if (!call(tx, "show", new Class<?>[]{SC}, layer)) call(tx, "setVisibility", new Class<?>[]{SC, boolean.class}, layer, true);
            call(tx, "setPosition", new Class<?>[]{SC, float.class, float.class}, layer, 0f, 0f);
            call(tx, "setOpaque", new Class<?>[]{SC, boolean.class}, layer, true);
            call(tx, "apply", new Class<?>[]{});
            log("layer up " + w + "x" + h + " z=" + z);
            result = nRun(surface, file, w, h);
        } finally {
            if (layer != null && !call(tx, "remove", new Class<?>[]{SC}, layer)) call(tx, "hide", new Class<?>[]{SC}, layer);
            call(tx, "apply", new Class<?>[]{});
            if (surface != null) quiet(surface, "release");
            if (queue != null) quiet(queue, "destroy");
            log("layer removed");
        }
        return result < 0 ? 4 : 0;
    }

    private static boolean call(Object tx, String name, Class<?>[] types, Object... args) {
        try { TX.getMethod(name, types).invoke(tx, args); return true; } catch (Throwable missing) { return false; }
    }

    private static void quiet(Object o, String m) {
        try { o.getClass().getMethod(m).invoke(o); } catch (Throwable ignored) { }
    }
}
