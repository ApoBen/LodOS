#include <jni.h>

JNIEXPORT jstring JNICALL
Java_org_lodos_app_MainActivity_stringFromJNI(JNIEnv *env, jobject thiz) {
    (void)thiz;
    return (*env)->NewStringUTF(env, "LodOS native OK");
}
