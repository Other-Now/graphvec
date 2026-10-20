// checker: linearizability check of a chaos_client history with Porcupine.
//
// Each key is an independent register (PUT writes, GET reads; -1 = absent),
// checked separately. Outcomes:
//   ok   - the op happened, with the recorded output
//   fail - the op definitely did not happen (NOTLEADER): dropped
//   info - unknown (timeout, lost connection): a PUT may or may not have
//          taken effect at any point after its call, so it is kept with a
//          return time of +infinity; an unknown GET carries no information
//          and is dropped
//
// usage: checker history.jsonl [out.html]
// Prints a JSON summary; if a key is not linearizable and out.html is
// given, writes Porcupine's visualization of that key's history.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"sort"
	"time"

	"github.com/anishathalye/porcupine"
)

type rec struct {
	C    int    `json:"c"`
	S    int    `json:"s"`
	Op   string `json:"op"`
	K    int64  `json:"k"`
	V    int64  `json:"v"`
	Call int64  `json:"call"`
	Ret  int64  `json:"ret"`
	St   string `json:"st"`
}

type input struct {
	put bool
	v   int64
}

var register = porcupine.Model{
	Init: func() interface{} { return int64(-1) },
	Step: func(state, in, out interface{}) (bool, interface{}) {
		i := in.(input)
		if i.put {
			return true, i.v
		}
		return out.(int64) == state.(int64), state
	},
	DescribeOperation: func(in, out interface{}) string {
		i, ok := in.(input)
		if !ok {
			return "?"
		}
		if i.put {
			return fmt.Sprintf("put(%d)", i.v)
		}
		return fmt.Sprintf("get() -> %d", out.(int64))
	},
}

func main() {
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	byKey := map[int64][]porcupine.Operation{}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	n, pending, dropped := 0, 0, 0
	for sc.Scan() {
		var r rec
		if err := json.Unmarshal(sc.Bytes(), &r); err != nil {
			continue
		}
		n++
		if r.St == "fail" || (r.St == "info" && r.Op == "get") {
			dropped++
			continue
		}
		op := porcupine.Operation{ClientId: r.C, Input: input{r.Op == "put", r.V}, Call: r.Call,
			Output: r.V, Return: r.Ret}
		if r.Op == "put" {
			op.Output = int64(0)
		}
		if r.St == "info" {
			// Outstanding forever; give it its own client id so it does not
			// overlap that client's later operations.
			op.Return = math.MaxInt64
			op.ClientId = 1000000 + pending
			pending++
		}
		byKey[r.K] = append(byKey[r.K], op)
	}
	keys := make([]int64, 0, len(byKey))
	for k := range byKey {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })

	result, illegal, unknown := "ok", []int64{}, 0
	for _, k := range keys {
		res, info := porcupine.CheckOperationsVerbose(register, byKey[k], 60*time.Second)
		switch res {
		case porcupine.Illegal:
			if len(illegal) == 0 && len(os.Args) > 2 {
				porcupine.VisualizePath(register, info, os.Args[2])
			}
			illegal = append(illegal, k)
			result = "illegal"
		case porcupine.Unknown:
			unknown++
			if result == "ok" {
				result = "unknown"
			}
		}
	}
	out, _ := json.Marshal(map[string]interface{}{
		"ops": n, "checked_ops": n - dropped, "pending_puts": pending, "keys": len(keys),
		"result": result, "non_linearizable_keys": illegal, "timed_out_keys": unknown,
	})
	fmt.Println(string(out))
	if result == "illegal" {
		os.Exit(1)
	}
}
