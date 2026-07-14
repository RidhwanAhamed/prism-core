plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.prism.prism_smoke"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    defaultConfig {
        applicationId = "com.prism.prism_smoke"
        // AAudio (miniaudio's Android backend as configured) needs API 26+.
        minSdk = maxOf(26, flutter.minSdkVersion)
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        // The physical-device smoke targets arm64 (build plan Task 6); other ABIs can be
        // added when needed.
        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        // Cross-compile libprism_core.so straight from the repo's own CMake tree — the
        // same build the desktop suite verifies, no copied sources.
        externalNativeBuild {
            cmake {
                arguments +=
                    listOf(
                        "-DPRISM_BUILD_SHARED=ON",
                        "-DPRISM_BUILD_TESTS=OFF",
                        "-DPRISM_BUILD_HARNESS=OFF",
                        "-DANDROID_PLATFORM=android-26",
                    )
                targets += listOf("prism_core_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../../../CMakeLists.txt") // repo root
        }
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

flutter {
    source = "../.."
}
