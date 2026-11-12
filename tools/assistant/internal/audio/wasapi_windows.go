//go:build windows

package audio

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

// WASAPI shared-mode capture of the default microphone, without cgo and without a COM library:
// the few COM interfaces needed are called through their vtables with syscall.SyscallN. The
// stream is asked as 8 kHz mono 16-bit with the Windows sample rate converter
// (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY); if the driver refuses, the mix
// format is used and converted in Go (PCMConverter).

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
)

var (
	ole32             = syscall.NewLazyDLL("ole32.dll")
	procCoInit        = ole32.NewProc("CoInitializeEx")
	procCoUninit      = ole32.NewProc("CoUninitialize")
	procCoCreate      = ole32.NewProc("CoCreateInstance")
	procCoTaskMemFree = ole32.NewProc("CoTaskMemFree")
)

// vcall calls method idx of the COM object obj (a pointer to a pointer to a vtable).
func vcall(obj uintptr, idx int, args ...uintptr) uintptr {
	vt := *(*unsafe.Pointer)(*(*unsafe.Pointer)(unsafe.Pointer(&obj)))
	fn := *(*uintptr)(unsafe.Add(vt, idx*int(unsafe.Sizeof(uintptr(0)))))
	r, _, _ := syscall.SyscallN(fn, append([]uintptr{obj}, args...)...)
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

type wasapiBackend struct{}

func defaultBackend() MicBackend { return wasapiBackend{} }

func (wasapiBackend) Name() string { return "Windows WASAPI" }

func (wasapiBackend) Available() bool {
	ok := make(chan bool, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		hr, _, _ := procCoInit.Call(0, 0) // COINIT_MULTITHREADED
		if failed(hr) && uint32(hr) != 0x80010106 {
			ok <- false
			return
		}
		defer procCoUninit.Call()
		dev, err := defaultCaptureDevice()
		release(dev)
		ok <- err == nil
	}()
	return <-ok
}

func defaultCaptureDevice() (uintptr, error) {
	var enum uintptr
	hr, _, _ := procCoCreate.Call(uintptr(unsafe.Pointer(&clsidMMDeviceEnumerator)), 0, clsctxAll,
		uintptr(unsafe.Pointer(&iidIMMDeviceEnumerator)), uintptr(unsafe.Pointer(&enum)))
	if failed(hr) {
		return 0, hrErr("CoCreateInstance(MMDeviceEnumerator)", hr)
	}
	defer release(enum)
	var dev uintptr
	if hr := vcall(enum, 4, eCapture, eConsole, uintptr(unsafe.Pointer(&dev))); failed(hr) { // GetDefaultAudioEndpoint
		if hr2 := vcall(enum, 4, eCapture, eCommunications, uintptr(unsafe.Pointer(&dev))); failed(hr2) {
			return 0, hrErr("GetDefaultAudioEndpoint (is a microphone plugged in and allowed in Windows privacy settings?)", hr)
		}
	}
	return dev, nil
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

// capture runs the whole COM session on one locked OS thread until quit closes or the reader goes.
func capture(ps *pipeStream, quit <-chan struct{}, started chan<- error) error {
	hr, _, _ := procCoInit.Call(0, 0)
	if failed(hr) && uint32(hr) != 0x80010106 { // RPC_E_CHANGED_MODE: already initialised by someone else, fine
		return hrErr("CoInitializeEx", hr)
	}
	defer procCoUninit.Call()
	dev, err := defaultCaptureDevice()
	if err != nil {
		return err
	}
	defer release(dev)
	var client uintptr
	if hr := vcall(dev, 3, uintptr(unsafe.Pointer(&iidIAudioClient)), clsctxAll, 0, uintptr(unsafe.Pointer(&client))); failed(hr) { // Activate
		return hrErr("IMMDevice.Activate", hr)
	}
	defer release(client)

	// Preferred: ask 8 kHz mono 16-bit and let Windows convert.
	want := waveFormat(wavePCM, 1, Rate, 16)
	conv := NewPCMConverter(Rate, 1, 16, false)
	hr = vcall(client, 3, audclntShareShared, flagAutoConvertPCM|flagSrcDefaultQuality, hnsPerSecond, 0,
		uintptr(unsafe.Pointer(&want[0])), 0) // Initialize
	if failed(hr) {
		// Fallback: the mix format of the device, converted here. The client must be re-created
		// after a failed Initialize.
		release(client)
		client = 0
		if hr := vcall(dev, 3, uintptr(unsafe.Pointer(&iidIAudioClient)), clsctxAll, 0, uintptr(unsafe.Pointer(&client))); failed(hr) {
			return hrErr("IMMDevice.Activate", hr)
		}
		var pfmt uintptr
		if hr := vcall(client, 8, uintptr(unsafe.Pointer(&pfmt))); failed(hr) || pfmt == 0 { // GetMixFormat
			return hrErr("GetMixFormat", hr)
		}
		base := *(*unsafe.Pointer)(unsafe.Pointer(&pfmt))
		raw := unsafe.Slice((*byte)(base), 18)
		raw = unsafe.Slice((*byte)(base), 18+int(binary.LittleEndian.Uint16(raw[16:18])))
		c, ferr := converterFor(raw)
		fmtCopy := append([]byte(nil), raw...)
		if ferr != nil {
			procCoTaskMemFree.Call(pfmt)
			return ferr
		}
		conv = c
		hr = vcall(client, 3, audclntShareShared, 0, hnsPerSecond, 0, uintptr(unsafe.Pointer(&fmtCopy[0])), 0)
		procCoTaskMemFree.Call(pfmt)
		if failed(hr) {
			return hrErr("IAudioClient.Initialize", hr)
		}
	}
	var capc uintptr
	if hr := vcall(client, 14, uintptr(unsafe.Pointer(&iidIAudioCaptureClient)), uintptr(unsafe.Pointer(&capc))); failed(hr) { // GetService
		return hrErr("GetService(IAudioCaptureClient)", hr)
	}
	defer release(capc)
	if hr := vcall(client, 10); failed(hr) { // Start
		return hrErr("IAudioClient.Start", hr)
	}
	defer vcall(client, 11) // Stop
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
			var frames uint32
			if hr := vcall(capc, 5, uintptr(unsafe.Pointer(&frames))); failed(hr) { // GetNextPacketSize
				return hrErr("GetNextPacketSize", hr)
			}
			if frames == 0 {
				break
			}
			var data *byte
			var n, flags uint32
			if hr := vcall(capc, 3, uintptr(unsafe.Pointer(&data)), uintptr(unsafe.Pointer(&n)), uintptr(unsafe.Pointer(&flags)), 0, 0); failed(hr) { // GetBuffer
				return hrErr("GetBuffer", hr)
			}
			out = out[:0]
			if n > 0 && data != nil {
				buf := unsafe.Slice(data, int(n)*conv.BytesPerFrame())
				out = conv.Convert(buf, flags&bufferFlagSilent != 0, out)
			}
			vcall(capc, 4, uintptr(n)) // ReleaseBuffer
			if len(out) > 0 {
				if err := ps.Push(out); err != nil {
					return nil // the reader closed
				}
			}
		}
	}
}

// waveFormat builds a WAVEFORMATEX (18 bytes, packed).
func waveFormat(tag, channels, rate, bits int) []byte {
	b := make([]byte, 18)
	block := channels * bits / 8
	binary.LittleEndian.PutUint16(b[0:], uint16(tag))
	binary.LittleEndian.PutUint16(b[2:], uint16(channels))
	binary.LittleEndian.PutUint32(b[4:], uint32(rate))
	binary.LittleEndian.PutUint32(b[8:], uint32(rate*block))
	binary.LittleEndian.PutUint16(b[12:], uint16(block))
	binary.LittleEndian.PutUint16(b[14:], uint16(bits))
	return b
}

// converterFor builds the PCM converter for the mix format (raw WAVEFORMATEX[EXTENSIBLE]).
func converterFor(raw []byte) (*PCMConverter, error) {
	tag := int(binary.LittleEndian.Uint16(raw[0:]))
	ch := int(binary.LittleEndian.Uint16(raw[2:]))
	rate := int(binary.LittleEndian.Uint32(raw[4:]))
	bits := int(binary.LittleEndian.Uint16(raw[14:]))
	cb := int(binary.LittleEndian.Uint16(raw[16:]))
	if tag == waveExtensible && cb >= 22 && len(raw) >= 40 {
		tag = int(binary.LittleEndian.Uint32(raw[24:])) // first dword of the SubFormat GUID
	}
	switch {
	case tag == waveFloat && bits == 32:
		return NewPCMConverter(rate, ch, bits, true), nil
	case tag == wavePCM && (bits == 16 || bits == 24 || bits == 32):
		return NewPCMConverter(rate, ch, bits, false), nil
	}
	return nil, errors.New("the microphone format is not supported (not PCM 16/24/32 bit or float)")
}
