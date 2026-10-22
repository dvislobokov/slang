// test: block comments nest and may span lines
// status: passing
namespace spec.basics.nested_block_comments

import std.io

/* outer /* inner */ still a comment
   println("not code") */
func main() {
    var x = 1 /* inline */ + /* another /* nested */ one */ 2
    println(x)
    /*
    println("hidden")
    */
    println("done") // trailing line comment
}
