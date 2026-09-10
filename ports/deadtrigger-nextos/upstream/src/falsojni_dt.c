/*
 * Dead Trigger JNI profile.
 *
 * The base VM is the proven shared Unity/Android JNI contract. Dead Trigger
 * specializes the Activity, audio and InputDevice behavior consumed by this
 * exact Unity 2019 build.
 */
#include "../../ff5/src/falsojni.c"

#include "prefs_dt.h"

static int g_dt_prefs_object;
static int g_dt_prefs_editor;

static void dt_note_axis(jmethodID method, jint axis, jfloat value);

static const char *dt_jstring(jobject value) {
    return value ? (const char *)value : "";
}

static jobject dt_GetStaticObjectField(void *environment, jclass klass,
                                       jfieldID field) {
    const char *name = (const char *)field;
    if (name && !strcmp(name, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) {
        /*
         * AudioManager.getProperty receives the value of these Java String
         * constants, not their field IDs.  The base dispatcher recognizes a
         * compact key by its semantic suffix.
         */
        return make_jstring("FramesPerBuffer");
    }
    if (name && !strcmp(name, "PROPERTY_OUTPUT_SAMPLE_RATE"))
        return make_jstring("SampleRate");
    return f_GetStaticObjectField(environment, klass, field);
}

static jint dt_GetStaticIntField(void *environment, jclass klass,
                                 jfieldID field) {
    const char *name = (const char *)field;
    if (name && !strcmp(name, "GET_DEVICES_OUTPUTS"))
        return 2;
    if (name && !strcmp(name, "STREAM_MUSIC"))
        return 3;
    return f_GetStaticIntField(environment, klass, field);
}

static jobject dt_audio_property(const char *key) {
    if (key && strstr(key, "FRAMES_PER_BUFFER")) {
        fprintf(stderr, "[jni] AudioManager.getProperty(%s) -> 256\n", key);
        return make_jstring("256");
    }
    if (key && strstr(key, "SAMPLE_RATE")) {
        fprintf(stderr, "[jni] AudioManager.getProperty(%s) -> 48000\n", key);
        return make_jstring("48000");
    }
    return NULL;
}

/*
 * Unity's Android PlayerPrefs backend passes every managed key and string
 * value through android.net.Uri.encode/decode.  Returning a generic Java
 * sentinel here aliases every key to the same byte (0xac) and destroys the
 * save.  Reproduce Uri's default UTF-8 percent encoding instead.
 */
static int dt_uri_literal(unsigned char value, const char *extra) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           strchr("_-.!~'()*", value) != NULL ||
           (extra && strchr(extra, value) != NULL);
}

static jobject dt_uri_encode(const char *text, const char *extra) {
    static const char hex[] = "0123456789ABCDEF";
    if (!text)
        return make_jstring("");
    size_t length = strlen(text);
    if (length > (SIZE_MAX - 1u) / 3u)
        return make_jstring("");
    char *encoded = malloc(length * 3u + 1u);
    if (!encoded)
        return make_jstring("");
    size_t output = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)text[i];
        if (dt_uri_literal(value, extra)) {
            encoded[output++] = (char)value;
        } else {
            encoded[output++] = '%';
            encoded[output++] = hex[value >> 4];
            encoded[output++] = hex[value & 15u];
        }
    }
    encoded[output] = '\0';
    jobject result = make_jstring(encoded);
    free(encoded);
    return result;
}

static int dt_hex_digit(unsigned char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static jobject dt_uri_decode(const char *text) {
    if (!text)
        return make_jstring("");
    size_t length = strlen(text);
    char *decoded = malloc(length + 1u);
    if (!decoded)
        return make_jstring("");
    size_t output = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '%' && i + 2u < length) {
            int high = dt_hex_digit((unsigned char)text[i + 1u]);
            int low = dt_hex_digit((unsigned char)text[i + 2u]);
            if (high >= 0 && low >= 0) {
                decoded[output++] = (char)((high << 4) | low);
                i += 2u;
                continue;
            }
        }
        decoded[output++] = text[i];
    }
    decoded[output] = '\0';
    jobject result = make_jstring(decoded);
    free(decoded);
    return result;
}

static jobject dt_CallStaticObjectMethodV(void *environment, jclass klass,
                                          jmethodID method,
                                          va_list arguments) {
    const char *name = method_name(method);
    if (name && !strcmp(name, "encode")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *text = dt_jstring(va_arg(probe, jobject));
        const char *signature = method_signature(method);
        const char *extra = NULL;
        if (signature &&
            !strcmp(signature,
                    "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"))
            extra = dt_jstring(va_arg(probe, jobject));
        va_end(probe);
        return dt_uri_encode(text, extra);
    }
    if (name && !strcmp(name, "decode")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *text = dt_jstring(va_arg(probe, jobject));
        va_end(probe);
        return dt_uri_decode(text);
    }
    return f_CallStaticObjectMethodV(environment, klass, method, arguments);
}

static jobject dt_CallStaticObjectMethod(void *environment, jclass klass,
                                         jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jobject result =
        dt_CallStaticObjectMethodV(environment, klass, method, arguments);
    va_end(arguments);
    return result;
}

static jobject dt_CallStaticObjectMethodA(void *environment, jclass klass,
                                          jmethodID method,
                                          const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (name && !strcmp(name, "encode")) {
        const char *signature = method_signature(method);
        const char *text = arguments ? dt_jstring(arguments[0].l) : "";
        const char *extra =
            signature &&
                    !strcmp(signature,
                            "(Ljava/lang/String;Ljava/lang/String;)"
                            "Ljava/lang/String;") &&
                    arguments
                ? dt_jstring(arguments[1].l) : NULL;
        return dt_uri_encode(text, extra);
    }
    if (name && !strcmp(name, "decode"))
        return dt_uri_decode(arguments ? dt_jstring(arguments[0].l) : "");
    return f_CallStaticObjectMethodA(environment, klass, method, arguments);
}

static jobject dt_CallObjectMethodV(void *environment, jobject object,
                                    jmethodID method, va_list arguments) {
    const char *name = method_name(method);
    if (name && !strcmp(name, "getSharedPreferences")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *store = dt_jstring(va_arg(probe, jobject));
        (void)va_arg(probe, jint);
        va_end(probe);
        if (!dt_prefs_ready())
            fprintf(stderr,
                    "[prefs] getSharedPreferences(%s): carga falhou\n",
                    store);
        return &g_dt_prefs_object;
    }
    if (object == &g_dt_prefs_object && name && !strcmp(name, "edit"))
        return &g_dt_prefs_editor;
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getString")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *key = dt_jstring(va_arg(probe, jobject));
        jobject fallback = va_arg(probe, jobject);
        va_end(probe);
        char *stored = dt_prefs_get_string_copy(key);
        if (!stored)
            return fallback ? fallback : make_jstring("");
        jobject result = make_jstring(stored);
        free(stored);
        return result;
    }
    if (object == &g_dt_prefs_editor && name) {
        va_list probe;
        va_copy(probe, arguments);
        const char *key = NULL;
        int handled = 1;
        if (!strcmp(name, "putString")) {
            key = dt_jstring(va_arg(probe, jobject));
            const char *value = dt_jstring(va_arg(probe, jobject));
            (void)dt_prefs_set_string(key, value);
        } else if (!strcmp(name, "putInt")) {
            key = dt_jstring(va_arg(probe, jobject));
            (void)dt_prefs_set_int(key, va_arg(probe, jint));
        } else if (!strcmp(name, "putFloat")) {
            key = dt_jstring(va_arg(probe, jobject));
            (void)dt_prefs_set_float(key, (float)va_arg(probe, double));
        } else if (!strcmp(name, "putBoolean")) {
            key = dt_jstring(va_arg(probe, jobject));
            (void)dt_prefs_set_bool(key, va_arg(probe, jint) != 0);
        } else if (!strcmp(name, "putLong")) {
            key = dt_jstring(va_arg(probe, jobject));
            (void)dt_prefs_set_long(key, va_arg(probe, jlong));
        } else if (!strcmp(name, "remove")) {
            key = dt_jstring(va_arg(probe, jobject));
            (void)dt_prefs_remove(key);
        } else if (!strcmp(name, "clear")) {
            (void)dt_prefs_clear();
        } else {
            handled = 0;
        }
        va_end(probe);
        if (handled) {
            if (key)
                fprintf(stderr, "[prefs] %s(%s)\n", name, key);
            return &g_dt_prefs_editor;
        }
    }
    if (name && !strcmp(name, "getProperty")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *key = (const char *)va_arg(probe, jobject);
        va_end(probe);
        jobject value = dt_audio_property(key);
        if (value)
            return value;
    }
    return dispatch_object(environment, object, method, arguments);
}

static jobject dt_CallObjectMethod(void *environment, jobject object,
                                   jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jobject result =
        dt_CallObjectMethodV(environment, object, method, arguments);
    va_end(arguments);
    return result;
}

static jobject dt_CallObjectMethodA(void *environment, jobject object,
                                    jmethodID method,
                                    const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (name && !strcmp(name, "getSharedPreferences")) {
        const char *store =
            arguments ? dt_jstring(arguments[0].l) : "";
        if (!dt_prefs_ready())
            fprintf(stderr,
                    "[prefs] getSharedPreferences(%s): carga falhou\n",
                    store);
        return &g_dt_prefs_object;
    }
    if (object == &g_dt_prefs_object && name && !strcmp(name, "edit"))
        return &g_dt_prefs_editor;
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getString")) {
        const char *key = arguments ? dt_jstring(arguments[0].l) : "";
        jobject fallback = arguments ? arguments[1].l : NULL;
        char *stored = dt_prefs_get_string_copy(key);
        if (!stored)
            return fallback ? fallback : make_jstring("");
        jobject result = make_jstring(stored);
        free(stored);
        return result;
    }
    if (object == &g_dt_prefs_editor && name) {
        const char *key =
            arguments ? dt_jstring(arguments[0].l) : "";
        int handled = 1;
        if (!strcmp(name, "putString")) {
            (void)dt_prefs_set_string(
                key, arguments ? dt_jstring(arguments[1].l) : "");
        } else if (!strcmp(name, "putInt")) {
            (void)dt_prefs_set_int(key, arguments ? arguments[1].i : 0);
        } else if (!strcmp(name, "putFloat")) {
            (void)dt_prefs_set_float(key, arguments ? arguments[1].f : 0.0f);
        } else if (!strcmp(name, "putBoolean")) {
            (void)dt_prefs_set_bool(key,
                                    arguments && arguments[1].z != 0);
        } else if (!strcmp(name, "putLong")) {
            (void)dt_prefs_set_long(key, arguments ? arguments[1].j : 0);
        } else if (!strcmp(name, "remove")) {
            (void)dt_prefs_remove(key);
        } else if (!strcmp(name, "clear")) {
            (void)dt_prefs_clear();
            key = NULL;
        } else {
            handled = 0;
        }
        if (handled) {
            if (key)
                fprintf(stderr, "[prefs] %s(%s)\n", name, key);
            return &g_dt_prefs_editor;
        }
    }
    if (name && !strcmp(name, "getProperty")) {
        const char *key =
            arguments ? (const char *)arguments[0].l : NULL;
        jobject value = dt_audio_property(key);
        if (value)
            return value;
    }
    return dispatch_object_a(environment, object, method, arguments);
}

static jint dt_CallIntMethodV(void *environment, jobject object,
                              jmethodID method, va_list arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getInt")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *key = dt_jstring(va_arg(probe, jobject));
        jint fallback = va_arg(probe, jint);
        va_end(probe);
        int32_t stored;
        return dt_prefs_get_int(key, &stored) ? stored : fallback;
    }
    return f_CallIntMethodV(environment, object, method, arguments);
}

static jint dt_CallIntMethod(void *environment, jobject object,
                             jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jint result =
        dt_CallIntMethodV(environment, object, method, arguments);
    va_end(arguments);
    return result;
}

static jint dt_CallIntMethodA(void *environment, jobject object,
                              jmethodID method,
                              const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getInt")) {
        const char *key = arguments ? dt_jstring(arguments[0].l) : "";
        jint fallback = arguments ? arguments[1].i : 0;
        int32_t stored;
        return dt_prefs_get_int(key, &stored) ? stored : fallback;
    }
    return f_CallIntMethodA(environment, object, method, arguments);
}

static jfloat dt_CallFloatMethodV(void *environment, jobject object,
                                  jmethodID method, va_list arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getFloat")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *key = dt_jstring(va_arg(probe, jobject));
        float fallback = (float)va_arg(probe, double);
        va_end(probe);
        float stored;
        return dt_prefs_get_float(key, &stored) ? stored : fallback;
    }
    jint axis = 0;
    if (name && !strcmp(name, "getAxisValue")) {
        va_list probe;
        va_copy(probe, arguments);
        axis = va_arg(probe, jint);
        va_end(probe);
    }
    jfloat value =
        f_CallFloatMethodV(environment, object, method, arguments);
    if (name && !strcmp(name, "getAxisValue"))
        dt_note_axis(method, axis, value);
    return value;
}

static jfloat dt_CallFloatMethod(void *environment, jobject object,
                                 jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jfloat result =
        dt_CallFloatMethodV(environment, object, method, arguments);
    va_end(arguments);
    return result;
}

static jfloat dt_CallFloatMethodA(void *environment, jobject object,
                                  jmethodID method,
                                  const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getFloat")) {
        const char *key = arguments ? dt_jstring(arguments[0].l) : "";
        float fallback = arguments ? arguments[1].f : 0.0f;
        float stored;
        return dt_prefs_get_float(key, &stored) ? stored : fallback;
    }
    jint axis = name && !strcmp(name, "getAxisValue") && arguments
                    ? arguments[0].i : 0;
    jfloat value =
        f_CallFloatMethodA(environment, object, method, arguments);
    if (name && !strcmp(name, "getAxisValue"))
        dt_note_axis(method, axis, value);
    return value;
}

static jlong dt_CallLongMethodV(void *environment, jobject object,
                                jmethodID method, va_list arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getLong")) {
        va_list probe;
        va_copy(probe, arguments);
        const char *key = dt_jstring(va_arg(probe, jobject));
        jlong fallback = va_arg(probe, jlong);
        va_end(probe);
        int64_t stored;
        return dt_prefs_get_long(key, &stored) ? stored : fallback;
    }
    return f_CallLongMethodV(environment, object, method, arguments);
}

static jlong dt_CallLongMethod(void *environment, jobject object,
                               jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jlong result =
        dt_CallLongMethodV(environment, object, method, arguments);
    va_end(arguments);
    return result;
}

static jlong dt_CallLongMethodA(void *environment, jobject object,
                                jmethodID method,
                                const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name && !strcmp(name, "getLong")) {
        const char *key = arguments ? dt_jstring(arguments[0].l) : "";
        jlong fallback = arguments ? arguments[1].j : 0;
        int64_t stored;
        return dt_prefs_get_long(key, &stored) ? stored : fallback;
    }
    return f_CallLongMethodA(environment, object, method, arguments);
}

static jboolean dt_CallBooleanMethodV(void *environment, jobject object,
                                      jmethodID method, va_list arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name) {
        va_list probe;
        va_copy(probe, arguments);
        if (!strcmp(name, "contains")) {
            const char *key = dt_jstring(va_arg(probe, jobject));
            va_end(probe);
            return dt_prefs_contains(key) != 0;
        }
        if (!strcmp(name, "getBoolean")) {
            const char *key = dt_jstring(va_arg(probe, jobject));
            jboolean fallback = (jboolean)(va_arg(probe, jint) != 0);
            va_end(probe);
            int stored;
            return dt_prefs_get_bool(key, &stored)
                       ? (jboolean)(stored != 0) : fallback;
        }
        va_end(probe);
    }
    if (object == &g_dt_prefs_editor && name && !strcmp(name, "commit"))
        return dt_prefs_flush("commit") != 0;
    return f_CallBooleanMethodV(environment, object, method, arguments);
}

static jboolean dt_CallBooleanMethod(void *environment, jobject object,
                                     jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jboolean result =
        dt_CallBooleanMethodV(environment, object, method, arguments);
    va_end(arguments);
    return result;
}

static jboolean dt_CallBooleanMethodA(void *environment, jobject object,
                                      jmethodID method,
                                      const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_object && name) {
        const char *key = arguments ? dt_jstring(arguments[0].l) : "";
        if (!strcmp(name, "contains"))
            return dt_prefs_contains(key) != 0;
        if (!strcmp(name, "getBoolean")) {
            jboolean fallback =
                arguments ? (jboolean)(arguments[1].z != 0) : 0;
            int stored;
            return dt_prefs_get_bool(key, &stored)
                       ? (jboolean)(stored != 0) : fallback;
        }
    }
    if (object == &g_dt_prefs_editor && name && !strcmp(name, "commit"))
        return dt_prefs_flush("commit") != 0;
    return f_CallBooleanMethodA(environment, object, method, arguments);
}

static void dt_CallVoidMethodV(void *environment, jobject object,
                               jmethodID method, va_list arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_editor && name && !strcmp(name, "apply")) {
        (void)dt_prefs_flush("apply");
        return;
    }
    dispatch_void(environment, object, method, arguments);
}

static void dt_CallVoidMethod(void *environment, jobject object,
                              jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    dt_CallVoidMethodV(environment, object, method, arguments);
    va_end(arguments);
}

static void dt_CallVoidMethodA(void *environment, jobject object,
                               jmethodID method,
                               const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (object == &g_dt_prefs_editor && name && !strcmp(name, "apply")) {
        (void)dt_prefs_flush("apply");
        return;
    }
    dispatch_void_a(environment, object, method, arguments);
}

static jint dt_parse_integer(jmethodID method, jobject value) {
    const char *name = method_name(method);
    if (!name || strcmp(name, "parseInt"))
        return 0;
    const char *text = (const char *)value;
    char *end = NULL;
    long parsed = text ? strtol(text, &end, 10) : 0;
    if (!text || end == text || *end != '\0' ||
        parsed < INT32_MIN || parsed > INT32_MAX)
        return 0;
    fprintf(stderr, "[jni] Integer.parseInt(%s) -> %ld\n", text, parsed);
    return (jint)parsed;
}

static jint dt_CallStaticIntMethodV(void *environment, jclass klass,
                                    jmethodID method, va_list arguments) {
    const char *name = method_name(method);
    if (!name || strcmp(name, "parseInt"))
        return f_CallStaticIntMethodV(environment, klass, method, arguments);
    va_list probe;
    va_copy(probe, arguments);
    jobject value = va_arg(probe, jobject);
    va_end(probe);
    return dt_parse_integer(method, value);
}

static jint dt_CallStaticIntMethod(void *environment, jclass klass,
                                   jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    jint result =
        dt_CallStaticIntMethodV(environment, klass, method, arguments);
    va_end(arguments);
    return result;
}

static jint dt_CallStaticIntMethodA(void *environment, jclass klass,
                                    jmethodID method,
                                    const fake_jvalue *arguments) {
    const char *name = method_name(method);
    if (!name || strcmp(name, "parseInt"))
        return f_CallStaticIntMethodA(environment, klass, method, arguments);
    return dt_parse_integer(method, arguments ? arguments[0].l : NULL);
}

static void dt_note_axis(jmethodID method, jint axis, jfloat value) {
    static unsigned logged_axes;
    const char *name = method_name(method);
    if (!name || strcmp(name, "getAxisValue") ||
        (value > -0.05f && value < 0.05f))
        return;
    unsigned bit = axis >= 0 && axis < 31 ? 1u << axis : 0x80000000u;
    if (logged_axes & bit)
        return;
    logged_axes |= bit;
    fprintf(stderr, "[input] Unity consumiu eixo Android %d = %.3f\n",
            axis, value);
}

void dt_jni_install_android_contract(void) {
    /*
     * Unity exposes InputDevice.getName() through Input.GetJoystickNames().
     * Dead Trigger only installs its built-in mappings for a short whitelist.
     * SDL already normalizes the physical pad into the Xbox button layout, so
     * publish the matching Android identity and let the game's own
     * InputManagerController load its native Xbox profile.
     */
    g_input_device.name = "Microsoft X-Box 360 pad";
    g_input_device.descriptor = "nextos:xinput:0";
    env_vt[34] = (void *)dt_CallObjectMethod;
    env_vt[35] = (void *)dt_CallObjectMethodV;
    env_vt[36] = (void *)dt_CallObjectMethodA;
    env_vt[37] = (void *)dt_CallBooleanMethod;
    env_vt[38] = (void *)dt_CallBooleanMethodV;
    env_vt[39] = (void *)dt_CallBooleanMethodA;
    env_vt[49] = (void *)dt_CallIntMethod;
    env_vt[50] = (void *)dt_CallIntMethodV;
    env_vt[51] = (void *)dt_CallIntMethodA;
    env_vt[52] = (void *)dt_CallLongMethod;
    env_vt[53] = (void *)dt_CallLongMethodV;
    env_vt[54] = (void *)dt_CallLongMethodA;
    env_vt[55] = (void *)dt_CallFloatMethod;
    env_vt[56] = (void *)dt_CallFloatMethodV;
    env_vt[57] = (void *)dt_CallFloatMethodA;
    env_vt[61] = (void *)dt_CallVoidMethod;
    env_vt[62] = (void *)dt_CallVoidMethodV;
    env_vt[63] = (void *)dt_CallVoidMethodA;
    env_vt[114] = (void *)dt_CallStaticObjectMethod;
    env_vt[115] = (void *)dt_CallStaticObjectMethodV;
    env_vt[116] = (void *)dt_CallStaticObjectMethodA;
    env_vt[129] = (void *)dt_CallStaticIntMethod;
    env_vt[130] = (void *)dt_CallStaticIntMethodV;
    env_vt[131] = (void *)dt_CallStaticIntMethodA;
    env_vt[145] = (void *)dt_GetStaticObjectField;
    env_vt[150] = (void *)dt_GetStaticIntField;
    fprintf(stderr,
            "[jni] Android: audio 48000/256; gamepad Microsoft X-Box 360 pad\n");
}
