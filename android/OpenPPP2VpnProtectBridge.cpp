#include <ppp/stdafx.h>

#if defined(_ANDROID)

#include "OpenPPP2VpnProtectBridge.h"

#include <ppp/diagnostics/Error.h>
#include <ppp/diagnostics/Telemetry.h>

#include <android/log.h>

namespace ppp
{
    namespace android
    {
        namespace
        {
            constexpr const char* TAG = "OpenPPP2VpnProtect";

            class ProtectBridgeState final
            {
            public:
                JavaVM*                                             vm = NULLPTR;
                jclass                                              clazz = NULLPTR;
                jmethodID                                           protect = NULLPTR;
                jmethodID                                           otel_post = NULLPTR;
                bool                                                enabled = true;
                std::mutex                                          syncobj;
            };

            ProtectBridgeState& GetState() noexcept
            {
                static ProtectBridgeState state;
                return state;
            }

            bool ClearException(JNIEnv* env, const char* where) noexcept
            {
                if (NULLPTR == env || !env->ExceptionCheck())
                {
                    return false;
                }

                __android_log_print(ANDROID_LOG_WARN, TAG, "jni exception in %s", NULLPTR != where ? where : "unknown");
                env->ExceptionClear();
                return true;
            }

            JNIEnv* AttachEnvironment(JavaVM* vm, bool& attached) noexcept
            {
                attached = false;
                if (NULLPTR == vm)
                {
                    return NULLPTR;
                }

                JNIEnv* env = NULLPTR;
                jint result = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
                if (JNI_OK == result)
                {
                    return env;
                }

                if (JNI_EDETACHED != result)
                {
                    return NULLPTR;
                }

#if defined(__ANDROID__)
                if (JNI_OK != vm->AttachCurrentThread(&env, NULLPTR))
#else
                if (JNI_OK != vm->AttachCurrentThread(reinterpret_cast<void**>(&env), NULLPTR))
#endif
                {
                    return NULLPTR;
                }

                attached = true;
                return env;
            }

            bool CacheProtectClassLocked(ProtectBridgeState& state, JNIEnv* env) noexcept
            {
                if (NULLPTR == env)
                {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEnvironmentInvalid);
                    return false;
                }

                if (NULLPTR != state.clazz && NULLPTR != state.protect && NULLPTR != state.otel_post)
                {
                    return true;
                }

                jclass local_clazz = env->FindClass(LIBOPENPPP2_CLASSNAME);
                ClearException(env, "FindClass(" LIBOPENPPP2_CLASSNAME ")");
                if (NULLPTR == local_clazz)
                {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEnvironmentInvalid);
                    __android_log_print(ANDROID_LOG_WARN, TAG, "class not found: %s", LIBOPENPPP2_CLASSNAME);
                    return false;
                }

                jmethodID protect_method = env->GetStaticMethodID(local_clazz, "protect", "(I)Z");
                ClearException(env, "GetStaticMethodID(protect)");
                if (NULLPTR == protect_method)
                {
                    env->DeleteLocalRef(local_clazz);
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEventDispatchFailed);
                    __android_log_print(ANDROID_LOG_WARN, TAG, "method not found: protect(I)Z");
                    return false;
                }

                jmethodID otel_post_method = env->GetStaticMethodID(local_clazz, "otel_post", "(Ljava/lang/String;[B)Z");
                ClearException(env, "GetStaticMethodID(otel_post)");
                if (NULLPTR == otel_post_method)
                {
                    env->DeleteLocalRef(local_clazz);
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEventDispatchFailed);
                    __android_log_print(ANDROID_LOG_WARN, TAG, "method not found: otel_post(Ljava/lang/String;[B)Z");
                    return false;
                }

                jclass global_clazz = static_cast<jclass>(env->NewGlobalRef(local_clazz));
                env->DeleteLocalRef(local_clazz);
                ClearException(env, "NewGlobalRef(protect class)");
                if (NULLPTR == global_clazz)
                {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::MemoryAllocationFailed);
                    return false;
                }

                if (NULLPTR != state.clazz)
                {
                    env->DeleteGlobalRef(state.clazz);
                }

                state.clazz = global_clazz;
                state.protect = protect_method;
                state.otel_post = otel_post_method;
                return true;
            }

            bool TelemetryHttpPost(const char* url, const void* body, size_t body_len, void* user_data) noexcept
            {
                (void)user_data;
                if (NULLPTR == url || '\0' == url[0] || NULLPTR == body || body_len < 1)
                {
                    return false;
                }

                JavaVM* vm = NULLPTR;
                jclass clazz = NULLPTR;
                jmethodID otel_post_method = NULLPTR;
                {
                    ProtectBridgeState& state = GetState();
                    std::lock_guard<std::mutex> scope(state.syncobj);
                    vm = state.vm;
                    clazz = state.clazz;
                    otel_post_method = state.otel_post;
                }

                bool attached = false;
                JNIEnv* env = AttachEnvironment(vm, attached);
                if (NULLPTR == env)
                {
                    return false;
                }

                if (NULLPTR == clazz || NULLPTR == otel_post_method)
                {
                    ProtectBridgeState& state = GetState();
                    std::lock_guard<std::mutex> scope(state.syncobj);
                    if (!CacheProtectClassLocked(state, env))
                    {
                        if (attached && NULLPTR != vm)
                        {
                            vm->DetachCurrentThread();
                        }
                        return false;
                    }
                    clazz = state.clazz;
                    otel_post_method = state.otel_post;
                }

                jstring url_string = env->NewStringUTF(url);
                jbyteArray body_bytes = env->NewByteArray(static_cast<jsize>(body_len));
                if (NULLPTR == url_string || NULLPTR == body_bytes)
                {
                    ClearException(env, "NewStringUTF/NewByteArray(otel_post)");
                    if (NULLPTR != url_string)
                    {
                        env->DeleteLocalRef(url_string);
                    }
                    if (NULLPTR != body_bytes)
                    {
                        env->DeleteLocalRef(body_bytes);
                    }
                    if (attached && NULLPTR != vm)
                    {
                        vm->DetachCurrentThread();
                    }
                    return false;
                }

                env->SetByteArrayRegion(body_bytes, 0, static_cast<jsize>(body_len), reinterpret_cast<const jbyte*>(body));
                bool ok = false;
                if (!ClearException(env, "SetByteArrayRegion(otel_post)"))
                {
                    ok = env->CallStaticBooleanMethod(clazz, otel_post_method, url_string, body_bytes);
                    ClearException(env, "CallStaticBooleanMethod(otel_post)");
                }

                env->DeleteLocalRef(url_string);
                env->DeleteLocalRef(body_bytes);

                if (attached && NULLPTR != vm)
                {
                    vm->DetachCurrentThread();
                }
                return ok;
            }
        }

        bool InitializeProtectBridge(JavaVM* vm, JNIEnv* env) noexcept
        {
            if (NULLPTR == vm || NULLPTR == env)
            {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEnvironmentInvalid);
                return false;
            }

            ProtectBridgeState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            state.vm = vm;
            bool ok = CacheProtectClassLocked(state, env);
            if (ok)
            {
                ppp::telemetry::SetHttpPostSink(&TelemetryHttpPost, NULLPTR);
            }
            return ok;
        }

        void ShutdownProtectBridge(JNIEnv* env) noexcept
        {
            ProtectBridgeState& state = GetState();
            JavaVM* vm = NULLPTR;
            jclass clazz = NULLPTR;
            bool attached = false;

            {
                std::lock_guard<std::mutex> scope(state.syncobj);
                vm = state.vm;
                clazz = state.clazz;
                state.clazz = NULLPTR;
                state.protect = NULLPTR;
                state.otel_post = NULLPTR;
                state.vm = NULLPTR;
                state.enabled = false;
            }

            ppp::telemetry::SetHttpPostSink(NULLPTR, NULLPTR);

            if (NULLPTR == clazz)
            {
                return;
            }

            JNIEnv* release_env = env;
            if (NULLPTR == release_env)
            {
                release_env = AttachEnvironment(vm, attached);
            }

            if (NULLPTR != release_env)
            {
                release_env->DeleteGlobalRef(clazz);
            }

            if (attached && NULLPTR != vm)
            {
                vm->DetachCurrentThread();
            }
        }

        void SetProtectEnabled(bool value) noexcept
        {
            ProtectBridgeState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            state.enabled = value;
        }

        bool IsProtectEnabled() noexcept
        {
            ProtectBridgeState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            return state.enabled;
        }

        bool ProtectSocketFd(int fd) noexcept
        {
            if (fd < 0)
            {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtectorNetworkProtectInvalidSocket);
                return false;
            }

            JavaVM* vm = NULLPTR;
            jclass clazz = NULLPTR;
            jmethodID protect_method = NULLPTR;
            {
                ProtectBridgeState& state = GetState();
                std::lock_guard<std::mutex> scope(state.syncobj);
                if (!state.enabled)
                {
                    return false;
                }

                vm = state.vm;
                clazz = state.clazz;
                protect_method = state.protect;
            }

            bool attached = false;
            JNIEnv* env = AttachEnvironment(vm, attached);
            if (NULLPTR == env)
            {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEnvironmentInvalid);
                return false;
            }

            if (NULLPTR == clazz || NULLPTR == protect_method)
            {
                ProtectBridgeState& state = GetState();
                std::lock_guard<std::mutex> scope(state.syncobj);
                if (!CacheProtectClassLocked(state, env))
                {
                    if (attached)
                    {
                        vm->DetachCurrentThread();
                    }
                    return false;
                }

                clazz = state.clazz;
                protect_method = state.protect;
            }

            jboolean result = env->CallStaticBooleanMethod(clazz, protect_method, static_cast<jint>(fd));
            if (ClearException(env, "CallStaticBooleanMethod(protect)"))
            {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEventDispatchFailed);
                result = JNI_FALSE;
            }

            if (attached)
            {
                vm->DetachCurrentThread();
            }

            return result == JNI_TRUE;
        }
    }
}
#endif
