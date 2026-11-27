import java.io.File

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val envNdkVersion = System.getenv("ANDROID_NDK")
    ?.takeIf { it.isNotBlank() }
    ?.let { File(it).name }
    ?: "30.0.16248370"

android {
    namespace = "org.lodos.app"
    compileSdk = 36
    buildToolsVersion = "35.0.0"
    ndkVersion = envNdkVersion

    defaultConfig {
        applicationId = "org.lodos.app"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cFlags += listOf("-Wall", "-Wextra")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    // D4/T017: exec binaries shipped as lib*.so must exist on disk in nativeLibraryDir
    packaging {
        jniLibs {
            useLegacyPackaging = true
            keepDebugSymbols += listOf(
                "**/libproot.so",
                "**/libproot_loader.so",
                "**/liblodos_wl.so",
                "**/liblodos_wl_bin.so",
                "**/libshmfill.so"
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}

// The audio daemon is an executable stored in jniLibs so Android extracts it to nativeLibraryDir.
val stageAudioDaemon by tasks.registering(Exec::class) {
    val root = rootProject.projectDir.parentFile
    workingDir(root)
    commandLine("bash", "scripts/build-rs.sh", "native/lodos-audio", "--features", "android")
    doLast {
        val source = root.resolve("build/rs/arm64/lodos-audio")
        check(source.isFile) { "lodos-audio build did not produce $source" }
        val destination = project.file("src/main/jniLibs/arm64-v8a/liblodos_audio.so")
        destination.parentFile.mkdirs()
        source.copyTo(destination, overwrite = true)
        destination.setExecutable(true, false)
    }
}

tasks.named("preBuild").configure { dependsOn(stageAudioDaemon) }

dependencies {
    testImplementation("junit:junit:4.13.2")
}
