package bgfx_glfw

import (
	"unsafe"

	"github.com/go-gl/glfw/v3.1/glfw"
	"github.com/gmacd/go-bgfx"
)

func SetWindow(wnd *glfw.Window) {
	hwnd := wnd.GetWin32Window()
	bgfx.SetWin32Window(uintptr(unsafe.Pointer(hwnd)))
}
