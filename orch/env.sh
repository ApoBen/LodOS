# LodOS Android native toolchain environment
export ANDROID_HOME="$HOME/development/android"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK="$ANDROID_HOME/ndk/30.0.16248370"
export ANDROID_NDK_HOME="$ANDROID_NDK"
export ANDROID_NDK_ROOT="$ANDROID_NDK"

export PATH="$ANDROID_HOME/cmake/4.1.2/bin:$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"

# Rust (user-local cargo bin: cargo-ndk); idempotent
case ":$PATH:" in *":$HOME/.cargo/bin:"*) ;; *) export PATH="$HOME/.cargo/bin:$PATH" ;; esac
