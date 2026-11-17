package ir

import (
	"bytes"
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// call lowers a call; its result goes to dst (or a temp).
func (b *fb) call(e *syntax.CallExpr, dst int) int {
	return b.call1(e, dst, false)
}

// callDiscard lowers a call whose result isn't used.
func (b *fb) callDiscard(e *syntax.CallExpr) { b.call1(e, -1, true) }

func (b *fb) call1(e *syntax.CallExpr, dst int, discard bool) int {
	if h := b.l.info.Hrefs[e]; h != nil {
		return b.href(h, dst)
	}
	call := b.l.info.Calls[e]
	if call == nil {
		b.l.errorf(e, "internal: no call info")
		return -1
	}
	if call.Optional {
		return b.optionalCall(e, call, dst)
	}
	switch call.Kind {
	case check.CallFunc, check.CallMethod:
		return b.funcCall(e, call, dst, discard)
	case check.CallClosure:
		if id, ok := unparen(call.Recv).(*syntax.Ident); ok {
			if v, ok := b.l.info.Uses[id].(*check.Var); ok && v.Fn != nil {
				return b.localCall(e, call, v.Fn, dst, discard)
			}
		}
		return b.closureCall(e, call, dst)
	case check.CallIface:
		return b.ifaceCall(e, call, nil, dst)
	case check.CallNative:
		return b.nativeMethodCall(e, call, nil, dst)
	case check.CallNativeNew:
		return b.nativeNewCall(call, dst)
	case check.CallBuiltinMethod:
		return b.builtinMethod(e, call, nil, dst)
	case check.CallBuiltin:
		return b.builtinCall(e, call, dst)
	case check.CallStruct, check.CallCase:
		if r, ok := b.hoist(e, dst); ok {
			return r
		}
		if call.Kind == check.CallStruct {
			return b.structCall(call, dst)
		}
		return b.caseCall(call, dst)
	case check.CallComponent:
		return b.copyTo(b.element(e), dst)
	case check.CallStore:
		return b.storeCall(e, call, dst)
	case check.CallTr:
		return b.trCall(e, call, dst)
	}
	b.l.errorf(e, "internal: unknown call")
	return -1
}

// optionalCall is x?.f(…): none when x is.
func (b *fb) optionalCall(e *syntax.CallExpr, call *check.Call, dst int) int {
	res := b.rawType(e)
	x := b.value(call.Recv, -1)
	none, end := b.f.NewLabel(), b.f.NewLabel()
	r := -1
	if res != check.Void {
		r = b.out(dst, bc.KRef)
	}
	b.emitJump(op("JNONE"), x, none)
	in := b.unbox(x, check.Unopt(b.rawType(call.Recv)), -1)
	var v int
	switch call.Kind {
	case check.CallBuiltinMethod:
		v = b.builtinMethod(e, call, &in, -1)
	case check.CallIface:
		v = b.ifaceCall(e, call, &in, -1)
	case check.CallNative:
		v = b.nativeMethodCall(e, call, &in, -1)
	default:
		v = b.funcCallRecv(e, call, in, -1, false)
	}
	if r >= 0 {
		// The method's own result: an optional one (x?.f() where f
		// returns T?) is the chain's as it is, flattened, not wrapped
		// again.
		from := check.Unopt(res)
		if call.Result != nil {
			from = b.subst.Apply(call.Result)
		}
		b.toOptional(v, from, res, r)
	}
	b.jmp(end)
	b.f.Place(none)
	if r >= 0 {
		b.emit(op("KNONE"), r)
	}
	b.f.Place(end)
	return r
}

// args lowers a call's arguments, defaults filled in.
func (b *fb) args(call *check.Call, subst check.Subst) []int {
	var vals []int
	for i, a := range call.Args {
		if a == nil {
			if i < len(call.Params) && call.Params[i].Default != nil {
				saved := b.subst
				b.subst = subst
				vals = append(vals, b.value(call.Params[i].Default, -1))
				b.subst = saved
				continue
			}
			vals = append(vals, b.zeroValue(subst.Apply(call.Params[i].Type)))
			continue
		}
		vals = append(vals, b.value(a, -1))
	}
	return vals
}

// zeroValue is a register holding t's zero value (check.HasZero's), or
// its kind's empty register.
func (b *fb) zeroValue(t check.Type) int {
	switch x := t.(type) {
	case *check.Basic:
		switch x {
		case check.String:
			return b.kstr("")
		case check.Bytes:
			return b.callB("str.to_bytes", bc.KRef, b.kstr(""))
		}
	case *check.Array:
		r := b.temp(bc.KRef)
		sh := b.l.shape(check.Unopt(x.Elem))
		if sh < 0 {
			sh = bc.None
		}
		b.emit(op("NEWA"), r, int(b.l.kind(x.Elem)), sh, b.null(), 0)
		return r
	case *check.Map:
		r := b.temp(bc.KRef)
		b.emit(op("NEWM"), r, int(b.l.kind(x.Key)), int(b.l.kind(x.Elem)), b.null(), 0)
		return r
	case *check.Set:
		return b.callB("set.of", bc.KRef, b.konst(int64(b.l.kind(x.Elem))))
	}
	return b.zero(b.kind(t))
}

// invoke calls function idx with vals (registers) and returns its
// result (of kind k).
func (b *fb) invoke(idx int, vals []int, k bc.Kind, dst int, discard bool) int {
	ks := make([]bc.Kind, len(vals))
	for i, v := range vals {
		ks[i] = b.f.Kinds[v]
	}
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, v)
	}
	r := bc.None
	if k != bc.KVoid && !discard {
		r = b.out(dst, k)
	}
	b.emit(op("CALL"), r, idx, blk, len(vals))
	b.releaseRegs(blk, ks)
	if r == bc.None {
		return -1
	}
	return r
}

func (b *fb) funcCall(e *syntax.CallExpr, call *check.Call, dst int, discard bool) int {
	f := call.Func
	if f.Recv == nil {
		var targs []check.Type
		for _, t := range call.TypeArgs {
			targs = append(targs, b.typ(t))
		}
		idx := b.l.funcIndex(f, targs, nil)
		s := b.calleeSubst(f, targs, nil)
		vals := b.args(call, s)
		return b.invoke(idx, vals, b.l.kind(s.Apply(f.Sig.Result)), dst, discard)
	}
	// A method: its receiver first.
	var recv int
	switch {
	case call.SelfCall && (f.Recv.Kind == check.StoreType || f.Recv.Kind == check.ComponentType):
		recv = b.env(e)
	case call.SelfCall:
		recv = b.readVar(b.ctx.Self, e)
	default:
		recv = b.value(call.Recv, -1)
	}
	return b.funcCallRecv(e, call, recv, dst, discard)
}

func (b *fb) funcCallRecv(e *syntax.CallExpr, call *check.Call, recv int, dst int, discard bool) int {
	f := call.Func
	var rt check.Type
	if f.Recv.Kind == check.StructType || f.Recv.Kind == check.EnumType {
		if call.RecvType != nil {
			rt = b.typ(check.Unopt(call.RecvType))
		} else if b.ctx.Self != nil {
			rt = b.typ(b.ctx.Self.Type)
		}
	} else if f.Recv.Kind == check.StoreType && len(f.Recv.TypeParams) > 0 {
		// A generic store's fn: lowered per instantiation.
		if call.RecvType != nil && !call.SelfCall {
			rt = b.typ(check.Unopt(call.RecvType))
		} else {
			rt = b.typ(f.Recv.Type)
		}
	}
	var targs []check.Type
	for _, t := range call.TypeArgs {
		targs = append(targs, b.typ(t))
	}
	idx := b.l.funcIndex(f, targs, rt)
	s := b.calleeSubst(f, targs, rt)
	vals := append([]int{recv}, b.args(call, s)...)
	return b.invoke(idx, vals, b.l.kind(s.Apply(f.Sig.Result)), dst, discard)
}

// calleeSubst is the substitution a callee's signature is read with.
func (b *fb) calleeSubst(f *check.Function, targs []check.Type, recv check.Type) check.Subst {
	s := check.Subst{}
	if n, ok := recv.(*check.Named); ok {
		for k, v := range n.Subst() {
			s[k] = v
		}
	}
	for i, tp := range f.TypeParams {
		if i < len(targs) {
			s[tp] = targs[i]
		}
	}
	return s
}

func (b *fb) closureCall(e *syntax.CallExpr, call *check.Call, dst int) int {
	ft, _ := b.typ(call.RecvType).(*check.FuncType)
	fn := b.value(call.Recv, -1)
	var vals []int
	for _, a := range call.Args {
		vals = append(vals, b.value(a, -1))
	}
	ks := make([]bc.Kind, len(vals))
	for i, v := range vals {
		ks[i] = b.f.Kinds[v]
	}
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, v)
	}
	k := bc.KRef
	if ft != nil && ft.Result != check.Void {
		k = b.l.kind(ft.Result)
	}
	r := b.out(dst, k)
	b.emit(op("CALLC"), r, fn, blk, len(vals))
	b.releaseRegs(blk, ks)
	if ft != nil && ft.Result == check.Void {
		return -1
	}
	return r
}

func (b *fb) ifaceCall(e *syntax.CallExpr, call *check.Call, recv *int, dst int) int {
	var x int
	if recv != nil {
		x = *recv
	} else {
		x = b.value(call.Recv, -1)
	}
	vals := append([]int{x}, b.args(call, nil)...)
	return b.ifaceInvoke(call.Name, call, vals, dst)
}

// ifaceInvoke calls an interface's method name with vals (the receiver,
// then the arguments). The receiver's type, read with this
// instantiation's types, says how: an interface's value through CALLI
// (its shape's method of that name), a struct's or an enum's (a
// constrained type parameter's, here) its method directly.
func (b *fb) ifaceInvoke(name string, call *check.Call, vals []int, dst int) int {
	k := b.l.kind(b.typ(call.Result))
	if n, ok := b.typ(check.Unopt(call.RecvType)).(*check.Named); ok && (n.Obj.Kind == check.StructType || n.Obj.Kind == check.EnumType) {
		if f := n.Obj.Methods[name]; f != nil {
			r := b.invoke(b.l.funcIndex(f, nil, n), vals, k, dst, false)
			if k == bc.KVoid {
				return -1
			}
			return r
		}
	}
	ks := make([]bc.Kind, len(vals))
	for i, v := range vals {
		ks[i] = b.f.Kinds[v]
	}
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, v)
	}
	r := bc.None
	if k != bc.KVoid {
		r = b.out(dst, k)
	}
	b.emit(op("CALLI"), r, b.l.str(name), blk, len(vals))
	b.releaseRegs(blk, ks)
	if r == bc.None {
		return -1
	}
	return r
}

// ---------------------------------------------------------------------
// Built-in methods

func (b *fb) builtinMethod(e *syntax.CallExpr, call *check.Call, recv *int, dst int) int {
	m := call.Method
	res := b.typ(call.Result)
	k := b.l.kind(res)
	switch m.Special {
	case "len":
		x := b.recvValue(call, recv)
		r := b.out(dst, bc.KInt)
		b.emit(op("LEN"), r, x)
		return r
	case "isNone", "isSome":
		x := b.recvValue(call, recv)
		r := b.out(dst, bc.KBool)
		b.emit(op("ISNONE"), r, x)
		if m.Special == "isSome" {
			b.emit(op("NOT"), r, r)
		}
		return r
	case "mapGet":
		x := b.recvValue(call, recv)
		key := b.value(call.Args[0], -1)
		r := b.out(dst, bc.KRef)
		b.emit(op("MGET"), r, x, key)
		return r
	case "enumerated":
		return b.enumerated(call, recv, dst)
	case "i2f", "f2i":
		x := b.recvValue(call, recv)
		if m.Special == "i2f" {
			r := b.out(dst, bc.KFloat)
			b.emit(op("I2F"), r, x)
			return r
		}
		r := b.out(dst, bc.KInt)
		b.emit(op("F2I"), r, x)
		return r
	}
	// Arguments first (before the receiver is taken out, when it's
	// changed in place), in the builtin's order.
	vals := b.args(call, nil)
	if m.Variadic {
		// The variadic parameter's arguments, as one array.
		k := len(m.Params) - 1
		ek := b.l.kind(b.typ(call.Params[k].Type))
		rest := vals[k:]
		ks := repeatKind(ek, len(rest))
		blk := b.regs(ks)
		for i, v := range rest {
			b.move(blk+i, v)
		}
		arr := b.temp(bc.KRef)
		b.emit(op("NEWA"), arr, int(ek), bc.None, blk, len(rest))
		b.releaseRegs(blk, ks)
		vals = append(vals[:k:k], arr)
	}
	if m.Order != nil {
		ord := make([]int, len(m.Order))
		for i, j := range m.Order {
			ord[i] = vals[j]
		}
		vals = ord
	}
	switch m.Extra {
	case "kindU":
		u := b.typ(call.Bound["U"])
		vals = append(vals, b.konst(int64(b.l.kind(u))))
	case "false":
		vals = append(vals, b.konst(0))
	case "true":
		vals = append(vals, b.konst(1))
	}
	if m.Recv == "ref" {
		return b.refMethod(m, b.recvValue(call, recv), vals, res, dst)
	}
	for _, c := range m.Consts {
		vals = append(vals, b.konst(c))
	}
	r := bc.None
	if m.Async {
		// An async builtin returns its future; the value is the await's.
		r = b.temp(bc.KRef)
	} else if k != bc.KVoid {
		r = b.out(dst, k)
	}
	if m.Inout && recv == nil {
		lv := b.prepare(call.Recv)
		b.update(lv, func(cur int) {
			b.callInout(m, cur, vals, r)
		})
	} else {
		x := b.recvValue(call, recv)
		b.callBTo(m.Builtin, r, append([]int{x}, vals...)...)
	}
	if m.Async {
		if k == bc.KVoid {
			b.emit(op("AWAIT"), b.temp(bc.KRef), r)
			return -1
		}
		v := b.out(dst, k)
		b.emit(op("AWAIT"), v, r)
		return v
	}
	if r == bc.None {
		return -1
	}
	return r
}

func (b *fb) recvValue(call *check.Call, recv *int) int {
	if recv != nil {
		return *recv
	}
	return b.value(call.Recv, -1)
}

// callInout calls an inout builtin on cur, the receiver's own register:
// it's moved into the arguments' block, changed there, and moved back.
func (b *fb) callInout(m *check.Method, cur int, vals []int, r int) {
	ks := []bc.Kind{bc.KRef}
	for _, v := range vals {
		ks = append(ks, b.f.Kinds[v])
	}
	blk := b.regs(ks)
	null := b.null()
	for i, v := range vals {
		b.move(blk+1+i, v)
	}
	b.emit(op("MOV"), blk, cur)
	b.emit(op("MOV"), cur, null)
	back := func() {
		b.emit(op("MOV"), cur, blk)
		b.emit(op("MOV"), blk, null)
	}
	if m.Throws || m.Fails || hasClosureArg(m) {
		b.protect(func() {
			b.emit(op("CALLB"), r, b.l.builtin(m.Builtin), blk, len(ks))
		}, func(int) { back() })
	} else {
		b.emit(op("CALLB"), r, b.l.builtin(m.Builtin), blk, len(ks))
	}
	back()
	b.releaseRegs(blk, ks)
}

func hasClosureArg(m *check.Method) bool {
	for _, p := range m.Params {
		if _, ok := p.Type.(*check.FuncType); ok {
			return true
		}
	}
	return false
}

// enumerated is xs.enumerated(): an array of (index, element).
func (b *fb) enumerated(call *check.Call, recv *int, dst int) int {
	x := b.recvValue(call, recv)
	at, _ := b.typ(check.Unopt(call.RecvType)).(*check.Array)
	tup := b.typ(call.Result).(*check.Array).Elem.(*check.Tuple)
	shape := b.l.shape(tup)
	out := b.out(dst, bc.KRef)
	b.emit(op("NEWA"), out, int(bc.KRef), shape, 0, 0)
	n, i, c := b.temp(bc.KInt), b.temp(bc.KInt), b.temp(bc.KBool)
	b.emit(op("LEN"), n, x)
	b.loadInt(i, 0)
	top, end := b.f.Here(), b.f.NewLabel()
	b.emit(op("LTI"), c, i, n)
	b.emitJump(op("JF"), c, end)
	el := b.temp(b.l.kind(at.Elem))
	b.emit(op("IDX"), el, x, i)
	t := b.newStruct(shape, tup.Elems, []int{i, el}, -1)
	m := b.l.methodPush()
	b.callInout(m, out, []int{t}, bc.None)
	b.emit(op("ADDIK"), i, i, 1)
	b.jmp(top)
	b.f.Place(end)
	return out
}

func (l *Lowerer) methodPush() *check.Method {
	for _, m := range check.Methods {
		if m.Recv == "array" && m.Name == "push" {
			return m
		}
	}
	return nil
}

// ---------------------------------------------------------------------
// Builtins

func (b *fb) builtinCall(e *syntax.CallExpr, call *check.Call, dst int) int {
	bi := call.Builtin
	switch bi.ID {
	case check.BPrint:
		v := b.value(call.Args[0], -1)
		s := b.describe(v, b.typeOf(call.Args[0]))
		b.callBTo("print", bc.None, s)
		return -1
	case check.BExpect:
		return b.expect(e, call)
	case check.BPanic:
		v := b.value(call.Args[0], -1)
		b.callBTo("panic", bc.None, v)
		return -1
	case check.BSleep:
		d := b.value(call.Args[0], -1)
		f := b.callB("time.sleep", bc.KRef, d)
		w := b.temp(bc.KRef)
		b.emit(op("AWAIT"), w, f)
		return -1
	case check.BAll, check.BRace:
		return b.allRace(e, call, dst)
	case check.BWithTimeout:
		return b.withTimeout(e, call, dst)
	case check.BLive:
		ft := b.typ(call.Result).(*check.FuncType)
		init := b.valueReg(b.value(call.Args[0], -1), b.l.kind(ft.Result))
		src := b.value(call.Args[1], -1)
		return b.copyTo(b.callB("live.new", bc.KRef, init, src, b.konst(int64(b.l.kind(ft.Result)))), dst)
	case check.BEnvironment:
		return b.copyTo(b.callB("ui.environment", bc.KRef), dst)
	case check.BSet:
		// Set(xs): its distinct elements; Set<T>(): none.
		st := b.typ(call.Result).(*check.Set)
		if len(call.Args) == 1 {
			xs := b.value(call.Args[0], -1)
			return b.copyTo(b.callB("set.from", bc.KRef, xs), dst)
		}
		return b.copyTo(b.callB("set.of", bc.KRef, b.konst(int64(b.l.kind(st.Elem)))), dst)
	case check.BStream:
		// Stream<T>(produce): started when it's first read.
		st := b.typ(call.Result).(*check.Stream)
		p := b.value(call.Args[0], -1)
		return b.copyTo(b.callB("stream.new", bc.KRef, p, b.konst(int64(b.l.kind(st.Elem)))), dst)
	case check.BNever:
		f := b.callB("async.never", bc.KRef)
		w := b.temp(bc.KRef)
		b.emit(op("AWAIT"), w, f)
		return -1
	case check.BIdle:
		f := b.callB(uiTestIdle, bc.KRef)
		w := b.temp(bc.KRef)
		b.emit(op("AWAIT"), w, f)
		return -1
	case check.BMount:
		var bl int
		if p := call.Args[1]; p != nil {
			// The builder provides the store, then builds the view.
			store := b.value(p, -1)
			key := b.l.typeKey(b.rawType(p))
			sv := &check.Var{}
			x := call.Args[0]
			ctx := b.l.info.Ctxs[x]
			bl = b.makeClosure(closure{name: "mount", ctxs: []*check.FuncCtx{ctx}, extra: []extraCap{{sv, store}}, ret: bc.KRef, throws: true, body: func(c *fb) {
				c.callBTo(uiProvide, bc.None, c.kstr(key), c.vars[sv])
				v, _ := c.viewExpr(x)
				c.emit(op("RET"), v)
			}})
		} else {
			bl = b.builder(call.Args[0])
		}
		platform := b.konst(-1)
		if x := call.Args[2]; x != nil {
			platform = b.value(x, -1)
		}
		at := b.kstr("")
		if x := call.Args[3]; x != nil {
			at = b.value(x, -1)
		}
		return b.copyTo(b.callB(uiTestMount, bc.KRef, bl, platform, at), dst)
	case check.BEmbedText:
		return b.copyTo(b.kstr(b.l.info.Embedded[e]), dst)
	case check.BEmbedBytes:
		return b.copyTo(b.callB("str.to_bytes", bc.KRef, b.kstr(b.l.info.Embedded[e])), dst)
	case check.BEmbedJSON:
		// Read as json.decode reads it; the checker read it the same way
		// (check.checkJSON), so it doesn't fail.
		t := b.typ(call.Result)
		return b.copyTo(b.callB("json.decode", b.l.kind(t), b.kstr(b.l.info.Embedded[e]), b.kstr(b.l.desc(t))), dst)
	case check.BEmbedTexts, check.BEmbedFiles:
		// [path: contents]: a map of the files, made as a literal is.
		files := b.l.info.EmbeddedFiles[e]
		vk := bc.KRef
		var ks []bc.Kind
		var vals []int
		for _, f := range files {
			v := b.kstr(f.Data)
			if call.Builtin.ID == check.BEmbedFiles {
				v = b.callB("str.to_bytes", bc.KRef, v)
			}
			ks = append(ks, bc.KRef, vk)
			vals = append(vals, b.kstr(f.Path), v)
		}
		blk := b.regs(ks)
		for i, v := range vals {
			b.move(blk+i, v)
		}
		r := b.out(dst, bc.KRef)
		b.emit(op("NEWM"), r, int(bc.KRef), int(vk), blk, len(files))
		b.releaseRegs(blk, ks)
		return r
	case check.BNavigate:
		return b.navigate(call, 0)
	case check.BReplace:
		return b.navigate(call, 2)
	case check.BPush, check.BPresent:
		bl := b.builder(call.Args[0])
		name := uiPush
		if bi.ID == check.BPresent {
			name = uiPresent
		}
		b.callBTo(name, bc.None, bl)
		return -1
	case check.BPop:
		b.callBTo(uiPop, bc.None)
		return -1
	case check.BDismiss:
		b.callBTo(uiDismiss, bc.None)
		return -1
	case check.BMin, check.BMax:
		x := b.value(call.Args[0], -1)
		y := b.value(call.Args[1], -1)
		t := b.typeOf(call.Args[0])
		name := map[bool]string{true: "math.min", false: "math.max"}[bi.ID == check.BMin]
		if t == check.Float {
			name += "f"
		} else {
			name += "i"
		}
		return b.copyTo(b.callB(name, b.l.kind(t), x, y), dst)
	case check.BAbs:
		x := b.value(call.Args[0], -1)
		t := b.typeOf(call.Args[0])
		if t == check.Float {
			return b.copyTo(b.callB("math.absf", bc.KFloat, x), dst)
		}
		return b.copyTo(b.callB("math.absi", bc.KInt, x), dst)
	case check.BFloat:
		x := b.value(call.Args[0], -1)
		if b.f.Kinds[x] == bc.KFloat {
			return b.copyTo(x, dst)
		}
		r := b.out(dst, bc.KFloat)
		b.emit(op("I2F"), r, x)
		return r
	case check.BInt:
		x := b.value(call.Args[0], -1)
		if b.f.Kinds[x] == bc.KInt {
			return b.copyTo(x, dst)
		}
		r := b.out(dst, bc.KInt)
		b.emit(op("F2I"), r, x)
		return r
	case check.BError:
		m := b.value(call.Args[0], -1)
		return b.copyTo(b.callB("error", bc.KRef, m), dst)
	case check.BJSONDecode:
		text := b.value(call.Args[0], -1)
		t := b.typ(call.Result)
		d := b.kstr(b.l.desc(t))
		return b.copyTo(b.callB("json.decode", b.l.kind(t), text, d), dst)
	case check.BJSONEncode:
		v := b.value(call.Args[0], -1)
		t := b.typeOf(call.Args[0])
		d := b.kstr(b.l.desc(t))
		var ind int
		if call.Args[1] != nil {
			ind = b.value(call.Args[1], -1)
		} else {
			ind = b.konst(0)
		}
		return b.copyTo(b.callB("json.encode", bc.KRef, v, d, ind), dst)
	case check.BRuntime:
		return b.runtimeCall(call, dst)
	case check.BAnimate:
		return b.animateCall(call)
	case check.BRefNew:
		return b.refNew(dst)
	case check.BPathNew:
		return b.copyTo(b.callB(pathNew, bc.KRef), dst)
	case check.BShader:
		return b.shaderProgram(e, dst)
	case check.BMath:
		var vals []int
		for _, a := range call.Args {
			vals = append(vals, b.value(a, -1))
		}
		k := b.l.kind(bi.Sig.Result)
		if k == bc.KVoid {
			b.callBTo(bi.RT, bc.None, vals...)
			return -1
		}
		return b.copyTo(b.callB(bi.RT, k, vals...), dst)
	}
	b.l.errorf(e, "can't lower %s yet", bi.Name())
	return -1
}

// expect(cond) throws when cond is false, saying what failed and where;
// for a comparison, with both sides' values.
func (b *fb) expect(e *syntax.CallExpr, call *check.Call) int {
	cond := call.Args[0]
	text := b.source(cond)
	ok := b.f.NewLabel()
	if be, isBin := unparen(cond).(*syntax.BinaryExpr); isBin && isComparison(be.Op) && describable(b.typeOf(be.X)) && describable(b.typeOf(be.Y)) {
		xt, yt := b.typeOf(be.X), b.typeOf(be.Y)
		x := b.value(be.X, -1)
		y := b.value(be.Y, -1)
		r := b.binop(be.Op, x, y, xt, yt, check.Bool, -1)
		b.emitJump(op("JT"), r, ok)
		parts := []int{b.kstr("expected " + text + " (" + b.where(e) + "): got "), b.describe(x, xt), b.kstr(" and "), b.describe(y, yt)}
		msg := b.concat(parts)
		if len(call.Args) > 1 {
			msg = b.concat([]int{b.value(call.Args[1], -1), b.kstr(": "), msg})
		}
		ev := b.callB("error", bc.KRef, msg)
		b.emit(op("THROW"), ev)
		b.f.Place(ok)
		return -1
	}
	r := b.value(cond, -1)
	b.emitJump(op("JT"), r, ok)
	msg := b.kstr("expected " + text + " (" + b.where(e) + ")")
	if len(call.Args) > 1 {
		msg = b.concat([]int{b.value(call.Args[1], -1), b.kstr(": "), msg})
	}
	ev := b.callB("error", bc.KRef, msg)
	b.emit(op("THROW"), ev)
	b.f.Place(ok)
	return -1
}

func isComparison(t syntax.Token) bool {
	switch t {
	case syntax.EQL, syntax.NEQ, syntax.LSS, syntax.LEQ, syntax.GTR, syntax.GEQ:
		return true
	}
	return false
}

func describable(t check.Type) bool {
	switch check.Unopt(t) {
	case check.Int, check.Float, check.Bool, check.String, check.Duration:
		return true
	}
	if t == check.NoneType {
		return false
	}
	switch check.Unopt(t).(type) {
	case *check.Array, *check.Named, *check.Map:
		return true
	}
	return false
}

func (b *fb) concat(parts []int) int {
	r := parts[0]
	for _, p := range parts[1:] {
		t := b.temp(bc.KRef)
		b.emit(op("CONCAT"), t, r, p)
		r = t
	}
	return r
}

// source is the text of an expression, as asuka fmt writes it.
func (b *fb) source(e syntax.Expr) string {
	f := b.l.fset.File(e.Range().Start)
	var buf bytes.Buffer
	if err := syntax.Fprint(&buf, f, e); err != nil {
		return "the condition"
	}
	return strings.TrimSpace(buf.String())
}

func unparen(e syntax.Expr) syntax.Expr {
	for {
		p, ok := e.(*syntax.ParenExpr)
		if !ok {
			return e
		}
		e = p.X
	}
}

// allRace lowers all(…) and race(…): each call started as a task, then
// awaited together.
func (b *fb) allRace(e *syntax.CallExpr, call *check.Call, dst int) int {
	res := b.typ(call.Result)
	isAll := call.Builtin.ID == check.BAll
	// One array of tasks.
	if len(call.Args) == 1 {
		if at, ok := b.typeOf(call.Args[0]).(*check.Array); ok {
			if _, ok := at.Elem.(*check.Task); ok {
				arr := b.value(call.Args[0], -1)
				name := map[bool]string{true: "async.all", false: "async.race"}[isAll]
				f := b.callB(name, bc.KRef, arr)
				k := b.l.kind(res)
				r := b.out(dst, k)
				b.emit(op("AWAIT"), r, f)
				return r
			}
		}
	}
	// all's results are references (boxed scalars), so one array holds
	// them all; race's have the one type.
	boxed := isAll
	var futs []int
	for _, a := range call.Args {
		futs = append(futs, b.startTask(a, boxed))
	}
	blk := b.regs(repeatKind(bc.KRef, len(futs)))
	for i, f := range futs {
		b.move(blk+i, f)
	}
	arr := b.temp(bc.KRef)
	b.emit(op("NEWA"), arr, int(bc.KRef), bc.None, blk, len(futs))
	b.releaseRegs(blk, repeatKind(bc.KRef, len(futs)))
	if !isAll {
		f := b.callB("async.race", bc.KRef, arr)
		k := b.l.kind(res)
		if k == bc.KVoid {
			w := b.temp(bc.KRef)
			b.emit(op("AWAIT"), w, f)
			return -1
		}
		r := b.out(dst, k)
		b.emit(op("AWAIT"), r, f)
		return r
	}
	f := b.callB("async.all", bc.KRef, arr)
	results := b.temp(bc.KRef)
	b.emit(op("AWAIT"), results, f)
	if res == check.Void {
		return -1
	}
	types := []check.Type{res}
	if tt, ok := res.(*check.Tuple); ok {
		types = tt.Elems
	}
	var vals []int
	for i, t := range types {
		idx := b.konst(int64(i))
		v := b.temp(bc.KRef)
		b.emit(op("IDX"), v, results, idx)
		vals = append(vals, b.unbox(v, t, -1))
	}
	if len(types) == 1 {
		return b.copyTo(vals[0], dst)
	}
	return b.newStruct(b.l.shape(res), types, vals, dst)
}

// startTask starts an async call (or takes a task) as a future; boxed:
// its result is a reference (a scalar boxed).
func (b *fb) startTask(a syntax.Expr, boxed bool) int {
	t := b.rawType(a)
	if tk, ok := t.(*check.Task); ok {
		f := b.value(a, -1)
		if !boxed || b.l.kind(tk.Elem) == bc.KRef || b.l.kind(tk.Elem) == bc.KVoid {
			return f
		}
		// A task that awaits it and boxes its result.
		return b.taskOf(a, f, tk.Elem, true)
	}
	return b.taskOf(a, -1, t, boxed)
}

// taskOf starts a task running the async call a (or awaiting the
// future f), and returns its future.
func (b *fb) taskOf(a syntax.Expr, f int, t check.Type, boxed bool) int {
	k := b.l.kind(t)
	ret := k
	if boxed && k != bc.KRef && k != bc.KVoid {
		ret = bc.KRef
	}
	call, _ := unparen(a).(*syntax.CallExpr)
	// What the call needs, computed here: its receiver and arguments.
	var pre []int
	var ci *check.Call
	if f >= 0 {
		pre = []int{f}
	} else if call != nil {
		ci = b.l.info.Calls[call]
		if ci != nil && (ci.Kind == check.CallMethod && !ci.SelfCall || ci.Kind == check.CallClosure || ci.Kind == check.CallIface) {
			pre = append(pre, b.value(ci.Recv, -1))
		} else if ci != nil && ci.Kind == check.CallMethod && ci.SelfCall {
			if ci.Func.Recv.Kind == check.StoreType || ci.Func.Recv.Kind == check.ComponentType {
				pre = append(pre, b.env(a))
			} else {
				pre = append(pre, b.readVar(b.ctx.Self, a))
			}
		}
		if ci != nil {
			switch ci.Kind {
			case check.CallClosure:
				for _, x := range ci.Args {
					pre = append(pre, b.value(x, -1))
				}
			default:
				pre = append(pre, b.args(ci, nil)...)
			}
		}
	}
	// The task's closure captures them.
	var extra []extraCap
	vars := make([]*check.Var, len(pre))
	for i, r := range pre {
		v := &check.Var{}
		vars[i] = v
		extra = append(extra, extraCap{v, r})
	}
	clos := b.makeClosure(closure{name: "task", extra: extra, ret: ret, async: true, throws: true, body: func(c *fb) {
		regs := make([]int, len(vars))
		for i, v := range vars {
			regs[i] = c.vars[v]
		}
		var v int
		switch {
		case f >= 0:
			v = c.temp(c.l.kind(t))
			if k == bc.KVoid {
				v = c.temp(bc.KRef)
			}
			c.emit(op("AWAIT"), v, regs[0])
		case ci == nil:
			v = -1
		default:
			v = c.callPrepared(ci, regs, k)
		}
		if ret == bc.KRef && k != bc.KRef && k != bc.KVoid {
			bx := c.temp(bc.KRef)
			c.emit(op("BOX"), bx, v, int(k))
			v = bx
		}
		if k == bc.KVoid {
			c.emit(op("RET0"))
			return
		}
		c.ret(v, ret)
	}})
	fut := b.temp(bc.KRef)
	b.emit(op("TASK"), fut, clos)
	return fut
}

// callPrepared makes a call whose receiver and arguments are in regs.
func (b *fb) callPrepared(ci *check.Call, regs []int, k bc.Kind) int {
	switch ci.Kind {
	case check.CallFunc:
		var targs []check.Type
		for _, t := range ci.TypeArgs {
			targs = append(targs, b.typ(t))
		}
		return b.invoke(b.l.funcIndex(ci.Func, targs, nil), regs, k, -1, false)
	case check.CallMethod:
		var rt check.Type
		if ci.Func.Recv.Kind == check.StructType || ci.Func.Recv.Kind == check.EnumType {
			if ci.RecvType != nil {
				rt = b.typ(ci.RecvType)
			} else if b.ctx != nil && b.ctx.Self != nil {
				rt = b.typ(b.ctx.Self.Type)
			}
		}
		var targs []check.Type
		for _, t := range ci.TypeArgs {
			targs = append(targs, b.typ(t))
		}
		return b.invoke(b.l.funcIndex(ci.Func, targs, rt), regs, k, -1, false)
	case check.CallClosure:
		ks := make([]bc.Kind, len(regs)-1)
		for i, r := range regs[1:] {
			ks[i] = b.f.Kinds[r]
		}
		blk := b.regs(ks)
		for i, r := range regs[1:] {
			b.move(blk+i, r)
		}
		r := b.temp(bc.KRef)
		if k != bc.KVoid {
			r = b.temp(k)
		}
		b.emit(op("CALLC"), r, regs[0], blk, len(ks))
		b.releaseRegs(blk, ks)
		return r
	case check.CallIface:
		if r := b.ifaceInvoke(ci.Name, ci, regs, -1); r >= 0 {
			return r
		}
		return b.temp(bc.KRef)
	case check.CallBuiltin:
		if ci.Builtin.ID == check.BSleep {
			f := b.callB("time.sleep", bc.KRef, regs[0])
			w := b.temp(bc.KRef)
			b.emit(op("AWAIT"), w, f)
			return -1
		}
	}
	return b.valueReg(-1, k)
}

// ---------------------------------------------------------------------
// Making values

func (b *fb) structCall(call *check.Call, dst int) int {
	t := b.typ(call.Type).(*check.Named)
	s := t.Subst()
	vals := b.args(call, s)
	var types []check.Type
	for _, f := range t.Obj.Fields {
		types = append(types, s.Apply(f.Type))
	}
	if t.Obj.Pkg != nil {
		if hook := StructHooks[t.Obj.Pkg.Path+"."+t.Obj.Name()]; hook != nil {
			b.structHook(hook, t, call, vals)
		}
	}
	shape := b.l.shape(t)
	if t.Obj.IsError {
		ks := make([]bc.Kind, len(vals))
		for i, ty := range types {
			ks[i] = b.l.kind(ty)
		}
		blk := b.regs(ks)
		for i, v := range vals {
			b.move(blk+i, v)
		}
		r := b.out(dst, bc.KRef)
		b.emit(op("NEWE"), r, shape, 1, blk)
		b.releaseRegs(blk, ks)
		return r
	}
	return b.newStruct(shape, types, vals, dst)
}

func (b *fb) caseCall(call *check.Call, dst int) int {
	t := b.typ(call.Type).(*check.Named)
	s := t.Subst()
	vals := b.args(call, s)
	ks := make([]bc.Kind, len(vals))
	for i, p := range call.Case.Params {
		pt := s.Apply(p)
		if pt == nil {
			pt = b.l.prog.Prelude.Scope.Lookup("Error").(*check.TypeName).Type
		}
		ks[i] = b.l.kind(pt)
	}
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, v)
	}
	r := b.out(dst, bc.KRef)
	b.emit(op("NEWE"), r, b.l.shape(t), call.Case.Index+caseBase(t), blk)
	b.releaseRegs(blk, ks)
	return r
}

// ---------------------------------------------------------------------
// Closures

func (b *fb) lambda(e *syntax.LambdaExpr) int {
	if rb := b.l.info.BuilderLambdas[e]; rb != nil {
		// Built with accessors: a List's header.
		return b.routeBuilder(rb)
	}
	ctx := b.l.info.Ctxs[e]
	ft, _ := b.typ(b.l.info.Types[e]).(*check.FuncType)
	if ft == nil {
		ft = &check.FuncType{Result: check.Void}
	}
	ret := b.l.kind(ft.Result)
	return b.makeClosure(closure{name: "closure@" + itoa(int(b.l.line(e))), ctxs: []*check.FuncCtx{ctx}, params: ctx.Params, ret: ret, async: ctx.Awaits, throws: true, body: func(c *fb) {
		c.ctx = ctx
		switch body := e.Body.(type) {
		case syntax.Expr:
			if ret == bc.KVoid {
				c.exprStmt(body)
				c.emit(op("RET0"))
				return
			}
			c.ret(c.value(body, -1), ret)
		case *syntax.Block:
			c.block(body)
			c.ret(-1, ret)
		}
	}})
}

func (b *fb) spawn(e *syntax.SpawnExpr, dst int) int {
	ctx := b.l.info.Ctxs[e]
	t := b.typ(ctx.Result)
	ret := b.l.kind(t)
	clos := b.makeClosure(closure{name: "spawn", ctxs: []*check.FuncCtx{ctx}, ret: ret, throws: true, byValue: true, body: func(c *fb) {
		c.ctx = ctx
		items := e.Body.Items
		n := len(items)
		if n > 0 {
			if es, ok := items[n-1].(*syntax.ExprStmt); ok && b.l.info.Ctxs[es] == ctx {
				c.stmts(items[:n-1])
				c.ret(c.value(es.X, -1), ret)
				return
			}
		}
		c.stmts(items)
		c.ret(-1, ret)
	}})
	k := b.konst(int64(ret))
	return b.copyTo(b.callB("async.spawn", bc.KRef, clos, k), dst)
}

// withTimeout(d) { body } races the body against a timer that throws.
func (b *fb) withTimeout(e *syntax.CallExpr, call *check.Call, dst int) int {
	if len(call.Args) < 2 {
		return -1
	}
	d := b.value(call.Args[0], -1)
	lam := call.Args[1].(*syntax.LambdaExpr)
	body := b.lambda(lam)
	t1 := b.temp(bc.KRef)
	b.emit(op("TASK"), t1, body)
	dv := &check.Var{}
	msg := "timed out (" + b.where(e) + ")"
	timer := b.makeClosure(closure{name: "timeout", extra: []extraCap{{dv, d}}, ret: bc.KVoid, async: true, throws: true, body: func(c *fb) {
		f := c.callB("time.sleep", bc.KRef, c.vars[dv])
		w := c.temp(bc.KRef)
		c.emit(op("AWAIT"), w, f)
		c.throwMsg(msg)
		c.emit(op("RET0"))
	}})
	t2 := b.temp(bc.KRef)
	b.emit(op("TASK"), t2, timer)
	ks := repeatKind(bc.KRef, 2)
	blk := b.regs(ks)
	b.move(blk, t1)
	b.move(blk+1, t2)
	arr := b.temp(bc.KRef)
	b.emit(op("NEWA"), arr, int(bc.KRef), bc.None, blk, 2)
	b.releaseRegs(blk, ks)
	f := b.callB("async.race", bc.KRef, arr)
	// Its value is the block's, which wins the race (the timer throws).
	if k := b.l.kind(b.typ(call.Result)); k != bc.KVoid {
		r := b.out(dst, k)
		b.emit(op("AWAIT"), r, f)
		return r
	}
	w := b.temp(bc.KRef)
	b.emit(op("AWAIT"), w, f)
	return -1
}

// StructHooks make a standard library struct's fields when the app
// compiles, from its first field, a constant string: svg.Icon's drawing
// from its markup, shader.Program's code from its source. A hook gives
// the fields it makes by name (a string, []byte, float64 or int64), or an
// error, which is the compiler's (at the literal). lang/compile registers
// them, by the struct's package path and name ("std/svg.Icon").
var StructHooks = map[string]func(source string) (map[string]any, error){}

// structHook fills the fields hook makes, for a literal whose first
// field is a constant string.
func (b *fb) structHook(hook func(string) (map[string]any, error), t *check.Named, call *check.Call, vals []int) {
	var src *syntax.StringLit
	if len(call.Args) > 0 {
		src, _ = call.Args[0].(*syntax.StringLit)
	}
	if src == nil || len(src.Interps) > 0 {
		at := syntax.Node(nil)
		if len(call.Args) > 0 && call.Args[0] != nil {
			at = call.Args[0]
		}
		if at != nil {
			b.l.errorf(at, "%s's %s is made when the app compiles: give it a string literal", t.Obj.Name(), t.Obj.Fields[0].Name)
		}
		return
	}
	segs := src.Segments
	text := ""
	if src.Multiline {
		text = literalText(check.Dedent(segs[0]), src.Raw, true)
	} else {
		text = literalText(segs[0], src.Raw, false)
	}
	fields, err := hook(text)
	if err != nil {
		b.l.errorf(src, "%s", err)
		return
	}
	for i, f := range t.Obj.Fields {
		v, ok := fields[f.Name]
		if !ok || i >= len(vals) {
			continue
		}
		switch v := v.(type) {
		case string:
			vals[i] = b.kstr(v)
		case []byte:
			vals[i] = b.callB("str.to_bytes", bc.KRef, b.kstr(string(v)))
		case float64:
			r := b.temp(bc.KFloat)
			b.emit(op("KFLOAT"), r, b.l.floatConst(v))
			vals[i] = r
		case int64:
			r := b.temp(bc.KInt)
			b.loadInt(r, v)
			vals[i] = r
		}
	}
}
