plugins {
    id("com.android.application")
}

android {
    namespace = "org.solarnx.client"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.solarnx.client"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.2.5-alpha"

        externalNativeBuild {
            cmake {
                // Everything static except SDL2 + main: SDLActivity loads libSDL2.so + libmain.so.
                arguments += "-DSNXUI_BUILD_HOST=OFF"
                targets("main")
                abiFilters("arm64-v8a", "armeabi-v7a", "x86_64")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        named("main") {
            // The shared shell assets (ui.rcml, ui.rcss, fonts, img) land at the assets root -
            // the file interface reads them via SDL_RWops/AAssetManager.
            assets.srcDirs("../../Ui/assets")
        }
    }
}
