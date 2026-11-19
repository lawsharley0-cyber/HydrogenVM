#pragma once
#include "runtime/android/Jni.hpp"

namespace Hydrogen::Runtime::Android::Dex {

bool invokeFromJni(Jni& jni, Ctx& context, const Jni::MethodInfo& method, jobject receiver,
                   const std::vector<JValue>& args, const std::string& receiverClass,
                   bool nonvirtual, JValue& result);
// Invoke an interpreted lifecycle method without falling back to JNI stubs.
bool invokeActivity(Jni& jni, Ctx& context, const std::string& type, jobject receiver,
                    const std::string& name, const std::string& signature, const std::vector<JValue>& args);
uint32_t& pendingException(Process& process);
bool instantiateFromJni(Jni& jni, Ctx& context, const std::string& cls, const std::string& ctorSig,
                        const std::vector<JValue>& args, jobject& out);
bool classInfoFromJni(Process& process, const std::string& cls, std::string& superCls, uint32_t& flags);
bool isAssignableFromJni(Process& process, const std::string& fromCls, const std::string& toCls);

} // namespace Hydrogen::Runtime::Android::Dex
