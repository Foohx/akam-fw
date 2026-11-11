package sdcard

import (
	"bytes"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// A minimal reader of Apple XML property lists (the output of `diskutil ... -plist`). It is deliberately
// small and strict: no DTD, no entity expansion (encoding/xml honours only the five predefined entities and
// refuses any other), and hard caps on the size, the depth, the number of nodes and the length of a string,
// so a hostile or corrupt document cannot exhaust memory or the stack.
const (
	plistMaxBytes  = 4 << 20
	plistMaxDepth  = 32
	plistMaxNodes  = 200000
	plistMaxString = 64 << 10
)

// Dict is a plist <dict>. Values are Dict, []any, string, int64, float64, bool or []byte (<data>, undecoded);
// a <date> is kept as its text.
type Dict map[string]any

type plistParser struct {
	d     *xml.Decoder
	nodes int
}

// ParsePlist parses one property list whose root is a dict.
func ParsePlist(data []byte) (Dict, error) {
	if len(data) > plistMaxBytes {
		return nil, errors.New("plist: document too large")
	}
	p := &plistParser{d: xml.NewDecoder(bytes.NewReader(data))}
	p.d.Strict = true
	p.d.Entity = nil // only the predefined entities
	for {
		t, err := p.d.Token()
		if err != nil {
			return nil, fmt.Errorf("plist: %v", err)
		}
		switch tt := t.(type) {
		case xml.ProcInst, xml.Comment:
		case xml.CharData:
			if len(bytes.TrimSpace(tt)) > 0 {
				return nil, errors.New("plist: text before the root element")
			}
		case xml.Directive:
			if err := checkDoctype(string(tt)); err != nil {
				return nil, err
			}
		case xml.StartElement:
			if tt.Name.Local != "plist" {
				return nil, errors.New("plist: root element is not <plist>")
			}
			return p.root()
		default:
			return nil, errors.New("plist: unexpected content before <plist>")
		}
	}
}

// checkDoctype allows the standard Apple declaration and nothing else: an internal subset ("[") is where
// entity definitions live.
func checkDoctype(s string) error {
	s = strings.TrimSpace(s)
	rest := strings.TrimPrefix(s, "DOCTYPE plist")
	apple := strings.HasPrefix(rest, ` PUBLIC "-//Apple//DTD PLIST 1.0//EN"`)
	if rest == s || (rest != "" && !apple) || strings.ContainsAny(s, "[]") || strings.Contains(s, "ENTITY") {
		return errors.New("plist: unsupported DOCTYPE")
	}
	return nil
}

func (p *plistParser) root() (Dict, error) {
	st, _, err := p.startOrEnd()
	if err != nil {
		return nil, err
	}
	v, err := p.value(st, 1)
	if err != nil {
		return nil, err
	}
	d, ok := v.(Dict)
	if !ok {
		return nil, errors.New("plist: the root is not a dict")
	}
	if err := p.endOf("plist"); err != nil {
		return nil, err
	}
	for {
		t, err := p.d.Token()
		if err == io.EOF {
			return d, nil
		}
		if err != nil {
			return nil, fmt.Errorf("plist: %v", err)
		}
		switch tt := t.(type) {
		case xml.Comment:
		case xml.CharData:
			if len(bytes.TrimSpace(tt)) > 0 {
				return nil, errors.New("plist: text after the document")
			}
		default:
			return nil, errors.New("plist: content after the document")
		}
	}
}

// startOrEnd returns the next start element, or end=true at the end of the enclosing element; whitespace and
// comments are skipped, anything else is refused.
func (p *plistParser) startOrEnd() (st xml.StartElement, end bool, err error) {
	for {
		t, e := p.d.Token()
		if e != nil {
			return st, false, fmt.Errorf("plist: %v", e)
		}
		switch tt := t.(type) {
		case xml.StartElement:
			return tt, false, nil
		case xml.EndElement:
			return st, true, nil
		case xml.Comment:
		case xml.CharData:
			if len(bytes.TrimSpace(tt)) > 0 {
				return st, false, errors.New("plist: unexpected text")
			}
		default:
			return st, false, errors.New("plist: unexpected content")
		}
	}
}

func (p *plistParser) endOf(name string) error {
	_, end, err := p.startOrEnd()
	if err != nil {
		return err
	}
	if !end {
		return fmt.Errorf("plist: unexpected element inside <%s>", name)
	}
	return nil
}

// text reads the character data of a leaf element up to its end tag.
func (p *plistParser) text() (string, error) {
	var sb strings.Builder
	for {
		t, err := p.d.Token()
		if err != nil {
			return "", fmt.Errorf("plist: %v", err)
		}
		switch tt := t.(type) {
		case xml.CharData:
			if sb.Len()+len(tt) > plistMaxString {
				return "", errors.New("plist: string too long")
			}
			sb.Write(tt)
		case xml.Comment:
		case xml.EndElement:
			return sb.String(), nil
		default:
			return "", errors.New("plist: unexpected element in a text value")
		}
	}
}

func (p *plistParser) value(st xml.StartElement, depth int) (any, error) {
	if depth > plistMaxDepth {
		return nil, errors.New("plist: nested too deeply")
	}
	if p.nodes++; p.nodes > plistMaxNodes {
		return nil, errors.New("plist: too many nodes")
	}
	switch st.Name.Local {
	case "dict":
		return p.dict(depth)
	case "array":
		var arr []any
		for {
			s, end, err := p.startOrEnd()
			if err != nil {
				return nil, err
			}
			if end {
				return arr, nil
			}
			v, err := p.value(s, depth+1)
			if err != nil {
				return nil, err
			}
			arr = append(arr, v)
		}
	case "string", "date":
		return p.text()
	case "data":
		s, err := p.text()
		if err != nil {
			return nil, err
		}
		return []byte(strings.Join(strings.Fields(s), "")), nil
	case "integer":
		s, err := p.text()
		if err != nil {
			return nil, err
		}
		n, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
		if err != nil {
			return nil, fmt.Errorf("plist: bad integer %q", s)
		}
		return n, nil
	case "real":
		s, err := p.text()
		if err != nil {
			return nil, err
		}
		f, err := strconv.ParseFloat(strings.TrimSpace(s), 64)
		if err != nil {
			return nil, fmt.Errorf("plist: bad real %q", s)
		}
		return f, nil
	case "true", "false":
		if err := p.endOf(st.Name.Local); err != nil {
			return nil, err
		}
		return st.Name.Local == "true", nil
	}
	return nil, fmt.Errorf("plist: unknown element <%s>", st.Name.Local)
}

func (p *plistParser) dict(depth int) (Dict, error) {
	out := Dict{}
	for {
		s, end, err := p.startOrEnd()
		if err != nil {
			return nil, err
		}
		if end {
			return out, nil
		}
		if s.Name.Local != "key" {
			return nil, errors.New("plist: dict entry without a <key>")
		}
		if p.nodes++; p.nodes > plistMaxNodes {
			return nil, errors.New("plist: too many nodes")
		}
		key, err := p.text()
		if err != nil {
			return nil, err
		}
		vs, vend, err := p.startOrEnd()
		if err != nil {
			return nil, err
		}
		if vend {
			return nil, errors.New("plist: key without a value")
		}
		v, err := p.value(vs, depth+1)
		if err != nil {
			return nil, err
		}
		if _, dup := out[key]; dup {
			return nil, fmt.Errorf("plist: duplicate key %q", key)
		}
		out[key] = v
	}
}

// Typed accessors: ok is false when the key is absent or has another type.

func (d Dict) Str(k string) (string, bool) { s, ok := d[k].(string); return s, ok }
func (d Dict) Bool(k string) (bool, bool)  { b, ok := d[k].(bool); return b, ok }
func (d Dict) Int(k string) (int64, bool)  { n, ok := d[k].(int64); return n, ok }
func (d Dict) Arr(k string) ([]any, bool)  { a, ok := d[k].([]any); return a, ok }
