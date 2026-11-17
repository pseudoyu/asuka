//go:build e2e

package e2e

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
)

// Plain removes the platform's field decoration, but keeps an app's own
// background, border and corners, regardless of the order of its props.
func TestFieldStyleAndroid(t *testing.T) {
	s := androidApp(t, "e2e/testdata/field-style", "dev.asuka.fieldstyle")
	waitFor(t, s, []string{"Field styling"})
	if out, err := s.adb("shell", "uiautomator", "dump", "/sdcard/asuka-ui.xml"); err != nil {
		t.Fatalf("field bounds: %v: %s", err, out)
	}
	out, err := s.adb("exec-out", "cat", "/sdcard/asuka-ui.xml")
	if err != nil {
		t.Fatal(err)
	}
	matches := boundsRE.FindAllStringSubmatch(out, -1)
	if len(matches) != 5 {
		t.Fatalf("got %d fields, want 5: %s", len(matches), out)
	}
	var fields []image.Rectangle
	for _, m := range matches {
		n := func(i int) int { v, _ := strconv.Atoi(m[i]); return v }
		fields = append(fields, image.Rect(n(1), n(2), n(3), n(4)))
	}
	shot := func(name string) image.Image {
		t.Helper()
		b, err := exec.Command(adb(), "-s", s.serial, "exec-out", "screencap", "-p").Output()
		if err != nil {
			t.Fatal(err)
		}
		if dir := os.Getenv("ASUKA_CONFORMANCE_OUT"); dir != "" {
			if err := os.MkdirAll(dir, 0o755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(dir, "android-field-"+name+".png"), b, 0o644); err != nil {
				t.Fatal(err)
			}
		}
		img, err := png.Decode(bytes.NewReader(b))
		if err != nil {
			t.Fatal(err)
		}
		return img
	}
	white := color.NRGBA{255, 255, 255, 255}
	page := color.NRGBA{242, 244, 246, 255}
	border := color.NRGBA{204, 216, 224, 255}
	pixel := func(img image.Image, p image.Point, want color.NRGBA) {
		t.Helper()
		got := color.NRGBAModel.Convert(img.At(p.X, p.Y)).(color.NRGBA)
		if got != want {
			t.Errorf("pixel %v = %v, want %v", p, got, want)
		}
	}
	check := func(img image.Image, r image.Rectangle, line color.NRGBA) {
		t.Helper()
		pixel(img, image.Pt(r.Max.X-r.Dx()/10, (r.Min.Y+r.Max.Y)/2), white)
		pixel(img, image.Pt((r.Min.X+r.Max.X)/2, r.Min.Y+1), line)
		pixel(img, r.Min.Add(image.Pt(1, 1)), page) // outside the rounded corner
	}
	img := shot("initial")
	for i, name := range []string{"style before plain", "plain before inline style", "multiline"} {
		t.Log(name)
		check(img, fields[i], border)
		r := fields[i]
		ink := r.Max.X
		for y := r.Min.Y; y < r.Max.Y; y++ {
			for x := r.Min.X; x < ink; x++ {
				c := color.NRGBAModel.Convert(img.At(x, y)).(color.NRGBA)
				if c.R >= 100 && c.R < 200 && c.G >= 100 && c.G < 200 && c.B >= 100 && c.B < 200 {
					ink = x // the placeholder, not the colored border or page
				}
			}
		}
		inset := float64(ink-r.Min.X) * 240 / float64(r.Dx())
		if ink == r.Max.X || inset < 15 || inset > 22 {
			t.Errorf("placeholder starts %.1f points in, want the 14-point padding plus 1.5-point border", inset)
		}
	}
	// Plain with no style still has no fill or underline. The native
	// field beside it still has its own underline (a positive control).
	r := fields[3]
	pixel(img, image.Pt((r.Min.X+r.Max.X)/2, r.Max.Y-2), page)
	r = fields[4]
	nativeLine := false
	for y := r.Min.Y + r.Dy()*3/4; y < r.Max.Y; y++ {
		if color.NRGBAModel.Convert(img.At((r.Min.X+r.Max.X)/2, y)) != page {
			nativeLine = true
		}
	}
	if !nativeLine {
		t.Error("the unstyled native field lost its underline")
	}
}
