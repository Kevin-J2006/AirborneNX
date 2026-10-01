/*
 * FalsoJNI_ImplBridge.c
 *
 * Fake Java Native Interface, providing JavaVM and JNIEnv objects.
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2021 Rinnegatamante
 * Copyright (C) 2022 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "FalsoJNI_Impl.h"
#include "FalsoJNI_Logger.h"

#include "FalsoJNI_ImplBridge.h"

#include <string.h>
#include <malloc.h>
#include <pthread.h>

#include "converter.h"

#define JDA_MAGIC 0xB00B1E5
#define FALSOJNI_TYPED_METHOD_FALLBACK 1
#define FALSOJNI_TYPED_FIELD_FALLBACK 1
#define FALLBACK_ID_CAPACITY 512

extern JNIEnv jni;

typedef struct {
    char *name;
    char *signature;
    METHOD_TYPE type;
} FallbackMethod;

typedef union {
    jobject object_value;
    jboolean boolean_value;
    jbyte byte_value;
    jchar char_value;
    jshort short_value;
    jint int_value;
    jlong long_value;
    jfloat float_value;
    jdouble double_value;
} FallbackFieldValue;

typedef struct {
    char *name;
    char *signature;
    FIELD_TYPE type;
    jboolean has_value;
    FallbackFieldValue value;
} FallbackField;

static FallbackMethod fallback_methods[FALLBACK_ID_CAPACITY];
static FallbackField fallback_fields[FALLBACK_ID_CAPACITY];
static int fallback_method_count;
static int fallback_field_count;
static pthread_mutex_t fallback_id_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char fallback_opaque_object;
static jobject fallback_empty_string;
static jobject fallback_empty_arrays[9];

static METHOD_TYPE method_type_from_signature(const char *name,
                                              const char *signature) {
    if (name) {
        size_t length = strlen(name);
        static const char constructor_suffix[] = "/<init>";
        size_t suffix_length = sizeof(constructor_suffix) - 1;
        if (length >= suffix_length &&
            strcmp(name + length - suffix_length, constructor_suffix) == 0)
            return METHOD_TYPE_OBJECT;
    }

    const char *result = signature ? strrchr(signature, ')') : NULL;
    if (!result || !result[1])
        return METHOD_TYPE_UNKNOWN;

    switch (result[1]) {
        case 'V': return METHOD_TYPE_VOID;
        case 'L':
        case '[': return METHOD_TYPE_OBJECT;
        case 'Z': return METHOD_TYPE_BOOLEAN;
        case 'B': return METHOD_TYPE_BYTE;
        case 'C': return METHOD_TYPE_CHAR;
        case 'S': return METHOD_TYPE_SHORT;
        case 'I': return METHOD_TYPE_INT;
        case 'J': return METHOD_TYPE_LONG;
        case 'F': return METHOD_TYPE_FLOAT;
        case 'D': return METHOD_TYPE_DOUBLE;
        default: return METHOD_TYPE_UNKNOWN;
    }
}

static FIELD_TYPE field_type_from_signature(const char *signature) {
    if (!signature || !signature[0])
        return FIELD_TYPE_UNKNOWN;

    switch (signature[0]) {
        case 'L':
        case '[': return FIELD_TYPE_OBJECT;
        case 'Z': return FIELD_TYPE_BOOLEAN;
        case 'B': return FIELD_TYPE_BYTE;
        case 'C': return FIELD_TYPE_CHAR;
        case 'S': return FIELD_TYPE_SHORT;
        case 'I': return FIELD_TYPE_INT;
        case 'J': return FIELD_TYPE_LONG;
        case 'F': return FIELD_TYPE_FLOAT;
        case 'D': return FIELD_TYPE_DOUBLE;
        default: return FIELD_TYPE_UNKNOWN;
    }
}

static FallbackMethod *fallback_method_by_id(jmethodID id) {
    FallbackMethod *result = NULL;
    pthread_mutex_lock(&fallback_id_mutex);
    for (int index = 0; index < fallback_method_count; index++) {
        if ((jmethodID)&fallback_methods[index] == id) {
            result = &fallback_methods[index];
            break;
        }
    }
    pthread_mutex_unlock(&fallback_id_mutex);
    return result;
}

static FallbackField *fallback_field_by_id(jfieldID id) {
    FallbackField *result = NULL;
    pthread_mutex_lock(&fallback_id_mutex);
    for (int index = 0; index < fallback_field_count; index++) {
        if ((jfieldID)&fallback_fields[index] == id) {
            result = &fallback_fields[index];
            break;
        }
    }
    pthread_mutex_unlock(&fallback_id_mutex);
    return result;
}

static jmethodID fallback_method_id(const char *name, const char *signature,
                                    METHOD_TYPE type) {
    jmethodID result = NULL;
    pthread_mutex_lock(&fallback_id_mutex);
    for (int index = 0; index < fallback_method_count; index++) {
        if (strcmp(fallback_methods[index].name, name) == 0 &&
            strcmp(fallback_methods[index].signature, signature) == 0) {
            result = (jmethodID)&fallback_methods[index];
            goto out;
        }
    }

    if (fallback_method_count >= FALLBACK_ID_CAPACITY)
        goto out;

    char *saved_name = strdup(name);
    char *saved_signature = strdup(signature);
    if (!saved_name || !saved_signature) {
        free(saved_name);
        free(saved_signature);
        goto out;
    }

    FallbackMethod *method = &fallback_methods[fallback_method_count++];
    method->name = saved_name;
    method->signature = saved_signature;
    method->type = type;
    result = (jmethodID)method;

out:
    pthread_mutex_unlock(&fallback_id_mutex);
    return result;
}

static jfieldID fallback_field_id(const char *name, const char *signature,
                                  FIELD_TYPE type) {
    jfieldID result = NULL;
    pthread_mutex_lock(&fallback_id_mutex);
    for (int index = 0; index < fallback_field_count; index++) {
        if (strcmp(fallback_fields[index].name, name) == 0 &&
            strcmp(fallback_fields[index].signature, signature) == 0) {
            result = (jfieldID)&fallback_fields[index];
            goto out;
        }
    }

    if (fallback_field_count >= FALLBACK_ID_CAPACITY)
        goto out;

    char *saved_name = strdup(name);
    char *saved_signature = strdup(signature);
    if (!saved_name || !saved_signature) {
        free(saved_name);
        free(saved_signature);
        goto out;
    }

    FallbackField *field = &fallback_fields[fallback_field_count++];
    field->name = saved_name;
    field->signature = saved_signature;
    field->type = type;
    field->has_value = JNI_FALSE;
    memset(&field->value, 0, sizeof(field->value));
    result = (jfieldID)field;

out:
    pthread_mutex_unlock(&fallback_id_mutex);
    return result;
}

static const char *object_result_signature(const char *signature) {
    const char *result = signature ? strrchr(signature, ')') : NULL;
    return result && result[1] ? result + 1 : signature;
}

static jobject neutral_object(const char *signature) {
    const char *result = object_result_signature(signature);
    if (!result)
        return (jobject)&fallback_opaque_object;
    if (strcmp(result, "Ljava/lang/String;") == 0) {
        pthread_mutex_lock(&fallback_id_mutex);
        if (!fallback_empty_string)
            fallback_empty_string = jni->NewStringUTF(&jni, "");
        jobject value = fallback_empty_string;
        pthread_mutex_unlock(&fallback_id_mutex);
        return value;
    }
    if (result[0] != '[')
        return (jobject)&fallback_opaque_object;

    int slot;
    switch (result[1]) {
        case 'Z': slot = 0; break;
        case 'B': slot = 1; break;
        case 'C': slot = 2; break;
        case 'S': slot = 3; break;
        case 'I': slot = 4; break;
        case 'J': slot = 5; break;
        case 'F': slot = 6; break;
        case 'D': slot = 7; break;
        default: slot = 8; break;
    }

    pthread_mutex_lock(&fallback_id_mutex);
    if (!fallback_empty_arrays[slot]) {
        switch (slot) {
            case 0: fallback_empty_arrays[slot] =
                        (jobject)jni->NewBooleanArray(&jni, 0); break;
            case 1: fallback_empty_arrays[slot] =
                        (jobject)jni->NewByteArray(&jni, 0); break;
            case 2: fallback_empty_arrays[slot] =
                        (jobject)jni->NewCharArray(&jni, 0); break;
            case 3: fallback_empty_arrays[slot] =
                        (jobject)jni->NewShortArray(&jni, 0); break;
            case 4: fallback_empty_arrays[slot] =
                        (jobject)jni->NewIntArray(&jni, 0); break;
            case 5: fallback_empty_arrays[slot] =
                        (jobject)jni->NewLongArray(&jni, 0); break;
            case 6: fallback_empty_arrays[slot] =
                        (jobject)jni->NewFloatArray(&jni, 0); break;
            case 7: fallback_empty_arrays[slot] =
                        (jobject)jni->NewDoubleArray(&jni, 0); break;
            default: fallback_empty_arrays[slot] =
                         (jobject)jni->NewObjectArray(&jni, 0, NULL, NULL); break;
        }
    }
    jobject value = fallback_empty_arrays[slot];
    pthread_mutex_unlock(&fallback_id_mutex);
    return value;
}

jfieldID getFieldIdByName(const char* name, const char* signature) {
    FIELD_TYPE expected_type = field_type_from_signature(signature);
    for (int i = 0; i < nameToFieldId_size() / sizeof(NameToFieldID); i++) {
        if (strcmp(name, nameToFieldId[i].name) == 0 &&
            nameToFieldId[i].f == expected_type) {
            return (jfieldID) nameToFieldId[i].id;
        }
    }

    if (expected_type == FIELD_TYPE_UNKNOWN) {
        fjni_logv_err("Invalid field signature for \"%s\": \"%s\"", name,
                      signature ? signature : "(null)");
        return NULL;
    }

    jfieldID result = fallback_field_id(name, signature, expected_type);
    fjni_logv_warn("Typed fallback field \"%s\" \"%s\": 0x%x", name,
                   signature, (int)result);
    return result;
}

const char* fieldTypeToStr(FIELD_TYPE t) {
    switch (t) {
        case FIELD_TYPE_INT:
            return "FIELD_TYPE_INT";
        case FIELD_TYPE_OBJECT:
            return "FIELD_TYPE_OBJECT";
        case FIELD_TYPE_BOOLEAN:
            return "FIELD_TYPE_BOOLEAN";
        case FIELD_TYPE_BYTE:
            return "FIELD_TYPE_BYTE";
        case FIELD_TYPE_CHAR:
            return "FIELD_TYPE_CHAR";
        case FIELD_TYPE_SHORT:
            return "FIELD_TYPE_SHORT";
        case FIELD_TYPE_LONG:
            return "FIELD_TYPE_LONG";
        case FIELD_TYPE_FLOAT:
            return "FIELD_TYPE_FLOAT";
        case FIELD_TYPE_DOUBLE:
            return "FIELD_TYPE_DOUBLE";
        default:
            return "FIELD_TYPE_UNKNOWN";
    }
}

jsize getFieldTypeSize(FIELD_TYPE fieldType) {
    switch (fieldType) {
        case FIELD_TYPE_OBJECT:
            return sizeof(jobject);
        case FIELD_TYPE_BOOLEAN:
            return sizeof(jboolean);
        case FIELD_TYPE_BYTE:
            return sizeof(jbyte);
        case FIELD_TYPE_CHAR:
            return sizeof(jchar);
        case FIELD_TYPE_SHORT:
            return sizeof(jshort);
        case FIELD_TYPE_INT:
            return sizeof(jint);
        case FIELD_TYPE_LONG:
            return sizeof(jlong);
        case FIELD_TYPE_FLOAT:
            return sizeof(jfloat);
        case FIELD_TYPE_DOUBLE:
            return sizeof(jdouble);
        default:
            return sizeof(void *);
    }
}

jobject getObjectFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type != FIELD_TYPE_OBJECT)
            return NULL;
        return fallback->has_value ? fallback->value.object_value
                                   : neutral_object(fallback->signature);
    }
    getFieldValueById(jobject, FIELD_TYPE_OBJECT, FieldsObject, fieldsObject, fieldsObject_size, id, (jobject)0x42424242);
}

jint getIntFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_INT && fallback->has_value
                   ? fallback->value.int_value : 0;
    getFieldValueById(jint, FIELD_TYPE_INT, FieldsInt, fieldsInt, fieldsInt_size, id, 1);
}

jboolean getBooleanFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_BOOLEAN && fallback->has_value
                   ? fallback->value.boolean_value : JNI_FALSE;
    getFieldValueById(jboolean, FIELD_TYPE_BOOLEAN, FieldsBoolean, fieldsBoolean, fieldsBoolean_size, id, JNI_FALSE);
}

jbyte getByteFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_BYTE && fallback->has_value
                   ? fallback->value.byte_value : 0;
    getFieldValueById(jbyte, FIELD_TYPE_BYTE, FieldsByte, fieldsByte, fieldsByte_size, id, 'a');
}

jchar getCharFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_CHAR && fallback->has_value
                   ? fallback->value.char_value : 0;
    getFieldValueById(jchar, FIELD_TYPE_CHAR, FieldsChar, fieldsChar, fieldsChar_size, id, 'b');
}

jshort getShortFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_SHORT && fallback->has_value
                   ? fallback->value.short_value : 0;
    getFieldValueById(jshort, FIELD_TYPE_SHORT, FieldsShort, fieldsShort, fieldsShort_size, id, 1);
}

jlong getLongFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_LONG && fallback->has_value
                   ? fallback->value.long_value : 0;
    getFieldValueById(jlong, FIELD_TYPE_LONG, FieldsLong, fieldsLong, fieldsLong_size, id, 1);
}

jfloat getFloatFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_FLOAT && fallback->has_value
                   ? fallback->value.float_value : 0.0f;
    getFieldValueById(jfloat, FIELD_TYPE_FLOAT, FieldsFloat, fieldsFloat, fieldsFloat_size, id, 1.0f);
}

jdouble getDoubleFieldValueById(jfieldID id) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback)
        return fallback->type == FIELD_TYPE_DOUBLE && fallback->has_value
                   ? fallback->value.double_value : 0.0;
    getFieldValueById(jdouble, FIELD_TYPE_DOUBLE, FieldsDouble, fieldsDouble, fieldsDouble_size, id, 1);
}

void setObjectFieldValueById(jfieldID id, jobject value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_OBJECT) {
            fallback->value.object_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jobject, FIELD_TYPE_OBJECT, FieldsObject, fieldsObject, fieldsObject_size, id, value);
}

void setIntFieldValueById(jfieldID id, jint value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_INT) {
            fallback->value.int_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jint, FIELD_TYPE_INT, FieldsInt, fieldsInt, fieldsInt_size, id, value);
}

void setBooleanFieldValueById(jfieldID id, jboolean value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_BOOLEAN) {
            fallback->value.boolean_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jboolean, FIELD_TYPE_BOOLEAN, FieldsBoolean, fieldsBoolean, fieldsBoolean_size, id, value);
}

void setByteFieldValueById(jfieldID id, jbyte value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_BYTE) {
            fallback->value.byte_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jbyte, FIELD_TYPE_BYTE, FieldsByte, fieldsByte, fieldsByte_size, id, value);
}

void setCharFieldValueById(jfieldID id, jchar value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_CHAR) {
            fallback->value.char_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jchar, FIELD_TYPE_CHAR, FieldsChar, fieldsChar, fieldsChar_size, id, value);
}

void setShortFieldValueById(jfieldID id, jshort value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_SHORT) {
            fallback->value.short_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jshort, FIELD_TYPE_SHORT, FieldsShort, fieldsShort, fieldsShort_size, id, value);
}

void setLongFieldValueById(jfieldID id, jlong value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_LONG) {
            fallback->value.long_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jlong, FIELD_TYPE_LONG, FieldsLong, fieldsLong, fieldsLong_size, id, value);
}

void setFloatFieldValueById(jfieldID id, jfloat value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_FLOAT) {
            fallback->value.float_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jfloat, FIELD_TYPE_FLOAT, FieldsFloat, fieldsFloat, fieldsFloat_size, id, value);
}

void setDoubleFieldValueById(jfieldID id, jdouble value) {
    FallbackField *fallback = fallback_field_by_id(id);
    if (fallback) {
        if (fallback->type == FIELD_TYPE_DOUBLE) {
            fallback->value.double_value = value;
            fallback->has_value = JNI_TRUE;
        }
        return;
    }
    setFieldValueById(jdouble, FIELD_TYPE_DOUBLE, FieldsDouble, fieldsDouble, fieldsDouble_size, id, value);
}

jmethodID getMethodIdByName(const char* name, const char* signature) {
    METHOD_TYPE expected_type = method_type_from_signature(name, signature);
    /* Exact-signature entries win.  This prevents a String result handler
     * from being reused for an array overload that has the same global name. */
    for (int i = 0; i < nameToMethodId_size() / sizeof(NameToMethodID); i++) {
        if (strcmp(name, nameToMethodId[i].name) == 0 &&
            nameToMethodId[i].f == expected_type &&
            nameToMethodId[i].signature &&
            signature && strcmp(signature, nameToMethodId[i].signature) == 0) {
            return (jmethodID) nameToMethodId[i].id;
        }
    }

    for (int i = 0; i < nameToMethodId_size() / sizeof(NameToMethodID); i++) {
        if (strcmp(name, nameToMethodId[i].name) == 0 &&
            nameToMethodId[i].f == expected_type &&
            !nameToMethodId[i].signature) {
            return (jmethodID) nameToMethodId[i].id;
        }
    }

    if (expected_type == METHOD_TYPE_UNKNOWN) {
        fjni_logv_err("Invalid method signature for \"%s\": \"%s\"", name,
                      signature ? signature : "(null)");
        return NULL;
    }

    jmethodID result = fallback_method_id(name, signature, expected_type);
    fjni_logv_warn("Typed fallback method \"%s\" \"%s\": 0x%x", name,
                   signature, (int)result);
    return result;
}

jobject methodObjectCall(jmethodID id, va_list args) {
#ifdef ASPHALT8_VITA3K_SUPPORT
    if (!id) {
        (void)args;
        fjni_log_warn("method ID 0 object call, returning empty string for Vita3K");
        return neutral_object("()Ljava/lang/String;");
    }
#endif

    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return fallback->type == METHOD_TYPE_OBJECT
                   ? neutral_object(fallback->signature) : NULL;
    }

    for (int i = 0; i < methodsObject_size() / sizeof(MethodsObject); i++) {
        if (methodsObject[i].id == (int)id) {
            return methodsObject[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return NULL;
}

void methodVoidCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return;
    }

    for (int i = 0; i < methodsVoid_size() / sizeof(MethodsVoid); i++) {
        if (methodsVoid[i].id == (int)id) {
            return methodsVoid[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
}

jboolean methodBooleanCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return JNI_FALSE;
    }

    for (int i = 0; i < methodsBoolean_size() / sizeof(MethodsBoolean); i++) {
        if (methodsBoolean[i].id == (int)id) {
            return methodsBoolean[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return JNI_FALSE;
}

jbyte methodByteCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return 0;
    }

    for (int i = 0; i < methodsByte_size() / sizeof(MethodsByte); i++) {
        if (methodsByte[i].id == (int)id) {
            return methodsByte[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return 0;
}

jshort methodShortCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return 0;
    }

    for (int i = 0; i < methodsShort_size() / sizeof(MethodsShort); i++) {
        if (methodsShort[i].id == (int)id) {
            return methodsShort[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return 0;
}

jdouble methodDoubleCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return 0.0;
    }

    for (int i = 0; i < methodsDouble_size() / sizeof(MethodsDouble); i++) {
        if (methodsDouble[i].id == (int)id) {
            return methodsDouble[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return 0;
}

jchar methodCharCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return 0;
    }

    for (int i = 0; i < methodsChar_size() / sizeof(MethodsChar); i++) {
        if (methodsChar[i].id == (int)id) {
            return methodsChar[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return 0;
}

jlong methodLongCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return 0;
    }

    for (int i = 0; i < methodsLong_size() / sizeof(MethodsLong); i++) {
        if (methodsLong[i].id == (int)id) {
            return methodsLong[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return -1;
}

jint methodIntCall(jmethodID id, va_list args) {
#ifdef ASPHALT8_VITA3K_SUPPORT
    if (!id) {
        (void)args;
        fjni_log_warn("method ID 0 int call, returning 0 for Vita3K");
        return 0;
    }
#endif

    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return 0;
    }

    for (int i = 0; i < methodsInt_size() / sizeof(MethodsInt); i++) {
        if (methodsInt[i].id == (int)id) {
            return methodsInt[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return -1;
}

jfloat methodFloatCall(jmethodID id, va_list args) {
    FallbackMethod *fallback = fallback_method_by_id(id);
    if (fallback) {
        (void)args;
        return 0.0f;
    }

    for (int i = 0; i < methodsFloat_size() / sizeof(MethodsFloat); i++) {
        if (methodsFloat[i].id == (int)id) {
            return methodsFloat[i].Method(id, args);
        }
    }

    fjni_logv_warn("method ID %i not found!", (int)id);
    return -1;
}

JavaDynArray * jda_alloc(jsize len, FIELD_TYPE type) {
    if (len < 0)
        return NULL;

    size_t element_size = (size_t)getFieldTypeSize(type);
    size_t byte_count = (size_t)len * element_size;
    if (len > 0 && byte_count / (size_t)len != element_size)
        return NULL;

    /* Java arrays are zero-initialized, and a zero-length array is still a
     * valid non-NULL object.  Allocate one backing byte for that case. */
    void * array = calloc(1, byte_count ? byte_count : 1);
    if (!array) {
        return NULL;
    }

    JavaDynArray * ret = malloc(sizeof(JavaDynArray));
    if (!ret) {
        free(array);
        return NULL;
    }

    ret->magic = JDA_MAGIC;
    ret->array = array;
    ret->len = len;
    ret->type = type;

    return ret;
}

jsize jda_sizeof(JavaDynArray * jda) {
    if (!jda)
        return -1;

    return jda->len;
}

jboolean jda_realloc(JavaDynArray * jda, jsize len) {
    if (!jda)
        return JNI_FALSE;

    void * res = realloc(jda->array, len * getFieldTypeSize(jda->type));
    if (res == NULL) {
        return JNI_FALSE;
    }

    jda->array = res;

    return JNI_TRUE;
}

jboolean jda_free(JavaDynArray * jda) {
    if (!jda)
        return JNI_FALSE;

    if (jda->magic != JDA_MAGIC)
        return JNI_FALSE;

    free(jda->array);
    free(jda);

    return JNI_TRUE;
}

jboolean jstr_utf16_to_utf8(JavaString * jstr) {
    if (!jstr) return JNI_FALSE;

    if (jstr->utf8 == NULL) {
        jstr->utf8 = jda_alloc(jstr->utf16->len+1, FIELD_TYPE_BYTE);
        if (jstr->utf8 == NULL) {
            return JNI_FALSE;
        }
    } else if (jstr->utf8->len < jstr->utf16->len+1) {
        if (jda_realloc(jstr->utf8, jstr->utf16->len+1) == JNI_FALSE) {
            return JNI_FALSE;
        }
    }

    utf16_to_utf8(jstr->utf16->array, jstr->utf16->len, jstr->utf8->array, jstr->utf8->len);

    char * arr = jstr->utf8->array;
    arr[jstr->utf8->len - 1] = '\0';
    return JNI_TRUE;
}

jboolean jstr_utf8_to_utf16(JavaString * jstr) {
    if (!jstr) return JNI_FALSE;

    if (jstr->utf8 == NULL) {
        return JNI_FALSE;
    }

    if (jstr->utf16->len + 1 < jstr->utf8->len) {
        if (jda_realloc(jstr->utf16, jstr->utf8->len - 1) == JNI_FALSE) {
            return JNI_FALSE;
        }
    }

    utf8_to_utf16(jstr->utf8->array, jstr->utf8->len - 1, jstr->utf16->array, jstr->utf16->len);

    return JNI_TRUE;
}

va_list _AtoV(int dummy, ...) {
    va_list args1;
    va_start(args1, dummy);
    va_list args2;
    va_copy(args2, args1);
    va_end(args1);
    return args2;
}
