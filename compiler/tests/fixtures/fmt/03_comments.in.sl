// test: comments survive formatting
// status: passing

namespace app

/* Block comment
   spanning lines. */
func f(x int) int { // trailing after brace
    // leading comment


    var y = x + 1 // trailing comment
    /* own line block */
    if y > 2 {
        // only a comment
    }
    var z = 1 /* inline */ + 2
    return y + z
    // dangling at end of block
}

// end of file comment
