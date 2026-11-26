#!/usr/bin/env python3
"""lodos-vk-bridge codegen v0: Khronos vk.xml -> C marshal code.

usage: gen.py <vk.xml> <cmds.txt> <outdir>
emits: vkb_cmds.h, vkb_marshal.h, vkb_marshal.c, vkb_test_gen.c (fill/eq/roundtrip helpers)
       + RPC layer (rpcgen.py): vkb_rpc.h, vkb_rpc_common.c, vkb_rpc_client.c, vkb_rpc_server.c

Wire format (host-endian, 64-bit host assumed):
  cmd header: u32 VKB_CMD_<cmd>
  scalar/enum/flags: raw sizeof(T); size_t: u64; handle: u64
  pointer (single):  u8 present [+ element]
  pointer (len):     u8 present [+ u32 count + count*element]
  char* string:      u8 present [+ u32 len(incl NUL) + bytes]
  fixed array:       elements (raw block if scalar)
  struct:            members in order; pNext must be NULL (chains TODO -> assert)
  union:             raw sizeof(T) (no pointers inside assumed)
  funcpointer:       not encoded, decoded as NULL
  pAllocator param:  u8 present only, decoded as NULL (guest allocator unusable on host)
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

N_FILL = 3  # element count used by generated fill helpers


def api_ok(e):
    a = e.get('api')
    return a is None or 'vulkan' in a.split(',')


class Unsupported(Exception):
    pass


class Decl:
    """member or param declaration."""

    def __init__(self, e):
        self.name = e.find('name').text
        self.base = e.find('type').text
        pre = e.text or ''
        mid = e.find('type').tail or ''
        post = e.find('name').tail or ''
        self.const_outer = 'const' in pre
        self.depth = mid.count('*')
        self.inner_const = 'const' in mid  # e.g. "* const*"
        # fixed array dims: text + <enum> children after <name>
        after = post
        seen_name = False
        for c in e:
            if c.tag == 'name':
                seen_name = True
                continue
            if seen_name and c.tag != 'comment':
                after += (c.text or '') + (c.tail or '')
            elif seen_name:
                after += c.tail or ''
        self.dims = re.findall(r'\[([^\]]+)\]', after)
        self.bitfield = ':' in after
        self.len = [x for x in (e.get('len') or '').split(',') if x]
        self.values = e.get('values')
        # C type text (no name, no dims)
        self.ctype = ' '.join((pre + self.base + mid).split())


class Reg:
    def __init__(self, path):
        root = ET.parse(path).getroot()
        self.cat = {}
        self.alias = {}
        self.objtype = {}
        self.ret = {}
        self.structs = {}
        self.structextends = {}
        self.core_types = set()
        for f in root.findall('feature'):
            if api_ok(f):
                for req in f.findall('require'):
                    for tp in req.findall('type'):
                        self.core_types.add(tp.get('name'))
        for ext in root.find('extensions').findall('extension'):
            supp = (ext.get('supported') or '').split(',')
            if 'vulkan' in supp and not ext.get('platform') and not ext.get('provisional'):
                for req in ext.findall('require'):
                    if api_ok(req):
                        for tp in req.findall('type'):
                            self.core_types.add(tp.get('name'))
        for t in root.find('types').findall('type'):
            if not api_ok(t):
                continue
            name = t.get('name') or (t.find('name').text if t.find('name') is not None else None)
            if not name:
                continue
            if t.get('alias'):
                self.alias[name] = t.get('alias')
                continue
            self.cat[name] = t.get('category')
            if t.get('objtypeenum'):
                self.objtype[name] = t.get('objtypeenum')
            if t.get('category') in ('struct', 'union'):
                self.structs[name] = [Decl(m) for m in t.findall('member') if api_ok(m)]
                if t.get('structextends'):
                    self.structextends[name] = [x for x in t.get('structextends').split(',') if x]
        self.cmds = {}
        for c in root.iter('command'):
            if not api_ok(c):
                continue
            if c.get('alias'):
                continue
            proto = c.find('proto')
            if proto is None:
                continue
            nm = proto.find('name').text
            self.cmds[nm] = [Decl(p) for p in c.findall('param') if api_ok(p)]
            self.ret[nm] = proto.find('type').text

    def res(self, t):
        while t in self.alias:
            t = self.alias[t]
        return t

    def kind(self, t):
        t = self.res(t)
        if t == 'size_t':
            return 'size'
        if t == 'void':
            raise Unsupported('void value')
        c = self.cat.get(t)
        if c in (None, 'basetype', 'enum', 'bitmask'):
            return 'scalar'
        if c == 'handle':
            return 'handle'
        if c == 'struct':
            return 'struct'
        if c == 'union':
            return 'union'
        if c == 'funcpointer':
            return 'funcptr'
        raise Unsupported('type %s category %s' % (t, c))


class Gen:
    def __init__(self, reg):
        self.r = reg
        self.used_structs = []  # topo order (deps first)
        self.bad_structs = {}
        self.uid = 0

    def u(self):
        self.uid += 1
        return self.uid

    # ---- classification -------------------------------------------------
    def classify(self, d, is_param):
        """returns (kind, info) or raises Unsupported."""
        r = self.r
        if d.bitfield:
            raise Unsupported('bitfield member %s' % d.name)
        if d.depth == 0:
            if len(d.dims) > 1:
                raise Unsupported('multi-dim array %s' % d.name)
            k = r.kind(d.base)
            if k == 'struct':
                self.need_struct(r.res(d.base))
            if d.dims:
                return ('fixed', k)
            return ('value', k)
        if d.name == 'pNext':
            return ('pnext', None)
        if is_param and d.name == 'pAllocator':
            return ('skip_alloc', None)
        if d.dims:
            raise Unsupported('array of pointers %s' % d.name)
        ln = d.len
        if ln and ('latexmath' in ln[0] or ln[0].startswith('(')):
            raise Unsupported('computed len %s' % ln[0])
        if d.depth == 1:
            if r.res(d.base) == 'char' and ln and ln[0] == 'null-terminated':
                return ('string', None)
            k = r.kind(d.base)  # raises for void*
            if k == 'struct':
                self.need_struct(r.res(d.base))
            if k == 'funcptr':
                raise Unsupported('pointer to funcpointer %s' % d.name)
            if ln:
                return ('array', k)
            return ('single', k)
        if d.depth == 2 and r.res(d.base) == 'char' and len(ln) == 2 and ln[1] == 'null-terminated':
            return ('strarray', None)
        raise Unsupported('pointer depth %d %s' % (d.depth, d.name))

    def need_struct(self, s):
        if s in self.used_structs:
            return
        if s in self.bad_structs:
            raise Unsupported('struct %s: %s' % (s, self.bad_structs[s]))
        if self.r.cat.get(s) != 'struct':
            raise Unsupported('not a struct %s' % s)
        try:
            for m in self.r.structs[s]:
                self.classify(m, False)
        except Unsupported as ex:
            self.bad_structs[s] = str(ex)
            raise Unsupported('struct %s: %s' % (s, ex))
        if s not in self.used_structs:
            self.used_structs.append(s)

    def elem_ctype(self, d):
        if d.depth == 1:
            return self.r.res(d.base)
        return ('const ' if d.const_outer else '') + self.r.res(d.base) + '*'

    # ---- encode ---------------------------------------------------------
    def enc_val(self, k, base, ex):
        if k == 'scalar':
            return ['VKB_PUT(b, n, &(%s), sizeof(%s));' % (ex, base)]
        if k == 'size' or k == 'handle':
            cast = '(uint64_t)(uintptr_t)' if k == 'handle' else '(uint64_t)'
            return ['{ uint64_t t_ = %s(%s); VKB_PUT(b, n, &t_, 8); }' % (cast, ex)]
        if k == 'struct':
            return ['n += vkb_enc_%s(b ? b + n : NULL, &(%s));' % (base, ex)]
        if k == 'union':
            return ['VKB_PUT(b, n, &(%s), sizeof(%s));' % (ex, base)]
        if k == 'funcptr':
            return ['/* funcpointer %s not encoded */' % ex]
        raise AssertionError(k)

    def enc_str(self, ex):
        return ['{ uint8_t p_ = (%s) != NULL; VKB_PUT(b, n, &p_, 1);' % ex,
                '  if (p_) { uint32_t l_ = (uint32_t)strlen(%s) + 1; VKB_PUT(b, n, &l_, 4); VKB_PUT(b, n, (%s), l_); } }' % (ex, ex)]

    def enc(self, d, ex, cnt, is_param):
        k, ek = self.classify(d, is_param)
        base = self.r.res(d.base)
        if k == 'value':
            return self.enc_val(ek, base, ex)
        if k == 'fixed':
            if ek == 'scalar':
                return ['VKB_PUT(b, n, &(%s), sizeof(%s));' % (ex, ex)]
            i = 'i%d' % self.u()
            return ['for (size_t %s = 0; %s < sizeof(%s) / sizeof((%s)[0]); %s++) {' % (i, i, ex, ex, i)] + \
                ['    ' + x for x in self.enc_val(ek, base, '(%s)[%s]' % (ex, i))] + ['}']
        if k == 'pnext':
            return ['assert((%s) == NULL && "vkb: pNext chains TODO");' % ex]
        if k == 'skip_alloc':
            return ['{ uint8_t p_ = (%s) != NULL; VKB_PUT(b, n, &p_, 1); } /* pAllocator: presence only */' % ex]
        if k == 'string':
            return self.enc_str(ex)
        if k == 'single':
            return ['{ uint8_t p_ = (%s) != NULL; VKB_PUT(b, n, &p_, 1);' % ex, '  if (p_) {'] + \
                ['    ' + x for x in self.enc_val(ek, base, '(*(%s))' % ex)] + ['  } }']
        if k in ('array', 'strarray'):
            i = 'i%d' % self.u()
            body = self.enc_str('(%s)[%s]' % (ex, i)) if k == 'strarray' else \
                self.enc_val(ek, base, '(%s)[%s]' % (ex, i))
            return ['{ uint8_t p_ = (%s) != NULL; VKB_PUT(b, n, &p_, 1);' % ex,
                    '  if (p_) { uint32_t c_ = (uint32_t)(%s); VKB_PUT(b, n, &c_, 4);' % cnt(d.len[0]),
                    '    for (uint32_t %s = 0; %s < c_; %s++) {' % (i, i, i)] + \
                ['      ' + x for x in body] + ['    } } }']
        raise AssertionError(k)

    # ---- decode ---------------------------------------------------------
    def dec_val(self, k, base, lv):
        if k == 'scalar' or k == 'union':
            return ['VKB_GET(b, n, &(%s), sizeof(%s));' % (lv, base)]
        if k == 'size':
            return ['{ uint64_t t_; VKB_GET(b, n, &t_, 8); %s = (size_t)t_; }' % lv]
        if k == 'handle':
            return ['{ uint64_t t_; VKB_GET(b, n, &t_, 8); %s = (%s)(uintptr_t)t_; }' % (lv, base)]
        if k == 'struct':
            return ['n += vkb_dec_%s(b + n, a, &(%s));' % (base, lv)]
        if k == 'funcptr':
            return ['%s = NULL;' % lv]
        raise AssertionError(k)

    def dec_str(self, lv):
        return ['{ uint8_t p_; VKB_GET(b, n, &p_, 1);',
                '  if (p_) { uint32_t l_; VKB_GET(b, n, &l_, 4); char *s_ = (char *)vkb_arena_alloc(a, l_);',
                '    VKB_GET(b, n, s_, l_); s_[l_ ? l_ - 1 : 0] = 0; %s = s_; } else %s = NULL; }' % (lv, lv)]

    def dec(self, d, lv, is_param):
        k, ek = self.classify(d, is_param)
        base = self.r.res(d.base)
        if k == 'value':
            return self.dec_val(ek, base, lv)
        if k == 'fixed':
            if ek == 'scalar':
                return ['VKB_GET(b, n, &(%s), sizeof(%s));' % (lv, lv)]
            i = 'i%d' % self.u()
            return ['for (size_t %s = 0; %s < sizeof(%s) / sizeof((%s)[0]); %s++) {' % (i, i, lv, lv, i)] + \
                ['    ' + x for x in self.dec_val(ek, base, '(%s)[%s]' % (lv, i))] + ['}']
        if k == 'pnext' or k == 'skip_alloc':
            pre = ['{ uint8_t p_; VKB_GET(b, n, &p_, 1); (void)p_; }'] if k == 'skip_alloc' else []
            return pre + ['%s = NULL;' % lv]
        if k == 'string':
            return self.dec_str(lv)
        et = self.elem_ctype(d)
        if k == 'single':
            return ['{ uint8_t p_; VKB_GET(b, n, &p_, 1);',
                    '  if (p_) { %s *e_ = (%s *)vkb_arena_alloc(a, sizeof(%s));' % (et, et, et)] + \
                ['    ' + x for x in self.dec_val(ek, base, '(*e_)')] + \
                ['    %s = (void *)e_; } else %s = NULL; }' % (lv, lv)]
        if k in ('array', 'strarray'):
            i = 'i%d' % self.u()
            body = self.dec_str('e_[%s]' % i) if k == 'strarray' else self.dec_val(ek, base, 'e_[%s]' % i)
            return ['{ uint8_t p_; VKB_GET(b, n, &p_, 1);',
                    '  if (p_) { uint32_t c_; VKB_GET(b, n, &c_, 4);',
                    '    %s *e_ = (%s *)vkb_arena_alloc(a, (size_t)c_ * sizeof(%s));' % (et, et, et),
                    '    for (uint32_t %s = 0; %s < c_; %s++) {' % (i, i, i)] + \
                ['      ' + x for x in body] + \
                ['    }', '    %s = (void *)e_; } else %s = NULL; }' % (lv, lv)]
        raise AssertionError(k)

    # ---- test helpers: fill / eq ----------------------------------------
    def fill_val(self, k, base, lv):
        if k in ('scalar', 'size', 'handle', 'union'):
            return ['vkb_fill_bytes(&(%s), sizeof(%s), seed);' % (lv, lv)]
        if k == 'struct':
            return ['fill_%s(&(%s), a, seed);' % (base, lv)]
        if k == 'funcptr':
            return ['%s = NULL;' % lv]
        raise AssertionError(k)

    def fill(self, d, lv, setcnt, is_param):
        """setcnt(lenname) -> lines setting the count lvalue to N_FILL (post pass)."""
        k, ek = self.classify(d, is_param)
        base = self.r.res(d.base)
        if d.values and d.name == 'sType':
            return ['%s = %s;' % (lv, d.values.split(',')[0])], []
        if k == 'value':
            return self.fill_val(ek, base, lv), []
        if k == 'fixed':
            if ek == 'scalar':
                return ['vkb_fill_bytes(&(%s), sizeof(%s), seed);' % (lv, lv)], []
            i = 'i%d' % self.u()
            return ['for (size_t %s = 0; %s < sizeof(%s) / sizeof((%s)[0]); %s++) {' % (i, i, lv, lv, i)] + \
                ['    ' + x for x in self.fill_val(ek, base, '(%s)[%s]' % (lv, i))] + ['}'], []
        if k in ('pnext', 'skip_alloc'):
            return ['%s = NULL;' % lv], []
        if k == 'string':
            return ['%s = vkb_fill_str(a, seed);' % lv], []
        et = self.elem_ctype(d)
        if k == 'single':
            return ['{ %s *e_ = (%s *)vkb_arena_alloc(a, sizeof(%s));' % (et, et, et)] + \
                ['  ' + x for x in self.fill_val(ek, base, '(*e_)')] + ['  %s = (void *)e_; }' % lv], []
        i = 'i%d' % self.u()
        body = ['e_[%s] = vkb_fill_str(a, seed);' % i] if k == 'strarray' else \
            self.fill_val(ek, base, 'e_[%s]' % i)
        return ['{ %s *e_ = (%s *)vkb_arena_alloc(a, %d * sizeof(%s));' % (et, et, N_FILL, et),
                '  for (uint32_t %s = 0; %s < %d; %s++) {' % (i, i, N_FILL, i)] + \
            ['    ' + x for x in body] + ['  }', '  %s = (void *)e_; }' % lv], setcnt(d.len[0])

    def eq_val(self, k, base, x, y):
        if k in ('scalar', 'union', 'size', 'handle'):
            return ['if (memcmp(&(%s), &(%s), sizeof(%s))) return 0;' % (x, y, x)]
        if k == 'struct':
            return ['if (!eq_%s(&(%s), &(%s))) return 0;' % (base, x, y)]
        if k == 'funcptr':
            return ['if ((%s) != NULL) return 0;' % y]
        raise AssertionError(k)

    def eq_str(self, x, y):
        return ['if (!vkb_streq(%s, %s)) return 0;' % (x, y)]

    def eq(self, d, x, y, cnt, is_param):
        k, ek = self.classify(d, is_param)
        base = self.r.res(d.base)
        if k in ('value', 'fixed') and ek == 'scalar':
            return ['if (memcmp(&(%s), &(%s), sizeof(%s))) return 0;' % (x, y, x)]
        if k == 'value':
            return self.eq_val(ek, base, x, y)
        if k == 'fixed':
            i = 'i%d' % self.u()
            return ['for (size_t %s = 0; %s < sizeof(%s) / sizeof((%s)[0]); %s++) {' % (i, i, x, x, i)] + \
                ['    ' + z for z in self.eq_val(ek, base, '(%s)[%s]' % (x, i), '(%s)[%s]' % (y, i))] + ['}']
        if k in ('pnext', 'skip_alloc'):
            return ['if ((%s) != NULL) return 0;' % y]
        if k == 'string':
            return self.eq_str(x, y)
        hdr = ['if (((%s) == NULL) != ((%s) == NULL)) return 0;' % (x, y)]
        if k == 'single':
            return hdr + ['if (%s) {' % x] + ['    ' + z for z in self.eq_val(ek, base, '(*(%s))' % x, '(*(%s))' % y)] + ['}']
        i = 'i%d' % self.u()
        xi, yi = '(%s)[%s]' % (x, i), '(%s)[%s]' % (y, i)
        body = self.eq_str(xi, yi) if k == 'strarray' else self.eq_val(ek, base, xi, yi)
        return hdr + ['if (%s) for (uint32_t %s = 0; %s < (uint32_t)(%s); %s++) {' % (x, i, i, cnt(d.len[0]), i)] + \
            ['    ' + z for z in body] + ['}']


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    xmlp, cmdsp, out = sys.argv[1:]
    reg = Reg(xmlp)
    g = Gen(reg)
    cmds = [l.split('#')[0].strip() for l in open(cmdsp)]
    cmds = [c for c in cmds if c]
    os.makedirs(out, exist_ok=True)

    ok, bad = [], {}
    for c in cmds:
        if c not in reg.cmds:
            sys.exit('gen.py: unknown command %s' % c)
        try:
            for p in reg.cmds[c]:
                g.classify(p, True)
            ok.append(c)
        except Unsupported as ex:
            bad[c] = str(ex)

    hdr = '/* generated by native/vk-bridge/gen/gen.py from vk.xml - do not edit */\n'

    # ---- vkb_cmds.h
    h = [hdr, '#ifndef VKB_CMDS_H', '#define VKB_CMDS_H', '', 'enum vkb_cmd {', '    VKB_CMD_INVALID = 0,']
    for c in cmds:
        h.append('    VKB_CMD_%s,%s' % (c, '' if c in ok else ' /* stub: %s */' % bad[c]))
    h += ['    VKB_CMD_COUNT', '};', '', 'static const char *const vkb_cmd_names[VKB_CMD_COUNT] = {', '    "INVALID",']
    h += ['    "%s",' % c for c in cmds]
    h += ['};', '', '#endif', '']
    open(os.path.join(out, 'vkb_cmds.h'), 'w').write('\n'.join(h))

    def proto_enc(c):
        ps = ['uint8_t *b'] + ['%s %s' % (p.ctype, p.name) for p in reg.cmds[c]]
        return 'size_t vkb_enc_%s(%s)' % (c, ', '.join(ps))

    def proto_dec(c):
        ps = ['const uint8_t *b', 'vkb_arena *a'] + ['%s *%s' % (p.ctype, p.name) for p in reg.cmds[c]]
        return 'size_t vkb_dec_%s(%s)' % (c, ', '.join(ps))

    # ---- vkb_marshal.h
    h = [hdr, '#ifndef VKB_MARSHAL_H', '#define VKB_MARSHAL_H', '#include <stddef.h>', '#include <stdint.h>',
         '#include <vulkan/vulkan.h>', '#include "vkb_cmds.h"', '',
         '/* bump allocator for decoded pointees; aborts on overflow (v0) */',
         'typedef struct { uint8_t *base; size_t cap, used; } vkb_arena;',
         'void *vkb_arena_alloc(vkb_arena *a, size_t sz);', '',
         '/* vkb_enc_*: b==NULL -> returns required size. returns bytes written.',
         '   vkb_dec_*: returns bytes consumed, 0 on cmd id mismatch. no input bounds checks (v0). */']
    for c in cmds:
        if c in ok:
            h += [proto_enc(c) + ';', proto_dec(c) + ';']
        else:
            h += ['/* %s: stub, unsupported: %s */' % (c, bad[c])]
    h += ['', '#endif', '']
    open(os.path.join(out, 'vkb_marshal.h'), 'w').write('\n'.join(h))

    # ---- vkb_marshal.c
    m = [hdr, '#include <assert.h>', '#include <stdlib.h>', '#include <string.h>', '#include "vkb_marshal.h"', '',
         '#define VKB_PUT(b, n, src, sz) do { if (b) memcpy((b) + (n), (src), (sz)); (n) += (sz); } while (0)',
         '#define VKB_GET(b, n, dst, sz) do { memcpy((dst), (b) + (n), (sz)); (n) += (sz); } while (0)', '',
         'void *vkb_arena_alloc(vkb_arena *a, size_t sz) {',
         '    size_t off = (a->used + 15) & ~(size_t)15;',
         '    if (sz == 0) sz = 1;',
         '    if (off + sz > a->cap) abort();',
         '    a->used = off + sz;',
         '    return a->base + off;', '}', '']
    for s in g.used_structs:
        m.append('static size_t vkb_enc_%s(uint8_t *b, const %s *s);' % (s, s))
        m.append('static size_t vkb_dec_%s(const uint8_t *b, vkb_arena *a, %s *s);' % (s, s))
    m.append('')
    for s in g.used_structs:
        mem = reg.structs[s]
        cnt = lambda nm: 's->%s' % nm
        m.append('static size_t vkb_enc_%s(uint8_t *b, const %s *s) {' % (s, s))
        m.append('    size_t n = 0;')
        for d in mem:
            m += ['    ' + x for x in g.enc(d, 's->%s' % d.name, cnt, False)]
        m += ['    return n;', '}', '']
        m.append('static size_t vkb_dec_%s(const uint8_t *b, vkb_arena *a, %s *s) {' % (s, s))
        m.append('    size_t n = 0; (void)a;')
        for d in mem:
            m += ['    ' + x for x in g.dec(d, 's->%s' % d.name, False)]
        m += ['    return n;', '}', '']
    for c in cmds:
        if c not in ok:
            m += ['/* %s: not generated (stub). reason: %s */' % (c, bad[c]), '']
            continue
        ps = reg.cmds[c]
        isptr = {p.name: p.depth > 0 for p in ps}
        cnt = lambda nm: ('((%s) ? *(%s) : 0)' % (nm, nm)) if isptr.get(nm) else nm
        m += [proto_enc(c) + ' {', '    size_t n = 0;',
              '    { uint32_t id_ = VKB_CMD_%s; VKB_PUT(b, n, &id_, 4); }' % c]
        for p in ps:
            m += ['    ' + x for x in g.enc(p, p.name, cnt, True)]
        m += ['    return n;', '}', '']
        m += [proto_dec(c) + ' {', '    size_t n = 0; (void)a;',
              '    { uint32_t id_; VKB_GET(b, n, &id_, 4); if (id_ != VKB_CMD_%s) return 0; }' % c]
        for p in ps:
            m += ['    ' + x for x in g.dec(p, '(*%s)' % p.name, True)]
        m += ['    return n;', '}', '']
    open(os.path.join(out, 'vkb_marshal.c'), 'w').write('\n'.join(m))

    # ---- vkb_test_gen.c (fill / deep-eq / roundtrip per cmd; included by test/roundtrip.c)
    t = [hdr, '/* test helpers; expects vkb_fill_bytes, vkb_fill_str, vkb_streq defined by includer */', '']
    for s in g.used_structs:
        t.append('static void fill_%s(%s *s, vkb_arena *a, uint32_t *seed);' % (s, s))
        t.append('static int eq_%s(const %s *x, const %s *y);' % (s, s, s))
    t.append('')
    for s in g.used_structs:
        mem = reg.structs[s]
        t.append('static void fill_%s(%s *s, vkb_arena *a, uint32_t *seed) {' % (s, s))
        t.append('    (void)a; (void)seed;')
        post = []
        for d in mem:
            lines, pc = g.fill(d, 's->%s' % d.name, lambda nm: ['s->%s = %d;' % (nm, N_FILL)], False)
            t += ['    ' + x for x in lines]
            post += pc
        t += ['    ' + x for x in post] + ['}', '']
        t.append('static int eq_%s(const %s *x, const %s *y) {' % (s, s, s))
        for d in mem:
            t += ['    ' + z for z in g.eq(d, 'x->%s' % d.name, 'y->%s' % d.name, lambda nm: 'x->%s' % nm, False)]
        t += ['    return 1;', '}', '']
    for c in ok:
        ps = reg.cmds[c]
        isptr = {p.name: p.depth > 0 for p in ps}
        t.append('static int vkb_rt_%s(vkb_arena *a, vkb_arena *da, uint8_t *buf, size_t cap, uint32_t *seed) {' % c)
        t.append('    (void)a; (void)seed;')
        for p in ps:
            t.append('    %s x_%s; %s y_%s;' % (p.ctype, p.name, p.ctype, p.name))
        post = []
        for p in ps:
            setc = lambda nm: [('*x_%s = %d;' if isptr.get(nm) else 'x_%s = %d;') % (nm, N_FILL)]
            lines, pc = g.fill(p, 'x_%s' % p.name, setc, True)
            t += ['    ' + x for x in lines]
            post += pc
        t += ['    ' + x for x in post]
        args = ', '.join('x_%s' % p.name for p in ps)
        dargs = ', '.join('&y_%s' % p.name for p in ps)
        t += ['    size_t need = vkb_enc_%s(NULL, %s);' % (c, args),
              '    if (need > cap) return 0;',
              '    if (vkb_enc_%s(buf, %s) != need) return 0;' % (c, args),
              '    if (vkb_dec_%s(buf, da, %s) != need) return 0;' % (c, dargs)]
        cnt = lambda nm: ('*x_%s' % nm) if isptr.get(nm) else 'x_%s' % nm
        for p in ps:
            t += ['    ' + z for z in g.eq(p, 'x_%s' % p.name, 'y_%s' % p.name, cnt, True)]
        t += ['    return 1;', '}', '']
    t += ['typedef int (*vkb_rt_fn)(vkb_arena *, vkb_arena *, uint8_t *, size_t, uint32_t *);',
          'static const struct { const char *name; vkb_rt_fn fn; } vkb_rt_table[] = {']
    t += ['    { "%s", vkb_rt_%s },' % (c, c) for c in ok]
    t += ['};', '#define VKB_RT_COUNT (sizeof(vkb_rt_table) / sizeof(vkb_rt_table[0]))', '']
    open(os.path.join(out, 'vkb_test_gen.c'), 'w').write('\n'.join(t))

    import rpcgen
    rpcgen.emit(reg, Gen, Unsupported, cmds, out, hdr)

    print('gen.py: %d cmds ok, %d stub, %d structs -> %s' % (len(ok), len(bad), len(g.used_structs), out))
    for c, why in bad.items():
        print('gen.py: stub %s: %s' % (c, why))


if __name__ == '__main__':
    main()
