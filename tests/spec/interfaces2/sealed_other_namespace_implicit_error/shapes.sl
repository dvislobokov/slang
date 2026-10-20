// error: can only be implemented in namespace
// status: pending
namespace spec.interfaces2.sealed_other_namespace_implicit_error.shapes

sealed interface Shape {
    func Area() float
}

struct Circle {
    R float
    func Area() float { return 3.0 * this.R * this.R }
}
