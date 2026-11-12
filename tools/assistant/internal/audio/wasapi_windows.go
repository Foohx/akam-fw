//go:build windows && (amd64 || arm64)

package audio

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"runtime"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// WASAPI shared-mode capture of the default microphone, without cgo and without a COM library:
// the few COM interfaces needed are called through their vtables with syscall.SyscallN. The
// stream is asked as 8 kHz mono 16-bit with the Windows sample rate converter
// (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY); if the driver refuses, the mix
// format is used and converted in Go (PCMConverter).
//
// 64-bit only (amd64, arm64): every argument is one 64-bit register, which is how the 64-bit
// hnsBufferDuration of IAudioClient.Initialize is passed. Other Windows builds have no built-in
// microphone (backend_other.go).
//
// Memory rule: a pointer handed to COM is turned into a uintptr only for objects on the heap
// (scratch, the format buffers, the package-level GUIDs), never for stack variables, because the
// Go stack can move between the conversion and the call.

type guid struct {
	D1 uint32
	D2 uint16
	D3 uint16
	D4 [8]byte
}

var (
	clsidMMDeviceEnumerator = guid{0xBCDE0395, 0xE52F, 0x467C, [8]byte{0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}}
	iidIMMDeviceEnumerator  = guid{0xA95664D2, 0x9614, 0x4F35, [8]byte{0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}}
	iidIAudioClient         = guid{0x1CB9AD4C, 0xDBFA, 0x4C32, [8]byte{0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}}
	iidIAudioCaptureClient  = guid{0xC8ADBD64, 0xE71E, 0x48A0, [8]byte{0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}}
)

const wfxMax = 18 + 22 // WAVEFORMATEX plus the EXTENSIBLE tail

const (
	clsctxAll             = 23
	eCapture              = 1
	eConsole              = 0
	eCommunications       = 2
	audclntShareShared    = 0
	flagAutoConvertPCM    = 0x80000000
	flagSrcDefaultQuality = 0x08000000
	bufferFlagSilent      = 0x2
	hnsPerSecond          = 10_000_000
	wavePCM               = 1
	waveFloat             = 3
	waveExtensible        = 0xFFFE
	rpcEChangedMode       = 0x80010106
)

var (
	ole32             = syscall.NewLazyDLL("ole32.dll")
	procCoInit        = ole32.NewProc("CoInitializeEx")
	procCoUninit      = ole32.NewProc("CoUninitialize")
	procCoCreate      = ole32.NewProc("CoCreateInstance")
	procCoTaskMemFree = ole32.NewProc("CoTaskMemFree")
)

// scratch holds the out-parameters of the COM calls. It is allocated on the heap on purpose
// (see newScratch), so its address stays valid for the whole call.
type scratch struct {
	enum, dev, client, capc, pfmt uintptr
	data                          *byte
	n, flags, frames              uint32
	wfx                           []byte // WAVEFORMATEX[EXTENSIBLE] handed to Initialize (heap, see newScratch)
}

var scratchSink atomic.Pointer[scratch]

func newScratch() *scratch {
	s := new(scratch)
	s.wfx = make([]byte, wfxMax)
	scratchSink.Store(s) // makes s escape: it can never live on the (movable) stack
	return s
}

func ptr[T any](p *T) uintptr { return uintptr(unsafe.Pointer(p)) }

// vcall calls method idx of the COM object obj (a pointer to a pointer to a vtable).
func vcall(obj uintptr, idx int, args ...uintptr) uintptr {
	vt := *(*unsafe.Pointer)(*(*unsafe.Pointer)(unsafe.Pointer(&obj)))
	fn := *(*uintptr)(unsafe.Add(vt, idx*int(unsafe.Sizeof(uintptr(0)))))
	var a [12]uintptr // no make/append here: the CI escape check (tests/assistant_test.sh) forbids stack buffers in this file
	a[0] = obj
	n := 1 + copy(a[1:], args)
	r, _, _ := syscall.SyscallN(fn, a[:n]...)
	return r
}

func hrErr(what string, hr uintptr) error {
	return fmt.Errorf("%s failed (HRESULT 0x%08X)", what, uint32(hr))
}

func failed(hr uintptr) bool { return int32(hr) < 0 }

func release(obj uintptr) {
	if obj != 0 {
		vcall(obj, 2)
	}
}

// coInit initialises COM on this (locked) thread. The returned function undoes it, and is a no-op
// when CoInitializeEx did not succeed (RPC_E_CHANGED_MODE: someone else set the apartment, which
// is fine for us but must not be uninitialised by us).
func coInit() (func(), error) {
	hr, _, _ := procCoInit.Call(0, 0) // COINIT_MULTITHREADED
	switch {
	case !failed(hr):
		return func() { procCoUninit.Call() }, nil
	case uint32(hr) == rpcEChangedMode:
		return func() {}, nil
	}
	return func() {}, hrErr("CoInitializeEx", hr)
}

type wasapiBackend struct{}

func defaultBackend() MicBackend { return wasapiBackend{} }

func (wasapiBackend) Name() string { return "Windows WASAPI" }

func (wasapiBackend) Available() bool {
	ok := make(chan bool, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		undo, err := coInit()
		defer undo()
		if err != nil {
			ok <- false
			return
		}
		s := newScratch()
		err = defaultCaptureDevice(s)
		release(s.dev)
		ok <- err == nil
	}()
	return <-ok
}

// defaultCaptureDevice puts the default capture endpoint (console role, else communications) in s.dev.
func defaultCaptureDevice(s *scratch) error {
	hr, _, _ := procCoCreate.Call(ptr(&clsidMMDeviceEnumerator), 0, clsctxAll, ptr(&iidIMMDeviceEnumerator), ptr(&s.enum))
	if failed(hr) {
		return hrErr("CoCreateInstance(MMDeviceEnumerator)", hr)
	}
	defer func() { release(s.enum) }()
	if hr := vcall(s.enum, 4, eCapture, eConsole, ptr(&s.dev)); failed(hr) { // GetDefaultAudioEndpoint
		if hr2 := vcall(s.enum, 4, eCapture, eCommunications, ptr(&s.dev)); failed(hr2) {
			return hrErr("GetDefaultAudioEndpoint (is a microphone plugged in and allowed in Windows privacy settings?)", hr)
		}
	}
	return nil
}

func (b wasapiBackend) Open(ctx context.Context) (io.ReadCloser, error) {
	quit := make(chan struct{})
	started := make(chan error, 1)
	ps := newPipeStream(nil)
	ps.stop = func() { close(quit) }
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		err := capture(ps, quit, started)
		if err != nil {
			select {
			case started <- err: // not started yet
			default:
			}
			ps.Fail(err)
		}
	}()
	if err := <-started; err != nil {
		_ = ps.Close()
		return nil, err
	}
	ps.watchContext(ctx)
	return ps, nil
}

// activateClient creates an IAudioClient on the device into s.client.
func activateClient(s *scratch) error {
	if hr := vcall(s.dev, 3, ptr(&iidIAudioClient), clsctxAll, 0, ptr(&s.client)); failed(hr) { // IMMDevice.Activate
		return hrErr("IMMDevice.Activate", hr)
	}
	return nil
}

// capture runs the whole COM session on one locked OS thread until quit closes or the reader goes.
// Every release is a closure over the scratch fields, so it always acts on the object that is
// current when the function returns (the client is re-created by the fallback below).
func capture(ps *pipeStream, quit <-chan struct{}, started chan<- error) error {
	undo, err := coInit()
	defer undo()
	if err != nil {
		return err
	}
	s := newScratch()
	if err := defaultCaptureDevice(s); err != nil {
		return err
	}
	defer func() { release(s.dev) }()
	if err := activateClient(s); err != nil {
		return err
	}
	defer func() { release(s.client) }()

	// Preferred: ask 8 kHz mono 16-bit and let Windows convert.
	fillWaveFormat(s.wfx, wavePCM, 1, Rate, 16)
	conv := NewPCMConverter(Rate, 1, 16, false)
	hr := vcall(s.client, 3, audclntShareShared, flagAutoConvertPCM|flagSrcDefaultQuality, hnsPerSecond, 0, ptr(&s.wfx[0]), 0) // Initialize
	if failed(hr) {
		// Fallback: the mix format of the device, converted here. An IAudioClient cannot be
		// initialised twice, so a new one replaces the first (which is released now).
		release(s.client)
		s.client = 0
		if err := activateClient(s); err != nil {
			return err
		}
		if hr := vcall(s.client, 8, ptr(&s.pfmt)); failed(hr) { // GetMixFormat
			return hrErr("GetMixFormat", hr)
		}
		if s.pfmt == 0 {
			return errors.New("GetMixFormat returned no format (the audio driver gave an empty microphone format)")
		}
		base := *(*unsafe.Pointer)(unsafe.Pointer(&s.pfmt))
		hdr := unsafe.Slice((*byte)(base), 18)
		// cbSize is clamped: 22 for WAVE_FORMAT_EXTENSIBLE, else 0 (a driver value is not trusted).
		cb := 0
		if binary.LittleEndian.Uint16(hdr[0:2]) == waveExtensible && binary.LittleEndian.Uint16(hdr[16:18]) >= 22 {
			cb = 22
		}
		n := copy(s.wfx, unsafe.Slice((*byte)(base), 18+cb))
		for i := n; i < len(s.wfx); i++ {
			s.wfx[i] = 0
		}
		binary.LittleEndian.PutUint16(s.wfx[16:], uint16(cb))
		procCoTaskMemFree.Call(s.pfmt)
		s.pfmt = 0
		c, ferr := converterFor(s.wfx[:18+cb])
		if ferr != nil {
			return ferr
		}
		conv = c
		hr = vcall(s.client, 3, audclntShareShared, 0, hnsPerSecond, 0, ptr(&s.wfx[0]), 0)
		if failed(hr) {
			return hrErr("IAudioClient.Initialize", hr)
		}
	}
	if hr := vcall(s.client, 14, ptr(&iidIAudioCaptureClient), ptr(&s.capc)); failed(hr) { // GetService
		return hrErr("GetService(IAudioCaptureClient)", hr)
	}
	defer func() { release(s.capc) }()
	if hr := vcall(s.client, 10); failed(hr) { // Start
		return hrErr("IAudioClient.Start", hr)
	}
	defer func() { vcall(s.client, 11) }() // Stop, registered only once the final client runs
	started <- nil

	var out []int16
	tick := time.NewTicker(10 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-quit:
			return nil
		case <-tick.C:
		}
		for {
			if hr := vcall(s.capc, 5, ptr(&s.frames)); failed(hr) { // GetNextPacketSize
				return hrErr("GetNextPacketSize", hr)
			}
			if s.frames == 0 {
				break
			}
			if hr := vcall(s.capc, 3, ptr(&s.data), ptr(&s.n), ptr(&s.flags), 0, 0); failed(hr) { // GetBuffer
				return hrErr("GetBuffer", hr)
			}
			if s.n == 0 {
				s.data = nil
				vcall(s.capc, 4, 0) // ReleaseBuffer
				break
			}
			out = out[:0]
			n := s.n
			if n > 0 && s.data != nil {
				buf := unsafe.Slice(s.data, int(n)*conv.BytesPerFrame())
				out = conv.Convert(buf, s.flags&bufferFlagSilent != 0, out)
			}
			vcall(s.capc, 4, uintptr(n)) // ReleaseBuffer
			s.data = nil                 // the buffer is no longer ours
			if len(out) > 0 {
				if err := ps.Push(out); err != nil {
					return nil // the reader closed
				}
			}
		}
	}
}

// fillWaveFormat writes a WAVEFORMATEX (18 bytes, packed) into b, which must be heap memory (s.wfx).
func fillWaveFormat(b []byte, tag, channels, rate, bits int) {
	for i := range b {
		b[i] = 0
	}
	block := channels * bits / 8
	binary.LittleEndian.PutUint16(b[0:], uint16(tag))
	binary.LittleEndian.PutUint16(b[2:], uint16(channels))
	binary.LittleEndian.PutUint32(b[4:], uint32(rate))
	binary.LittleEndian.PutUint32(b[8:], uint32(rate*block))
	binary.LittleEndian.PutUint16(b[12:], uint16(block))
	binary.LittleEndian.PutUint16(b[14:], uint16(bits))
}

// subFormatIs reports whether sub is the whole SubFormat GUID {tag-0000-0010-8000-00AA00389B71}
// (KSDATAFORMAT_SUBTYPE_PCM for tag 1, IEEE_FLOAT for tag 3).
func subFormatIs(sub []byte, tag uint32) bool {
	want := [16]byte{0, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}
	binary.LittleEndian.PutUint32(want[0:], tag)
	return len(sub) == 16 && string(sub) == string(want[:])
}

// converterFor builds the PCM converter for the mix format (raw WAVEFORMATEX[EXTENSIBLE]).
func converterFor(raw []byte) (*PCMConverter, error) {
	tag := int(binary.LittleEndian.Uint16(raw[0:]))
	ch := int(binary.LittleEndian.Uint16(raw[2:]))
	rate := int(binary.LittleEndian.Uint32(raw[4:]))
	bits := int(binary.LittleEndian.Uint16(raw[14:]))
	cb := int(binary.LittleEndian.Uint16(raw[16:]))
	if tag == waveExtensible && cb >= 22 && len(raw) >= 40 {
		sub := raw[24:40]
		switch {
		case subFormatIs(sub, wavePCM):
			tag = wavePCM
		case subFormatIs(sub, waveFloat):
			tag = waveFloat
		default:
			return nil, errors.New("the microphone format is not supported (unknown SubFormat GUID)")
		}
	}
	switch {
	case tag == waveFloat && bits == 32:
		return NewPCMConverter(rate, ch, bits, true), nil
	case tag == wavePCM && (bits == 16 || bits == 24 || bits == 32):
		return NewPCMConverter(rate, ch, bits, false), nil
	}
	return nil, errors.New("the microphone format is not supported (not PCM 16/24/32 bit or float)")
}
