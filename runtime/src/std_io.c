/* std_io.c - io file and stdin functions (zell_io_*), thin wrappers over
 * io.c using the uniform `T!` convention: the value is returned and the error
 * goes to a `zell_error **err` out-parameter (NULL on success; err may be NULL). */
#include "rt_internal.h"

zell_string *zell_io_read_file(const zell_string *path, zell_error **err) {
    return zell_read_file(path, err);
}

void zell_io_write_file(const zell_string *path, const zell_string *data, zell_error **err) {
    zell_error *e = zell_write_file(path, data);
    if (err) *err = e;
    else zell_release(e);
}

zell_string *zell_io_read_line(zell_error **err) {
    return zell_read_line(err);
}
