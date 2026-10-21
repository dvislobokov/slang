// test: an attribute struct from a child namespace is used by its path (meta.Table, no import needed inside the parent); it must be exported, with exported fields
// status: passing
namespace spec.attributes2.other_namespace.meta

import std

@attribute(targets: [reflect.Target.Struct])
struct Table {
    Name string
}

@attribute(targets: [reflect.Target.Field])
struct Column {
    Name     string
    Nullable bool = true
}
