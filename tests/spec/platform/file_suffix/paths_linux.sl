// test: files named *_linux.sl are compiled only on linux and *_windows.sl only on windows
// status: pending
namespace spec.platform.file_suffix

func PlatformName() string {
    return "linux"
}

func Separator() string {
    return "/"
}
