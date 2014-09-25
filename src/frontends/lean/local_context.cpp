/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "kernel/abstract.h"
#include "kernel/replace_fn.h"
#include "frontends/lean/local_context.h"

namespace lean {
/** \brief Given a list of local constants \c locals
              (x_n : A_n) ... (x_0 : A_0)
    and a term \c e
              t[x_0, ..., x_n]
    return
              t[#n, ..., #0]
*/
expr abstract_locals(expr const & e, list<expr> const & locals) {
    lean_assert(std::all_of(locals.begin(), locals.end(), [](expr const & e) { return closed(e) && is_local(e); }));
    if (!has_local(e))
        return e;
    return replace(e, [=](expr const & m, unsigned offset) -> optional<expr> {
            if (!has_local(m))
                return some_expr(m); // expression m does not contain local constants
            if (is_local(m)) {
                unsigned i = 0;
                for (expr const & l : locals) {
                    if (mlocal_name(l) == mlocal_name(m))
                        return some_expr(copy_tag(m, mk_var(offset + i)));
                    i++;
                }
            }
            return none_expr();
        });
}

local_context::local_context() {}
local_context::local_context(list<expr> const & ctx) {
    set_ctx(ctx);
}

void local_context::set_ctx(list<expr> const & ctx) {
    m_ctx = ctx;
    buffer<expr> tmp;
    list<expr> it = ctx;
    while (it) {
        tmp.push_back(abstract_locals(head(it), tail(it)));
        it = tail(it);
    }
    m_ctx_abstracted = to_list(tmp.begin(), tmp.end());
    lean_assert(std::all_of(m_ctx_abstracted.begin(), m_ctx_abstracted.end(), [](expr const & e) { return is_local(e); }));
}

expr local_context::pi_abstract_context(expr e, tag g) const {
    e = abstract_locals(e, m_ctx);
    for (expr const & l : m_ctx_abstracted)
        e = mk_pi(local_pp_name(l), mlocal_type(l), e, local_info(l)).set_tag(g);
    return e;
}

static expr apply_context_core(expr const & f, list<expr> const & ctx, tag g) {
    if (ctx)
        return mk_app(apply_context_core(f, tail(ctx), g), head(ctx)).set_tag(g);
    else
        return f;
}

expr local_context::apply_context(expr const & f, tag g) const {
    return apply_context_core(f, m_ctx, g);
}

expr local_context::mk_type_metavar(name_generator & ngen, tag g) {
    name n = ngen.next();
    expr s = mk_sort(mk_meta_univ(ngen.next())).set_tag(g);
    expr t = pi_abstract_context(s, g);
    return ::lean::mk_metavar(n, t).set_tag(g);
}

expr local_context::mk_type_meta(name_generator & ngen, tag g) {
    return apply_context(mk_type_metavar(ngen, g), g);
}

expr local_context::mk_metavar(name_generator & ngen, optional<expr> const & type, tag g) {
    name n      = ngen.next();
    expr r_type = type ? *type : mk_type_meta(ngen, g);
    expr t      = pi_abstract_context(r_type, g);
    return ::lean::mk_metavar(n, t).set_tag(g);
}

expr local_context::mk_meta(name_generator & ngen, optional<expr> const & type, tag g) {
    expr mvar = mk_metavar(ngen, type, g);
    expr meta = apply_context(mvar, g);
    return meta;
}

void local_context::add_local(expr const & l) {
    lean_assert(is_local(l));
    m_ctx_abstracted = cons(abstract_locals(l, m_ctx), m_ctx_abstracted);
    m_ctx            = cons(l, m_ctx);
    lean_assert(length(m_ctx) == length(m_ctx_abstracted));
    lean_assert(is_local(head(m_ctx_abstracted)));
}

list<expr> const & local_context::get_data() const {
    return m_ctx;
}
}
