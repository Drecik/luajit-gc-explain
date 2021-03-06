/*
** Garbage collector.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_vm.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40        // 表示一次最大清除gc对象的个数
#define GCSWEEPCOST	10        // 表示每次清除的消耗
#define GCFINALIZECOST	100   // userdata每次的finalized消耗

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
// 如果该tvalue是一个gc对象且为白色的情况下，标记该节点，并把节点放入到灰色链表中
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
// 如果当前节点为白色，则把节点标记为灰色并放入到灰色链表中
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

/* Mark a white GCobj. */
// 标记gc对象为灰色节点并加入到灰色链表中
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lua_assert(iswhite(o) && !isdead(g, o));
  white2gray(o);    // 清空白色状态（白色和黑色标记位都为空的话即为灰色）
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    // userdata
    GCtab *mt = tabref(gco2ud(o)->metatable);
    // userdata不会有子节点，所以不会有灰色状态，如果引用到的话直接标记为黑色
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);  // 如果userdata设置了metatable，则标记metatable
    gc_markobj(g, tabref(gco2ud(o)->env));  // 标记userdata的环境变量
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    // upvalue
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));  // 如果upvalue对应的是gc对象的话标记该gc对象
    if (uv->closed)        // upvalue引用的局部变量在函数返回时被closed，因为这时候这些局部变量不会在变，所以可以直接转为黑色。没有被closed的upvalue，因为函数还没有返回可能会变，所以不被编辑为黑色，在atomic统一再做一次处理
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lua_assert(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO);
    // 如果类型不是string和cdata，加入到gray链表中
    // 这两种类型没有子元素，不需要往下遍历，同时只要不是白色，最后gc的时候也不会被清除，省略了gray2black步骤
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  // gray，grayagain，weak链表初始化为空
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);

  // 标记主线程，主线程环境变量，全局注册变量，以及root变量为灰色，并加入到灰色链表中
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);      // TODO：不太了解gcroot用在什么时候
  g->gc.state = GCSpropagate; // 设置状态为标记状态
}

/* Mark open upvalues. */
// 重新标记所有被标记过的upvalue对象
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    // 如果upvalue之前被标记过为灰色，则重新标记下upvalue对应的gc对象
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

/* Mark userdata in mmudata list. */
static void gc_mark_mmudata(global_State *g)
{
  GCobj *root = gcref(g->gc.mmudata);
  GCobj *u = root;
  if (u) {
    do {
      u = gcnext(u);
      makewhite(g, u);  /* Could be from previous GC. */
      gc_mark(g, u);
    } while (u != root);
  }
}

/* Separate userdata objects to be finalized to mmudata list. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  // gc链表mainthread前面是普通gc对象，后面挂载的全部都是userdata
  GCRef *p = &mainthread(g)->nextgc;
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    if (!(iswhite(o) || all) || isfinalized(gco2ud(o))) {
      p = &o->gch.nextgc;  // 如果不是可清楚对象，或者已经在上一次gc标记为finalized，则不处理
    } else if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
      markfinalized(o);  /* Done, as there's no __gc metamethod. */ // 没有'__gc'方法，直接标记为finalized
      p = &o->gch.nextgc;
    } else {  /* Otherwise move userdata to be finalized to mmudata list. */  // 标记为finalized，并加入到mmudata链表中，在finalized阶段进行处理
      m += sizeudata(gco2ud(o));
      markfinalized(o);
      *p = o->gch.nextgc;
      if (gcref(g->gc.mmudata)) {  /* Link to end of mmudata list. */
	GCobj *root = gcref(g->gc.mmudata);
	setgcrefr(o->gch.nextgc, root->gch.nextgc);
	setgcref(root->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
      } else {  /* Create circular list. */
	setgcref(o->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
      }
    }
  }
  return m;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
// 遍历table中的元素
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  cTValue *mode;
  // 如果table有metatable的话标记metatable
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_markobj(g, mt);
  
  // 查看table的弱引用设置
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    // 如果table有弱引用设置，设置了弱引用的table在atomic阶段进行删除
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      CTState *cts = ctype_ctsG(g);
      if (cts && cts->finalizer == t) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
  // 标记为弱引用，并把该节点加入到全局的弱引用链表中
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
      }
    }
  }
  // 如果值和value都是弱引用，直接返回
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  // 如果值没有设置弱引用，标记所有数值部分的tvalue
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
  }
  // 标记所有的hash table部分
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lua_assert(!tvisnil(&n->key));
  // 标记进行弱引用判断
	if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n->key);
	if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  // 标记函数的环境变量
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    // 如果是lua的函数
    uint32_t i;
    lua_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);

    // 标记该函数的proto对象
    gc_markobj(g, funcproto(fn));

    // 标记函数所有upvalue对象
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    // 如果是c函数
    uint32_t i;
    // 标记所有的upvalue对象
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lua_assert(traceno != G2J(g)->cur.traceno);
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  // 标记该chunk的名字
  gc_mark_str(proto_chunkname(pt));

  // 标记所有proto用到的常量。TODO：不太理解
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  // TODO:
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  // 标记thread的所有栈中的元素
  for (o = tvref(th->stack)+1; o < top; o++)
    gc_marktv(g, o);
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    // atomic阶段清除没有被使用的栈元素
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  // 标记线程用到的环境变量
  gc_markobj(g, tabref(th->env));

  // TODO:
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  lua_assert(isgray(o));
  // 将节点标记为黑色，并从灰色链表中移除
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */

  if (LJ_LIKELY(gct == ~LJ_TTAB)) {

    // table
    GCtab *t = gco2tab(o);
    // 遍历table中的元素，如果table是有弱引用设置的话，重新标记为灰色，将在atomic中重新遍历一次
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  // 弱表之所以不能为黑色，是为了不触发barrierback；为了弥补可能的强引用关系，弱表将在atomic阶段再遍历一次
    // 根据table元素数量，返回此次step的消耗值
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);

  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {

    // fucntion
    GCfunc *fn = gco2func(o);

    // 遍历函数内部的引用的gc对象
    gc_traverse_func(g, fn);

    // 根据是lua函数还是c函数，返回根据upvalue个数相关的消耗值
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);

  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {

    // proto
    GCproto *pt = gco2pt(o);

    // 遍历proto内部的引用的gc对象
    gc_traverse_proto(g, pt);
    return pt->sizept;

  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    // 加入到grayagain，在atomic中在遍历一次用于清除未使用的栈变量
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    black2gray(o);  // 标记为灰色，不触发barrier，因为修改会很频繁，在atomic重新标记一次

    // 遍历thrad中引用的gc对象
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    // TODO:
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lua_assert(0);
    return 0;
#endif
  }
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Try to shrink some common data structures. */
static void gc_shrink(global_State *g, lua_State *L)
{
  if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
    lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
  if (g->tmpbuf.sz > LJ_MIN_SBUF*2)
    lj_str_resizebuf(L, &g->tmpbuf, g->tmpbuf.sz >> 1);  /* Shrink temp buf. */
}

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), LJ_MAX_MEM)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);   // 可以通过currentwhite变量来控制需要删除的对象
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lua_assert(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED));
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lua_assert(isdead(g, o) || ow == LJ_GC_SFIXED);
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);   // 直接调用注册好的清除函数回调
    }
  }
  return p;
}

/* Check whether we can clear a key or a value slot from a table. */
// 检查该元素是否可以被清除，第二个参数val表示是value还是key
static int gc_mayclear(cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    // 字符串的话将被作为强引用，why?
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(strV(o));  /* And need to be marked. */
      return 0;
    }
    // 白色的肯定可以被删除
    if (iswhite(gcV(o)))
      return 1;  /* Object is about to be collected. */

    // 如果是userdata，且是value，以及finalized状态的可以被删除
    // userdata无法作为key?
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
// 清除弱表中的元素，如果该元素要被删除的话
static void gc_clearweak(GCobj *o)
{
  while (o) {
    GCtab *t = gco2tab(o);
    lua_assert((t->marked & LJ_GC_WEAK));
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      // 检查array里面的每个元素
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1))
	  setnilV(tv);
      }
    }
    // 检查hash表每个元素
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
  // 如果可以被清除，设置值为nil
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1)))
	  setnilV(&n->val);
      }
    }
    o = gcref(t->gclist);
  }
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  MSize oldt = g->gc.threshold;
  int errcode;
  TValue *top;
  lj_trace_abort(g);
  top = L->top;
  L->top = top+2;
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  copyTV(L, top, mo);
  setgcV(L, top+1, o, ~o->gch.gct);
  errcode = lj_vm_pcall(L, top+1, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

/* Finalize one userdata or cdata object from the mmudata list. */
// 调用userdata的'__gc'方法
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo;
  lua_assert(gcref(g->jit_L) == NULL);  /* Must not be called on trace. */
  /* Unchain from list of userdata to be finalized. */
  if (o == gcref(g->gc.mmudata))    // 结束
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(gcref(g->gc.mmudata)->gch.nextgc, o->gch.nextgc);
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    setgcrefr(o->gch.nextgc, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      g->gc.nocdatafin = 0;
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  /* Add userdata back to the main userdata list and make it white. */
  // 加回到gc链表，并标记为白色，因为调用'__gc'方法可能会使该userdata又活过来，例如加入到内存池中
  setgcrefr(o->gch.nextgc, mainthread(g)->nextgc);
  setgcref(mainthread(g)->nextgc, o);
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  // 调用'__gc'方法，如果有的话
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L);
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *node = noderef(t->node);
    ptrdiff_t i;
    setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
    for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
      if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
	GCobj *o = gcV(&node[i].key);
	TValue tmp;
	makewhite(g, o);
	o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
	copyTV(L, &tmp, &node[i].val);
	setnilV(&node[i].val);
	gc_call_finalizer(g, L, &tmp, o);
      }
  }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i, strmask;
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.root);
  strmask = g->strmask;
  for (i = 0; i <= strmask; i++)  /* Free all string hash chains. */
    gc_fullsweep(g, &g->strhash[i]);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  // 重新标记所有的upvalue，并进行遍历
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  // 弱表重新在遍历一次
  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lua_assert(!iswhite(obj2gco(mainthread(g))));
  gc_markobj(g, L);  /* Mark running thread. */   // 标记主线程
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */    // 全局对象在重新遍历一次，因为一些metatable的修改不会触发barrier
  gc_propagate_gray(g);  /* Propagate all of the above. */

  // grayagain重新在遍历一次，grayagain链表存放所有barrierback的对象
  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_mmudata(g);  /* Mark them. */ // 改变这些对象的颜色，确保不再sweep阶段被处理（会在finalized阶段被处理）
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
  // 清除所有weak表的元素
  gc_clearweak(gcref(g->gc.weak));

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (MSize)udsize;  /* Initial estimate. */
}

/* GC state machine. Returns a cost estimate for each step performed. */
// gc状态机函数，根据当前gc的状态进行相应的操作，并返回该操作的消耗
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    // 当前处于暂停状态，说明开始新一轮的gc，进行一些gc的初始化操作
    gc_mark_start(g);
    return 0;
  case GCSpropagate:
    if (gcref(g->gc.gray) != NULL)  // 如果当前还有灰色节点，则继续进行标记阶段
      return propagatemark(g);
    g->gc.state = GCSatomic;  // 标记阶段结束，进入到原子阶段
    return 0;
  case GCSatomic:
    if (gcref(g->jit_L))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {    // 清除字符串阶段
    MSize old = g->gc.total;
    gc_fullsweep(g, &g->strhash[g->gc.sweepstr++]);  /* Sweep one chain. */ // 一次遍历一个字符串hash table的冲突链表
    if (g->gc.sweepstr > g->strmask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    MSize old = g->gc.total;
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));  // 遍历gc链表，遍历GCSWEEPMAX个gc对象
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      gc_shrink(g, L);
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */  // 检查是否需要进入finalized状态
	g->gc.state = GCSfinalize;
#if LJ_HASFFI
	g->gc.nocdatafin = 1;
#endif
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	g->gc.debt = 0;
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      if (gcref(g->jit_L))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one userdata object. */
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
#if LJ_HASFFI
    if (!g->gc.nocdatafin) lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
#endif
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    return 0;
  default:
    lua_assert(0);
    return 0;
  }
}

// 执行一次可以设置的步长大小的增量gc操作，该函数会在g->gc.total >= g->gc.threshold触发，即当前lua虚拟机总内存大于等于设置的gc阈值
// 控制步长大小的变量为g->gc.stepmul，可以通过lua_gc(lua_State *L, int what, int data)，what传LUA_GCSETSTEPMUL进行设置
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  MSize lim;
  int32_t ostate = g->vmstate;      // 保存当前的虚拟机状态
  setvmstate(g, GC);                // 设置当前虚拟机状态为GC状态

  // 计算该次gc的步长大小，用到了可以配置的变量g->gc.stepmul，该值默认是200
  // GCSTEPSIZE默认大小为1024
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;

  // 如果g->gc.stepmul被设置成为0，那lim讲被设置成一个很大的数，即，这次step会一直执行到gc完成才退出
  if (lim == 0)
    lim = LJ_MAX_MEM;
  
  // 累计当前欠了多少内存
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    // 执行一次step，并扣除该step的消耗
    lim -= (MSize)gc_onestep(L);
    // GCSpause状态表示当前处于暂停状态，在整个gc过程完成时候会进入到该状态，所以这里判断整个gc是否完成
    if (g->gc.state == GCSpause) {
      // 设置下次进行gc的阈值，该阈值可以被控制g->gc.pause，该值默认为200
      // g->gc.estimate表示当前lua虚拟机实际使用的内存，所以在默认情况下，下次gc的阈值为当前实际使用的内存的两倍
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;    // 重新设置回旧的虚拟机状态
      return 1;  // 返回1表示整个gc已经结束
    }
  } while ((int32_t)lim > 0); // 循环一直到当前步长结束

  // 如果当前欠的内存比默认步长小，说明内存当前超过的不多，则适当增加下阈值，延长下下次gc的时间
  if (g->gc.debt < GCSTEPSIZE) {
    g->gc.threshold = g->gc.total + GCSTEPSIZE;
    g->vmstate = ostate;    // 重新设置回旧的虚拟机状态
    return -1;
  } else {
    // 如果当前欠的内存比较多，则从当前所欠内存里减去当前step的消耗（这是一个预估值），并且设置下次会立即gc
    g->gc.debt -= GCSTEPSIZE;
    g->gc.threshold = g->gc.total;  // 阈值设置为g->gc.total，则下次判断是否需要gc的时候还是会判断为true，所以会继续gc
    g->vmstate = ostate;    // 重新设置回旧的虚拟机状态
    return 0;
  }
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->jit_L));
  L->base = mref(G(L)->jit_base, TValue);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lua_assert(g->gc.state == GCSfinalize || g->gc.state == GCSpause);
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(&uv->tv))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
}
#endif

/* -- Allocator ----------------------------------------------------------- */

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, MSize osz, MSize nsz)
{
  global_State *g = G(L);
  lua_assert((osz == 0) == (p == NULL));
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lua_assert((nsz == 0) == (p == NULL));
  lua_assert(checkptr32(p));
  g->gc.total = (g->gc.total - osz) + nsz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, MSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lua_assert(checkptr32(o));
  g->gc.total += size;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  return o;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

