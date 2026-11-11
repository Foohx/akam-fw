package image

import (
	"fmt"
	"strings"
)

// unifiedDiff is a small `diff -u` (3 lines of context) for the report of the service.sh patch. The files
// are a few hundred lines, so the quadratic LCS is fine.
func unifiedDiff(nameA, nameB, a, b string) string {
	x, y := records([]byte(a)), records([]byte(b))
	n, m := len(x), len(y)
	lcs := make([][]int, n+1)
	for i := range lcs {
		lcs[i] = make([]int, m+1)
	}
	for i := n - 1; i >= 0; i-- {
		for j := m - 1; j >= 0; j-- {
			if x[i] == y[j] {
				lcs[i][j] = lcs[i+1][j+1] + 1
			} else {
				lcs[i][j] = max(lcs[i+1][j], lcs[i][j+1])
			}
		}
	}
	type op struct {
		c    byte // ' ', '-', '+'
		text string
	}
	var ops []op
	i, j := 0, 0
	for i < n || j < m {
		switch {
		case i < n && j < m && x[i] == y[j]:
			ops = append(ops, op{' ', x[i]})
			i++
			j++
		case i < n && (j >= m || lcs[i+1][j] >= lcs[i][j+1]):
			ops = append(ops, op{'-', x[i]})
			i++
		default:
			ops = append(ops, op{'+', y[j]})
			j++
		}
	}
	const ctx = 3
	var out strings.Builder
	fmt.Fprintf(&out, "--- %s\n+++ %s\n", nameA, nameB)
	for k := 0; k < len(ops); {
		if ops[k].c == ' ' {
			k++
			continue
		}
		start := max(k-ctx, 0)
		end := k
		for end < len(ops) {
			if ops[end].c != ' ' {
				end++
				continue
			}
			nxt := end
			for nxt < len(ops) && ops[nxt].c == ' ' {
				nxt++
			}
			if nxt < len(ops) && nxt-end <= 2*ctx {
				end = nxt
				continue
			}
			end = min(end+ctx, len(ops))
			break
		}
		la, lb := 0, 0
		for _, o := range ops[:start] {
			if o.c != '+' {
				la++
			}
			if o.c != '-' {
				lb++
			}
		}
		na, nb := 0, 0
		for _, o := range ops[start:end] {
			if o.c != '+' {
				na++
			}
			if o.c != '-' {
				nb++
			}
		}
		fmt.Fprintf(&out, "@@ -%d,%d +%d,%d @@\n", la+1, na, lb+1, nb)
		for _, o := range ops[start:end] {
			out.WriteString(string(o.c) + o.text + "\n")
		}
		k = end
	}
	return out.String()
}
