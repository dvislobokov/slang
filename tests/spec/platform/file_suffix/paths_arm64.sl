// test: files named *_linux.sl are compiled only on linux and *_windows.sl only on windows
// status: pending
namespace spec.platform.file_suffix

// Only on arm64: declares nothing that clashes with amd64 builds.
const ArmOnly = 1
