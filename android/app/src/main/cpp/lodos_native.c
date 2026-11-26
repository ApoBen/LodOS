#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

JNIEXPORT jstring JNICALL
Java_org_lodos_app_MainActivity_stringFromJNI(JNIEnv *env, jobject thiz) {
    (void)thiz;
    return (*env)->NewStringUTF(env, "LodOS native OK");
}

JNIEXPORT jstring JNICALL
Java_org_lodos_app_ProbeActivity_probeDmaHeapNative(JNIEnv *env, jobject thiz) {
    (void)thiz;
    char buf[256];
    const char *path = "/dev/dma_heap/system";
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        snprintf(buf, sizeof(buf), "fail open(%s) errno=%d (%s)", path, err, strerror(err));
        return (*env)->NewStringUTF(env, buf);
    }

    struct dma_heap_allocation_data req;
    memset(&req, 0, sizeof(req));
    req.len = 4096;
    req.fd_flags = O_RDWR | O_CLOEXEC;
    int rc = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &req);
    int err = errno;
    close(fd);
    if (rc == 0) {
        if (req.fd >= 0) close((int)req.fd);
        snprintf(buf, sizeof(buf), "ok open+alloc(4096) %s", path);
    } else {
        snprintf(buf, sizeof(buf), "ok open_only alloc_fail errno=%d (%s) %s", err, strerror(err), path);
    }
    return (*env)->NewStringUTF(env, buf);
}
