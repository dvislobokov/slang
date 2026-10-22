// The multi-flavour lld driver shipped in the Zell toolchain as bin/lld
// (docs/PLATFORMS.md, "Дистрибутив"; scripts/build-lld.sh builds it).
// It is lld's own tools/lld/lld.cpp reduced to the ELF, COFF, MinGW and
// Mach-O drivers: the flavour comes from argv[0] (ld.lld, ld64.lld,
// lld-link) or from `-flavor gnu|darwin|link` as the first argument.
#include "lld/Common/Driver.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(mingw)
LLD_HAS_DRIVER(macho)

int main(int argc, const char **argv) {
  llvm::InitLLVM x(argc, argv);
  std::vector<const char *> args(argv, argv + argc);
  const lld::DriverDef drivers[] = {
      {lld::WinLink, &lld::coff::link},
      {lld::Gnu, &lld::elf::link},
      {lld::MinGW, &lld::mingw::link},
      {lld::Darwin, &lld::macho::link},
  };
  lld::Result r = lld::lldMain(args, llvm::outs(), llvm::errs(), drivers);
  return r.retCode;
}

// lld-link merges Windows manifests (/manifest:embed with /manifestinput)
// through libxml2. The toolchain must not depend on libxml2 (it is not on
// a clean system), so this binary carries LLVM's own build of
// WindowsManifestMerger without libxml2 (LLVM_ENABLE_LIBXML2=OFF):
// merging reports an error, everything else links as usual. Defining it
// here keeps the linker from pulling libLLVMWindowsManifest.a's member.
#include "llvm/WindowsManifest/WindowsManifestMerger.h"
#include "llvm/Support/MemoryBuffer.h"

namespace llvm {
namespace windows_manifest {
char WindowsManifestError::ID = 0;
WindowsManifestError::WindowsManifestError(const Twine &Msg) : Msg(Msg.str()) {}
void WindowsManifestError::log(raw_ostream &OS) const { OS << Msg; }

class WindowsManifestMerger::WindowsManifestMergerImpl {};

WindowsManifestMerger::WindowsManifestMerger() : Impl(std::make_unique<WindowsManifestMergerImpl>()) {}
WindowsManifestMerger::~WindowsManifestMerger() = default;
Error WindowsManifestMerger::merge(MemoryBufferRef) {
  return make_error<WindowsManifestError>("no libxml2: merging Windows manifests is not supported by this lld");
}
std::unique_ptr<MemoryBuffer> WindowsManifestMerger::getMergedManifest() { return nullptr; }
bool isAvailable() { return false; }
} // namespace windows_manifest
} // namespace llvm

// lld initialises every LLVM target for LTO (InitializeAllTargets). Zell
// links only x86-64 and AArch64 code, so the other backends compiled into
// the distribution's static LLVM are replaced by empty initialisers here
// (scripts/build-lld.sh passes the list as ZELL_LLD_STUB_TARGETS): their
// archive members are never pulled in, which keeps bin/lld small. LTO of
// bitcode for those targets fails with "no registered target".
#ifdef ZELL_LLD_STUB_TARGETS
#define ZELL_STUB_TARGET(T)                                                    \
  extern "C" void LLVMInitialize##T##TargetInfo() {}                           \
  extern "C" void LLVMInitialize##T##Target() {}                               \
  extern "C" void LLVMInitialize##T##TargetMC() {}                             \
  extern "C" void LLVMInitialize##T##AsmPrinter() {}                           \
  extern "C" void LLVMInitialize##T##AsmParser() {}                            \
  extern "C" void LLVMInitialize##T##Disassembler() {}                         \
  extern "C" void LLVMInitialize##T##TargetMCA() {}
ZELL_LLD_STUB_TARGETS
#endif
