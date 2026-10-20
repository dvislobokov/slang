// test: @cfg before the namespace line excludes a whole file
// status: pending
@cfg(not(os: "windows"))
namespace spec.platform.file_suffix_cfg_attr

func Backend() string {
    return "posix"
}
