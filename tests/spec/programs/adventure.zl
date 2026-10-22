// test: text-adventure state machine: enum rooms with methods, a command enum parsed from text, nested switches, enum map keys, Set
// status: passing
namespace spec.programs.adventure

import std
import std.collections
import std.io

enum Room {
    Hall
    Library
    Vault
    Garden
    Outside

    func Describe() string {
        switch this {
            case Room.Hall: return "a dusty hall"
            case Room.Library: return "a library full of old books"
            case Room.Vault: return "a cold vault"
            case Room.Garden: return "an overgrown garden"
            default:
                return "the open road"
        }
    }

    func Exits() []string {
        switch this {
            case Room.Hall: return ["north", "east"]
            case Room.Library: return ["south", "down"]
            case Room.Vault: return ["up"]
            case Room.Garden: return ["west", "gate"]
            default:
                var none []string = []
                return none
        }
    }

    func Next(dir string) Room {
        switch this {
            case Room.Hall:
                switch dir {
                    case "north": return Room.Library
                    case "east": return Room.Garden
                }
            case Room.Library:
                switch dir {
                    case "south": return Room.Hall
                    case "down": return Room.Vault
                }
            case Room.Vault:
                if dir == "up" { return Room.Library }
            case Room.Garden:
                switch dir {
                    case "west": return Room.Hall
                    case "gate": return Room.Outside
                }
        }
        return this
    }
}

enum Cmd {
    Go(dir string)
    Take(item string)
    Use(item string)
    Look
    Quit
}

func Parse(line string) Cmd! {
    var w = strings.Fields(line)
    if len(w) == 0 {
        return error("empty command")
    }
    switch w[0] {
        case "go":
            if len(w) == 2 { return Cmd.Go(w[1]) }
        case "take":
            if len(w) == 2 { return Cmd.Take(w[1]) }
        case "use":
            if len(w) == 2 { return Cmd.Use(w[1]) }
        case "look": return Cmd.Look
        case "quit": return Cmd.Quit
    }
    return error("I don't understand: " + line)
}

struct Game {
    room  Room
    items map[Room]string
    bag   Set<string>
    moves int
    done  bool

    func Go(dir string) {
        var next = this.room.Next(dir)
        if next == this.room {
            println(fmt.Format("You can't go {0} from {1}.", dir, this.room))
            return
        }
        if next == Room.Vault && !this.bag.Has("lamp") {
            println("It is too dark to go down without a lamp.")
            return
        }
        if next == Room.Outside && !this.bag.Has("key") {
            println("The gate is locked.")
            return
        }
        this.room = next
        this.moves += 1
        if next == Room.Outside {
            println("You unlock the gate and escape!")
            this.done = true
            return
        }
        println(fmt.Format("You enter the {0}.", next))
    }

    func Take(item string) {
        if this.items[this.room] != item {
            println(fmt.Format("There is no {0} here.", item))
            return
        }
        delete(this.items, this.room)
        this.bag.Add(item)
        println("Taken:", item)
    }

    func Use(item string) {
        if !this.bag.Has(item) {
            println(fmt.Format("You don't have the {0}.", item))
            return
        }
        switch item {
            case "lamp": println("The lamp glows.")
            default:
                println("Nothing to unlock here.")
        }
    }

    func Look() {
        println(
            "You are in",
            this.room.Describe() + ".",
            "Exits:",
            strings.Join(this.room.Exits(), " "),
        )
    }
}

func main() {
    var g = Game{room: Room.Hall, items: map[Room]string{Room.Garden: "lamp", Room.Vault: "key"}}
    var script = [
        "look",
        "go down",
        "dance",
        "go north",
        "go down",
        "go south",
        "go east",
        "take sword",
        "take lamp",
        "use lamp",
        "use key",
        "go gate",
        "go west",
        "go north",
        "go down",
        "look",
        "take key",
        "use key",
        "go up",
        "go south",
        "go east",
        "go gate",
        "look",
    ]
    for line in script {
        if g.done { break }
        println("> " + line)
        var cmd = try Parse(line) catch err {
            println(err)
            continue
        }
        switch cmd {
            case Cmd.Go(d): g.Go(d)
            case Cmd.Take(i): g.Take(i)
            case Cmd.Use(i): g.Use(i)
            case Cmd.Look: g.Look()
            case Cmd.Quit: g.done = true
        }
    }
    println("moves:", g.moves, "bag:", g.bag.Items(), "final room:", g.room)
}
