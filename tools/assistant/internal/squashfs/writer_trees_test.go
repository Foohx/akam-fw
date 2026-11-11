package squashfs

import (
	"fmt"
	"math/rand"
	"strings"
)

// Synthetic trees for the writer tests (nothing here comes from a camera or from the host).

type treeCase struct {
	name string
	root *Node
}

func dirN(name string, mode uint16, kids ...*Node) *Node {
	return NewDir(name, mode).Add(kids...)
}

func fileN(name string, mode uint16, data []byte) *Node { return NewFile(name, mode, data) }

// mixedBytes returns n bytes: random for the first third, text then zeros, to give xz something to do and
// the sparse detection a partial block.
func mixedBytes(seed int64, n int) []byte {
	b := make([]byte, n)
	copy(b, pseudoRandom(seed, n/3))
	copy(b[n/3:], textBytes(n/3))
	if n > 0 && b[0] == 0 {
		b[0] = 1 // never all zero: see zeroSmallTree
	}
	return b
}

func tinyTree() *Node {
	return dirN("", 0o755,
		fileN("a", 0o644, []byte("hello")),
		fileN("empty", 0o644, nil),
		NewSymlink("l", "a"),
	)
}

func edgeSizesTree() *Node {
	const bs = 131072
	root := dirN("", 0o755)
	sizes := []int{0, 1, 2, 4095, 4096, 4097, 8191, 8192, 8193, bs - 1, bs, bs + 1, 2 * bs, 2*bs + 1, 3*bs - 1, 3*bs + 777}
	for i, n := range sizes {
		root.Add(fileN(fmt.Sprintf("rand_%02d_%d", i, n), 0o644, pseudoRandom(int64(i), n)))
		root.Add(fileN(fmt.Sprintf("text_%02d_%d", i, n), 0o644, textBytes(n)))
		if n == 0 || n >= bs {
			root.Add(fileN(fmt.Sprintf("zero_%02d_%d", i, n), 0o640, make([]byte, n)))
		}
		root.Add(fileN(fmt.Sprintf("mixed_%02d_%d", i, n), 0o600, mixedBytes(int64(100+i), n)))
	}
	// a hole in the middle and a zero tail
	holey := append(append(append(pseudoRandom(7, 5000), make([]byte, 2*bs)...), textBytes(3000)...), make([]byte, bs+10)...)
	root.Add(fileN("holey", 0o644, holey))
	return root
}

// zeroSmallTree has all-zero files smaller than a block. They are stored as one sparse block and no data;
// their start_block is meaningless for readers. mksquashfs sometimes gives them the start of an earlier
// sparse file instead of the current data position (when fragments are in flight; the rule is not clear),
// so these trees are left out of the byte for byte comparison.
func zeroSmallTree() *Node {
	root := dirN("", 0o755)
	for i, n := range []int{1, 2, 100, 4095, 4096, 4097, 8192, 70000, 131071} {
		root.Add(fileN(fmt.Sprintf("z%02d_%d", i, n), 0o644, make([]byte, n)))
		root.Add(fileN(fmt.Sprintf("z%02d_%d_copy", i, n), 0o644, make([]byte, n)))
		root.Add(fileN(fmt.Sprintf("s%02d", i), 0o644, textBytes(100+i)))
	}
	root.Add(fileN("big", 0o644, pseudoRandom(3, 200000)))
	return root
}

func manyFragmentsTree() *Node {
	rng := rand.New(rand.NewSource(42))
	root := dirN("", 0o755)
	for i := 0; i < 400; i++ {
		n := rng.Intn(6000)
		var data []byte
		if i%3 == 0 {
			data = pseudoRandom(int64(i), n)
		} else {
			data = textBytes(n)
		}
		root.Add(fileN(fmt.Sprintf("f%04d", i), 0o644, data))
	}
	// big incompressible tails that cannot share a fragment
	for i := 0; i < 5; i++ {
		root.Add(fileN(fmt.Sprintf("g%d", i), 0o644, pseudoRandom(int64(1000+i), 90000)))
	}
	return root
}

func duplicatesTree() *Node {
	big := pseudoRandom(5, 300000)
	small := pseudoRandom(6, 700)
	root := dirN("", 0o755,
		fileN("a_big", 0o644, big),
		fileN("b_small", 0o644, small),
		fileN("c_other", 0o644, pseudoRandom(8, 120000)),
		fileN("d_big_copy", 0o644, big),
		fileN("e_small_copy", 0o644, small),
		fileN("h_zero_big_a", 0o644, make([]byte, 400000)),
		fileN("i_zero_big_b", 0o644, make([]byte, 400000)),
		fileN("j_empty_a", 0o644, nil),
		fileN("k_empty_b", 0o644, nil),
		dirN("sub", 0o755,
			fileN("x", 0o644, big),
			fileN("y", 0o644, small),
			fileN("z", 0o644, pseudoRandom(9, 130000)),
		),
	)
	// small files that fill a fragment, so that a copy comes after the flush
	for i := 0; i < 8; i++ {
		root.Add(fileN(fmt.Sprintf("m%d", i), 0o644, pseudoRandom(int64(50+i), 40000)))
	}
	root.Add(fileN("n_copy_of_b", 0o644, small))
	root.Add(fileN("o_copy_of_m1", 0o644, pseudoRandom(51, 40000)))
	return root
}

func bigDirTree(entries int, nameLen int) *Node {
	d := dirN("many", 0o755)
	for i := 0; i < entries; i++ {
		name := fmt.Sprintf("%05d_", i)
		for len(name) < nameLen {
			name += "n"
		}
		d.Add(fileN(name, 0o644, []byte(fmt.Sprintf("file %d\n", i))))
	}
	return dirN("", 0o755, d, fileN("zz", 0o644, []byte("after")))
}

func longNamesTree(maxName int) *Node {
	root := dirN("", 0o755)
	for i, n := range []int{1, 2, 100, 254, 255, 256} {
		if n > maxName {
			continue
		}
		name := strings.Repeat(string(rune('a'+i)), n)
		root.Add(fileN(name, 0o644, []byte("x")))
		root.Add(dirN("D"+name[:min(len(name), 250)], 0o755, fileN(name, 0o644, nil)))
	}
	root.Add(NewSymlink("long_target", strings.Repeat("d/", 2000)))
	return root
}

func deepTree() *Node {
	root := dirN("", 0o755)
	cur := root
	for i := 0; i < 100; i++ {
		d := dirN(fmt.Sprintf("level%03d", i), 0o755, fileN("f", 0o644, textBytes(i*30)))
		cur.Add(d)
		cur = d
	}
	cur.Add(fileN("leaf", 0o600, []byte("bottom")))
	return root
}

func specialsTree() *Node {
	return dirN("", 0o755,
		dirN("dev", 0o755,
			NewDevice("null", TypeChar, 0o666, 1, 3),
			NewDevice("sda", TypeBlock, 0o660, 8, 0),
			NewDevice("big", TypeChar, 0o600, 259, 70),
			NewDevice("huge", TypeBlock, 0o600, 4095, 1048575),
		),
		&Node{Name: "pipe", Type: TypeFifo, Mode: 0o640},
		&Node{Name: "sock", Type: TypeSocket, Mode: 0o600},
		fileN("suid", 0o4755, textBytes(3000)),
		fileN("sgid", 0o2755, textBytes(10)),
		dirN("sticky", 0o1777),
		dirN("empty", 0o700),
		NewSymlink("abs", "/etc/passwd"),
		NewSymlink("dangling", "nowhere"),
	)
}

// randomTree is a seeded tree of mixed content, with enough names to need several directory headers.
func randomTree(seed int64) *Node {
	rng := rand.New(rand.NewSource(seed))
	budget := 3 << 20 // bytes of content in the whole tree
	entries := 3000   // entries left
	var gen func(depth int) *Node
	gen = func(depth int) *Node {
		d := dirN("", uint16([]int{0o755, 0o700, 0o555, 0o1777}[rng.Intn(4)]))
		n := rng.Intn(40)
		if rng.Intn(8) == 0 {
			n = 200 + rng.Intn(400)
		}
		used := map[string]bool{}
		for i := 0; i < n && entries > 0; i++ {
			entries--
			name := fmt.Sprintf("%s%d", strings.Repeat("n", rng.Intn(60)), rng.Intn(100000))
			if used[name] {
				continue
			}
			used[name] = true
			var c *Node
			switch r := rng.Intn(20); {
			case r < 2 && depth < 6 && budget > 0 && entries > 0:
				c = gen(depth + 1)
				c.Name = name
			case r < 12:
				sz := []int{0, 1, 17, 500, 4000, 70000, 131071, 131072, 131073, 300000}[rng.Intn(10)]
				if sz > budget {
					sz = rng.Intn(1000)
				}
				budget -= sz
				var data []byte
				switch rng.Intn(4) {
				case 0:
					data = pseudoRandom(rng.Int63(), sz)
				case 1:
					data = textBytes(sz)
				case 2:
					data = make([]byte, sz)
					if sz > 0 && sz < 131072 {
						data[0] = 1 // an all-zero file smaller than a block: see zeroSmallTree
					}
				default:
					data = mixedBytes(rng.Int63(), sz)
				}
				if rng.Intn(6) == 0 && len(data) > 0 {
					data = textBytes(len(data)) // collisions: same content
				}
				c = fileN(name, uint16([]int{0o644, 0o755, 0o600, 0o4755}[rng.Intn(4)]), data)
			case r < 15:
				c = NewSymlink(name, strings.Repeat("t", 1+rng.Intn(300)))
			case r < 16:
				c = &Node{Name: name, Type: TypeFifo, Mode: 0o644}
			case r < 17:
				c = &Node{Name: name, Type: TypeSocket, Mode: 0o644}
			case r < 18:
				c = NewDevice(name, TypeChar, 0o600, uint32(rng.Intn(300)), uint32(rng.Intn(300)))
			default:
				c = dirN(name, 0o750)
			}
			d.Add(c)
		}
		return d
	}
	return gen(0)
}

func diffTrees() []treeCase {
	cases := []treeCase{
		{"empty-root", dirN("", 0o755)},
		{"only-dirs", dirN("", 0o755, dirN("a", 0o755, dirN("b", 0o700)), dirN("c", 0o755))},
		{"tiny", tinyTree()},
		{"edge-sizes", edgeSizesTree()},
		{"many-fragments", manyFragmentsTree()},
		{"duplicates", duplicatesTree()},
		{"dir-100", bigDirTree(100, 0)},
		{"dir-256", bigDirTree(256, 0)},
		{"dir-257", bigDirTree(257, 0)},
		{"dir-700-long-names", bigDirTree(700, 40)},
		{"dir-3000-long-names", bigDirTree(3000, 40)},
		{"long-names", longNamesTree(255)},
		{"deep", deepTree()},
		{"specials", specialsTree()},
	}
	for seed := int64(1); seed <= 12; seed++ {
		cases = append(cases, treeCase{fmt.Sprintf("random-%d", seed), randomTree(seed)})
	}
	return cases
}
