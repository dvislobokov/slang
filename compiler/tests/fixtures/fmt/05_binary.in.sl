namespace app

func check(a int, b int) bool {
    var x = a+b*2-(a/b)%3
    var ok = a>0&&b>0||!(a==b)
    if firstConditionIsTrue(a) && secondConditionIsAlsoTrue(b) && thirdConditionHoldsAsWell(a, b) {
        return true
    }
    var s = "a long string literal that is concatenated " + toString(a) + " with other parts " + toString(b)
    x += -a
    return ok
}
