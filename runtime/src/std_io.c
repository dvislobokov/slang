/* std_io.c - std.io file and stdin functions (slang_io_*), thin wrappers over
 * io.c using the uniform `T!` convention: the value is returned and the error
 * goes to a `slang_error **err` out-parameter (NULL on success; err may be NULL). */
#include "rt_internal.h"

slang_string *slang_io_read_file(const slang_string *path, slang_error **err) {
    return slang_read_file(path, err);
}

void slang_io_write_file(const slang_string *path, const slang_string *data, slang_error **err) {
    slang_error *e = slang_write_file(path, data);
    if (err) *err = e;
    else slang_release(e);
}

slang_string *slang_io_read_line(slang_error **err) {
    return slang_read_line(err);
}
