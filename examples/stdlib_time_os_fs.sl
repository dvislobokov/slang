namespace examples.stdlib_time_os_fs

import std
import std.io

// 2023-11-14 22:13:20.123 UTC (a Tuesday), as Unix milliseconds.
const Launch = 1700000000123

func Times() void! {
    println("format:", time.Format(Launch, "%Y-%m-%d %H:%M:%S.%L"))
    println("long:", time.Format(Launch, "%A, %d %B %Y"))
    println("date:", time.Year(Launch), time.Month(Launch), time.Day(Launch))
    println("clock:", time.Hour(Launch), time.Minute(Launch), time.Second(Launch))
    println("weekday:", time.Weekday(Launch), "epoch weekday:", time.Weekday(0))
    println("before epoch:", time.Format(-1, "%Y-%m-%d %H:%M:%S.%L"))

    var leap = time.Parse("2024-02-29 12:30:00", "%Y-%m-%d %H:%M:%S")?
    println("parsed:", leap, time.Format(leap, "%d.%m.%Y %H:%M"))
    var zoned = time.Parse("2023-11-14T23:13:20+0100", "%Y-%m-%dT%H:%M:%S%z")?
    println("offset applied:", zoned == Launch - 123)
    var bad = try time.Parse("14/11/2023", "%Y-%m-%d") else -1
    println("bad parse:", bad)

    // Wall-clock values are never printed, only checked.
    var start = time.Now()
    time.Sleep(5)
    println("slept >= 5ms:", time.Since(start) >= 5, "clock after launch:", start > Launch)
    println("nanos agree:", time.NowNanos() / 1000000 >= start)
}

func Env() void! {
    println("has args:", len(os.Args()) >= 1)
    println("unset: [" + os.Getenv("SLANG_EXAMPLE_SURELY_UNSET") + "]")
    os.Setenv("SLANG_EXAMPLE_GREETING", "hello")?
    println("getenv:", os.Getenv("SLANG_EXAMPLE_GREETING"))
    try os.Setenv("", "x") catch err {
        println("setenv with empty name fails")
    }
    var host = os.Hostname()?
    println("pid > 0:", os.Pid() > 0, "hostname set:", len(host) > 0)
}

func Paths() {
    println("join:", fs.Join("a/b", "../c.txt"), fs.Join("/srv/", "/app"), fs.Join("", "x"))
    println("base:", fs.Base("/srv/app/main.sl"), fs.Base("dir/"), fs.Base(""))
    println("dir:", fs.Dir("/srv/app/main.sl"), fs.Dir("main.sl"), fs.Dir("/"))
    println("ext:", fs.Ext("archive.tar.gz"), "[" + fs.Ext("Makefile") + "]")
    var abs = try fs.Abs("x/../y") else ""
    println("abs:", strings.HasPrefix(abs, "/"), strings.HasSuffix(abs, "/y"))
}

// Works inside root using relative paths.
func Files(root string) void! {
    var home = os.Cwd()?
    fs.MkdirAll(fs.Join(root, "data/logs"))?
    os.Chdir(root)?
    var here = os.Cwd()?
    println("in temp dir:", fs.Base(here) == fs.Base(root))

    io.WriteFile("data/notes.txt", "first line\n")?
    fs.AppendFile("data/notes.txt", "second line\n")?
    println("size:", fs.Size("data/notes.txt")?)
    var lines = io.ReadLines("data/notes.txt")?
    println("lines:", len(lines), lines[1])

    io.WriteLines("data/list.txt", ["pear", "apple", "fig"])?
    fs.Copy("data/list.txt", "data/logs/list.bak")?
    var copied = io.ReadLines("data/logs/list.bak")?
    println("copy:", copied)
    fs.Rename("data/notes.txt", "data/notes.md")?
    println("entries:", fs.ReadDir("data")?)
    println("exists:", fs.Exists("data/notes.md"), fs.Exists("data/notes.txt"))
    println("is dir:", fs.IsDir("data/logs"), fs.IsDir("data/list.txt"))
    var modified = fs.ModTime("data/list.txt")?
    println("modified within a minute:", time.Now() - modified < 60000)

    try fs.Remove("data") catch err {
        println("remove of a non-empty dir fails")
    }
    try fs.Mkdir("data") catch err {
        println("mkdir:", err)
    }
    var missing = try fs.Size("data/missing.txt") else -1
    println("missing size:", missing)
    fs.Remove("data/notes.md")?
    println("after remove:", fs.ReadDir("data")?)
    os.Chdir(home)?
}

func main() {
    try Times() catch err {
        println("time error:", err)
    }
    try Env() catch err {
        println("os error:", err)
    }
    Paths()
    var root = fs.Join(fs.TempDir(), fmt.Format("slang-example-fs-{0}", os.Pid()))
    try Files(root) catch err {
        println("fs error:", err)
    }
    try fs.RemoveAll(root) catch err {
        println("cleanup error:", err)
    }
    println("cleaned up:", !fs.Exists(root))
}
