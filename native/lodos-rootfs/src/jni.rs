//! JNI bindings for `org.lodos.app.setup.NativeRootfs`.

use std::ffi::{CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::ptr;

use jni_sys::{jobject, jstring, JNIEnv};

use crate::extract_tarball;

unsafe fn jstring_to_string(env: *mut JNIEnv, s: jstring) -> Option<String> {
    if env.is_null() || s.is_null() {
        return None;
    }
    let iface = *env;
    if iface.is_null() {
        return None;
    }
    let chars = ((*iface).v1_1.GetStringUTFChars)(env, s, ptr::null_mut());
    if chars.is_null() {
        return None;
    }
    let owned = CStr::from_ptr(chars).to_string_lossy().into_owned();
    ((*iface).v1_1.ReleaseStringUTFChars)(env, s, chars);
    Some(owned)
}

unsafe fn string_to_jstring(env: *mut JNIEnv, s: &str) -> jstring {
    if env.is_null() {
        return ptr::null_mut();
    }
    let iface = *env;
    if iface.is_null() {
        return ptr::null_mut();
    }
    let sanitized = s.replace('\0', "");
    match CString::new(sanitized) {
        Ok(cs) => ((*iface).v1_1.NewStringUTF)(env, cs.as_ptr()),
        Err(_) => ptr::null_mut(),
    }
}

unsafe fn throw_io_exception(env: *mut JNIEnv, msg: &str) {
    if env.is_null() {
        return;
    }
    let iface = *env;
    if iface.is_null() {
        return;
    }
    let cls_name = b"java/io/IOException\0";
    let cls = ((*iface).v1_1.FindClass)(env, cls_name.as_ptr() as *const _);
    if !cls.is_null() {
        let sanitized = msg.replace('\0', "");
        if let Ok(cs) = CString::new(sanitized) {
            ((*iface).v1_1.ThrowNew)(env, cls, cs.as_ptr());
        }
    }
}

/// JNI entry: `org.lodos.app.setup.NativeRootfs.extract(src: String, dst: String): String`
///
/// Extracts the rootfs tarball at `src` into `dst` and returns the formatted summary string
/// (`files=... dirs=... symlinks=... hardlinks=... skipped_dev=... ms=...`).
/// Throws `java.io.IOException` and returns null on error.
#[no_mangle]
pub unsafe extern "system" fn Java_org_lodos_app_setup_NativeRootfs_extract(
    env: *mut JNIEnv,
    _class_or_obj: jobject,
    src: jstring,
    dst: jstring,
) -> jstring {
    let res = catch_unwind(AssertUnwindSafe(|| -> Result<String, String> {
        let src_str = jstring_to_string(env, src).ok_or_else(|| "src path is null".to_string())?;
        let dst_str = jstring_to_string(env, dst).ok_or_else(|| "dst path is null".to_string())?;
        let summary = extract_tarball(Path::new(&src_str), Path::new(&dst_str))
            .map_err(|e| e.to_string())?;
        Ok(summary.to_string())
    }));

    match res {
        Ok(Ok(summary)) => string_to_jstring(env, &summary),
        Ok(Err(err)) => {
            throw_io_exception(env, &err);
            ptr::null_mut()
        }
        Err(_) => {
            throw_io_exception(env, "panic in lodos-rootfs native extract");
            ptr::null_mut()
        }
    }
}
