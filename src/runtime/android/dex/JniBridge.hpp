#pragma once
#include "runtime/android/Jni.hpp"

namespace Hydrogen::Runtime::Android::Dex {

bool invokeFromJni(Jni& jni, Ctx& context, const Jni::MethodInfo& method, jobject receiver,
                   const std::vector<JValue>& args, const std::string& receiverClass,
                   bool nonvirtual, JValue& result);
uint32_t& pendingException(Process& process);

} // namespace Hydrogen::Runtime::Android::Dex
