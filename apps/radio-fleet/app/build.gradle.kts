plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
android {
    namespace = "com.mharlos.radiofleet"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.mharlos.radiofleet"
        minSdk = 26
        targetSdk = 34
        versionCode = providers.gradleProperty("radioVersionCode").orNull?.toIntOrNull() ?: 1
        versionName = "0.1." + versionCode
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
dependencies {
    testImplementation("junit:junit:4.13.2")
}
