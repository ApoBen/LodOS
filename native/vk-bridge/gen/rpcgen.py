"""lodos-vk-bridge RPC codegen (called from gen.py). Emits a request/reply layer on top of vkb_wire.h.

files: vkb_rpc.h          ids, client API protos (vkbc_<cmd>), server fn table + dispatch
       vkb_rpc_common.c   struct codecs (vkrw_<S> encode / vkrr_<S> bounds-checked decode + pNext out-chains)
       vkb_rpc_client.c   vkbc_<cmd>: encode request -> vkbc__call() -> decode reply into out params
       vkb_rpc_server.c   vkr_srv_dispatch(): decode request -> call driver fn -> encode reply

Request:  u32 cmd, params in order:
  in value/handle/struct/string/array : as gen.py wire format (handles -> u64 id via vkr_h_out)
  pNext (in struct)                   : u8 present (server rejects !=0 with VK_ERROR_FEATURE_NOT_PRESENT)
  pAllocator                          : u8 present only, server passes NULL
  out single flat (non-const T*)      : u8 present
  out single chained (sType+pNext)    : u8 present [+ u32 chain_len + chain_len * u32 sType]
  in/out count (T* used as len)       : u8 present [+ value]
  out array flat (len=countptr)       : u8 present [+ u32 capacity]
  out array chained (len=countptr)    : u8 present [+ u32 capacity + capacity * chain_req]
Reply:    u32 cmd, i32 VkResult (void cmds: VK_SUCCESS or bridge error); if result >= 0, per out param
  in order: u8 present [+ value] / arrays: u8 present [+ u32 n (<= capacity) + n elems].
  Chained out structs encode payload (preserving client sType & pNext) + per-node u8 supported [+ body];
  unknown sTypes send u8(0) (zero-skip, never send garbage).
Handle map: server ids are table indices (vkr_h_out inserts, vkr_h_in looks up, vkDestroy* drops).
Two-call idiom: count is in/out, array gets capacity = *count at call time; VK_INCOMPLETE is >= 0.
"""
import os
from gen import discover_in_pnext, eval_len_expr, len_refs, stype_enum as gen_stype_enum

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

    def stype_enum(s):
        return gen_stype_enum(R, s)

    def chained_out(s):
        """Out struct with sType + void* pNext header and flat remaining members."""
        if not stype_enum(s):
            return False
        for m in R.structs[s][2:]:
            if m.depth or m.name == 'pNext' or m.bitfield or len(m.dims) > 1:
                return False
            b = R.res(m.base)
            if R.cat.get(b) == 'struct' and not flat(b):
                return False
        return True

    def register_body_deps(s):
        for m in R.structs[s][2:]:
            b = R.res(m.base)
            if R.cat.get(b) == 'struct':
                g.need_struct(b)

    def lenlist(ps):
        return {p.len[0] for p in ps if p.len}

    def dirn(p, ps):
        """'in' | 'out' | 'inout'"""
        k, ek = g.classify(p, True)
        if p.depth == 1 and not p.const_outer and k in ('single', 'array', 'bytes'):
            if k == 'single' and ek in ('scalar', 'size') and p.name in lenlist(ps):
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
        if k == 'pnext':
            return ['vkr_in_pnext_enc(w, (const void *)(%s));' % ex]
        if k == 'skip_alloc':
            return ['vkr_put_u8(w, (%s) != NULL);' % ex]
        if k == 'string':
            return ['vkr_put_str(w, %s);' % ex]
        if k == 'bytes':
            return ['vkr_put_u8(w, (%s) != NULL);' % ex,
                    'if (%s) { uint32_t c_ = (uint32_t)(%s); vkr_put_u32(w, c_); if (c_) vkr_put(w, %s, c_); }' %
                    (ex, cnt(d.len[0]), ex)]
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
            return ['%s = (void *)vkr_in_pnext_dec(r);' % lv]
        if k == 'skip_alloc':
            return ['(void)vkr_get_u8(r);', '%s = NULL;' % lv]
        if k == 'string':
            return ['%s = vkr_get_str(r);' % lv]
        if k == 'bytes':
            return ['if (vkr_get_u8(r)) { uint32_t c_ = vkr_get_u32(r); %s = c_;' % nvar,
                    '    uint8_t *e_ = (uint8_t *)vkr_alloc_arr(r, c_, 1, 1);',
                    '    if (e_ && c_) vkr_get(r, e_, c_);',
                    '    %s = (const void *)e_; } else %s = NULL;' % (lv, lv)]
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
        """wire array count must equal the declared count expression over non-pointer members/params."""
        names = {d.name: d for d in decls}
        res = []
        for d in decls:
            k, _ = g.classify(d, is_param)
            if k not in ('array', 'strarray', 'bytes'):
                continue
            ln0 = d.len[0]
            if '->' in ln0:
                continue
            refs = len_refs(ln0)
            if any(nm not in names or names[nm].depth != 0 for nm in refs):
                continue
            exp = eval_len_expr(ln0, acc)
            if ln0 == 'codeSize / 4' and 'codeSize' in names:
                res.append('if (%s && ((%s) %% 4 != 0 || %s != (uint32_t)%s)) vkr_fail(&r->err, VKR_E_COUNT);' %
                           (acc(d.name), acc('codeSize'), nv(d.name), exp))
            else:
                res.append('if (%s && %s != (uint32_t)%s) vkr_fail(&r->err, VKR_E_COUNT);' %
                           (acc(d.name), nv(d.name), exp))
        return res

    # ---- pick commands & chained out-structs ---------------------------
    ok, bad = [], {}
    chained_roots = []
    for c in cmds:
        ps = R.cmds[c]
        try:
            rt = R.ret[c]
            if rt not in ('VkResult', 'void') and R.kind(rt) != 'scalar':
                raise Unsupported('return type %s' % rt)
            for p in ps:
                k, ek = g.classify(p, True)
                dr = dirn(p, ps)
                if dr == 'out':
                    if ek == 'struct':
                        bs = R.res(p.base)
                        if not flat(bs):
                            if chained_out(bs):
                                if bs not in chained_roots:
                                    chained_roots.append(bs)
                            else:
                                raise Unsupported('out struct %s not flat/chained' % bs)
                    if k in ('array', 'bytes'):
                        ln0 = p.len[0]
                        if '->' in ln0:
                            pnm, mnm = ln0.split('->', 1)
                            qp = [q for q in ps if q.name == pnm and dirn(q, ps) == 'in' and g.classify(q, True) == ('single', 'struct')]
                            qm = [m for m in R.structs.get(R.res(qp[0].base), []) if m.name == mnm and m.depth == 0] if qp else []
                            if not qm:
                                raise Unsupported('out array %s len %s' % (p.name, ln0))
                        else:
                            ln = [q for q in ps if q.name == ln0]
                            if not ln or (ln[0].depth and dirn(ln[0], ps) != 'inout'):
                                raise Unsupported('out array %s len %s' % (p.name, ln0))
            ok.append(c)
        except Unsupported as ex:
            bad[c] = str(ex)

    # Discover all unguarded vulkan_core.h flat structs extending any used chained_out root
    pnext_exts = []
    seen_stypes = set()
    if chained_roots:
        root_set = set(chained_roots)
        for s, exts in R.structextends.items():
            if s not in R.core_types:
                continue
            if not (root_set & set(exts)):
                continue
            if not chained_out(s):
                continue
            st = stype_enum(s)
            if st and st not in seen_stypes:
                seen_stypes.add(st)
                pnext_exts.append(s)

    chained_all = []
    for s in chained_roots + pnext_exts:
        if s not in chained_all:
            register_body_deps(s)
            chained_all.append(s)

    in_roots = set()
    def collect_in_structs(s):
        if s in in_roots or R.cat.get(s) != 'struct':
            return
        in_roots.add(s)
        for m in R.structs[s]:
            b = R.res(m.base)
            if R.cat.get(b) == 'struct':
                collect_in_structs(b)
    for c in ok:
        ps = R.cmds[c]
        for p in ps:
            if dirn(p, ps) == 'in':
                collect_in_structs(R.res(p.base))
    in_pnext_exts = discover_in_pnext(R, g, Unsupported, in_roots)

    structs = list(g.used_structs)

    # ---- vkb_rpc.h ------------------------------------------------------
    def cproto(c, prefix, extra):
        ps = [extra] + ['%s %s' % (p.ctype, p.name) for p in R.cmds[c]]
        rt = R.ret[c] if R.ret[c] != 'void' else 'VkResult'
        return '%s %s%s(%s)' % (rt, prefix, c, ', '.join(ps))

    h = [hdr, '#ifndef VKB_RPC_H', '#define VKB_RPC_H', '#include "vkb_wire.h"', '#include "vkb_cmds.h"', '']
    for c in ok:
        h.append('#define VKB_HAS_%s 1' % c)
    if ok:
        h.append('')
    for s in structs:
        h += ['void vkrw_%s(vkr_wr *w, const %s *s);' % (s, s), 'void vkrr_%s(vkr_rd *r, %s *s);' % (s, s)]
    for s in chained_all:
        h += ['void vkrw_body_%s(vkr_wr *w, const %s *s);' % (s, s), 'void vkrr_body_%s(vkr_rd *r, %s *s);' % (s, s)]
    h += ['', '/* in pNext chain helpers (pure-input core structextends; unknown sType -> VK_ERROR_FEATURE_NOT_PRESENT) */',
          'void vkr_in_pnext_enc(vkr_wr *w, const void *pnext);',
          'void *vkr_in_pnext_dec(vkr_rd *r);']
    if chained_roots:
        h += ['', '/* out pNext chain helpers (unknown sTypes -> zero-skip, never send garbage) */',
              'typedef struct { uint32_t n; int32_t *stypes; void **nodes; } vkr_out_chain;',
              'void vkr_pnext_req(vkr_wr *w, const void *pnext);',
              'void *vkr_pnext_srv_alloc(vkr_rd *r, vkr_out_chain *ch);',
              'void vkr_pnext_srv_enc(vkr_wr *w, const vkr_out_chain *ch);',
              'void vkr_pnext_cli_dec(vkr_rd *r, void *pnext);']
    h += ['', '/* ---- client (implemented by client lib): transport hooks */',
          'typedef struct vkbc vkbc;',
          'uint8_t *vkbc__buf(vkbc *c, size_t *cap);',
          '/* send req (in vkbc__buf), wait reply; returns 0 or -errno */',
          'int vkbc__call(vkbc *c, size_t len, const uint8_t **rep, size_t *replen);',
          '/* drop local shm mapping(s) on unmap/free/destroy (mem==0 drops all on dev) */',
          'void vkbc__drop_map(vkbc *c, uint64_t dev, uint64_t mem);',
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
    if 'VkWriteDescriptorSet' in structs or 'VkDescriptorSetLayoutBinding' in structs:
        m += ['static inline int vkr_ds_has_imm_sampler(VkDescriptorType t) {',
              '    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;',
              '}', '']
    if 'VkWriteDescriptorSet' in structs:
        m += ['static inline int vkr_ds_has_img(VkDescriptorType t) {',
              '    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||',
              '           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||',
              '           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;',
              '}',
              'static inline int vkr_ds_has_buf(VkDescriptorType t) {',
              '    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||',
              '           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;',
              '}',
              'static inline int vkr_ds_has_texel(VkDescriptorType t) {',
              '    return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;',
              '}', '']
    for s in structs:
        mem = R.structs[s]
        s_cnt = lambda ln0: eval_len_expr(ln0, lambda nm: 's->%s' % nm)
        m.append('void vkrw_%s(vkr_wr *w, const %s *s) {' % (s, s))
        m.append('    (void)w; (void)s;')
        if s == 'VkGraphicsPipelineCreateInfo':
            m += ['    int gp_tess_ = (s->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;',
                  '    if (!gp_tess_ && s->pStages) {',
                  '        for (uint32_t gi_ = 0; gi_ < s->stageCount; gi_++)',
                  '            if (s->pStages[gi_].stage & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))',
                  '                gp_tess_ = 1;',
                  '    }',
                  '    int gp_dyn_vp_ = 0, gp_dyn_sc_ = 0, gp_dyn_rd_ = 0, gp_dyn_vi_ = 0;',
                  '    if (s->pDynamicState && s->pDynamicState->pDynamicStates) {',
                  '        for (uint32_t gi_ = 0; gi_ < s->pDynamicState->dynamicStateCount; gi_++) {',
                  '            VkDynamicState ds_ = s->pDynamicState->pDynamicStates[gi_];',
                  '            if (ds_ == VK_DYNAMIC_STATE_VIEWPORT || ds_ == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) gp_dyn_vp_ = 1;',
                  '            else if (ds_ == VK_DYNAMIC_STATE_SCISSOR || ds_ == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) gp_dyn_sc_ = 1;',
                  '            else if (ds_ == VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE) gp_dyn_rd_ = 1;',
                  '            else if (ds_ == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) gp_dyn_vi_ = 1;',
                  '        }',
                  '    }',
                  '    int gp_discard_ = !(s->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) &&',
                  '                      s->pRasterizationState && s->pRasterizationState->rasterizerDiscardEnable && !gp_dyn_rd_;']
        for d in mem:
            if s in ('VkBufferCreateInfo', 'VkImageCreateInfo') and d.name == 'queueFamilyIndexCount':
                m += ['    { uint32_t qfc_ = (s->sharingMode == VK_SHARING_MODE_CONCURRENT) ? s->queueFamilyIndexCount : 0;',
                      '      vkr_put(w, &qfc_, sizeof(uint32_t)); }']
            elif s in ('VkBufferCreateInfo', 'VkImageCreateInfo') and d.name == 'pQueueFamilyIndices':
                m += ['    { const uint32_t *qfi_ = (s->sharingMode == VK_SHARING_MODE_CONCURRENT) ? s->pQueueFamilyIndices : NULL;'] + \
                     ['      ' + x for x in enc_in(d, 'qfi_', lambda ln0: '((s->sharingMode == VK_SHARING_MODE_CONCURRENT) ? s->queueFamilyIndexCount : 0)', False)] + ['    }']
            elif s == 'VkFramebufferCreateInfo' and d.name == 'pAttachments':
                m += ['    { const VkImageView *fa_ = (s->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) ? NULL : s->pAttachments;'] + \
                     ['      ' + x for x in enc_in(d, 'fa_', s_cnt, False)] + ['    }']
            elif s == 'VkGraphicsPipelineCreateInfo' and d.name == 'pVertexInputState':
                m += ['    { const VkPipelineVertexInputStateCreateInfo *vi_ = gp_dyn_vi_ ? NULL : s->pVertexInputState;'] + \
                     ['      ' + x for x in enc_in(d, 'vi_', s_cnt, False)] + ['    }']
            elif s == 'VkDescriptorSetLayoutBinding' and d.name == 'pImmutableSamplers':
                m += ['    { const VkSampler *is_ = vkr_ds_has_imm_sampler(s->descriptorType) ? s->pImmutableSamplers : NULL;'] + \
                     ['      ' + x for x in enc_in(d, 'is_', s_cnt, False)] + ['    }']
            elif s == 'VkWriteDescriptorSet' and d.name == 'pImageInfo':
                i = 'i%d' % g.u()
                m += ['    { const VkDescriptorImageInfo *pi_ = vkr_ds_has_img(s->descriptorType) ? s->pImageInfo : NULL;',
                      '      vkr_put_u8(w, pi_ != NULL);',
                      '      if (pi_) { uint32_t c_ = (uint32_t)(s->descriptorCount); vkr_put_u32(w, c_);',
                      '          for (uint32_t %s = 0; %s < c_; %s++) {' % (i, i, i),
                      '              VkDescriptorImageInfo ii_ = pi_[%s];' % i,
                      '              if (!vkr_ds_has_imm_sampler(s->descriptorType)) ii_.sampler = VK_NULL_HANDLE;',
                      '              if (s->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) ii_.imageView = VK_NULL_HANDLE;',
                      '              vkrw_VkDescriptorImageInfo(w, &ii_);',
                      '          } } }']
            elif s == 'VkWriteDescriptorSet' and d.name == 'pBufferInfo':
                m += ['    { const VkDescriptorBufferInfo *pb_ = vkr_ds_has_buf(s->descriptorType) ? s->pBufferInfo : NULL;'] + \
                     ['      ' + x for x in enc_in(d, 'pb_', s_cnt, False)] + ['    }']
            elif s == 'VkWriteDescriptorSet' and d.name == 'pTexelBufferView':
                m += ['    { const VkBufferView *pt_ = vkr_ds_has_texel(s->descriptorType) ? s->pTexelBufferView : NULL;'] + \
                     ['      ' + x for x in enc_in(d, 'pt_', s_cnt, False)] + ['    }']
            elif s == 'VkGraphicsPipelineCreateInfo' and d.name == 'pTessellationState':
                m += ['    { const VkPipelineTessellationStateCreateInfo *ts_ = gp_tess_ ? s->pTessellationState : NULL;'] + \
                     ['      ' + x for x in enc_in(d, 'ts_', s_cnt, False)] + ['    }']
            elif s == 'VkGraphicsPipelineCreateInfo' and d.name == 'pViewportState':
                m += ['    { VkPipelineViewportStateCreateInfo vp_tmp_;',
                      '      const VkPipelineViewportStateCreateInfo *vp_ = gp_discard_ ? NULL : s->pViewportState;',
                      '      if (vp_ && (gp_dyn_vp_ || gp_dyn_sc_)) {',
                      '          vp_tmp_ = *vp_;',
                      '          if (gp_dyn_vp_) vp_tmp_.pViewports = NULL;',
                      '          if (gp_dyn_sc_) vp_tmp_.pScissors = NULL;',
                      '          vp_ = &vp_tmp_;',
                      '      }'] + ['      ' + x for x in enc_in(d, 'vp_', s_cnt, False)] + ['    }']
            elif s == 'VkGraphicsPipelineCreateInfo' and d.name in ('pMultisampleState', 'pDepthStencilState', 'pColorBlendState'):
                m += ['    { %s st_ = gp_discard_ ? NULL : s->%s;' % (d.ctype, d.name)] + \
                     ['      ' + x for x in enc_in(d, 'st_', s_cnt, False)] + ['    }']
            else:
                m += ['    ' + x for x in enc_in(d, 's->%s' % d.name, s_cnt, False)]
        m += ['}', '']
        m.append('void vkrr_%s(vkr_rd *r, %s *s) {' % (s, s))
        m.append('    (void)r; (void)s;')
        arrs = [d for d in mem if g.classify(d, False)[0] in ('array', 'strarray', 'bytes')]
        for d in arrs:
            m.append('    uint32_t n_%s = 0; (void)n_%s;' % (d.name, d.name))
        for d in mem:
            m += ['    ' + x for x in dec_in(d, 's->%s' % d.name, False, 'n_%s' % d.name)]
        m += ['    ' + x for x in count_checks(mem, lambda nm: 's->%s' % nm, lambda nm: 'n_%s' % nm, False)]
        m += ['}', '']

    for s in in_pnext_exts:
        mem = R.structs[s][2:]
        s_cnt = lambda ln0: eval_len_expr(ln0, lambda nm: 's->%s' % nm)
        m.append('static void vkrw_in_ext_%s(vkr_wr *w, const %s *s) {' % (s, s))
        m.append('    (void)w; (void)s;')
        for d in mem:
            m += ['    ' + x for x in enc_in(d, 's->%s' % d.name, s_cnt, False)]
        m += ['}', '']
        m.append('static void vkrr_in_ext_%s(vkr_rd *r, %s *s) {' % (s, s))
        m.append('    (void)r; (void)s;')
        arrs = [d for d in mem if g.classify(d, False)[0] in ('array', 'strarray', 'bytes')]
        for d in arrs:
            m.append('    uint32_t n_%s = 0; (void)n_%s;' % (d.name, d.name))
        for d in mem:
            m += ['    ' + x for x in dec_in(d, 's->%s' % d.name, False, 'n_%s' % d.name)]
        m += ['    ' + x for x in count_checks(R.structs[s], lambda nm: 's->%s' % nm, lambda nm: 'n_%s' % nm, False)]
        m += ['}', '']

    m += ['static _Thread_local uint32_t t_in_pnext_depth;', '',
          'static int vkr_in_pnext_skip(const VkBaseInStructure *c) {',
          '    if (c->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||',
          '        c->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) return 1;',
          '    if (c->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {',
          '        const VkPipelineCreationFeedbackCreateInfo *fb = (const VkPipelineCreationFeedbackCreateInfo *)c;',
          '        if (fb->pPipelineCreationFeedback) *fb->pPipelineCreationFeedback = (VkPipelineCreationFeedback){ 0 };',
          '        if (fb->pPipelineStageCreationFeedbacks)',
          '            for (uint32_t i = 0; i < fb->pipelineStageCreationFeedbackCount; i++)',
          '                fb->pPipelineStageCreationFeedbacks[i] = (VkPipelineCreationFeedback){ 0 };',
          '        return 1;',
          '    }',
          '    return 0;',
          '}', '',
          'static int vkr_in_pnext_enc_one(vkr_wr *w, VkStructureType st, const void *nd) {',
          '    switch ((int32_t)st) {']
    for s in in_pnext_exts:
        m.append('    case %s: vkr_put_u8(w, 1); vkrw_in_ext_%s(w, (const %s *)nd); return 1;' %
                 (stype_enum(s), s, s))
    m += ['    default: (void)w; (void)nd; return 0;', '    }', '}', '',
          'static void *vkr_in_pnext_dec_one(vkr_rd *r, VkStructureType st) {',
          '    switch ((int32_t)st) {']
    for s in in_pnext_exts:
        m.append('    case %s: { %s *nd = (%s *)vkr_alloc(r, sizeof(%s)); if (nd) vkrr_in_ext_%s(r, nd); return nd; }' %
                 (stype_enum(s), s, s, s, s))
    m += ['    default: (void)r; return NULL;', '    }', '}', '',
          'void vkr_in_pnext_enc(vkr_wr *w, const void *pnext) {',
          '    if (!pnext) { vkr_put_u8(w, 0); return; }',
          '    if (t_in_pnext_depth >= 16u) { vkr_fail(&w->err, VKR_E_BOUNDS); return; }',
          '    uint32_t n = 0;',
          '    for (const VkBaseInStructure *c = (const VkBaseInStructure *)pnext; c; c = c->pNext) {',
          '        if (vkr_in_pnext_skip(c)) continue;',
          '        if (++n > VKR_MAX_PNEXT) { vkr_fail(&w->err, VKR_E_BOUNDS); return; }',
          '    }',
          '    if (!n) { vkr_put_u8(w, 0); return; }',
          '    vkr_put_u8(w, 1);',
          '    vkr_put_u32(w, n);',
          '    t_in_pnext_depth++;',
          '    for (const VkBaseInStructure *c = (const VkBaseInStructure *)pnext; c; c = c->pNext) {',
          '        if (vkr_in_pnext_skip(c)) continue;',
          '        vkr_put_u32(w, (uint32_t)c->sType);',
          '        if (!vkr_in_pnext_enc_one(w, c->sType, c)) vkr_put_u8(w, 0);',
          '    }',
          '    t_in_pnext_depth--;',
          '}', '',
          'void *vkr_in_pnext_dec(vkr_rd *r) {',
          '    if (!vkr_get_u8(r)) return NULL;',
          '    if (t_in_pnext_depth >= 16u) { vkr_fail(&r->err, VKR_E_BOUNDS); return NULL; }',
          '    uint32_t n = vkr_get_u32(r);',
          '    if (!n || n > VKR_MAX_PNEXT) { vkr_fail(&r->err, VKR_E_BOUNDS); return NULL; }',
          '    VkBaseOutStructure *head = NULL, *tail = NULL;',
          '    t_in_pnext_depth++;',
          '    for (uint32_t i = 0; i < n && !r->err; i++) {',
          '        VkStructureType st = (VkStructureType)vkr_get_u32(r);',
          '        if (!vkr_get_u8(r)) { vkr_fail(&r->err, VKR_E_PNEXT); break; }',
          '        VkBaseOutStructure *b = (VkBaseOutStructure *)vkr_in_pnext_dec_one(r, st);',
          '        if (!b) { if (!r->err) vkr_fail(&r->err, VKR_E_PNEXT); break; }',
          '        b->sType = st;',
          '        b->pNext = NULL;',
          '        if (tail) tail->pNext = b; else head = b;',
          '        tail = b;',
          '    }',
          '    t_in_pnext_depth--;',
          '    return head;',
          '}', '']

    for s in chained_all:
        mem = R.structs[s][2:]
        s_cnt = lambda ln0: eval_len_expr(ln0, lambda nm: 's->%s' % nm)
        m.append('void vkrw_body_%s(vkr_wr *w, const %s *s) {' % (s, s))
        m.append('    (void)w; (void)s;')
        for d in mem:
            m += ['    ' + x for x in enc_in(d, 's->%s' % d.name, s_cnt, False)]
        m += ['}', '']
        m.append('void vkrr_body_%s(vkr_rd *r, %s *s) {' % (s, s))
        m.append('    (void)r; (void)s;')
        for d in mem:
            m += ['    ' + x for x in dec_in(d, 's->%s' % d.name, False, 'n_unused')]
        m += ['}', '']

    if chained_roots:
        m += ['static void *vkr_pnext_alloc_one(vkr_rd *r, VkStructureType st) {',
              '    switch ((int32_t)st) {']
        for s in pnext_exts:
            m.append('    case %s: return vkr_alloc(r, sizeof(%s));' % (stype_enum(s), s))
        m += ['    default: (void)r; return NULL;', '    }', '}', '',
              'static void vkr_pnext_enc_one(vkr_wr *w, VkStructureType st, const void *nd) {',
              '    switch ((int32_t)st) {']
        for s in pnext_exts:
            m.append('    case %s: vkrw_body_%s(w, (const %s *)nd); break;' % (stype_enum(s), s, s))
        m += ['    default: (void)w; (void)nd; break;', '    }', '}', '',
              'static int vkr_pnext_dec_one(vkr_rd *r, VkStructureType st, void *nd) {',
              '    switch ((int32_t)st) {']
        for s in pnext_exts:
            m.append('    case %s: vkrr_body_%s(r, (%s *)nd); return 1;' % (stype_enum(s), s, s))
        m += ['    default: (void)r; (void)nd; return 0;', '    }', '}', '',
              'void vkr_pnext_req(vkr_wr *w, const void *pnext) {',
              '    uint32_t n = 0;',
              '    const VkBaseOutStructure *c = (const VkBaseOutStructure *)pnext;',
              '    for (; c && n < VKR_MAX_PNEXT; c = c->pNext) n++;',
              '    if (c) { vkr_fail(&w->err, VKR_E_BOUNDS); return; }',
              '    vkr_put_u32(w, n);',
              '    for (c = (const VkBaseOutStructure *)pnext; c; c = c->pNext)',
              '        vkr_put_u32(w, (uint32_t)c->sType);',
              '}', '',
              'void *vkr_pnext_srv_alloc(vkr_rd *r, vkr_out_chain *ch) {',
              '    ch->n = vkr_get_u32(r);',
              '    ch->stypes = NULL; ch->nodes = NULL;',
              '    if (ch->n > VKR_MAX_PNEXT) { vkr_fail(&r->err, VKR_E_BOUNDS); ch->n = 0; return NULL; }',
              '    if (!ch->n) return NULL;',
              '    ch->stypes = (int32_t *)vkr_alloc_arr(r, ch->n, sizeof(int32_t), 4);',
              '    ch->nodes = (void **)vkr_alloc(r, (size_t)ch->n * sizeof(void *));',
              '    if (!ch->stypes || !ch->nodes) { ch->n = 0; return NULL; }',
              '    VkBaseOutStructure *head = NULL, *tail = NULL;',
              '    for (uint32_t i = 0; i < ch->n; i++) {',
              '        int32_t st = (int32_t)vkr_get_u32(r);',
              '        ch->stypes[i] = st;',
              '        int dup = 0;',
              '        for (uint32_t j = 0; j < i; j++) if (ch->stypes[j] == st && ch->nodes[j]) { dup = 1; break; }',
              '        void *nd = (dup || r->err) ? NULL : vkr_pnext_alloc_one(r, (VkStructureType)st);',
              '        ch->nodes[i] = nd;',
              '        if (nd) {',
              '            VkBaseOutStructure *b = (VkBaseOutStructure *)nd;',
              '            b->sType = (VkStructureType)st;',
              '            b->pNext = NULL;',
              '            if (tail) tail->pNext = b; else head = b;',
              '            tail = b;',
              '        }',
              '    }',
              '    return head;',
              '}', '',
              'void vkr_pnext_srv_enc(vkr_wr *w, const vkr_out_chain *ch) {',
              '    for (uint32_t i = 0; i < ch->n; i++) {',
              '        if (!ch->nodes[i]) { vkr_put_u8(w, 0); continue; }',
              '        vkr_put_u8(w, 1);',
              '        vkr_pnext_enc_one(w, (VkStructureType)ch->stypes[i], ch->nodes[i]);',
              '    }',
              '}', '',
              'void vkr_pnext_cli_dec(vkr_rd *r, void *pnext) {',
              '    for (VkBaseOutStructure *c = (VkBaseOutStructure *)pnext; c && !r->err; c = c->pNext) {',
              '        uint8_t ok = vkr_get_u8(r);',
              '        if (r->err) break;',
              '        if (ok && !vkr_pnext_dec_one(r, c->sType, c)) { vkr_fail(&r->err, VKR_E_PNEXT); break; }',
              '    }',
              '}', '']
    open(os.path.join(out, 'vkb_rpc_common.c'), 'w').write('\n'.join(m))

    def cnt_expr(ps, ln0):
        isptr = {p.name: p.depth > 0 for p in ps}
        return eval_len_expr(ln0, lambda nm: nm, isptr)

    def is_chained_param(p):
        _, ek = g.classify(p, True)
        return ek == 'struct' and not flat(R.res(p.base)) and chained_out(R.res(p.base))

    # ---- vkb_rpc_client.c ----------------------------------------------
    m = [hdr, '#include "vkb_rpc.h"', '',
         'uint64_t vkr_h_out(void *hctx, VkObjectType t, uint64_t h) { (void)hctx; (void)t; return h; }',
         'uint64_t vkr_h_in(void *hctx, VkObjectType t, uint64_t id, int *ok) { (void)hctx; (void)t; *ok = 1; return id; }',
         'void vkr_h_drop(void *hctx, VkObjectType t, uint64_t h) { (void)hctx; (void)t; (void)h; }', '']
    for c in ok:
        ps = R.cmds[c]
        rt = R.ret[c]
        ret_scalar = rt not in ('VkResult', 'void')
        err_r = lambda code, rt=rt, rs=ret_scalar: ('(%s)0' % rt) if rs else code
        cnt = lambda nm, ps=ps: cnt_expr(ps, nm)
        m += [cproto(c, 'vkbc_', 'vkbc *c') + ' {']
        outs = [p for p in ps if dirn(p, ps) != 'in']
        for p in outs:
            if g.classify(p, True)[0] in ('array', 'bytes'):
                m.append('    uint32_t cap_%s = (uint32_t)(%s);' % (p.name, cnt(p.len[0])))
        m += ['    size_t bcap; uint8_t *buf = vkbc__buf(c, &bcap);',
              '    vkr_wr w_ = { buf, bcap, 0, 0, NULL }, *w = &w_;',
              '    vkr_put_u32(w, VKB_CMD_%s);' % c]
        for p in ps:
            dr = dirn(p, ps)
            k, _ = g.classify(p, True)
            ch = is_chained_param(p)
            if dr == 'in':
                m += ['    ' + x for x in enc_in(p, p.name, cnt, True)]
            elif dr == 'inout':
                m += ['    vkr_put_u8(w, %s != NULL);' % p.name,
                      '    if (%s) vkr_put(w, %s, sizeof(*%s));' % (p.name, p.name, p.name)]
            elif k == 'single':
                m.append('    vkr_put_u8(w, %s != NULL);' % p.name)
                if ch:
                    m.append('    if (%s) vkr_pnext_req(w, %s->pNext);' % (p.name, p.name))
            else:
                m += ['    vkr_put_u8(w, %s != NULL);' % p.name,
                      '    if (%s) {' % p.name,
                      '        vkr_put_u32(w, cap_%s);' % p.name]
                if ch:
                    i = 'i%d' % g.u()
                    m += ['        for (uint32_t %s = 0; %s < cap_%s; %s++)' % (i, i, p.name, i),
                          '            vkr_pnext_req(w, %s[%s].pNext);' % (p.name, i)]
                m.append('    }')
        m += ['    if (w->err) return %s;' % err_r('VK_ERROR_OUT_OF_HOST_MEMORY'),
              '    const uint8_t *rep; size_t replen;',
              '    if (vkbc__call(c, w->off, &rep, &replen) < 0) return %s;' % err_r('VK_ERROR_DEVICE_LOST'),
              '    vkr_rd r_ = { rep, replen, 0, 0, NULL, NULL }, *r = &r_;',
              '    if (vkr_get_u32(r) != VKB_CMD_%s) return %s;' % (c, err_r('VK_ERROR_UNKNOWN')),
              '    VkResult res; vkr_get(r, &res, sizeof res);',
              '    if (r->err) return %s;' % err_r('VK_ERROR_UNKNOWN'),
              '    if (res < 0) return %s;' % err_r('res')]
        for p in outs:
            k, ek = g.classify(p, True)
            base = R.res(p.base)
            ch = is_chained_param(p)
            m += ['    { uint8_t p_ = vkr_get_u8(r);',
                  '      if (p_ != (%s != NULL)) return %s;' % (p.name, err_r('VK_ERROR_UNKNOWN'))]
            if k == 'single':
                if ch:
                    m += ['      if (p_) {',
                          '          %s->sType = %s;' % (p.name, stype_enum(base)),
                          '          vkrr_body_%s(r, %s);' % (base, p.name),
                          '          vkr_pnext_cli_dec(r, %s->pNext);' % p.name,
                          '      } }']
                else:
                    m += ['      if (p_) {'] + ['          ' + x for x in dec_val(ek, base, '(*%s)' % p.name)] + ['      } }']
            elif k == 'bytes':
                m += ['      if (p_) { uint32_t n_ = vkr_get_u32(r);',
                      '          if (n_ > cap_%s) return %s;' % (p.name, err_r('VK_ERROR_UNKNOWN')),
                      '          if (n_) vkr_get(r, %s, n_);' % p.name,
                      '      } }']
            else:
                i = 'i%d' % g.u()
                m += ['      if (p_) { uint32_t n_ = vkr_get_u32(r);',
                      '          if (n_ > cap_%s) return %s;' % (p.name, err_r('VK_ERROR_UNKNOWN')),
                      '          for (uint32_t %s = 0; %s < n_ && !r->err; %s++) {' % (i, i, i)]
                if ch:
                    m += ['              %s[%s].sType = %s;' % (p.name, i, stype_enum(base)),
                          '              vkrr_body_%s(r, &%s[%s]);' % (base, p.name, i),
                          '              vkr_pnext_cli_dec(r, %s[%s].pNext);' % (p.name, i)]
                else:
                    m += ['              ' + x for x in dec_val(ek, base, '%s[%s]' % (p.name, i))]
                m += ['          } } }']
        if ret_scalar:
            m += ['    %s ret_ = 0; vkr_get(r, &ret_, sizeof ret_);' % rt,
                  '    if (r->err) return (%s)0;' % rt,
                  '    return ret_;', '}', '']
        else:
            m.append('    if (r->err) return VK_ERROR_UNKNOWN;')
            if c in ('vkUnmapMemory', 'vkFreeMemory'):
                m.append('    vkbc__drop_map(c, (uint64_t)(uintptr_t)device, (uint64_t)(uintptr_t)memory);')
            elif c == 'vkDestroyDevice':
                m.append('    vkbc__drop_map(c, (uint64_t)(uintptr_t)device, 0);')
            m += ['    return res;', '}', '']
    open(os.path.join(out, 'vkb_rpc_client.c'), 'w').write('\n'.join(m))

    # ---- vkb_rpc_server.c ----------------------------------------------
    m = [hdr, '#include "vkb_rpc.h"', '',
         'int vkr_srv_load(vkr_srv_fns *f, void *(*getproc)(const char *name, void *ud), void *ud) {',
         '    int miss = 0;']
    for c in ok:
        m.append('    if (!f->%s && !(f->%s = (PFN_%s)getproc("%s", ud))) miss++;' % (c, c, c, c))
    m += ['    return miss;', '}', '',
          'static VkResult vkr_err_res(int e) { return e == VKR_E_PNEXT ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_UNKNOWN; }', '']
    for c in ok:
        ps = R.cmds[c]
        m += ['static void srv_%s(const vkr_srv_fns *f, vkr_rd *r, vkr_wr *w) {' % c]
        for p in ps:
            m.append('    %s %s = 0;' % (p.ctype, p.name))
            k, _ = g.classify(p, True)
            if k in ('array', 'strarray', 'bytes'):
                m.append('    uint32_t n_%s = 0; (void)n_%s;' % (p.name, p.name))
            if dirn(p, ps) == 'out' and is_chained_param(p):
                if k == 'single':
                    m.append('    vkr_out_chain ch_%s = { 0 };' % p.name)
                else:
                    m.append('    vkr_out_chain *ch_%s = NULL;' % p.name)
        for p in ps:
            dr = dirn(p, ps)
            k, ek = g.classify(p, True)
            base = R.res(p.base)
            et = g.elem_ctype(p) if p.depth else None
            ch = is_chained_param(p)
            if dr == 'in':
                m += ['    ' + x for x in dec_in(p, p.name, True, 'n_%s' % p.name)]
            elif dr == 'inout':
                m += ['    if (vkr_get_u8(r)) { %s = (%s *)vkr_alloc(r, sizeof(%s));' % (p.name, et, et),
                      '        if (%s) vkr_get(r, %s, sizeof(*%s)); }' % (p.name, p.name, p.name)]
            elif k == 'single':
                if ch:
                    m += ['    if (vkr_get_u8(r)) {',
                          '        %s = (%s *)vkr_alloc(r, sizeof(%s));' % (p.name, et, et),
                          '        void *pn_ = vkr_pnext_srv_alloc(r, &ch_%s);' % p.name,
                          '        if (%s) { %s->sType = %s; %s->pNext = pn_; } }' % (p.name, p.name, stype_enum(base), p.name)]
                else:
                    m.append('    if (vkr_get_u8(r)) %s = (%s *)vkr_alloc(r, sizeof(%s));' % (p.name, et, et))
            elif k == 'bytes':
                m += ['    if (vkr_get_u8(r)) { n_%s = vkr_get_u32(r);' % p.name,
                      '        if (n_%s > VKR_MAX_BYTES) vkr_fail(&r->err, VKR_E_BOUNDS);' % p.name,
                      '        %s = vkr_alloc_arr(r, n_%s, 1, 0); }' % (p.name, p.name)]
                ln = p.len[0]
                if [q for q in ps if q.name == ln and q.depth]:
                    m.append('    if (%s && (!%s || *%s > n_%s)) vkr_fail(&r->err, VKR_E_COUNT);' % (p.name, ln, ln, p.name))
                else:
                    m.append('    if (((uint32_t)(%s) > 0 && !%s) || (%s && n_%s != (uint32_t)(%s))) vkr_fail(&r->err, VKR_E_COUNT);' %
                             (ln, p.name, p.name, p.name, ln))
            else:
                m += ['    if (vkr_get_u8(r)) { n_%s = vkr_get_u32(r);' % p.name,
                      '        if (n_%s > VKR_MAX_ARR) vkr_fail(&r->err, VKR_E_BOUNDS);' % p.name,
                      '        %s = (%s *)vkr_alloc_arr(r, n_%s, sizeof(%s), %s);' % (p.name, et, p.name, et, '4' if ch else '0')]
                if ch:
                    i = 'i%d' % g.u()
                    m += ['        ch_%s = (vkr_out_chain *)vkr_alloc(r, (size_t)(n_%s ? n_%s : 1) * sizeof(*ch_%s));' % (p.name, p.name, p.name, p.name),
                          '        if (%s && ch_%s) for (uint32_t %s = 0; %s < n_%s && !r->err; %s++) {' % (p.name, p.name, i, i, p.name, i),
                          '            %s[%s].sType = %s;' % (p.name, i, stype_enum(base)),
                          '            %s[%s].pNext = vkr_pnext_srv_alloc(r, &ch_%s[%s]);' % (p.name, i, p.name, i),
                          '        }']
                m.append('    }')
                ln = p.len[0]
                if '->' in ln:
                    pnm, mnm = ln.split('->', 1)
                    m.append('    { uint32_t exp_ = %s ? (uint32_t)(%s->%s) : 0;' % (pnm, pnm, mnm))
                    m.append('      if ((!%s && %s) || (exp_ > 0 && !%s) || (%s && n_%s != exp_)) vkr_fail(&r->err, VKR_E_COUNT); }' %
                             (pnm, p.name, p.name, p.name, p.name))
                elif [q for q in ps if q.name == ln and q.depth]:
                    m.append('    if (%s && (!%s || *%s > n_%s)) vkr_fail(&r->err, VKR_E_COUNT);' % (p.name, ln, ln, p.name))
                else:
                    m.append('    if ((uint32_t)(%s) > 0 && !%s) vkr_fail(&r->err, VKR_E_COUNT);' % (ln, p.name))
        m += ['    ' + x for x in count_checks(ps, lambda nm: nm, lambda nm: 'n_%s' % nm, True)]
        m += ['    vkr_put_u32(w, VKB_CMD_%s);' % c,
              '    if (r->err) { VkResult e_ = vkr_err_res(r->err); vkr_put(w, &e_, sizeof e_); return; }']
        args = ', '.join(p.name for p in ps)
        rt = R.ret[c]
        ret_scalar = rt not in ('VkResult', 'void')
        if rt == 'VkResult':
            m.append('    VkResult res = f->%s(%s);' % (c, args))
        elif rt == 'void':
            m += ['    f->%s(%s);' % (c, args), '    VkResult res = VK_SUCCESS;']
        else:
            m += ['    %s ret_ = f->%s(%s);' % (rt, c, args), '    VkResult res = VK_SUCCESS;']
        if c.startswith('vkDestroy'):
            obj = 'Vk' + c[len('vkDestroy'):]
            for p in ps:
                if p.depth == 0 and R.res(p.base) == obj and g.classify(p, True)[1] == 'handle':
                    m.append('    if (%s) vkr_h_drop(w->hctx, %s, (uint64_t)(uintptr_t)%s);' % (p.name, objtype(p.base), p.name))
        elif c == 'vkFreeMemory':
            m.append('    if (memory) vkr_h_drop(w->hctx, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t)(uintptr_t)memory);')
        elif c == 'vkFreeCommandBuffers':
            m.append('    if (pCommandBuffers) for (uint32_t i_ = 0; i_ < commandBufferCount; i_++)')
            m.append('        if (pCommandBuffers[i_]) vkr_h_drop(w->hctx, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(uintptr_t)pCommandBuffers[i_]);')
        elif c == 'vkFreeDescriptorSets':
            m.append('    if (res == VK_SUCCESS && pDescriptorSets) for (uint32_t i_ = 0; i_ < descriptorSetCount; i_++)')
            m.append('        if (pDescriptorSets[i_]) vkr_h_drop(w->hctx, VK_OBJECT_TYPE_DESCRIPTOR_SET, (uint64_t)(uintptr_t)pDescriptorSets[i_]);')
        m += ['    vkr_put(w, &res, sizeof res);', '    if (res < 0) return;']
        for p in ps:
            dr = dirn(p, ps)
            if dr == 'in':
                continue
            k, ek = g.classify(p, True)
            base = R.res(p.base)
            ch = is_chained_param(p)
            m.append('    vkr_put_u8(w, %s != NULL);' % p.name)
            if k == 'single':
                if ch:
                    m += ['    if (%s) { vkrw_body_%s(w, %s); vkr_pnext_srv_enc(w, &ch_%s); }' % (p.name, base, p.name, p.name)]
                else:
                    m += ['    if (%s) {' % p.name] + ['        ' + x for x in enc_val(ek, base, '(*%s)' % p.name)] + ['    }']
            elif k == 'bytes':
                m += ['    if (%s) { uint32_t c_ = (uint32_t)(%s); if (c_ > n_%s) c_ = n_%s; vkr_put_u32(w, c_);' %
                      (p.name, cnt_expr(ps, p.len[0]), p.name, p.name),
                      '        if (c_) vkr_put(w, %s, c_); }' % p.name]
            else:
                i = 'i%d' % g.u()
                m += ['    if (%s) { uint32_t c_ = (uint32_t)(%s); if (c_ > n_%s) c_ = n_%s; vkr_put_u32(w, c_);' %
                      (p.name, cnt_expr(ps, p.len[0]), p.name, p.name),
                      '        for (uint32_t %s = 0; %s < c_; %s++) {' % (i, i, i)]
                if ch:
                    m += ['            vkrw_body_%s(w, &%s[%s]);' % (base, p.name, i),
                          '            vkr_pnext_srv_enc(w, &ch_%s[%s]);' % (p.name, i)]
                else:
                    m += ['            ' + x for x in enc_val(ek, base, '%s[%s]' % (p.name, i))]
                m += ['        } }']
        if ret_scalar:
            m.append('    vkr_put(w, &ret_, sizeof ret_);')
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

    emit_icd_inc(R, ok, cmds, in_roots, set(in_pnext_exts), out, hdr)

    print('rpcgen: %d cmds bridged, %d not, %d structs (%d pNext in-exts, %d pNext out-exts)' %
          (len(ok), len(bad), len(structs), len(in_pnext_exts), len(pnext_exts)))
    for c, why in bad.items():
        print('rpcgen: skip %s: %s' % (c, why))


CUSTOM_ICD_CMDS = {
    'vkEnumerateInstanceVersion',
    'vkEnumerateInstanceLayerProperties',
    'vkEnumerateInstanceExtensionProperties',
    'vkCreateInstance',
    'vkDestroyInstance',
    'vkEnumeratePhysicalDevices',
    'vkEnumeratePhysicalDeviceGroups',
    'vkGetPhysicalDeviceProperties',
    'vkGetPhysicalDeviceProperties2',
    'vkEnumerateDeviceLayerProperties',
    'vkEnumerateDeviceExtensionProperties',
    'vkCreateDevice',
    'vkDestroyDevice',
    'vkGetDeviceQueue',
    'vkGetDeviceQueue2',
    'vkQueueSubmit',
    'vkQueueSubmit2',
    'vkAllocateMemory',
    'vkMapMemory',
    'vkGetMemoryFdKHR',
    'vkGetMemoryFdPropertiesKHR',
    'vkDestroyCommandPool',
    'vkAllocateCommandBuffers',
    'vkFreeCommandBuffers',
    'vkBeginCommandBuffer',
    'vkCmdExecuteCommands',
    'vkCreateDescriptorUpdateTemplate',
    'vkDestroyDescriptorUpdateTemplate',
    'vkUpdateDescriptorSetWithTemplate',
    'vkCmdPushDescriptorSetWithTemplate',
}

DENY_DEV_EXTS = {
    # Unbridged external sync FDs / host pointers / DRM modifiers
    'VK_KHR_external_semaphore_fd',
    'VK_KHR_external_fence_fd',
    'VK_EXT_external_memory_host',
    'VK_EXT_external_memory_acquire_unmodified',
    'VK_EXT_queue_family_foreign',
    'VK_EXT_image_drm_format_modifier',
    # WSI / swapchain / display (comes in S2)
    'VK_KHR_swapchain',
    'VK_KHR_swapchain_mutable_format',
    'VK_KHR_swapchain_maintenance1',
    'VK_EXT_swapchain_maintenance1',
    'VK_KHR_incremental_present',
    'VK_KHR_display_swapchain',
    'VK_KHR_shared_presentable_image',
    'VK_KHR_present_id',
    'VK_KHR_present_id2',
    'VK_KHR_present_wait',
    'VK_KHR_present_wait2',
    'VK_EXT_present_timing',
    'VK_EXT_display_control',
    'VK_EXT_hdr_metadata',
    'VK_GOOGLE_display_timing',
    # Memory map v2 / placed map (shadow memfd hooks vkMapMemory/vkUnmapMemory)
    'VK_KHR_map_memory2',
    'VK_EXT_map_memory_placed',
    # Callbacks / pipeline libraries / descriptor buffer
    'VK_EXT_device_memory_report',
    'VK_EXT_device_address_binding_report',
    'VK_EXT_graphics_pipeline_library',
    'VK_KHR_pipeline_library',
    'VK_EXT_descriptor_buffer',
    'VK_EXT_descriptor_heap',
}


def emit_icd_inc(R, ok, cmds, in_roots, in_pnext_set, out, hdr):
    import re
    ok_set = set(ok)
    icd_avail = set(ok) | {'vkEnumerateInstanceLayerProperties', 'vkEnumerateDeviceLayerProperties'}
    if 'vkMapMemory' in cmds:
        icd_avail.add('vkMapMemory')
    if 'vkUpdateDescriptorSets' in ok_set:
        icd_avail |= {'vkCreateDescriptorUpdateTemplate', 'vkDestroyDescriptorUpdateTemplate',
                      'vkUpdateDescriptorSetWithTemplate'}
    if 'vkCmdPushDescriptorSet' in ok_set:
        icd_avail.add('vkCmdPushDescriptorSetWithTemplate')

    base_active = {
        'VK_VERSION_1_0', 'VK_VERSION_1_1', 'VK_VERSION_1_2', 'VK_VERSION_1_3',
        'VK_BASE_VERSION_1_0', 'VK_COMPUTE_VERSION_1_0', 'VK_GRAPHICS_VERSION_1_0',
        'VK_BASE_VERSION_1_1', 'VK_COMPUTE_VERSION_1_1', 'VK_GRAPHICS_VERSION_1_1',
        'VK_BASE_VERSION_1_2', 'VK_COMPUTE_VERSION_1_2', 'VK_GRAPHICS_VERSION_1_2',
        'VK_BASE_VERSION_1_3', 'VK_COMPUTE_VERSION_1_3', 'VK_GRAPHICS_VERSION_1_3',
        'VK_KHR_get_physical_device_properties2',
        'VK_KHR_external_memory_capabilities',
        'VK_KHR_external_semaphore_capabilities',
        'VK_KHR_external_fence_capabilities',
    }

    def dep_ok(expr, active):
        if not expr:
            return True
        def tok_val(mo):
            t = mo.group(0)
            if '::' in t:
                return 'True'
            return 'True' if t in active else 'False'
        s = re.sub(r'[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z0-9_]+)*', tok_val, expr)
        s = s.replace('(', ' ( ').replace(')', ' ) ').replace('+', ' and ').replace(',', ' or ')
        return bool(eval(s, {'__builtins__': {}}))

    cand = {en for en in R.dev_exts if en not in DENY_DEV_EXTS}
    changed = True
    while changed:
        changed = False
        active = base_active | cand
        drop = set()
        for en in cand:
            for dep, rcmds, rtypes in R.dev_exts[en]:
                if not dep_ok(dep, active):
                    continue
                if any(c not in icd_avail for c in rcmds):
                    drop.add(en)
                    break
                bad_t = False
                for tp in rtypes:
                    exts = R.structextends.get(tp)
                    if exts and R.cat.get(tp) == 'struct' and (in_roots & set(exts)):
                        if tp not in in_pnext_set and tp != 'VkPipelineCreationFeedbackCreateInfo':
                            bad_t = True
                            break
                if bad_t:
                    drop.add(en)
                    break
        if drop:
            cand -= drop
            changed = True

    ic = [hdr, '/* Auto-generated ICD thunks, proc tables, and device extension allowlist */', '']
    for c in ok:
        if c in CUSTOM_ICD_CMDS:
            continue
        ps = R.cmds[c]
        if not ps:
            continue
        b0 = R.res(ps[0].base)
        if b0 not in ('VkPhysicalDevice', 'VkDevice', 'VkQueue', 'VkCommandBuffer'):
            continue
        rt = R.ret[c]
        pdecl = ', '.join('%s %s' % (p.ctype, p.name) for p in ps)
        ic.append('static VKAPI_ATTR %s VKAPI_CALL icd_%s(%s) {' % (rt, c, pdecl))
        if b0 == 'VkQueue':
            ic.append('    if (!%s) return%s;' % (ps[0].name, ' VK_ERROR_DEVICE_LOST' if rt == 'VkResult' else ''))
            a0 = 'QUEUE_RAW(%s)' % ps[0].name
        elif b0 == 'VkPhysicalDevice':
            a0 = 'PD_RAW(%s)' % ps[0].name
        elif b0 == 'VkDevice':
            if (c.startswith('vkDestroy') or c in ('vkFreeMemory', 'vkUnmapMemory')) and len(ps) >= 2 and R.kind(ps[1].base) == 'handle' and ps[1].depth == 0:
                ic.append('    if (!%s) return;' % ps[1].name)
            elif c == 'vkFreeDescriptorSets':
                ic.append('    if (!descriptorSetCount) return VK_SUCCESS;')
            a0 = 'DEV_RAW(%s)' % ps[0].name
        else:
            a0 = 'CMDBUF_RAW(%s)' % ps[0].name
        call_args = ', '.join(['c_', a0] + [p.name for p in ps[1:]])
        if rt == 'VkResult':
            ic.append('    ICD_CALL_RES(vkbc_%s(%s));' % (c, call_args))
        elif rt == 'void':
            ic.append('    ICD_CALL_VOID(vkbc_%s(%s));' % (c, call_args))
        else:
            ic += ['    pthread_mutex_lock(&g_mu);',
                   '    vkbc *c_ = icd_conn_locked();',
                   '    %s r_ = c_ ? vkbc_%s(%s) : (%s)0;' % (rt, c, call_args, rt),
                   '    pthread_mutex_unlock(&g_mu);',
                   '    return r_;']
        ic += ['}', '']

    def add_ent_with_aliases(lst, c):
        lst.append('    ENT("%s", icd_%s),' % (c, c))
        for al in R.valid_cmd_aliases.get(c, []):
            lst.append('    ENT("%s", icd_%s),' % (al, c))

    ic.append('static const proc_ent k_inst_procs[] = {')
    for c in ('vkDestroyInstance', 'vkEnumeratePhysicalDevices', 'vkEnumeratePhysicalDeviceGroups', 'vkCreateDevice'):
        if c in icd_avail:
            add_ent_with_aliases(ic, c)
    ic.append('    ENT("vkGetDeviceProcAddr", icd_vkGetDeviceProcAddr),')
    ic += ['};', '']

    ic.append('static const proc_ent k_pdev_procs[] = {')
    for c in cmds:
        if c in icd_avail and c != 'vkCreateDevice' and R.cmds.get(c) and R.res(R.cmds[c][0].base) == 'VkPhysicalDevice':
            add_ent_with_aliases(ic, c)
    if 'vkEnumerateDeviceLayerProperties' in icd_avail and 'vkEnumerateDeviceLayerProperties' not in cmds:
        add_ent_with_aliases(ic, 'vkEnumerateDeviceLayerProperties')
    ic += ['};', '']

    ic.append('static const proc_ent k_dev_procs[] = {')
    ic.append('    ENT("vkGetDeviceProcAddr", icd_vkGetDeviceProcAddr),')
    ordered_dev = list(cmds) + [x for x in ('vkCreateDescriptorUpdateTemplate', 'vkDestroyDescriptorUpdateTemplate',
                                            'vkUpdateDescriptorSetWithTemplate', 'vkCmdPushDescriptorSetWithTemplate')
                                if x not in cmds]
    for c in ordered_dev:
        if c in icd_avail and R.cmds.get(c) and R.res(R.cmds[c][0].base) in ('VkDevice', 'VkQueue', 'VkCommandBuffer'):
            add_ent_with_aliases(ic, c)
    ic += ['};', '']

    ic.append('static int icd_dev_ext_supported(const char *name) {')
    ic.append('    static const char *const k_dev_exts[] = {')
    for en in sorted(cand):
        ic.append('        "%s",' % en)
    ic += ['        NULL',
           '    };',
           '    for (size_t i = 0; k_dev_exts[i]; i++)',
           '        if (strcmp(name, k_dev_exts[i]) == 0) return 1;',
           '    return 0;',
           '}', '']
    open(os.path.join(out, 'vkb_icd_gen.inc'), 'w').write('\n'.join(ic))
