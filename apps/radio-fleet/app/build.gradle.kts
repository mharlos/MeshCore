plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("com.chaquo.python")
}
// One persistent certificate must sign every public APK update.
// CI injects the keystore from repository Actions secrets, never from Git.
val fleetKeystorePath = System.getenv("RADIO_FLEET_KEYSTORE_FILE")
val fleetKeystorePassword = System.getenv("RADIO_FLEET_SIGNING_PASSWORD")
val hasFleetSigning = !fleetKeystorePath.isNullOrBlank() && !fleetKeystorePassword.isNullOrBlank()

android {
    signingConfigs {
        if (hasFleetSigning) {
            create("fleetRelease") {
                storeFile = file(fleetKeystorePath!!)
                storePassword = fleetKeystorePassword
                // Keystore alias and format are configurable for private CI
                // signing workflows; default to the dedicated PKCS#12 identity.
                keyAlias = System.getenv("RADIO_FLEET_SIGNING_ALIAS") ?: "farmradiofleet"
                keyPassword = fleetKeystorePassword
                storeType = System.getenv("RADIO_FLEET_KEYSTORE_TYPE") ?: "PKCS12"
            }
        }
    }
    buildTypes {
        getByName("release") {
            if (hasFleetSigning) {
                signingConfig = signingConfigs.getByName("fleetRelease")
            }
            isMinifyEnabled = false
        }
    }
    namespace = "com.mharlos.radiofleet"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.mharlos.radiofleet"
        minSdk = 26
        targetSdk = 34
        versionCode = providers.gradleProperty("radioVersionCode").orNull?.toIntOrNull() ?: 1
        versionName = "0.2." + versionCode
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
chaquopy {
    defaultConfig {
        version = "3.12"
    }
}
dependencies {
    testImplementation("junit:junit:4.13.2")
}
