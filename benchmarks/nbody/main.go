package main

import (
	"fmt"
	"math"
)

type Body struct {
	X, Y, Z, VX, VY, VZ, Mass float64
}

func (b *Body) Energy() float64 {
	return 0.5 * b.Mass * (b.VX*b.VX + b.VY*b.VY + b.VZ*b.VZ)
}

func advance(bodies []Body, dt float64) {
	n := len(bodies)
	for i := 0; i < n; i++ {
		for j := i + 1; j < n; j++ {
			dx := bodies[i].X - bodies[j].X
			dy := bodies[i].Y - bodies[j].Y
			dz := bodies[i].Z - bodies[j].Z
			d2 := dx*dx + dy*dy + dz*dz + 0.01
			mag := dt / (d2 * math.Sqrt(d2))
			bodies[i].VX -= dx * bodies[j].Mass * mag
			bodies[i].VY -= dy * bodies[j].Mass * mag
			bodies[i].VZ -= dz * bodies[j].Mass * mag
			bodies[j].VX += dx * bodies[i].Mass * mag
			bodies[j].VY += dy * bodies[i].Mass * mag
			bodies[j].VZ += dz * bodies[i].Mass * mag
		}
	}
	for i := 0; i < n; i++ {
		bodies[i].X += dt * bodies[i].VX
		bodies[i].Y += dt * bodies[i].VY
		bodies[i].Z += dt * bodies[i].VZ
	}
}

func main() {
	bodies := make([]Body, 0)
	for i := 0; i < 5; i++ {
		f := float64(i)
		bodies = append(bodies, Body{X: f, Y: f * 0.5, Z: f * 0.25, VX: 0.1 * f, VY: 0.2, VZ: 0.3, Mass: 1.0 + f})
	}
	for step := 0; step < 5000000; step++ {
		advance(bodies, 0.001)
	}
	e := 0.0
	for i := range bodies {
		e += bodies[i].Energy()
	}
	fmt.Println(math.Floor(e * 1000.0))
}
