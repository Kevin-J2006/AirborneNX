#include "java.h"
#include "utils/init.h"
#include "reimpl/controls.h"
#include "utils/logger.h"
#include <falso_jni/FalsoJNI_Impl.h>
#include <switch.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

enum {
    M_STRING_EMPTY = 1,
    M_DEVICE_FIRMWARE,
    M_DEVICE_IDENTIFIER,
    M_GL_DID,
    M_DEVICE_USER_AGENT,
    M_PHONE_IP,
    M_PHONE_MAC,
    M_PROFILE_DATA,
    M_RES_PROFILE_NAME,
    M_OBB_FOLDER,
    M_OBB_FULL_PATH,
    M_REGION_FORMAT,
    M_PACKAGE_NAME,
    M_PHONE_WIDTH,
    M_PHONE_HEIGHT,
    M_FREE_SPACE,
    M_CPU_CORES,
    M_PHONE_MEMORY,
    M_CPU_SPEED,
    M_PHONE_DII,
    M_PV_SCALE,
    M_TRUE,
    M_FALSE,
    M_VOID,
    M_RESOURCE,
    M_DEVICE_NAME,
    M_PHONE_MANUFACTURER,
    M_PHONE_MODEL,
    M_PHONE_DEVICE,
    M_DEVICE_COUNTRY,
    M_DEVICE_REGION,
    M_DEVICE_LANGUAGE,
    M_APK_PATH,
    M_SAVE_FOLDER,
    M_SD_FOLDER,
    M_KEYBOARD_TEXT,
    M_KEYBOARD_VISIBLE,
    M_KEYBOARD_SHOW,
    M_KEYBOARD_HIDE,
    M_ENSURE_PATH,
    M_CONTROLLER_LISTENER_REGISTERED,
    M_CONTROLLER_LISTENER_UNREGISTERED,
    M_PATH_SDCARD,
    M_PATH_OBB,
    M_PATH_DATA,
    M_PATH_SAVE,
    M_PATH_TEMP,
    M_PATH_NATIVE_LIB,
    M_ASSET_AS_BYTES,
    M_GAME_NAME,
    M_DEFAULT_IGP,
    M_CPU_ABI,
    M_DPI
};

static jobject new_string(const char *value) {
    return jni->NewStringUTF(&jni, value ? value : "");
}

static char s_device_id[33] = "0123456789abcdef0123456789abcdef";
static char s_keyboard_text[256] = "";

// AndroidUtils.GetAssetAsString(String name) -> byte[] with the asset's bytes
static jobject asset_as_bytes(va_list args) {
    jstring jname = va_arg(args, jstring);
    const char *name = jname ? jni->GetStringUTFChars(&jni, jname, NULL) : NULL;
    if (!name) return NULL;
    if (name[0] == '.') name++;
    while (name[0] == '/') name++;

    char path[512];
    snprintf(path, sizeof(path), "%s%s", ASSETS_PATH, name);
    FILE *f = fopen(path, "rb");
    l_info("[java] GetAssetAsString(%s) -> %s", name, f ? "ok" : "missing");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    jbyte *data = (jbyte *)malloc(size > 0 ? size : 1);
    size_t got = data ? fread(data, 1, size, f) : 0;
    fclose(f);

    jbyteArray array = jni->NewByteArray(&jni, (jsize)got);
    if (array && got) jni->SetByteArrayRegion(&jni, array, 0, (jsize)got, data);
    free(data);
    return (jobject)array;
}

static jobject object_result(jmethodID id, va_list args) {
    switch ((intptr_t)id) {
        case M_PATH_SDCARD:         return new_string(ANDROID_SDCARD);
        case M_PATH_OBB:            return new_string("NA");
        case M_PATH_DATA:           return new_string(ANDROID_FILES);
        case M_PATH_SAVE:           return new_string(ANDROID_INTERNAL);
        case M_PATH_TEMP:           return new_string(ANDROID_CACHE);
        case M_PATH_NATIVE_LIB:     return new_string(ANDROID_LIBDIR);
        case M_ASSET_AS_BYTES:      return asset_as_bytes(args);
        case M_GAME_NAME:           return new_string("A8HM");
        case M_DEFAULT_IGP:         return new_string("A8HM");
        case M_CPU_ABI:             return new_string("arm64-v8a");
        case M_DEVICE_FIRMWARE:     return new_string("19.0.1");
        case M_DEVICE_IDENTIFIER:   return new_string(s_device_id);
        case M_GL_DID:              return new_string("switch_gl_did");
        case M_DEVICE_USER_AGENT:   return new_string("Mozilla/5.0 (Nintendo Switch; ShareApplet) AppleWebKit/609.4 (KHTML, like Gecko) NF/6.0.2.22.5 NintendoSwitch/19.0.1");
        case M_PHONE_IP:            return new_string("127.0.0.1");
        case M_PHONE_MAC:           return new_string("00:04:20:00:00:00");
        case M_PROFILE_DATA:        return new_string("{}");
        case M_RES_PROFILE_NAME:    return new_string("RES_2");
        case M_OBB_FOLDER:          return new_string(ANDROID_FILES);
        case M_OBB_FULL_PATH:       return new_string(ANDROID_FILES);
        case M_REGION_FORMAT:       return new_string("en_US");
        case M_PACKAGE_NAME:        return new_string("com.gameloft.android.ANMP.GloftA8HM");
        case M_DEVICE_NAME:         return new_string("Nintendo Switch");
        case M_PHONE_MANUFACTURER:  return new_string("Nintendo");
        case M_PHONE_MODEL:         return new_string("Nintendo Switch");
        case M_PHONE_DEVICE:        return new_string("Switch");
        case M_DEVICE_COUNTRY:      return new_string("US");
        case M_DEVICE_REGION:       return new_string("en_US");
        case M_DEVICE_LANGUAGE:     return new_string("en");
        case M_APK_PATH:            return new_string(ANDROID_APK);
        case M_SAVE_FOLDER:         return new_string(ANDROID_INTERNAL);
        case M_SD_FOLDER:           return new_string(ANDROID_FILES);
        case M_KEYBOARD_TEXT:       return new_string(s_keyboard_text);
        case M_RESOURCE:            return (jobject)jni->NewByteArray(&jni, 0);
        case M_STRING_EMPTY:
        default:
            return new_string("");
    }
}

static jint int_result(jmethodID id, va_list args) {
    (void)args;
    switch ((intptr_t)id) {
        case M_PHONE_WIDTH:   return ASPHALT8_RENDER_WIDTH_DEFAULT;
        case M_PHONE_HEIGHT:  return ASPHALT8_RENDER_HEIGHT_DEFAULT;
        case M_FREE_SPACE:    return 10 * 1024 * 1024; // 10 GB in KB
        case M_CPU_CORES:     return 4;
        case M_TRUE:          return 1;
        default:              return 0;
    }
}

static jlong long_result(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 4096LL * 1024LL * 1024LL; // 4GB RAM
}

static jfloat float_result(jmethodID id, va_list args) {
    (void)args;
    if ((intptr_t)id == M_CPU_SPEED) return 1.785f; // GHz, matches cpuinfo_max_freq
    if ((intptr_t)id == M_PV_SCALE)  return 1.0f;
    if ((intptr_t)id == M_DPI)       return 236.0f; // 1280x720 on a 6.2" panel
    return 0.0f;
}

static jdouble double_result(jmethodID id, va_list args) {
    (void)args;
    if ((intptr_t)id == M_PHONE_DII) return 6.2; // 6.2 inch diagonal screen
    return 0.0;
}

static jboolean boolean_result(jmethodID id, va_list args) {
    (void)args;
    if ((intptr_t)id == M_KEYBOARD_VISIBLE) return JNI_FALSE;
    return (intptr_t)id == M_TRUE ? JNI_TRUE : JNI_FALSE;
}

static void void_result(jmethodID id, va_list args) {
    (void)args;
    if ((intptr_t)id == M_CONTROLLER_LISTENER_REGISTERED)   controls_set_listener(true);
    if ((intptr_t)id == M_CONTROLLER_LISTENER_UNREGISTERED) controls_set_listener(false);
}

NameToMethodID nameToMethodId[] = {
    // GL2JNILib data-installer state. The data is already on the SD card, so
    // report "checked, nothing to download, verified".
    {M_TRUE, "GetIsCompleteCheck", METHOD_TYPE_BOOLEAN},
    {M_TRUE, "GetIsCompleteVerify", METHOD_TYPE_BOOLEAN},
    {M_TRUE, "GetIsDownloadComplete", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "GetIsNeedDownload", METHOD_TYPE_BOOLEAN},
    {M_TRUE, "StartDownLoadData", METHOD_TYPE_BOOLEAN},
    {M_TRUE, "StartDownLoadDataMain", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "IsSilentAlive", METHOD_TYPE_BOOLEAN},
    {M_STRING_EMPTY, "GetError", METHOD_TYPE_INT},

    // PackageUtils.AndroidUtils (v4.0.0l)
    {M_PATH_SDCARD, "RetrieveSDCardPath", METHOD_TYPE_OBJECT},
    {M_PATH_OBB, "RetrieveObbPath", METHOD_TYPE_OBJECT},
    {M_PATH_DATA, "RetrieveDataPath", METHOD_TYPE_OBJECT},
    {M_PATH_SAVE, "RetrieveSavePath", METHOD_TYPE_OBJECT},
    {M_PATH_TEMP, "RetrieveTempPath", METHOD_TYPE_OBJECT},
    {M_PATH_NATIVE_LIB, "RetrieveNativeLibraryPath", METHOD_TYPE_OBJECT},
    {M_ASSET_AS_BYTES, "GetAssetAsString", METHOD_TYPE_OBJECT, "(Ljava/lang/String;)[B"},
    {M_DEVICE_IDENTIFIER, "GetAndroidID", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "GetSerial", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "GetCPUSerial", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "GetHDIDFV", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "GetDeviceIMEI", METHOD_TYPE_OBJECT},
    {M_PHONE_MANUFACTURER, "GetDeviceManufacturer", METHOD_TYPE_OBJECT},
    {M_PHONE_MODEL, "GetDeviceModel", METHOD_TYPE_OBJECT},
    {M_PHONE_DEVICE, "GetPhoneProduct", METHOD_TYPE_OBJECT},
    {M_PHONE_DEVICE, "GetPhoneDevice", METHOD_TYPE_OBJECT},
    {M_DEVICE_FIRMWARE, "GetFirmware", METHOD_TYPE_OBJECT},
    {M_PHONE_MAC, "GetMacAddress", METHOD_TYPE_OBJECT},
    {M_CPU_ABI, "GetCPUAbi", METHOD_TYPE_OBJECT},
    {M_DEVICE_COUNTRY, "GetCountry", METHOD_TYPE_OBJECT},
    {M_DEVICE_COUNTRY, "GetDeviceSettingsCountryCode", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "GetSimIsoCountryCode", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "GetCarrierAgent", METHOD_TYPE_OBJECT},
    {M_DEVICE_LANGUAGE, "GetDeviceLanguage", METHOD_TYPE_OBJECT},
    {M_DEVICE_USER_AGENT, "GetUserAgent", METHOD_TYPE_OBJECT},
    {M_GAME_NAME, "GetGameName", METHOD_TYPE_OBJECT},
    {M_DEFAULT_IGP, "GetDefaultIGP", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "GetInjectedIGP", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "GetInjectedSerialKey", METHOD_TYPE_OBJECT},
    {M_DPI, "GetXDpi", METHOD_TYPE_FLOAT},
    {M_DPI, "GetYDpi", METHOD_TYPE_FLOAT},
    {M_STRING_EMPTY, "initCheckConnectionType", METHOD_TYPE_INT},
    {M_FREE_SPACE, "GetFreeSpaceSDInKBytes", METHOD_TYPE_INT},
    {M_STRING_EMPTY, "GetTotalSizeNeedSpace", METHOD_TYPE_INT},

    {M_STRING_EMPTY, "GetCPUPartInfo", METHOD_TYPE_OBJECT},
    {M_DEVICE_FIRMWARE, "GetDeviceFirmware", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "GetDeviceIdentifier", METHOD_TYPE_OBJECT},
    {M_PHONE_IP, "GetPhoneIP", METHOD_TYPE_OBJECT},
    {M_PHONE_MAC, "GetPhoneMAC", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "getAndroidId", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "getSerial", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "getSerialNo", METHOD_TYPE_OBJECT},
    {M_DEVICE_FIRMWARE, "getDeviceFirmware", METHOD_TYPE_OBJECT},
    {M_PHONE_MAC, "getMacAddress", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "getDeviceIMEI", METHOD_TYPE_OBJECT},
    {M_DEVICE_IDENTIFIER, "getHDIDFV", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "getHDIDFVVersion", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "getGoogleAdId", METHOD_TYPE_OBJECT},
    {M_GL_DID, "getGLDID", METHOD_TYPE_OBJECT, "()Ljava/lang/String;"},
    {M_DEVICE_IDENTIFIER, "d1", METHOD_TYPE_OBJECT},
    {M_DEVICE_NAME, "getDeviceName", METHOD_TYPE_OBJECT},
    {M_PHONE_MANUFACTURER, "getPhoneManufacturer", METHOD_TYPE_OBJECT},
    {M_PHONE_MODEL, "getPhoneModel", METHOD_TYPE_OBJECT},
    {M_PHONE_DEVICE, "getPhoneDevice", METHOD_TYPE_OBJECT},
    {M_PHONE_DEVICE, "getPhoneProduct", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "retrieveDeviceCarrier", METHOD_TYPE_OBJECT},
    {M_DEVICE_COUNTRY, "retrieveDeviceCountry", METHOD_TYPE_OBJECT},
    {M_DEVICE_REGION, "retrieveDeviceRegion", METHOD_TYPE_OBJECT},
    {M_DEVICE_LANGUAGE, "retrieveDeviceLanguage", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "getPhoneCarrier", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "retrieveCPUSerial", METHOD_TYPE_OBJECT},
    {M_PROFILE_DATA, "GetProfilesStr", METHOD_TYPE_OBJECT},
    {M_RES_PROFILE_NAME, "GetResProfileName", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "GetSimCountryCode", METHOD_TYPE_OBJECT},
    {M_DEVICE_USER_AGENT, "getDeviceUserAgent", METHOD_TYPE_OBJECT},
    {M_APK_PATH, "GetApkPath", METHOD_TYPE_OBJECT, "()Ljava/lang/String;"},
    {M_SAVE_FOLDER, "getSaveFolder", METHOD_TYPE_OBJECT, "()Ljava/lang/String;"},
    {M_SD_FOLDER, "getSDFolder", METHOD_TYPE_OBJECT, "()Ljava/lang/String;"},
    {M_OBB_FOLDER, "getOBBFolder", METHOD_TYPE_OBJECT},
    {M_OBB_FULL_PATH, "getOBBFullPath", METHOD_TYPE_OBJECT},
    {M_REGION_FORMAT, "getRegionFormat", METHOD_TYPE_OBJECT},
    {M_DEVICE_COUNTRY, "getLocaleCountry", METHOD_TYPE_OBJECT},
    {M_DEVICE_LANGUAGE, "getLocaleLanguage", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "getRedirectUrl", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "getGameAPIAchivementID", METHOD_TYPE_OBJECT},
    {M_STRING_EMPTY, "getGameAPILeaderboardID", METHOD_TYPE_OBJECT},
    {M_PACKAGE_NAME, "getPackageName", METHOD_TYPE_OBJECT},
    {M_RESOURCE, "getResource", METHOD_TYPE_OBJECT},
    {M_KEYBOARD_TEXT, "GetVirtualKeyboardText", METHOD_TYPE_OBJECT, "()Ljava/lang/String;"},

    {M_PHONE_WIDTH, "GetPhoneWidth", METHOD_TYPE_INT},
    {M_PHONE_HEIGHT, "GetPhoneHeight", METHOD_TYPE_INT},
    {M_FREE_SPACE, "GetFreeSpaceInKBytes", METHOD_TYPE_INT},
    {M_CPU_CORES, "GetMaxCPUCore", METHOD_TYPE_INT},
    {M_STRING_EMPTY, "ComputeNumUnreadNews", METHOD_TYPE_INT},
    {M_STRING_EMPTY, "ParseWSLang", METHOD_TYPE_INT},
    {M_STRING_EMPTY, "getGoogleAdIdStatus", METHOD_TYPE_INT},
    {M_PHONE_MEMORY, "GetPhoneMemory", METHOD_TYPE_LONG},
    {M_CPU_SPEED, "GetMaxCPUSpeed", METHOD_TYPE_FLOAT},
    {M_PV_SCALE, "getPVScaleRate", METHOD_TYPE_FLOAT},
    {M_PHONE_DII, "GetPhoneDII", METHOD_TYPE_DOUBLE},

    {M_STRING_EMPTY, "HasConnectivity", METHOD_TYPE_INT},
    {M_TRUE, "isSlideEnabled", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "IsMobileConnection", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "IsOpenIGP", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "initTV", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "isZEUSDevice", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "FinishLoadWS", METHOD_TYPE_BOOLEAN},
    {M_FALSE, "setCurrentContext", METHOD_TYPE_BOOLEAN},
    {M_KEYBOARD_VISIBLE, "IsKeyboardVisible", METHOD_TYPE_BOOLEAN, "()Z"},

    {M_VOID, "BeginWelcomeScreen", METHOD_TYPE_VOID},
    {M_VOID, "CloseWelcomeScreen", METHOD_TYPE_VOID},
    {M_VOID, "CollectDataIGB", METHOD_TYPE_VOID},
    {M_VOID, "EnterForum", METHOD_TYPE_VOID},
    {M_VOID, "EnterNews", METHOD_TYPE_VOID},
    {M_VOID, "ExecuteTrackHits", METHOD_TYPE_VOID},
    {M_VOID, "Exit", METHOD_TYPE_VOID},
    {M_VOID, "GetGameLanguage", METHOD_TYPE_VOID},
    {M_VOID, "HideWelcomeScreen", METHOD_TYPE_VOID},
    {M_VOID, "LaunchIGP", METHOD_TYPE_VOID},
    {M_VOID, "LockSensor", METHOD_TYPE_VOID},
    {M_VOID, "NoBackWarning", METHOD_TYPE_VOID},
    {M_VOID, "OpenBrowser", METHOD_TYPE_VOID},
    {M_VOID, "OpenCustomerCare", METHOD_TYPE_VOID},
    {M_VOID, "OpenshowInGameBrowserWithUrl", METHOD_TYPE_VOID},
    {M_VOID, "PresentWelcomeScreen", METHOD_TYPE_VOID},
    {M_VOID, "RestartGame", METHOD_TYPE_VOID},
    {M_VOID, "SendAppToBackground", METHOD_TYPE_VOID},
    {M_VOID, "SetBaseUrl", METHOD_TYPE_VOID},
    {M_VOID, "SetPAU", METHOD_TYPE_VOID},
    {M_VOID, "SetRestarting", METHOD_TYPE_VOID},
    {M_VOID, "enableAccelerometer", METHOD_TYPE_VOID},
    {M_ENSURE_PATH, "ensurePathExists", METHOD_TYPE_VOID, "(Ljava/lang/String;)V"},
    {M_VOID, "setResourcePath", METHOD_TYPE_VOID},
    {M_VOID, "setViewSettings", METHOD_TYPE_VOID},
    {M_VOID, "enableUserLocation", METHOD_TYPE_VOID},
    {M_VOID, "disableUserLocation", METHOD_TYPE_VOID},
    {M_KEYBOARD_SHOW, "ShowKeyboard", METHOD_TYPE_VOID, "(Ljava/lang/String;)V"},
    {M_KEYBOARD_HIDE, "HideKeyboard", METHOD_TYPE_VOID, "()V"},
    {M_CONTROLLER_LISTENER_REGISTERED, "NativeListenerRegistered", METHOD_TYPE_VOID, "(I)V"},
    {M_CONTROLLER_LISTENER_UNREGISTERED, "NativeListenerUnRegistered", METHOD_TYPE_VOID, "()V"},
};

MethodsBoolean methodsBoolean[] = {
    {M_TRUE, boolean_result},
    {M_FALSE, boolean_result},
    {M_KEYBOARD_VISIBLE, boolean_result}
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {{M_PHONE_DII, double_result}};
MethodsFloat methodsFloat[] = {
    {M_CPU_SPEED, float_result},
    {M_PV_SCALE, float_result},
    {M_DPI, float_result}
};
MethodsInt methodsInt[] = {
    {M_PHONE_WIDTH, int_result},
    {M_PHONE_HEIGHT, int_result},
    {M_FREE_SPACE, int_result},
    {M_CPU_CORES, int_result},
    {M_STRING_EMPTY, int_result},
    {M_TRUE, int_result}
};
MethodsLong methodsLong[] = {{M_PHONE_MEMORY, long_result}};
MethodsObject methodsObject[] = {
    {M_STRING_EMPTY, object_result},
    {M_DEVICE_FIRMWARE, object_result},
    {M_DEVICE_IDENTIFIER, object_result},
    {M_GL_DID, object_result},
    {M_DEVICE_USER_AGENT, object_result},
    {M_PHONE_IP, object_result},
    {M_PHONE_MAC, object_result},
    {M_PROFILE_DATA, object_result},
    {M_RES_PROFILE_NAME, object_result},
    {M_OBB_FOLDER, object_result},
    {M_OBB_FULL_PATH, object_result},
    {M_REGION_FORMAT, object_result},
    {M_PACKAGE_NAME, object_result},
    {M_RESOURCE, object_result},
    {M_DEVICE_NAME, object_result},
    {M_PHONE_MANUFACTURER, object_result},
    {M_PHONE_MODEL, object_result},
    {M_PHONE_DEVICE, object_result},
    {M_DEVICE_COUNTRY, object_result},
    {M_DEVICE_REGION, object_result},
    {M_DEVICE_LANGUAGE, object_result},
    {M_APK_PATH, object_result},
    {M_SAVE_FOLDER, object_result},
    {M_SD_FOLDER, object_result},
    {M_KEYBOARD_TEXT, object_result},
    {M_PATH_SDCARD, object_result},
    {M_PATH_OBB, object_result},
    {M_PATH_DATA, object_result},
    {M_PATH_SAVE, object_result},
    {M_PATH_TEMP, object_result},
    {M_PATH_NATIVE_LIB, object_result},
    {M_ASSET_AS_BYTES, object_result},
    {M_GAME_NAME, object_result},
    {M_DEFAULT_IGP, object_result},
    {M_CPU_ABI, object_result}
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    {M_VOID, void_result},
    {M_KEYBOARD_SHOW, void_result},
    {M_KEYBOARD_HIDE, void_result},
    {M_ENSURE_PATH, void_result},
    {M_CONTROLLER_LISTENER_REGISTERED, void_result},
    {M_CONTROLLER_LISTENER_UNREGISTERED, void_result}
};

static char WINDOW_SERVICE[] = "window";
static const int SDK_INT = 22;

NameToFieldID nameToFieldId[] = {
    {0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT},
    {1, "SDK_INT", FIELD_TYPE_INT},
};
FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {{1, SDK_INT}};
FieldsObject fieldsObject[] = {{0, WINDOW_SERVICE}};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES

void java_init(void) {
    jni_init();
    l_info("FalsoJNI environment initialized for Nintendo Switch");
}
