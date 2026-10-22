namespace bench.nbody

import std
import std.io

struct Body {
    X    float
    Y    float
    Z    float
    VX   float
    VY   float
    VZ   float
    Mass float

    func Energy() float {
        return 0.5 * this.Mass * (this.VX * this.VX + this.VY * this.VY + this.VZ * this.VZ)
    }
}

func Advance(bodies []Body, dt float) {
    var n = len(bodies)
    for i in 0..n {
        for j in i + 1..n {
            var dx = bodies[i].X - bodies[j].X
            var dy = bodies[i].Y - bodies[j].Y
            var dz = bodies[i].Z - bodies[j].Z
            var d2 = dx * dx + dy * dy + dz * dz + 0.01
            var mag = dt / (d2 * math.Sqrt(d2))
            bodies[i].VX -= dx * bodies[j].Mass * mag
            bodies[i].VY -= dy * bodies[j].Mass * mag
            bodies[i].VZ -= dz * bodies[j].Mass * mag
            bodies[j].VX += dx * bodies[i].Mass * mag
            bodies[j].VY += dy * bodies[i].Mass * mag
            bodies[j].VZ += dz * bodies[i].Mass * mag
        }
    }
    for i in 0..n {
        bodies[i].X += dt * bodies[i].VX
        bodies[i].Y += dt * bodies[i].VY
        bodies[i].Z += dt * bodies[i].VZ
    }
}

func main() {
    var bodies []Body = []
    for i in 0..5 {
        var f = float(i)
        bodies = append(bodies, Body{
            X: f,
            Y: f * 0.5,
            Z: f * 0.25,
            VX: 0.1 * f,
            VY: 0.2,
            VZ: 0.3,
            Mass: 1.0 + f,
        })
    }
    for _ in 0..5000000 {
        Advance(bodies, 0.001)
    }
    var e = 0.0
    for b in bodies {
        e += b.Energy()
    }
    println(math.Floor(e * 1000.0))
}
