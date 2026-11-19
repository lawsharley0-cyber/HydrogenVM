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

} // namespace Hydrogen::Runtime::Android::Dex
