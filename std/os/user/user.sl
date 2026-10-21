// std.os.user (Go's os/user): accounts from the system databases
// (passwd/group through NSS; runtime/src/std_os_user.c). Ids are strings,
// as in Go, so the API also fits non-numeric platforms.
namespace std.os.user

struct User {
    Uid      string
    Gid      string
    Username string
    // The full name (first GECOS field), may be empty.
    Name    string
    HomeDir string
}

struct Group {
    Gid  string
    Name string
}

func mk(v []string) User {
    return User{Uid: v[0], Gid: v[1], Username: v[2], Name: v[3], HomeDir: v[4]}
}

// The user running this process.
func Current() User! {
    return mk(rtLookup("", rtUid(), true)?)
}

// The user with login name username.
func Lookup(username string) User! {
    return mk(rtLookup(username, 0, false)?)
}

// The user with numeric id uid ("0").
func LookupId(uid string) User! {
    var n = std.strconv.Atoi(uid)?("user: invalid userid " + std.strconv.Quote(uid))
    return mk(rtLookup("", n, true)?)
}

func LookupGroup(name string) Group! {
    var v = rtGroup(name, 0, false)?
    return Group{Gid: v[0], Name: v[1]}
}

func LookupGroupId(gid string) Group! {
    var n = std.strconv.Atoi(gid)?("user: invalid groupid " + std.strconv.Quote(gid))
    var v = rtGroup("", n, true)?
    return Group{Gid: v[0], Name: v[1]}
}
