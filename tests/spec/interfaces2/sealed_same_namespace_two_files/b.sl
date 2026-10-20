// test: implementations of a sealed interface may live in another file of the same namespace
// status: pending
namespace spec.interfaces2.sealed_same_namespace_two_files

struct Cow {
    func Sound() string { return "moo" }
}
