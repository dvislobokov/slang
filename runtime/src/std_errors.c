/* std_errors.c - std.errors.Wrap / Unwrap / Is / Trace / Frames (slang_errors_*), aliases over
 * error.c. errors.New and errors.As are lowered by the compiler to
 * slang_error_new and slang_error_as. */
#include "rt_internal.h"

slang_error *slang_errors_wrap(const slang_error *err, const slang_string *msg) {
    return slang_error_wrap((slang_error *)err, msg);
}

slang_error *slang_errors_unwrap(const slang_error *err) {
    return slang_error_unwrap(err);
}

bool slang_errors_is(const slang_error *err, const slang_error *target) {
    return slang_error_is(err, target);
}

slang_string *slang_errors_trace(const slang_error *err) {
    return slang_error_trace(err);
}

void slang_errors_frames(const slang_error *err, slang_slice *out) {
    slang_error_frames(err, out);
}
