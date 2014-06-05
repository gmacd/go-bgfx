## bgfx

Package bgfx is a wrapper around Branimir Karadžić's rendering library
of the same name.

See <https://github.com/bkaradzic/bgfx>.

`go get github.com/james4k/go-bgfx`

_In progress and likely only works on OS X at this point._ The current
plan is to get as much working as possible, with little concern for
performance or API design. This mostly means implementing a number of
the original bgfx examples and the necessary APIs.

### Examples

[GLFW](http://www.glfw.org/) is required to run the examples.

Installing and running the examples is simple, assuming you have your
$GOPATH/bin setup in $PATH:

```
$ go install github.com/james4k/go-bgfx/examples/...
$ bgfx-01-cubes
```

### A note on the git submodules

The submodules are only used when running the prepare.sh script to
generate go tool compatible code from the bgfx source. A side benefit is
that the package is nice and lean in its default state.
