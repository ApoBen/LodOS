"""lodos-vk-bridge RPC codegen (called from gen.py). Emits a request/reply layer on top of vkb_wire.h.

files: vkb_rpc.h          ids, client API protos (vkbc_<cmd>), server fn table + dispatch
       vkb_rpc_common.c   struct codecs (vkrw_<S> encode / vkrr_<S> bounds-checked decode)
       vkb_rpc_client.c   vkbc_<cmd>: encode request -> vkbc__call() -> decode reply into out params
       vkb_rpc_server.c   vkr_srv_dispatch(): decode request -> call driver fn -> encode reply

Request:  u32 cmd, params in order:
  in value/handle/struct/string/array : as gen.py wire format (handles -> u64 id via vkr_h_out)
  pNext                               : u8 present (server rejects !=0 with VK_ERROR_FEATURE_NOT_PRESENT)
  pAllocator                          : u8 present only, server passes NULL
  out single (non-const T*)           : u8 present
  in/out count (T* used as len)       : u8 present [+ value]
  out array (len=countptr)            : u8 present [+ u32 capacity]
Reply:    u32 cmd, i32 VkResult (void cmds: VK_SUCCESS or bridge error); if result >= 0, per out param
  in order: u8 present [+ value] / arrays: u8 present [+ u32 n (<= capacity) + n elems].
Handle map: server ids are table indices (vkr_h_out inserts, vkr_h_in looks up, vkDestroy* drops).
Two-call idiom: count is in/out, array gets capacity = *count at call time; VK_INCOMPLETE is >= 0.
Out structs must be flat (no pointers / pNext) in v0.
"""
import os

HDR_CMD_BASE = 1  # cmd ids follow vkb_cmds.h enum


def emit(reg, Gen, Unsupported, cmds, out, hdr):
    g = Gen(reg)
    R = reg

    def objtype(base):
        return R.objtype.get(R.res(base), 'VK_OBJECT_TYPE_UNKNOWN')

    def flat(s, seen=()):
        for m in R.structs[s]:
            if m.depth or m.name == 'pNext' or m.bitfield:
                return False
            b = R.res(m.base)
            if R.cat.get(b) == 'struct' and b not in seen and not flat(b, seen + (s,)):
                return False
        return True

    def lenlist(ps):
        return {p.len[0] for p in ps if p.len}

    def dirn(p, ps):
        """'in' | 'out' | 'inout'"""
        k, ek = g.classify(p, True)
        if p.depth == 1 and not p.const_outer and k in ('single', 'array'):
            if k == 'single' and ek == 'scalar' and p.name in lenlist(ps):
                return 'inout'
            return 'out'
        return 'in'

    # ---- per-element codecs -------------------------------------------
    def enc_val(k, base, ex):
        if k in ('scalar', 'union'):
            return ['vkr_put(w, &(%s), sizeof(%s));' % (ex, base)]
        if k == 'size':
            return ['vkr_put_u64(w, (uint64_t)(%s));' % ex]
        if k == 'handle':
            return ['vkr_put_u64(w, vkr_h_out(w->hctx, %s, (uint64_t)(uintptr_t)(%s)));' % (objtype(base), ex)]
        if k == 'struct':
            return ['vkrw_%s(w, &(%s));' % (base, ex)]
        if k == 'funcptr':
            return []
        raise AssertionError(k)

    def dec_val(k, base, lv):
        if k in ('scalar', 'union'):
            return ['vkr_get(r, &(%s), sizeof(%s));' % (lv, base)]
        if k == 'size':
            return ['%s = (size_t)vkr_get_u64(r);' % lv]
        if k == 'handle':
            return ['{ int ok_ = 1; uint64_t id_ = vkr_h_in(r->hctx, %s, vkr_get_u64(r), &ok_);' % objtype(base),
                    '  if (!ok_) { vkr_fail(&r->err, VKR_E_HANDLE); } %s = (%s)(uintptr_t)id_; }' % (lv, base)]
        if k == 'struct':
            return ['vkrr_%s(r, &(%s));' % (base, lv)]
        if k == 'funcptr':
            return ['%s = NULL;' % lv]
        raise AssertionError(k)

    def min_wire(k, base):
        if k in ('scalar', 'union'):
            return 'sizeof(%s)' % base
        if k in ('size', 'handle'):
            return '8'
        if k == 'strarray':
            return '1'
        return '0'

    def enc_in(d, ex, cnt, is_param):
        """encode an input value (struct member or in-param)."""
        k, ek = g.classify(d, is_param)
        base = R.res(d.base)
        if k == 'value':
            return enc_val(ek, base, ex)
        if k == 'fixed':
            if ek == 'scalar':
                return ['vkr_put(w, &(%s), sizeof(%s));' % (ex, ex)]
            i = 'i%d' % g.u()
            return ['for (size_t %s = 0; %s < sizeof(%s) / sizeof((%s)[0]); %s++) {' % (i, i, ex, ex, i)] + \
                ['    ' + x for x in enc_val(ek, base, '(%s)[%s]' % (ex, i))] + ['}']
        if k in ('pnext', 'skip_alloc'):
            return ['vkr_put_u8(w, (%s) != NULL);' % ex]
        if k == 'string':
            return ['vkr_put_str(w, %s);' % ex]
        if k == 'single':
            return ['vkr_put_u8(w, (%s) != NULL);' % ex, 'if (%s) {' % ex] + \
                ['    ' + x for x in enc_val(ek, base, '(*(%s))' % ex)] + ['}']
        if k in ('array', 'strarray'):
            i = 'i%d' % g.u()
            body = ['vkr_put_str(w, (%s)[%s]);' % (ex, i)] if k == 'strarray' else \
                enc_val(ek, base, '(%s)[%s]' % (ex, i))
            return ['vkr_put_u8(w, (%s) != NULL);' % ex,
                    'if (%s) { uint32_t c_ = (uint32_t)(%s); vkr_put_u32(w, c_);' % (ex, cnt(d.len[0])),
                    '    for (uint32_t %s = 0; %s < c_; %s++) {' % (i, i, i)] + \
                ['        ' + x for x in body] + ['    } }']
        raise AssertionError(k)

    def dec_in(d, lv, is_param, nvar):
        """decode an input value; arrays store wire count in nvar (for count checks)."""
        k, ek = g.classify(d, is_param)
        base = R.res(d.base)
        if k == 'value':
            return dec_val(ek, base, lv)
        if k == 'fixed':
            if ek == 'scalar':
                return ['vkr_get(r, &(%s), sizeof(%s));' % (lv, lv)]
            i = 'i%d' % g.u()
            return ['for (size_t %s = 0; %s < sizeof(%s) / sizeof((%s)[0]); %s++) {' % (i, i, lv, lv, i)] + \
                ['    ' + x for x in dec_val(ek, base, '(%s)[%s]' % (lv, i))] + ['}']
        if k == 'pnext':
            return ['if (vkr_get_u8(r)) vkr_fail(&r->err, VKR_E_PNEXT);', '%s = NULL;' % lv]
        if k == 'skip_alloc':
            return ['(void)vkr_get_u8(r);', '%s = NULL;' % lv]
        if k == 'string':
            return ['%s = vkr_get_str(r);' % lv]
        et = g.elem_ctype(d)
        if k == 'single':
            return ['if (vkr_get_u8(r)) { %s *e_ = (%s *)vkr_alloc(r, sizeof(%s));' % (et, et, et),
                    '    if (e_) {'] + ['        ' + x for x in dec_val(ek, base, '(*e_)')] + \
                ['    }', '    %s = (void *)e_; } else %s = NULL;' % (lv, lv)]
        if k in ('array', 'strarray'):
            i = 'i%d' % g.u()
            body = ['e_[%s] = vkr_get_str(r);' % i] if k == 'strarray' else dec_val(ek, base, 'e_[%s]' % i)
            return ['if (vkr_get_u8(r)) { uint32_t c_ = vkr_get_u32(r); %s = c_;' % nvar,
                    '    %s *e_ = (%s *)vkr_alloc_arr(r, c_, sizeof(%s), %s);' % (et, et, et, min_wire(k if k == 'strarray' else ek, base)),
                    '    if (e_) for (uint32_t %s = 0; %s < c_ && !r->err; %s++) {' % (i, i, i)] + \
                ['        ' + x for x in body] + \
                ['    }', '    %s = (void *)e_; } else %s = NULL;' % (lv, lv)]
        raise AssertionError(k)

    def count_checks(decls, acc, nv, is_param):
        """wire array count must equal the declared count member/param (scalar, simple name)."""
        names = {d.name: d for d in decls}
        res = []
        for d in decls:
            k, _ = g.classify(d, is_param)
            if k not in ('array', 'strarray'):
                continue
            ln = names.get(d.len[0])
            if ln is None or ln.depth != 0:
                continue
            res.append('if (%s && %s != (uint32_t)(%s)) vkr_fail(&r->err, VKR_E_COUNT);' %
                       (acc(d.name), nv(d.name), acc(ln.name)))
        return res

    # ---- pick commands -------------------------------------------------
    ok, bad = [], {}
    for c in cmds:
        ps = R.cmds[c]
        try:
            if R.ret[c] not in ('VkResult', 'void'):
                raise Unsupported('return type %s' % R.ret[c])
            for p in ps:
                k, ek = g.classify(p, True)
                dr = dirn(p, ps)
                if dr == 'out':
                    if ek == 'struct' and not flat(R.res(p.base)):
                        raise Unsupported('out struct %s not flat' % R.res(p.base))
                    if k == 'array':
                        ln = [q for q in ps if q.name == p.len[0]]
                        if not ln or (ln[0].depth and dirn(ln[0], ps) != 'inout'):
                            raise Unsupported('out array %s len %s' % (p.name, p.len[0]))
            ok.append(c)
        except Unsupported as ex:
            bad[c] = str(ex)
    structs = list(g.used_structs)

    # ---- vkb_rpc.h ------------------------------------------------------
    def cproto(c, prefix, extra):
        ps = [extra] + ['%s %s' % (p.ctype, p.name) for p in R.cmds[c]]
        return 'VkResult %s%s(%s)' % (prefix, c, ', '.join(ps))

    h = [hdr, '#ifndef VKB_RPC_H', '#define VKB_RPC_H', '#include "vkb_wire.h"', '#include "vkb_cmds.h"', '']
    for s in structs:
        h += ['void vkrw_%s(vkr_wr *w, const %s *s);' % (s, s), 'void vkrr_%s(vkr_rd *r, %s *s);' % (s, s)]
    h += ['', '/* ---- client (implemented by client lib): transport hooks */',
          'typedef struct vkbc vkbc;',
          'uint8_t *vkbc__buf(vkbc *c, size_t *cap);',
          '/* send req (in vkbc__buf), wait reply; returns 0 or -errno */',
          'int vkbc__call(vkbc *c, size_t len, const uint8_t **rep, size_t *replen);',
          '/* client API: same params as Vulkan, prefixed with vkbc *. void cmds return VK_SUCCESS/bridge error */']
    for c in ok:
        h.append(cproto(c, 'vkbc_', 'vkbc *c') + ';')
    for c, why in bad.items():
        h.append('/* %s: not bridged: %s */' % (c, why))
    h += ['', '/* ---- server */', 'typedef struct {']
    h += ['    PFN_%s %s;' % (c, c) for c in ok]
    h += ['} vkr_srv_fns;',
          '/* fills table via getproc(name); returns number of missing fns */',
          'int vkr_srv_load(vkr_srv_fns *f, void *(*getproc)(const char *name, void *ud), void *ud);',
          '/* decode req, call, encode reply into rep; returns reply length (0 = unknown cmd / no room) */',
          'size_t vkr_srv_dispatch(const vkr_srv_fns *f, void *hctx, vkr_arena *a,',
          '                        const uint8_t *req, size_t len, uint8_t *rep, size_t cap);',
          '', '#endif', '']
    open(os.path.join(out, 'vkb_rpc.h'), 'w').write('\n'.join(h))

    # ---- vkb_rpc_common.c ----------------------------------------------
    m = [hdr, '#include "vkb_rpc.h"', '']
    for s in structs:
        mem = R.structs[s]
        m.append('void vkrw_%s(vkr_wr *w, const %s *s) {' % (s, s))
        for d in mem:
            m += ['    ' + x for x in enc_in(d, 's->%s' % d.name, lambda nm: 's->%s' % nm, False)]
        m += ['}', '']
        m.append('void vkrr_%s(vkr_rd *r, %s *s) {' % (s, s))
        arrs = [d for d in mem if g.classify(d, False)[0] in ('array', 'strarray')]
        for d in arrs:
            m.append('    uint32_t n_%s = 0;' % d.name)
        for d in mem:
            m += ['    ' + x for x in dec_in(d, 's->%s' % d.name, False, 'n_%s' % d.name)]
        m += ['    ' + x for x in count_checks(mem, lambda nm: 's->%s' % nm, lambda nm: 'n_%s' % nm, False)]
        m += ['}', '']
    open(os.path.join(out, 'vkb_rpc_common.c'), 'w').write('\n'.join(m))

    def cnt_expr(ps, nm):
        isptr = {p.name: p.depth > 0 for p in ps}
        return ('((%s) ? *(%s) : 0)' % (nm, nm)) if isptr.get(nm) else nm

    # ---- vkb_rpc_client.c ----------------------------------------------
    m = [hdr, '#include "vkb_rpc.h"', '',
         'uint64_t vkr_h_out(void *hctx, VkObjectType t, uint64_t h) { (void)hctx; (void)t; return h; }',
         'uint64_t vkr_h_in(void *hctx, VkObjectType t, uint64_t id, int *ok) { (void)hctx; (void)t; *ok = 1; return id; }',
         'void vkr_h_drop(void *hctx, VkObjectType t, uint64_t h) { (void)hctx; (void)t; (void)h; }', '']
    for c in ok:
        ps = R.cmds[c]
        cnt = lambda nm, ps=ps: cnt_expr(ps, nm)
        m += [cproto(c, 'vkbc_', 'vkbc *c') + ' {']
        outs = [p for p in ps if dirn(p, ps) != 'in']
        for p in outs:
            if g.classify(p, True)[0] == 'array':
                m.append('    uint32_t cap_%s = (uint32_t)(%s);' % (p.name, cnt(p.len[0])))
        m += ['    size_t bcap; uint8_t *buf = vkbc__buf(c, &bcap);',
              '    vkr_wr w_ = { buf, bcap, 0, 0, NULL }, *w = &w_;',
              '    vkr_put_u32(w, VKB_CMD_%s);' % c]
        for p in ps:
            dr = dirn(p, ps)
            if dr == 'in':
                m += ['    ' + x for x in enc_in(p, p.name, cnt, True)]
            elif dr == 'inout':
                m += ['    vkr_put_u8(w, %s != NULL);' % p.name,
                      '    if (%s) vkr_put(w, %s, sizeof(*%s));' % (p.name, p.name, p.name)]
            elif g.classify(p, True)[0] == 'single':
                m.append('    vkr_put_u8(w, %s != NULL);' % p.name)
            else:
                m += ['    vkr_put_u8(w, %s != NULL);' % p.name,
                      '    if (%s) vkr_put_u32(w, cap_%s);' % (p.name, p.name)]
        m += ['    if (w->err) return VK_ERROR_OUT_OF_HOST_MEMORY;',
              '    const uint8_t *rep; size_t replen;',
              '    if (vkbc__call(c, w->off, &rep, &replen) < 0) return VK_ERROR_DEVICE_LOST;',
              '    vkr_rd r_ = { rep, replen, 0, 0, NULL, NULL }, *r = &r_;',
              '    if (vkr_get_u32(r) != VKB_CMD_%s) return VK_ERROR_UNKNOWN;' % c,
              '    VkResult res; vkr_get(r, &res, sizeof res);',
              '    if (r->err) return VK_ERROR_UNKNOWN;',
              '    if (res < 0) return res;']
        for p in outs:
            k, ek = g.classify(p, True)
            base = R.res(p.base)
            m += ['    { uint8_t p_ = vkr_get_u8(r);',
                  '      if (p_ != (%s != NULL)) return VK_ERROR_UNKNOWN;' % p.name]
            if k == 'single':
                m += ['      if (p_) {'] + ['          ' + x for x in dec_val(ek, base, '(*%s)' % p.name)] + ['      } }']
            else:
                i = 'i%d' % g.u()
                m += ['      if (p_) { uint32_t n_ = vkr_get_u32(r);',
                      '          if (n_ > cap_%s) return VK_ERROR_UNKNOWN;' % p.name,
                      '          for (uint32_t %s = 0; %s < n_ && !r->err; %s++) {' % (i, i, i)] + \
                    ['              ' + x for x in dec_val(ek, base, '%s[%s]' % (p.name, i))] + \
                    ['          } } }']
        m += ['    if (r->err) return VK_ERROR_UNKNOWN;', '    return res;', '}', '']
    open(os.path.join(out, 'vkb_rpc_client.c'), 'w').write('\n'.join(m))

    # ---- vkb_rpc_server.c ----------------------------------------------
    m = [hdr, '#include "vkb_rpc.h"', '',
         'int vkr_srv_load(vkr_srv_fns *f, void *(*getproc)(const char *name, void *ud), void *ud) {',
         '    int miss = 0;']
    for c in ok:
        m.append('    if (!(f->%s = (PFN_%s)getproc("%s", ud))) miss++;' % (c, c, c))
    m += ['    return miss;', '}', '',
          'static VkResult vkr_err_res(int e) { return e == VKR_E_PNEXT ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_UNKNOWN; }', '']
    for c in ok:
        ps = R.cmds[c]
        m += ['static void srv_%s(const vkr_srv_fns *f, vkr_rd *r, vkr_wr *w) {' % c]
        for p in ps:
            m.append('    %s %s = 0;' % (p.ctype, p.name))
            if g.classify(p, True)[0] in ('array', 'strarray'):
                m.append('    uint32_t n_%s = 0; (void)n_%s;' % (p.name, p.name))
        for p in ps:
            dr = dirn(p, ps)
            k, ek = g.classify(p, True)
            et = g.elem_ctype(p) if p.depth else None
            if dr == 'in':
                m += ['    ' + x for x in dec_in(p, p.name, True, 'n_%s' % p.name)]
            elif dr == 'inout':
                m += ['    if (vkr_get_u8(r)) { %s = (%s *)vkr_alloc(r, sizeof(%s));' % (p.name, et, et),
                      '        if (%s) vkr_get(r, %s, sizeof(*%s)); }' % (p.name, p.name, p.name)]
            elif k == 'single':
                m.append('    if (vkr_get_u8(r)) %s = (%s *)vkr_alloc(r, sizeof(%s));' % (p.name, et, et))
            else:
                m += ['    if (vkr_get_u8(r)) { n_%s = vkr_get_u32(r);' % p.name,
                      '        if (n_%s > VKR_MAX_ARR) vkr_fail(&r->err, VKR_E_BOUNDS);' % p.name,
                      '        %s = (%s *)vkr_alloc_arr(r, n_%s, sizeof(%s), 0); }' % (p.name, et, p.name, et)]
                # client capacity must match the in/out count it sent
                ln = p.len[0]
                if [q for q in ps if q.name == ln and q.depth]:
                    m.append('    if (%s && (!%s || *%s > n_%s)) vkr_fail(&r->err, VKR_E_COUNT);' % (p.name, ln, ln, p.name))
        m += ['    ' + x for x in count_checks(ps, lambda nm: nm, lambda nm: 'n_%s' % nm, True)]
        m += ['    vkr_put_u32(w, VKB_CMD_%s);' % c,
              '    if (r->err) { VkResult e_ = vkr_err_res(r->err); vkr_put(w, &e_, sizeof e_); return; }']
        args = ', '.join(p.name for p in ps)
        if R.ret[c] == 'VkResult':
            m.append('    VkResult res = f->%s(%s);' % (c, args))
        else:
            m += ['    f->%s(%s);' % (c, args), '    VkResult res = VK_SUCCESS;']
        if c.startswith('vkDestroy') or c.startswith('vkFree'):
            obj = 'Vk' + c[len('vkDestroy'):] if c.startswith('vkDestroy') else None
            for p in ps:
                if p.depth == 0 and R.res(p.base) == obj and g.classify(p, True)[1] == 'handle':
                    m.append('    vkr_h_drop(w->hctx, %s, (uint64_t)(uintptr_t)%s);' % (objtype(p.base), p.name))
        m += ['    vkr_put(w, &res, sizeof res);', '    if (res < 0) return;']
        for p in ps:
            dr = dirn(p, ps)
            if dr == 'in':
                continue
            k, ek = g.classify(p, True)
            base = R.res(p.base)
            m.append('    vkr_put_u8(w, %s != NULL);' % p.name)
            if k == 'single':
                m += ['    if (%s) {' % p.name] + ['        ' + x for x in enc_val(ek, base, '(*%s)' % p.name)] + ['    }']
            else:
                i = 'i%d' % g.u()
                m += ['    if (%s) { uint32_t c_ = (uint32_t)(%s); if (c_ > n_%s) c_ = n_%s; vkr_put_u32(w, c_);' %
                      (p.name, cnt_expr(ps, p.len[0]), p.name, p.name),
                      '        for (uint32_t %s = 0; %s < c_; %s++) {' % (i, i, i)] + \
                    ['            ' + x for x in enc_val(ek, base, '%s[%s]' % (p.name, i))] + ['        } }']
        m += ['}', '']
    m += ['size_t vkr_srv_dispatch(const vkr_srv_fns *f, void *hctx, vkr_arena *a,',
          '                        const uint8_t *req, size_t len, uint8_t *rep, size_t cap) {',
          '    vkr_rd r = { req, len, 0, 0, a, hctx };',
          '    vkr_wr w = { rep, cap, 0, 0, hctx };',
          '    uint32_t cmd = vkr_get_u32(&r);',
          '    if (r.err) return 0;',
          '    switch (cmd) {']
    for c in ok:
        m.append('    case VKB_CMD_%s: if (!f->%s) return 0; srv_%s(f, &r, &w); break;' % (c, c, c))
    m += ['    default: return 0;', '    }', '    return w.err ? 0 : w.off;', '}', '']
    open(os.path.join(out, 'vkb_rpc_server.c'), 'w').write('\n'.join(m))

    print('rpcgen: %d cmds bridged, %d not, %d structs' % (len(ok), len(bad), len(structs)))
    for c, why in bad.items():
        print('rpcgen: skip %s: %s' % (c, why))
