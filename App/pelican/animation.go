package main

import (
	"image"
	"image/color"
	"math"

	"c1device"
	"golang.org/x/image/font"
	"golang.org/x/image/font/basicfont"
	"golang.org/x/image/math/fixed"
)

const frameCount = 12

var ink = color.Gray{Y: 0}
var paper = color.Gray{Y: 255}

type point struct{ x, y float64 }
type drawing struct{ *image.Gray }

func (d drawing) dot(x, y, radius int, c color.Gray) {
	for dy := -radius; dy <= radius; dy++ {
		for dx := -radius; dx <= radius; dx++ {
			if dx*dx+dy*dy <= radius*radius {
				d.SetGray(x+dx, y+dy, c)
			}
		}
	}
}

func (d drawing) line(a, b point, radius int, c color.Gray) {
	n := int(math.Ceil(math.Max(math.Abs(b.x-a.x), math.Abs(b.y-a.y))))
	if n < 1 {
		n = 1
	}
	for i := 0; i <= n; i++ {
		t := float64(i) / float64(n)
		d.dot(int(math.Round(a.x+(b.x-a.x)*t)), int(math.Round(a.y+(b.y-a.y)*t)), radius, c)
	}
}

func (d drawing) path(points []point, radius int, c color.Gray) {
	for i := 1; i < len(points); i++ {
		d.line(points[i-1], points[i], radius, c)
	}
}

func (d drawing) ellipse(cx, cy, rx, ry float64, c color.Gray) {
	for y := int(cy - ry); y <= int(cy+ry); y++ {
		for x := int(cx - rx); x <= int(cx+rx); x++ {
			dx, dy := (float64(x)-cx)/rx, (float64(y)-cy)/ry
			if dx*dx+dy*dy <= 1 {
				d.SetGray(x, y, c)
			}
		}
	}
}

func (d drawing) polygon(p []point, c color.Gray) {
	// Pixel-centre even/odd fill. Clipped to the tiny screen, no large buffers.
	for y := 0; y < c1device.DisplayHeight; y++ {
		for x := 0; x < c1device.DisplayWidth; x++ {
			inside := false
			j := len(p) - 1
			px, py := float64(x)+0.5, float64(y)+0.5
			for i := range p {
				if (p[i].y > py) != (p[j].y > py) && px < (p[j].x-p[i].x)*(py-p[i].y)/(p[j].y-p[i].y)+p[i].x {
					inside = !inside
				}
				j = i
			}
			if inside {
				d.SetGray(x, y, c)
			}
		}
	}
	d.path(append(append([]point{}, p...), p[0]), 0, c)
}

func (d drawing) curve(a, b, c, e point, radius int, shade color.Gray) {
	prev := a
	for i := 1; i <= 64; i++ {
		t := float64(i) / 64
		u := 1 - t
		p := point{u*u*u*a.x + 3*u*u*t*b.x + 3*u*t*t*c.x + t*t*t*e.x, u*u*u*a.y + 3*u*u*t*b.y + 3*u*t*t*c.y + t*t*t*e.y}
		d.line(prev, p, radius, shade)
		prev = p
	}
}

func (d drawing) text(x, y int, s string) {
	f := font.Drawer{Dst: d.Gray, Src: image.NewUniform(ink), Face: basicfont.Face7x13, Dot: fixed.P(x, y)}
	f.DrawString(s)
}

func render(step int) *image.Gray {
	step = ((step % frameCount) + frameCount) % frameCount
	d := drawing{image.NewGray(image.Rect(0, 0, c1device.DisplayWidth, c1device.DisplayHeight))}
	for i := range d.Pix {
		d.Pix[i] = 255
	}
	d.text(10, 14, "PELICAN / BICYCLE")
	d.text(241, 14, "BACK <")
	phase := float64(step) * 2 * math.Pi / frameCount
	bob := math.Sin(phase * 2)

	// Ground moves left while the bird stays centred. A static baseline keeps
	// the composition readable even when the panel skips an intermediate frame.
	d.line(point{18, 137}, point{278, 137}, 0, ink)
	for i := 0; i < 6; i++ {
		x := 20 + float64((i*48-step*4+288)%288)
		d.line(point{x, 142}, point{x + 12, 142}, 0, ink)
	}
	// Wheels, rotating spokes and hubs.
	for _, cx := range []float64{89, 213} {
		d.ellipse(cx, 108, 27, 27, ink)
		d.ellipse(cx, 108, 24, 24, paper)
		for k := 0; k < 6; k++ {
			a := phase + float64(k)*math.Pi/3
			d.line(point{cx, 108}, point{cx + 23*math.Cos(a), 108 + 23*math.Sin(a)}, 0, ink)
		}
		d.ellipse(cx, 108, 3, 3, ink)
	}
	// Bicycle diamond frame, seat, fork, stem and curved handlebars.
	d.path([]point{{89, 108}, {121, 74}, {149, 108}, {89, 108}}, 1, ink)
	d.path([]point{{121, 74}, {196, 74}, {149, 108}}, 1, ink)
	d.path([]point{{213, 108}, {195, 69}, {191, 57}, {208, 57}, {211, 63}}, 1, ink)
	d.line(point{121, 74}, point{117, 66}, 1, ink)
	d.line(point{107, 66}, point{130, 66}, 2, ink)
	d.ellipse(149, 108, 8, 8, ink)
	d.ellipse(149, 108, 5, 5, paper)

	// Two feet stay attached to opposite pedals through the entire cycle.
	for leg := 0; leg < 2; leg++ {
		a := phase + float64(leg)*math.Pi
		foot := point{149 + 12*math.Cos(a), 108 + 12*math.Sin(a)}
		knee := point{143 + 6*math.Sin(a), 78 + bob + 4*math.Cos(a)}
		d.path([]point{{126, 62 + bob}, knee, foot}, 1, ink)
		d.line(point{149, 108}, foot, 1, ink)
		d.polygon([]point{{foot.x - 5, foot.y - 2}, {foot.x + 9, foot.y + 2}, {foot.x - 6, foot.y + 3}}, ink)
	}

	// Tail and white body, outlined in black; long curved neck, enormous bill
	// and throat pouch make this a pelican rather than a generic long-beaked bird.
	d.polygon([]point{{109, 47 + bob}, {84, 38 + bob}, {96, 57 + bob}, {115, 63 + bob}}, ink)
	d.ellipse(128, 51+bob, 31, 18, ink)
	d.ellipse(128, 50+bob, 28, 15, paper)
	d.curve(point{143, 57 + bob}, point{170, 67 + bob}, point{149, 31 + bob}, point{172, 33 + bob}, 7, ink)
	d.curve(point{143, 55 + bob}, point{165, 62 + bob}, point{150, 30 + bob}, point{172, 33 + bob}, 5, paper)
	d.ellipse(176, 32+bob, 13, 10, ink)
	d.ellipse(176, 32+bob, 11, 8, paper)
	// Hanging lower pouch and long, almost horizontal upper bill.
	d.polygon([]point{{183, 35 + bob}, {236, 36 + bob}, {213, 51 + bob}, {198, 53 + bob}, {186, 46 + bob}}, ink)
	d.polygon([]point{{188, 38 + bob}, {230, 38 + bob}, {211, 48 + bob}, {199, 50 + bob}, {190, 44 + bob}}, paper)
	d.polygon([]point{{184, 30 + bob}, {245, 35 + bob}, {236, 39 + bob}, {184, 37 + bob}}, ink)
	d.line(point{188, 33 + bob}, point{232, 35 + bob}, 0, paper)
	d.ellipse(179, 29+bob, 2, 2, ink)
	// Wing reaches the handlebar; feather lines stay within the white body.
	d.curve(point{112, 44 + bob}, point{125, 38 + bob}, point{143, 50 + bob}, point{158, 56 + bob}, 1, ink)
	d.path([]point{{111, 47 + bob}, {129, 57 + bob}, {150, 59 + bob}, {194, 59}}, 1, ink)
	d.path([]point{{113, 51 + bob}, {124, 59 + bob}, {143, 61 + bob}}, 0, ink)
	d.line(point{190, 61}, point{201, 60}, 1, ink)
	return d.Gray
}

func pack(img *image.Gray) c1device.Frame {
	var f c1device.Frame
	for y := 0; y < c1device.DisplayHeight; y++ {
		for x := 0; x < c1device.DisplayWidth; x++ {
			if img.GrayAt(x, y).Y < 128 {
				f[(y/8)*c1device.DisplayWidth+x] |= 0x80 >> uint(y&7)
			}
		}
	}
	return f
}

func animationFrames() [frameCount]c1device.Frame {
	var frames [frameCount]c1device.Frame
	for i := range frames {
		frames[i] = pack(render(i))
	}
	return frames
}
