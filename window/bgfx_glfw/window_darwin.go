package bgfx_glfw

import (
	"unsafe"

	"github.com/go-gl/glfw/v3.1/glfw"
	"github.com/gmacd/go-bgfx"
)

func SetWindow(wnd *glfw.Window) {
	nswnd := wnd.GetNSGLContext()
	bgfx.SetCocoaWindow(uintptr(unsafe.Pointer(nswnd)))
}
