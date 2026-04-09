// 部分预置 libwebrtc.a 中的 sequence_checker_internal.o 在 Release/NDEBUG 下未编译
// ExpectationToString；业务在 RTC_DCHECK_IS_ON 时需本翻译单元提供定义（与官方
// rtc_base/synchronization/sequence_checker_internal.cc 中 #if RTC_DCHECK_IS_ON 块一致）。

#include "rtc_base/synchronization/sequence_checker_internal.h"

#include <string>

#include "api/task_queue/task_queue_base.h"
#include "rtc_base/checks.h"
#include "rtc_base/platform_thread_types.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {
namespace webrtc_sequence_checker_internal {

#if RTC_DCHECK_IS_ON
std::string SequenceCheckerImpl::ExpectationToString() const {
    const TaskQueueBase* const current_queue = TaskQueueBase::Current();
    const webrtc::PlatformThreadRef current_thread = webrtc::CurrentThreadRef();
    MutexLock scoped_lock(&lock_);
    if (!attached_) {
        return "Checker currently not attached.";
    }

    webrtc::StringBuilder message;
    message.AppendFormat(
        "# Expected: TQ: %p Thread: %p\n"
        "# Actual:   TQ: %p Thread: %p\n",
        valid_queue_, reinterpret_cast<const void*>(valid_thread_), current_queue,
        reinterpret_cast<const void*>(current_thread));

    if ((valid_queue_ || current_queue) && valid_queue_ != current_queue) {
        message << "TaskQueue doesn't match\n";
    } else if (!webrtc::IsThreadRefEqual(valid_thread_, current_thread)) {
        message << "Threads don't match\n";
    }

    return message.Release();
}
#endif  // RTC_DCHECK_IS_ON

}  // namespace webrtc_sequence_checker_internal
}  // namespace webrtc
