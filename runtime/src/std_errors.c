/* std_errors.c - std.errors.Wrap / Unwrap / Is / Trace / Frames (zell_errors_*), aliases over
 * error.c. errors.New and errors.As are lowered by the compiler to
 * zell_error_new and zell_error_as. */
#include "rt_internal.h"

zell_error *zell_errors_wrap(const zell_error *err, const zell_string *msg) {
    return zell_error_wrap((zell_error *)err, msg);
}

zell_error *zell_errors_unwrap(const zell_error *err) {
    return zell_error_unwrap(err);
}

bool zell_errors_is(const zell_error *err, const zell_error *target) {
    return zell_error_is(err, target);
}

zell_string *zell_errors_trace(const zell_error *err) {
    return zell_error_trace(err);
}

void zell_errors_frames(const zell_error *err, zell_slice *out) {
    zell_error_frames(err, out);
}
