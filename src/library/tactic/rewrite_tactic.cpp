/*
Copyright (c) 2015 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <algorithm>
#include <string>
#include "util/interrupt.h"
#include "util/list_fn.h"
#include "util/rb_map.h"
#include "util/sexpr/option_declarations.h"
#include "kernel/instantiate.h"
#include "kernel/error_msgs.h"
#include "kernel/abstract.h"
#include "kernel/replace_fn.h"
#include "kernel/for_each_fn.h"
#include "kernel/default_converter.h"
#include "kernel/inductive/inductive.h"
#include "library/normalize.h"
#include "library/kernel_serializer.h"
#include "library/reducible.h"
#include "library/util.h"
#include "library/expr_lt.h"
#include "library/match.h"
#include "library/projection.h"
#include "library/local_context.h"
#include "library/unifier.h"
#include "library/constants.h"
#include "library/generic_exception.h"
#include "library/tactic/rewrite_tactic.h"
#include "library/tactic/expr_to_tactic.h"
#include "library/tactic/class_instance_synth.h"

// #define TRACE_MATCH_PLUGIN

#ifndef LEAN_DEFAULT_REWRITER_MAX_ITERATIONS
#define LEAN_DEFAULT_REWRITER_MAX_ITERATIONS 200
#endif

#ifndef LEAN_DEFAULT_REWRITER_SYNTACTIC
#define LEAN_DEFAULT_REWRITER_SYNTACTIC false
#endif

#ifndef LEAN_DEFAULT_REWRITER_TRACE
#define LEAN_DEFAULT_REWRITER_TRACE true
#endif

namespace lean {
static name * g_rewriter_max_iterations = nullptr;
static name * g_rewriter_syntactic      = nullptr;
static name * g_rewriter_trace          = nullptr;

unsigned get_rewriter_max_iterations(options const & opts) {
    return opts.get_unsigned(*g_rewriter_max_iterations, LEAN_DEFAULT_REWRITER_MAX_ITERATIONS);
}

bool get_rewriter_syntactic(options const & opts) {
    return opts.get_bool(*g_rewriter_syntactic, LEAN_DEFAULT_REWRITER_SYNTACTIC);
}

bool get_rewriter_trace(options const & opts) {
    return opts.get_bool(*g_rewriter_trace, LEAN_DEFAULT_REWRITER_TRACE);
}

class unfold_info {
    list<name> m_names;
    location   m_location;
public:
    unfold_info() {}
    unfold_info(list<name> const & l, location const & loc):m_names(l), m_location(loc) {}
    list<name> const & get_names() const { return m_names; }
    location const & get_location() const { return m_location; }
    friend serializer & operator<<(serializer & s, unfold_info const & e) {
        write_list<name>(s, e.m_names);
        s << e.m_location;
        return s;
    }
    friend deserializer & operator>>(deserializer & d, unfold_info & e) {
        e.m_names = read_list<name>(d);
        d >> e.m_location;
        return d;
    }

    bool operator==(unfold_info const & i) const { return m_names == i.m_names && m_location == i.m_location; }
    bool operator!=(unfold_info const & i) const { return !operator==(i); }
};

class reduce_info {
    location  m_location;
public:
    reduce_info() {}
    reduce_info(location const & loc):m_location(loc) {}
    location const & get_location() const { return m_location; }
    friend serializer & operator<<(serializer & s, reduce_info const & e) {
        s << e.m_location;
        return s;
    }
    friend deserializer & operator>>(deserializer & d, reduce_info & e) {
        d >> e.m_location;
        return d;
    }

    bool operator==(reduce_info const & i) const { return m_location == i.m_location; }
    bool operator!=(reduce_info const & i) const { return !operator==(i); }
};

class rewrite_info {
public:
    enum multiplicity { Once, AtMostN, ExactlyN, ZeroOrMore, OneOrMore };
private:
    bool                 m_symm;
    multiplicity         m_multiplicity;
    optional<unsigned>   m_num;
    location             m_location;
    rewrite_info(bool symm, multiplicity m, optional<unsigned> const & n,
                 location const & loc):
        m_symm(symm), m_multiplicity(m), m_num(n), m_location(loc) {}
public:
    rewrite_info():m_symm(false), m_multiplicity(Once) {}
    static rewrite_info mk_once(bool symm, location const & loc) {
        return rewrite_info(symm, Once, optional<unsigned>(), loc);
    }

    static rewrite_info mk_at_most_n(unsigned n, bool symm, location const & loc) {
        return rewrite_info(symm, AtMostN, optional<unsigned>(n), loc);
    }

    static rewrite_info mk_exactly_n(unsigned n, bool symm, location const & loc) {
        return rewrite_info(symm, ExactlyN, optional<unsigned>(n), loc);
    }

    static rewrite_info mk_zero_or_more(bool symm, location const & loc) {
        return rewrite_info(symm, ZeroOrMore, optional<unsigned>(), loc);
    }

    static rewrite_info mk_one_or_more(bool symm, location const & loc) {
        return rewrite_info(symm, OneOrMore, optional<unsigned>(), loc);
    }

    bool operator==(rewrite_info const & i) const {
        return
            m_symm == i.m_symm && m_multiplicity == i.m_multiplicity &&
            m_num  == i.m_num  && m_location == i.m_location;
    }
    bool operator!=(rewrite_info const & i) const { return !operator==(i); }

    bool symm() const {
        return m_symm;
    }

    multiplicity get_multiplicity() const {
        return m_multiplicity;
    }

    bool has_num() const {
        return get_multiplicity() == AtMostN || get_multiplicity() == ExactlyN;
    }

    unsigned num() const {
        lean_assert(has_num());
        return *m_num;
    }

    location const & get_location() const { return m_location; }

    friend serializer & operator<<(serializer & s, rewrite_info const & e) {
        s << e.m_symm << static_cast<char>(e.m_multiplicity) << e.m_location;
        if (e.has_num())
            s << e.num();
        return s;
    }

    friend deserializer & operator>>(deserializer & d, rewrite_info & e) {
        char multp;
        d >> e.m_symm >> multp >> e.m_location;
        e.m_multiplicity = static_cast<rewrite_info::multiplicity>(multp);
        if (e.has_num())
            e.m_num = d.read_unsigned();
        return d;
    }
};

static expr * g_rewrite_tac                   = nullptr;

static name * g_rewrite_elem_name             = nullptr;
static std::string * g_rewrite_elem_opcode    = nullptr;

static name * g_rewrite_unfold_name             = nullptr;
static std::string * g_rewrite_unfold_opcode    = nullptr;

static name * g_rewrite_fold_name             = nullptr;
static std::string * g_rewrite_fold_opcode    = nullptr;

static name * g_rewrite_reduce_name             = nullptr;
static std::string * g_rewrite_reduce_opcode    = nullptr;

[[ noreturn ]] static void throw_re_ex() { throw exception("unexpected occurrence of 'rewrite' expression"); }

class rewrite_core_macro_cell : public macro_definition_cell {
public:
    virtual pair<expr, constraint_seq> get_type(expr const &, extension_context &) const { throw_re_ex(); }
    virtual optional<expr> expand(expr const &, extension_context &) const { throw_re_ex(); }
};

class rewrite_reduce_macro_cell : public rewrite_core_macro_cell {
    reduce_info m_info;
public:
    rewrite_reduce_macro_cell(reduce_info const & info):m_info(info) {}
    virtual name get_name() const { return *g_rewrite_reduce_name; }
    virtual void write(serializer & s) const {
        s << *g_rewrite_reduce_opcode << m_info;
    }
    reduce_info const & get_info() const { return m_info; }

    virtual bool operator==(macro_definition_cell const & other) const {
        if (auto o = dynamic_cast<rewrite_reduce_macro_cell const *>(&other))
            return m_info == o->m_info;
        return false;
    }
};

expr mk_rewrite_reduce(location const & loc) {
    macro_definition def(new rewrite_reduce_macro_cell(reduce_info(loc)));
    return mk_macro(def);
}

expr mk_rewrite_reduce_to(expr const & e, location const & loc) {
    macro_definition def(new rewrite_reduce_macro_cell(reduce_info(loc)));
    return mk_macro(def, 1, &e);
}

bool is_rewrite_reduce_step(expr const & e) {
    return is_macro(e) && macro_def(e).get_name() == *g_rewrite_reduce_name;
}

reduce_info const & get_rewrite_reduce_info(expr const & e) {
    lean_assert(is_rewrite_reduce_step(e));
    return static_cast<rewrite_reduce_macro_cell const*>(macro_def(e).raw())->get_info();
}

typedef reduce_info fold_info;

class rewrite_fold_macro_cell : public rewrite_core_macro_cell {
    fold_info m_info;
public:
    rewrite_fold_macro_cell(fold_info const & info):m_info(info) {}
    virtual name get_name() const { return *g_rewrite_fold_name; }
    virtual void write(serializer & s) const {
        s << *g_rewrite_fold_opcode << m_info;
    }
    fold_info const & get_info() const { return m_info; }

    virtual bool operator==(macro_definition_cell const & other) const {
        if (auto o = dynamic_cast<rewrite_fold_macro_cell const *>(&other))
            return m_info == o->m_info;
        return false;
    }
};

expr mk_rewrite_fold(expr const & e, location const & loc) {
    macro_definition def(new rewrite_fold_macro_cell(reduce_info(loc)));
    return mk_macro(def, 1, &e);
}

bool is_rewrite_fold_step(expr const & e) {
    return is_macro(e) && macro_def(e).get_name() == *g_rewrite_fold_name;
}

fold_info const & get_rewrite_fold_info(expr const & e) {
    lean_assert(is_rewrite_fold_step(e));
    return static_cast<rewrite_fold_macro_cell const*>(macro_def(e).raw())->get_info();
}

class rewrite_unfold_macro_cell : public rewrite_core_macro_cell {
    unfold_info m_info;
public:
    rewrite_unfold_macro_cell(unfold_info const & info):m_info(info) {}
    virtual name get_name() const { return *g_rewrite_unfold_name; }
    virtual void write(serializer & s) const {
        s << *g_rewrite_unfold_opcode << m_info;
    }
    unfold_info const & get_info() const { return m_info; }

    virtual bool operator==(macro_definition_cell const & other) const {
        if (auto o = dynamic_cast<rewrite_unfold_macro_cell const *>(&other))
            return m_info == o->m_info;
        return false;
    }
};

expr mk_rewrite_unfold(list<name> const & ns, location const & loc) {
    macro_definition def(new rewrite_unfold_macro_cell(unfold_info(ns, loc)));
    return mk_macro(def);
}

bool is_rewrite_unfold_step(expr const & e) {
    return is_macro(e) && macro_def(e).get_name() == *g_rewrite_unfold_name;
}

unfold_info const & get_rewrite_unfold_info(expr const & e) {
    lean_assert(is_rewrite_unfold_step(e));
    return static_cast<rewrite_unfold_macro_cell const*>(macro_def(e).raw())->get_info();
}

class rewrite_element_macro_cell : public rewrite_core_macro_cell {
    rewrite_info m_info;
public:
    rewrite_element_macro_cell(rewrite_info const & info):m_info(info) {}
    virtual name get_name() const { return *g_rewrite_elem_name; }
    virtual void write(serializer & s) const {
        s << *g_rewrite_elem_opcode << m_info;
    }
    rewrite_info const & get_info() const { return m_info; }

    virtual bool operator==(macro_definition_cell const & other) const {
        if (auto o = dynamic_cast<rewrite_element_macro_cell const *>(&other))
            return m_info == o->m_info;
        return false;
    }
};

expr mk_rw_macro(macro_definition const & def, optional<expr> const & pattern, expr const & H) {
    if (pattern) {
        expr args[2] = {H, *pattern};
        return mk_macro(def, 2, args);
    } else {
        return mk_macro(def, 1, &H);
    }
}

expr mk_rewrite_once(optional<expr> const & pattern, expr const & H, bool symm, location const & loc) {
    macro_definition def(new rewrite_element_macro_cell(rewrite_info::mk_once(symm, loc)));
    return mk_rw_macro(def, pattern, H);
}

expr mk_rewrite_zero_or_more(optional<expr> const & pattern, expr const & H, bool symm, location const & loc) {
    macro_definition def(new rewrite_element_macro_cell(rewrite_info::mk_zero_or_more(symm, loc)));
    return mk_rw_macro(def, pattern, H);
}

expr mk_rewrite_one_or_more(optional<expr> const & pattern, expr const & H, bool symm, location const & loc) {
    macro_definition def(new rewrite_element_macro_cell(rewrite_info::mk_one_or_more(symm, loc)));
    return mk_rw_macro(def, pattern, H);
}

expr mk_rewrite_at_most_n(optional<expr> const & pattern, expr const & H, bool symm, unsigned n, location const & loc) {
    macro_definition def(new rewrite_element_macro_cell(rewrite_info::mk_at_most_n(n, symm, loc)));
    return mk_rw_macro(def, pattern, H);
}

expr mk_rewrite_exactly_n(optional<expr> const & pattern, expr const & H, bool symm, unsigned n, location const & loc) {
    macro_definition def(new rewrite_element_macro_cell(rewrite_info::mk_exactly_n(n, symm, loc)));
    return mk_rw_macro(def, pattern, H);
}

bool is_rewrite_step(expr const & e) {
    return is_macro(e) && macro_def(e).get_name() == *g_rewrite_elem_name;
}

bool has_rewrite_pattern(expr const & e) {
    lean_assert(is_rewrite_step(e));
    return macro_num_args(e) == 2;
}

expr const & get_rewrite_rule(expr const & e) {
    lean_assert(is_rewrite_step(e));
    return macro_arg(e, 0);
}

expr const & get_rewrite_pattern(expr const & e) {
    lean_assert(has_rewrite_pattern(e));
    return macro_arg(e, 1);
}

rewrite_info const & get_rewrite_info(expr const & e) {
    lean_assert(is_rewrite_step(e));
    return static_cast<rewrite_element_macro_cell const*>(macro_def(e).raw())->get_info();
}

expr mk_rewrite_tactic_expr(buffer<expr> const & elems) {
    lean_assert(std::all_of(elems.begin(), elems.end(), [](expr const & e) {
                return is_rewrite_step(e) || is_rewrite_unfold_step(e) ||
                    is_rewrite_reduce_step(e) || is_rewrite_fold_step(e);
            }));
    return mk_app(*g_rewrite_tac, mk_expr_list(elems.size(), elems.data()));
}

class rewrite_match_plugin : public match_plugin {
#ifdef TRACE_MATCH_PLUGIN
    io_state       m_ios;
#endif
    type_checker & m_tc;
public:
#ifdef TRACE_MATCH_PLUGIN
    rewrite_match_plugin(io_state const & ios, type_checker & tc):
        m_ios(ios), m_tc(tc) {}
#else
    rewrite_match_plugin(io_state const &, type_checker & tc):
        m_tc(tc) {}
#endif

    virtual bool on_failure(expr const & p, expr const & t, match_context & ctx) const {
        try {
            constraint_seq cs;
            expr p1 = m_tc.whnf(p, cs);
            expr t1 = m_tc.whnf(t, cs);
            return !cs && (p1 != p || t1 != t) && ctx.match(p1, t1);
        } catch (exception&) {
            return false;
        }
    }

    // Return true iff the given declaration contains inst_implicit arguments
    bool has_inst_implicit_args(name const & d) const {
        if (auto decl = m_tc.env().find(d)) {
            expr const * it = &decl->get_type();
            while (is_pi(*it)) {
                if (binding_info(*it).is_inst_implicit())
                    return true;
                it = &binding_body(*it);
            }
            return false;
        } else {
            return false;
        }
    }

    virtual lbool pre(expr const & p, expr const & t, match_context & ctx) const {
        if (!is_app(p) || !is_app(t))
            return l_undef;
        expr const & p_fn = get_app_fn(p);
        if (!is_constant(p_fn))
            return l_undef;
        expr const & t_fn = get_app_fn(t);
        if (!is_constant(t_fn))
            return l_undef;
        if (!ctx.match(p_fn, t_fn))
            return l_undef;
        projection_info const * info = get_projection_info(m_tc.env(), const_name(p_fn));
        if (info && info->m_inst_implicit) {
            // Special support for projections
            buffer<expr> p_args, t_args;
            get_app_args(p, p_args);
            get_app_args(t, t_args);
            if (p_args.size() != t_args.size())
                return l_false;
            for (unsigned i = 0; i < p_args.size(); i++) {
                if (i == info->m_nparams)
                    continue; // skip structure
                if (!ctx.match(p_args[i], t_args[i]))
                    return l_false;
            }
            return l_true;
        }
        if (has_inst_implicit_args(const_name(p_fn))) {
            // Special support for declarations that contains inst_implicit arguments.
            // The idea is to skip them during matching.
            buffer<expr> p_args, t_args;
            get_app_args(p, p_args);
            get_app_args(t, t_args);
            if (p_args.size() != t_args.size())
                return l_false;
            expr const * it = &m_tc.env().get(const_name(p_fn)).get_type();
            for (unsigned i = 0; i < p_args.size(); i++) {
                if (is_pi(*it) && binding_info(*it).is_inst_implicit()) {
                    it = &binding_body(*it);
                    continue; // skip argument
                }
                if (!ctx.match(p_args[i], t_args[i]))
                    return to_lbool(on_failure(p, t, ctx)); // try to unfold if possible
                if (is_pi(*it))
                    it = &binding_body(*it);
            }
            return l_true;
        }
        return l_undef;
    }
};

class rewrite_fn {
    typedef std::shared_ptr<type_checker> type_checker_ptr;
    environment          m_env;
    io_state             m_ios;
    elaborate_fn         m_elab;
    proof_state          m_ps;
    name_generator       m_ngen;
    type_checker_ptr     m_tc;
    type_checker_ptr     m_matcher_tc;
    type_checker_ptr     m_unifier_tc; // reduce_to and check_trivial
    rewrite_match_plugin m_mplugin;
    goal                 m_g;
    local_context        m_ctx;
    substitution         m_subst;
    expr                 m_expr_loc; // auxiliary expression used for error localization

    bool                 m_use_trace;
    unsigned             m_max_iter;

    buffer<optional<level>> m_lsubst; // auxiliary buffer for pattern matching
    buffer<optional<expr>>  m_esubst; // auxiliary buffer for pattern matching

    [[ noreturn ]] void throw_rewrite_exception(char const * msg) {
        throw_generic_exception(msg, m_expr_loc);
    }

    [[ noreturn ]] void throw_rewrite_exception(sstream const & strm) {
        throw_generic_exception(strm, m_expr_loc);
    }

    [[ noreturn ]] void throw_max_iter_exceeded() {
        throw_rewrite_exception(sstream() << "rewrite tactic failed, maximum number of iterations exceeded "
                                << "(current threshold: " << m_max_iter
                                << ", increase the threshold by setting option 'rewrite.max_iter')");
    }

    void update_goal(goal const & g) {
        m_g   = g;
        m_ctx = m_g.to_local_context();
    }

    expr mk_meta(expr const & type) {
        return m_g.mk_meta(m_ngen.next(), type);
    }

    class rewriter_converter : public default_converter {
        list<name> const & m_to_unfold;
        bool             & m_unfolded;
    public:
        rewriter_converter(environment const & env, bool relax_main_opaque, list<name> const & to_unfold,
                           bool & unfolded):
            default_converter(env, relax_main_opaque),
            m_to_unfold(to_unfold), m_unfolded(unfolded) {}
        virtual bool is_opaque(declaration const & d) const {
            if (std::find(m_to_unfold.begin(), m_to_unfold.end(), d.get_name()) != m_to_unfold.end()) {
                m_unfolded = true;
                return false;
            } else {
                return true;
            }
        }
    };

    optional<expr> reduce(expr const & e, list<name> const & to_unfold) {
        bool unfolded          = !to_unfold;
        bool relax_main_opaque = false;
        auto tc = new type_checker(m_env, m_ngen.mk_child(),
                                   std::unique_ptr<converter>(new rewriter_converter(m_env, relax_main_opaque, to_unfold, unfolded)));
        constraint_seq cs;
        bool use_eta = true;
        expr r = normalize(*tc, e, cs, use_eta);
        if (!unfolded || cs) // FAIL if didn't unfolded or generated constraints
            return none_expr();
        return some_expr(r);
    }

    // Replace goal with definitionally equal one
    void replace_goal(expr const & new_type) {
        expr M = m_g.mk_meta(m_ngen.next(), new_type);
        goal new_g(M, new_type);
        assign(m_subst, m_g, M);
        update_goal(new_g);
    }

    bool process_reduce_goal(list<name> const & to_unfold) {
        if (auto new_type = reduce(m_g.get_type(), to_unfold)) {
            replace_goal(*new_type);
            return true;
        } else {
            return false;
        }
    }

    // Replace hypothesis type with definitionally equal one
    void replace_hypothesis(expr const & hyp, expr const & new_hyp_type) {
        expr new_hyp = update_mlocal(hyp, new_hyp_type);
        buffer<expr> new_hyps;
        m_g.get_hyps(new_hyps);
        for (expr & h : new_hyps) {
            if (mlocal_name(h) == mlocal_name(hyp)) {
                h = new_hyp;
                break;
            }
        }
        expr new_type = m_g.get_type();
        expr new_mvar = mk_metavar(m_ngen.next(), Pi(new_hyps, new_type));
        expr new_meta = mk_app(new_mvar, new_hyps);
        goal new_g(new_meta, new_type);
        assign(m_subst, m_g, new_meta);
        update_goal(new_g);
    }

    bool process_reduce_hypothesis(expr const & hyp, list<name> const & to_unfold) {
        if (auto new_hyp_type = reduce(mlocal_type(hyp), to_unfold)) {
            replace_hypothesis(hyp, *new_hyp_type);
            return true;
        } else {
            return false;
        }
    }

    bool process_reduce_step(list<name> const & to_unfold, location const & loc) {
        if (loc.is_goal_only())
            return process_reduce_goal(to_unfold);
        bool progress = false;
        buffer<expr> hyps;
        m_g.get_hyps(hyps);
        for (expr const & h : hyps) {
            if (!loc.includes_hypothesis(local_pp_name(h)))
                continue;
            if (process_reduce_hypothesis(h, to_unfold))
                progress = true;
        }
        if (loc.includes_goal()) {
            if (process_reduce_goal(to_unfold))
                progress = true;
        }
        return progress;
    }

    bool process_unfold_step(expr const & elem) {
        lean_assert(is_rewrite_unfold_step(elem));
        auto info = get_rewrite_unfold_info(elem);
        return process_reduce_step(info.get_names(), info.get_location());
    }

    optional<expr> fold(expr const & type, expr const & e, occurrence const & occ) {
        auto ecs       = m_elab(m_g, m_ngen.mk_child(), e, none_expr(), false);
        expr new_e     = ecs.first;
        if (ecs.second)
            return none_expr(); // contain constraints...
        optional<expr> unfolded_e = unfold_app(m_env, new_e);
        if (!unfolded_e)
            return none_expr();
        bool use_cache   = occ.is_all();
        unsigned occ_idx = 0;
        bool found       = false;
        expr new_type    =
            replace(type, [&](expr const & t, unsigned) {
                    if (closed(t)) {
                        constraint_seq cs;
                        if (m_tc->is_def_eq(t, *unfolded_e, justification(), cs) && !cs) {
                            occ_idx++;
                            if (occ.contains(occ_idx)) {
                                found = true;
                                return some_expr(new_e);
                            }
                        }
                    }
                    return none_expr();
                }, use_cache);
        if (found)
            return some_expr(new_type);
        else
            return none_expr();
    }

    bool process_fold_goal(expr const & e, occurrence const & occ) {
        if (auto new_type = fold(m_g.get_type(), e, occ)) {
            replace_goal(*new_type);
            return true;
        } else {
            return false;
        }
    }

    bool process_fold_hypothesis(expr const & hyp, expr const & e, occurrence const & occ) {
        if (auto new_hyp_type = fold(mlocal_type(hyp), e, occ)) {
            replace_hypothesis(hyp, *new_hyp_type);
            return true;
        } else {
            return false;
        }
    }

    bool process_fold_step(expr const & elem) {
        lean_assert(is_rewrite_fold_step(elem));
        location const & loc = get_rewrite_fold_info(elem).get_location();
        expr const & e       = macro_arg(elem, 0);
        if (loc.is_goal_only())
            return process_fold_goal(e, *loc.includes_goal());
        bool progress = false;
        buffer<expr> hyps;
        m_g.get_hyps(hyps);
        for (expr const & h : hyps) {
            auto occ = loc.includes_hypothesis(local_pp_name(h));
            if (!occ)
                continue;
            if (process_fold_hypothesis(h, e, *occ))
                progress = true;
        }
        if (auto occ = loc.includes_goal()) {
            if (process_fold_goal(e, *occ))
                progress = true;
        }
        return progress;
    }

    optional<expr> unify_with(expr const & t, expr const & e) {
        auto ecs       = m_elab(m_g, m_ngen.mk_child(), e, none_expr(), false);
        expr new_e     = ecs.first;
        buffer<constraint> cs;
        to_buffer(ecs.second, cs);
        constraint_seq cs_seq;
        if (!m_unifier_tc->is_def_eq(t, new_e, justification(), cs_seq))
            return none_expr();
        cs_seq.linearize(cs);
        unifier_config cfg;
        cfg.m_discard = true;
        unify_result_seq rseq = unify(m_env, cs.size(), cs.data(), m_ngen.mk_child(), m_subst, cfg);
        if (auto p = rseq.pull()) {
            substitution new_subst     = p->first.first;
            new_e   = new_subst.instantiate_all(new_e);
            if (has_expr_metavar_strict(new_e))
                return none_expr(); // new expressions was not completely instantiated
            m_subst = new_subst;
            return some_expr(new_e);
        }
        return none_expr();
    }

    bool process_reduce_to_goal(expr const & e) {
        if (auto new_type = unify_with(m_g.get_type(), e)) {
            replace_goal(*new_type);
            return true;
        } else {
            return false;
        }
    }

    bool process_reduce_to_hypothesis(expr const & hyp, expr const & e) {
        if (auto new_hyp_type = unify_with(mlocal_type(hyp), e)) {
            replace_hypothesis(hyp, *new_hyp_type);
            return true;
        } else {
            return false;
        }
    }

    bool process_reduce_to_step(expr const & e, location const & loc) {
        if (loc.is_goal_only())
            return process_reduce_to_goal(e);
        bool progress = false;
        buffer<expr> hyps;
        m_g.get_hyps(hyps);
        for (expr const & h : hyps) {
            if (!loc.includes_hypothesis(local_pp_name(h)))
                continue;
            if (process_reduce_to_hypothesis(h, e))
                progress = true;
        }
        if (loc.includes_goal()) {
            if (process_reduce_to_goal(e))
                progress = true;
        }
        return progress;
    }

    bool process_reduce_step(expr const & elem) {
        lean_assert(is_rewrite_reduce_step(elem));
        if (macro_num_args(elem) == 0) {
            auto info = get_rewrite_reduce_info(elem);
            return process_reduce_step(list<name>(), info.get_location());
        } else {
            auto info = get_rewrite_reduce_info(elem);
            return process_reduce_to_step(macro_arg(elem, 0), info.get_location());
        }
    }

    // Replace metavariables with special metavariables for the higher-order matcher. This is method is used when
    // converting an expression into a pattern.
    expr to_meta_idx(expr const & e) {
        m_lsubst.clear();
        m_esubst.clear();
        rb_map<expr, expr, expr_quick_cmp>  emap;
        name_map<level> lmap;

        auto to_meta_idx = [&](level const & l) {
            return replace(l, [&](level const & l) {
                    if (!has_meta(l)) {
                        return some_level(l);
                    } else if (is_meta(l)) {
                        if (auto it = lmap.find(meta_id(l))) {
                            return some_level(*it);
                        } else {
                            unsigned next_idx = m_lsubst.size();
                            level r = mk_idx_meta_univ(next_idx);
                            m_lsubst.push_back(none_level());
                            lmap.insert(meta_id(l), r);
                            return some_level(r);
                        }
                    } else {
                        return none_level();
                    }
                });
        };

        // return true if the arguments of e are not metavar applications
        auto no_meta_args = [&](expr const & e) {
            buffer<expr> args;
            get_app_args(e, args);
            return !std::any_of(args.begin(), args.end(), [&](expr const & e) { return is_meta(e); });
        };

        return replace(e, [&](expr const & e, unsigned) {
                if (!has_metavar(e)) {
                    return some_expr(e); // done
                } else if (is_binding(e)) {
                    unsigned next_idx = m_esubst.size();
                    expr r = mk_idx_meta(next_idx, m_tc->infer(e).first);
                    m_esubst.push_back(none_expr());
                    return some_expr(r);
                } else if (is_meta(e)) {
                    if (auto it = emap.find(e)) {
                        return some_expr(*it);
                    } else {
                        unsigned next_idx = m_esubst.size();
                        expr r = mk_idx_meta(next_idx, m_tc->infer(e).first);
                        m_esubst.push_back(none_expr());
                        if (no_meta_args(e))
                            emap.insert(e, r); // cache only if arguments of e are not metavariables
                        return some_expr(r);
                    }
                } else if (is_constant(e)) {
                    levels ls = map(const_levels(e), [&](level const & l) { return to_meta_idx(l); });
                    return some_expr(update_constant(e, ls));
                } else {
                    return none_expr();
                }
            });
    }

    // Given the rewrite step \c e, return a pattern to be used to locate the term to be rewritten.
    expr get_pattern(expr const & e) {
        lean_assert(is_rewrite_step(e));
        if (has_rewrite_pattern(e)) {
            return to_meta_idx(get_rewrite_pattern(e));
        } else {
            // Remark: we discard constraints generated producing the pattern.
            // Patterns are only used to locate positions where the rule should be applied.
            expr rule      = get_rewrite_rule(e);
            expr rule_type = m_tc->whnf(m_tc->infer(rule).first).first;
            while (is_pi(rule_type)) {
                expr meta  = mk_meta(binding_domain(rule_type));
                rule_type  = m_tc->whnf(instantiate(binding_body(rule_type), meta)).first;
            }
            if (!is_eq(rule_type))
                throw_rewrite_exception("invalid rewrite tactic, given lemma is not an equality");
            if (get_rewrite_info(e).symm()) {
                return to_meta_idx(app_arg(rule_type));
            } else {
                return to_meta_idx(app_arg(app_fn(rule_type)));
            }
        }
    }

    // Set m_esubst and m_lsubst elements to none
    void reset_subst() {
        for (optional<level> & l : m_lsubst)
            l = none_level();
        for (optional<expr> & e : m_esubst)
            e = none_expr();
    }

    pair<expr, constraint> mk_class_instance_elaborator(expr const & type) {
        unifier_config cfg;
        cfg.m_kind               = unifier_kind::VeryConservative;
        bool use_local_instances = true;
        bool is_strict           = false;
        return ::lean::mk_class_instance_elaborator(m_env, m_ios, m_ctx, m_ngen.next(), optional<name>(),
                                                    m_ps.relax_main_opaque(), use_local_instances, is_strict,
                                                    some_expr(type), m_expr_loc.get_tag(), cfg, nullptr);
    }

    // target, new_target, H  : represents the rewrite (H : target = new_target) for hypothesis
    // and (H : new_target = target) for goals
    typedef optional<std::tuple<expr, expr, expr>> find_result;

    struct failure {
        enum kind { Unification, Exception, HasMetavars };
        expr m_elab_lemma;
        expr m_subterm;
        kind m_kind;
        failure(expr const & elab_lemma, expr const & subterm, kind k):
            m_elab_lemma(elab_lemma), m_subterm(subterm), m_kind(k) {}

        format pp(formatter const & fmt) const {
            format r;
            switch (m_kind) {
            case Unification:
                r  = compose(line(), format("-fail to unify equation source"));
                r += pp_indent_expr(fmt, m_elab_lemma);
                r += compose(line(), format("with subterm"));
                r += pp_indent_expr(fmt, m_subterm);
                return r;
            case Exception:
                r  = compose(line(), format("-an exception occurred when unifying the subterm"));
                r += pp_indent_expr(fmt, m_subterm);
                return r;
            case HasMetavars:
                r  = compose(line(), format("-lemma still contains meta-variables"));
                r += pp_indent_expr(fmt, m_elab_lemma);
                r += compose(line(), format("after the equation source has been unified with subterm"));
                r += pp_indent_expr(fmt, m_subterm);
                return r;
            }
            lean_unreachable();
        }
    };

    // Store information for a goal or hypothesis being rewritten
    struct target_trace {
        bool           m_is_hypothesis;
        expr           m_target;        // term being rewritten
        list<failure>  m_failures;      // sub-terms that matched but failed to unify
        optional<expr> m_matched;       // sub-term  that matched and unified

        target_trace(expr const & t, bool is_hyp): m_is_hypothesis(is_hyp), m_target(t) {}

        format pp(formatter const & fmt) const {
            if (m_matched)
                return format();
            if (!m_failures) {
                if (m_is_hypothesis)
                    return line() + format("no subterm in the hypothesis '") + fmt(m_target) + format("' matched the pattern");
                else
                    return line() + format("no subterm in the goal matched the pattern");
            }
            format r;
            if (m_is_hypothesis)
                r = line() + format("matching failures in the hypothesis '") + fmt(m_target) + format("'");
            else
                r = line() + format("matching failures in the goal");
            buffer<failure> b;
            unsigned indent = get_pp_indent(fmt.get_options());
            to_buffer(m_failures, b);
            unsigned i = b.size();
            while (i > 0) {
                --i;
                r += nest(indent, b[i].pp(fmt));
            }
            return r;
        }
    };

    struct trace {
        expr                 m_lemma;    // lemma being used for rewriting
        expr                 m_pattern;  // extracted or given pattern
        buffer<target_trace> m_targets;  // trace for each element we tried to rewrite using m_lemma

        format pp(formatter const & fmt) const {
            format r("rewrite step failed using pattern");
            r += pp_indent_expr(fmt, m_pattern);
            for (target_trace const & t : m_targets) {
                r += t.pp(fmt);
            }
            return r;
        }
    };

    bool  m_trace_initialized;
    trace m_trace; // temporary object used to store execution trace for a rewrite step

    void clear_trace() {
        m_trace_initialized = false;
    }

    void init_trace(expr const & lemma, expr const & pattern) {
        if (m_use_trace) {
            m_trace_initialized = true;
            m_trace.m_lemma     = lemma;
            m_trace.m_pattern   = pattern;
            m_trace.m_targets.clear();
        }
    }

    void add_target(expr const & t, bool is_hyp) {
        if (m_use_trace)
            m_trace.m_targets.push_back(target_trace(t, is_hyp));
    }

    target_trace & latest_target() {
        lean_assert(m_use_trace);
        lean_assert(!m_trace.m_targets.empty());
        return m_trace.m_targets.back();
    }

    void add_target_failure(expr const & elab_term, expr const & subterm, failure::kind k) {
        if (m_use_trace) {
            target_trace & tt = latest_target();
            lean_assert(!tt.m_matched);
            tt.m_failures     = cons(failure(elab_term, subterm, k), tt.m_failures);
        }
    }

    void add_target_match(expr const & m) {
        if (m_use_trace) {
            target_trace & tt = latest_target();
            lean_assert(!tt.m_matched);
            tt.m_matched = m;
        }
    }

    // rule, new_t
    typedef optional<pair<expr, expr>> unify_result;

    // When successful, the result is the pair (H, new_t) where
    //   (H : new_t = t) if is_goal == true
    //   (H : t = new_t) if is_goal == false
    unify_result unify_target(expr const & t, expr const & orig_elem, bool is_goal) {
        try {
            expr rule         = get_rewrite_rule(orig_elem);
            auto rcs          = m_elab(m_g, m_ngen.mk_child(), rule, none_expr(), false);
            rule              = rcs.first;
            buffer<constraint> cs;
            to_buffer(rcs.second, cs);
            constraint_seq cs_seq;
            expr rule_type = m_tc->whnf(m_tc->infer(rule, cs_seq), cs_seq);
            while (is_pi(rule_type)) {
                expr meta;
                if (binding_info(rule_type).is_inst_implicit()) {
                    auto mc = mk_class_instance_elaborator(binding_domain(rule_type));
                    meta    = mc.first;
                    cs_seq += mc.second;
                } else {
                    meta = mk_meta(binding_domain(rule_type));
                }
                rule_type  = m_tc->whnf(instantiate(binding_body(rule_type), meta), cs_seq);
                rule       = mk_app(rule, meta);
            }
            lean_assert(is_eq(rule_type));
            bool symm = get_rewrite_info(orig_elem).symm();
            expr src;
            if (symm)
                src = app_arg(rule_type);
            else
                src = app_arg(app_fn(rule_type));
            if (!m_tc->is_def_eq(t, src, justification(), cs_seq)) {
                add_target_failure(src, t, failure::Unification);
                return unify_result();
            }
            cs_seq.linearize(cs);
            unifier_config cfg;
            cfg.m_kind         = unifier_kind::Liberal;
            cfg.m_discard      = true;
            unify_result_seq rseq = unify(m_env, cs.size(), cs.data(), m_ngen.mk_child(), m_subst, cfg);
            if (auto p = rseq.pull()) {
                substitution new_subst     = p->first.first;
                rule      = new_subst.instantiate_all(rule);
                rule_type = new_subst.instantiate_all(rule_type);
                if (has_expr_metavar_strict(rule) || has_expr_metavar_strict(rule_type)) {
                    add_target_failure(rule, t, failure::HasMetavars);
                    return unify_result(); // rule was not completely instantiated.
                }
                m_subst = new_subst;
                expr lhs = app_arg(app_fn(rule_type));
                expr rhs = app_arg(rule_type);
                add_target_match(t);
                if (is_goal) {
                    if (symm) {
                        return unify_result(rule, lhs);
                    } else {
                        rule = mk_symm(*m_tc, rule);
                        return unify_result(rule, rhs);
                    }
                } else {
                    if (symm) {
                        rule = mk_symm(*m_tc, rule);
                        return unify_result(rule, lhs);
                    } else {
                        return unify_result(rule, rhs);
                    }
                }
            } else {
                add_target_failure(src, t, failure::Unification);
                return unify_result();
            }
        } catch (exception&) {}
        add_target_failure(orig_elem, t, failure::Exception);
        return unify_result();
    }

    // Search for \c pattern in \c e. If \c t is a match, then try to unify the type of the rule
    // in the rewrite step \c orig_elem with \c t.
    // When successful, this method returns the target \c t, the fully elaborated rule \c r,
    // and the new value \c new_t (i.e., the expression that will replace \c t).
    //
    // \remark is_goal == true if \c e is the type of a goal. Otherwise, it is assumed to be the type
    // of a hypothesis. This flag affects the equality proof built by this method.
    find_result find_target(expr const & e, expr const & pattern, expr const & orig_elem, bool is_goal) {
        find_result result;
        for_each(e, [&](expr const & t, unsigned) {
                if (result)
                    return false; // stop search
                if (closed(t)) {
                    lean_assert(std::all_of(m_esubst.begin(), m_esubst.end(), [&](optional<expr> const & e) { return !e; }));
                    bool assigned = false;
                    bool r = match(pattern, t, m_lsubst, m_esubst, nullptr, nullptr, &m_mplugin, &assigned);
                    if (assigned)
                        reset_subst();
                    if (r) {
                        if (auto p = unify_target(t, orig_elem, is_goal)) {
                            result = std::make_tuple(t, p->second, p->first);
                            return false;
                        }
                    }
                }
                return true;
            });
        return result;
    }

    bool process_rewrite_hypothesis(expr const & hyp, expr const & orig_elem, expr const & pattern, occurrence const & occ) {
        add_target(hyp, true);
        expr Pa = mlocal_type(hyp);
        bool is_goal = false;
        if (auto it = find_target(Pa, pattern, orig_elem, is_goal)) {
            expr a, Heq, b; // Heq is a proof of a = b
            std::tie(a, b, Heq) = *it;

            bool has_dep_elim = inductive::has_dep_elim(m_env, get_eq_name());
            unsigned vidx = has_dep_elim ? 1 : 0;
            expr Px  = replace_occurrences(Pa, a, occ, vidx);
            expr Pb  = instantiate(Px, vidx, b);

            expr A   = m_tc->infer(a).first;
            level l1 = sort_level(m_tc->ensure_type(Pa).first);
            level l2 = sort_level(m_tc->ensure_type(A).first);
            expr H;
            if (has_dep_elim) {
                expr Haeqx = mk_app(mk_constant(get_eq_name(), {l1}), A, b, mk_var(0));
                expr P     = mk_lambda("x", A, mk_lambda("H", Haeqx, Px));
                H          = mk_app({mk_constant(get_eq_rec_name(), {l1, l2}), A, a, P, hyp, b, Heq});
            } else {
                H          = mk_app({mk_constant(get_eq_rec_name(), {l1, l2}), A, a, mk_lambda("x", A, Px), hyp, b, Heq});
            }

            expr new_hyp   = update_mlocal(hyp, Pb);
            buffer<expr> new_hyps;
            buffer<expr> args;
            m_g.get_hyps(new_hyps);
            for (expr & h : new_hyps) {
                if (mlocal_name(h) == mlocal_name(hyp)) {
                    h = new_hyp;
                    args.push_back(H);
                } else {
                    args.push_back(h);
                }
            }
            expr new_type = m_g.get_type();
            expr new_mvar = mk_metavar(m_ngen.next(), Pi(new_hyps, new_type));
            expr new_meta = mk_app(new_mvar, new_hyps);
            goal new_g(new_meta, new_type);
            assign(m_subst, m_g, mk_app(new_mvar, args));
            update_goal(new_g);
            return true;
        }
        return false;
    }

    bool process_rewrite_goal(expr const & orig_elem, expr const & pattern, occurrence const & occ) {
        expr Pa      = m_g.get_type();
        add_target(Pa, false);
        bool is_goal = true;
        if (auto it = find_target(Pa, pattern, orig_elem, is_goal)) {
            expr a, Heq, b;
            std::tie(a, b, Heq) = *it;

            // Given (a, b, P[a], Heq : b = a, occ), return (P[b], M : P[b], H : P[a])
            // where M is a metavariable application of a fresh metavariable, and H is a witness (based on M) for P[a].
            bool has_dep_elim = inductive::has_dep_elim(m_env, get_eq_name());
            unsigned vidx = has_dep_elim ? 1 : 0;
            expr Px  = replace_occurrences(Pa, a, occ, vidx);
            expr Pb  = instantiate(Px, vidx, b);
            expr A   = m_tc->infer(a).first;
            level l1 = sort_level(m_tc->ensure_type(Pa).first);
            level l2 = sort_level(m_tc->ensure_type(A).first);
            expr M   = m_g.mk_meta(m_ngen.next(), Pb);
            expr H;
            if (has_dep_elim) {
                expr Haeqx = mk_app(mk_constant(get_eq_name(), {l2}), A, b, mk_var(0));
                expr P     = mk_lambda("x", A, mk_lambda("H", Haeqx, Px));
                H          = mk_app({mk_constant(get_eq_rec_name(), {l1, l2}), A, b, P, M, a, Heq});
            } else {
                H          = mk_app({mk_constant(get_eq_rec_name(), {l1, l2}), A, b, mk_lambda("x", A, Px), M, a, Heq});
            }

            goal new_g(M, Pb);
            assign(m_subst, m_g, H);
            update_goal(new_g);
            // regular(m_env, m_ios) << "FOUND\n" << a << "\n==>\n" << b << "\nWITH\n" << Heq << "\n";
            // regular(m_env, m_ios) << H << "\n";
            return true;
        }
        return false;
    }

    bool process_rewrite_single_step(expr const & orig_elem, expr const & pattern) {
        check_system("rewrite tactic");
        rewrite_info const & info = get_rewrite_info(orig_elem);
        location const & loc      = info.get_location();
        if (loc.is_goal_only())
            return process_rewrite_goal(orig_elem, pattern, *loc.includes_goal());
        bool progress = false;
        buffer<expr> hyps;
        m_g.get_hyps(hyps);
        for (expr const & h : hyps) {
            auto occ = loc.includes_hypothesis(local_pp_name(h));
            if (!occ)
                continue;
            if (process_rewrite_hypothesis(h, orig_elem, pattern, *occ))
                progress = true;
        }
        if (auto occ = loc.includes_goal()) {
            if (process_rewrite_goal(orig_elem, pattern, *occ))
                progress = true;
        }
        return progress;
    }

    void check_max_iter(unsigned i) {
        if (i >= m_max_iter)
            throw_max_iter_exceeded();
    }

    bool process_rewrite_step(expr const & elem, expr const & orig_elem) {
        lean_assert(is_rewrite_step(elem));
        expr pattern              = get_pattern(elem);
        init_trace(orig_elem, pattern);
        // regular(m_env, m_ios) << "pattern: " << pattern << "\n";
        rewrite_info const & info = get_rewrite_info(elem);
        unsigned i, num;
        switch (info.get_multiplicity()) {
        case rewrite_info::Once:
            return process_rewrite_single_step(orig_elem, pattern);
        case rewrite_info::AtMostN:
            num = info.num();
            for (i = 0; i < std::min(num, m_max_iter); i++) {
                if (!process_rewrite_single_step(orig_elem, pattern))
                    return true;
            }
            check_max_iter(i);
            return true;
        case rewrite_info::ExactlyN:
            num = info.num();
            for (i = 0; i < std::min(num, m_max_iter); i++) {
                if (!process_rewrite_single_step(orig_elem, pattern))
                    return false;
            }
            check_max_iter(i);
            return true;
        case rewrite_info::ZeroOrMore:
            for (i = 0; i < m_max_iter; i++) {
                if (!process_rewrite_single_step(orig_elem, pattern))
                    return true;
            }
            throw_max_iter_exceeded();
        case rewrite_info::OneOrMore:
            if (!process_rewrite_single_step(orig_elem, pattern))
                return false;
            for (i = 0; i < m_max_iter; i++) {
                if (!process_rewrite_single_step(orig_elem, pattern))
                    return true;
            }
            throw_max_iter_exceeded();
        }
        lean_unreachable();
    }

    // Process the given rewrite element/step. This method destructively update
    // m_g, m_subst, m_ngen. It returns true if it succeeded and false otherwise.
    bool process_step(expr const & elem) {
        clear_trace();
        if (is_rewrite_unfold_step(elem)) {
            return process_unfold_step(elem);
        } else if (is_rewrite_fold_step(elem)) {
            return process_fold_step(elem);
        } else if (is_rewrite_reduce_step(elem)) {
            return process_reduce_step(elem);
        } else {
            expr rule = get_rewrite_rule(elem);
            expr new_elem;
            if (has_rewrite_pattern(elem)) {
                expr pattern     = m_elab(m_g, m_ngen.mk_child(), get_rewrite_pattern(elem), none_expr(), false).first;
                expr new_args[2] = { rule, pattern };
                new_elem         = mk_macro(macro_def(elem), 2, new_args);
            } else {
                rule     = m_elab(m_g, m_ngen.mk_child(), rule, none_expr(), false).first;
                new_elem = mk_macro(macro_def(elem), 1, &rule);
            }
            return process_rewrite_step(new_elem, elem);
        }
    }

    bool check_trivial_goal() {
        expr type = m_g.get_type();
        if (is_eq(type) || (is_iff(type) && m_env.impredicative())) {
            constraint_seq cs;
            expr lhs = app_arg(app_fn(type));
            expr rhs = app_arg(type);
            if (m_unifier_tc->is_def_eq(lhs, rhs, justification(), cs) && !cs) {
                expr H = is_eq(type) ? mk_refl(*m_tc, lhs) : mk_iff_refl(lhs);
                assign(m_subst, m_g, H);
                return true;
            } else {
                return false;
            }
        } else if (type == mk_true()) {
            assign(m_subst, m_g, mk_constant(get_eq_intro_name()));
            return true;
        } else {
            return false;
        }
    }

    class match_converter : public unfold_reducible_converter {
    public:
        match_converter(environment const & env, bool relax_main_opaque):
            unfold_reducible_converter(env, relax_main_opaque, true) {}
        virtual bool is_opaque(declaration const & d) const {
            if (is_projection(m_env, d.get_name()))
                return true;
            return unfold_reducible_converter::is_opaque(d);
        }
    };

    type_checker_ptr mk_matcher_tc() {
        if (get_rewriter_syntactic(m_ios.get_options())) {
            // use an everything opaque converter
            return mk_opaque_type_checker(m_env, m_ngen.mk_child());
        } else {
            return std::unique_ptr<type_checker>(new type_checker(m_env, m_ngen.mk_child(),
                   std::unique_ptr<converter>(new match_converter(m_env, m_ps.relax_main_opaque()))));
        }
    }

public:
    rewrite_fn(environment const & env, io_state const & ios, elaborate_fn const & elab, proof_state const & ps):
        m_env(env), m_ios(ios), m_elab(elab), m_ps(ps), m_ngen(ps.get_ngen()),
        m_tc(mk_type_checker(m_env, m_ngen.mk_child(), ps.relax_main_opaque(), UnfoldQuasireducible)),
        m_matcher_tc(mk_matcher_tc()),
        m_unifier_tc(mk_type_checker(m_env, m_ngen.mk_child(), ps.relax_main_opaque())),
        m_mplugin(m_ios, *m_matcher_tc) {
        m_ps = apply_substitution(m_ps);
        goals const & gs = m_ps.get_goals();
        lean_assert(gs);
        update_goal(head(gs));
        m_subst = m_ps.get_subst();
        m_max_iter  = get_rewriter_max_iterations(ios.get_options());
        m_use_trace = get_rewriter_trace(ios.get_options());
    }

    proof_state_seq operator()(buffer<expr> const & elems) {
        for (expr const & elem : elems) {
            flet<expr> set1(m_expr_loc, elem);
            if (!process_step(elem)) {
                if (m_ps.report_failure()) {
                    proof_state curr_ps(m_ps, cons(m_g, tail(m_ps.get_goals())), m_subst, m_ngen);
                    if (!m_use_trace || !m_trace_initialized) {
                        throw tactic_exception("rewrite step failed", some_expr(elem), curr_ps,
                                               [](formatter const &) { return format("invalid 'rewrite' tactic, rewrite step failed"); });
                    } else {
                        trace saved_trace = m_trace;
                        throw tactic_exception("rewrite step failed", some_expr(elem), curr_ps,
                                               [=](formatter const & fmt) {
                                                   format r = format("invalid 'rewrite' tactic, ");
                                                   r       += saved_trace.pp(fmt);
                                                   return r;
                                               });
                    }
                }
                return proof_state_seq();
            }
        }

        goals new_gs;
        if (check_trivial_goal())
            new_gs = tail(m_ps.get_goals());
        else
            new_gs = cons(m_g, tail(m_ps.get_goals()));
        proof_state new_ps(m_ps, new_gs, m_subst, m_ngen);
        return proof_state_seq(new_ps);
    }
};

tactic mk_rewrite_tactic(elaborate_fn const & elab, buffer<expr> const & elems) {
    return tactic([=](environment const & env, io_state const & ios, proof_state const & s) {
            goals const & gs = s.get_goals();
            if (empty(gs)) {
                throw_no_goal_if_enabled(s);
                return proof_state_seq();
            }
            return rewrite_fn(env, ios, elab, s)(elems);
        });
}

void initialize_rewrite_tactic() {
    g_rewriter_max_iterations = new name{"rewriter", "max_iter"};
    register_unsigned_option(*g_rewriter_max_iterations, LEAN_DEFAULT_REWRITER_MAX_ITERATIONS,
                             "(rewriter tactic) maximum number of iterations");
    g_rewriter_syntactic      = new name{"rewriter", "syntactic"};
    register_bool_option(*g_rewriter_syntactic, LEAN_DEFAULT_REWRITER_SYNTACTIC,
                         "(rewriter tactic) if true tactic will not unfold any constant when performing pattern matching");
    g_rewriter_trace          = new name{"rewriter", "trace"};
    register_bool_option(*g_rewriter_trace, LEAN_DEFAULT_REWRITER_TRACE,
                         "(rewriter tactic) if true tactic will generate a trace for rewrite step failures");
    name rewrite_tac_name{"tactic", "rewrite_tac"};
    g_rewrite_tac           = new expr(Const(rewrite_tac_name));
    g_rewrite_reduce_name   = new name("rewrite_reduce");
    g_rewrite_reduce_opcode = new std::string("RWR");
    g_rewrite_unfold_name   = new name("rewrite_unfold");
    g_rewrite_unfold_opcode = new std::string("RWU");
    g_rewrite_fold_name   = new name("rewrite_fold");
    g_rewrite_fold_opcode = new std::string("RWF");
    g_rewrite_elem_name     = new name("rewrite_element");
    g_rewrite_elem_opcode   = new std::string("RWE");
    register_macro_deserializer(*g_rewrite_reduce_opcode,
                                [](deserializer & d, unsigned num, expr const * args) {
                                    if (num > 1)
                                        throw corrupted_stream_exception();
                                    reduce_info info;
                                    d >> info;
                                    if (num == 0)
                                        return mk_rewrite_reduce(info.get_location());
                                    else
                                        return mk_rewrite_reduce_to(args[0], info.get_location());
                                });
    register_macro_deserializer(*g_rewrite_fold_opcode,
                                [](deserializer & d, unsigned num, expr const * args) {
                                    if (num != 1)
                                        throw corrupted_stream_exception();
                                    fold_info info;
                                    d >> info;
                                    return mk_rewrite_fold(args[0], info.get_location());
                                });
    register_macro_deserializer(*g_rewrite_unfold_opcode,
                                [](deserializer & d, unsigned num, expr const *) {
                                    if (num != 0)
                                        throw corrupted_stream_exception();
                                    unfold_info info;
                                    d >> info;
                                    macro_definition def(new rewrite_unfold_macro_cell(info));
                                    return mk_macro(def);
                                });
    register_macro_deserializer(*g_rewrite_elem_opcode,
                                [](deserializer & d, unsigned num, expr const * args) {
                                    if (num != 1 && num != 2)
                                        throw corrupted_stream_exception();
                                    rewrite_info info;
                                    d >> info;
                                    macro_definition def(new rewrite_element_macro_cell(info));
                                    return mk_macro(def, num, args);
                                });
    register_tac(rewrite_tac_name,
                 [](type_checker &, elaborate_fn const & elab, expr const & e, pos_info_provider const *) {
                     buffer<expr> args;
                     get_tactic_expr_list_elements(app_arg(e), args, "invalid 'rewrite' tactic, invalid argument");
                     for (expr const & arg : args) {
                         if (!is_rewrite_step(arg) && !is_rewrite_unfold_step(arg) &&
                             !is_rewrite_reduce_step(arg) && !is_rewrite_fold_step(arg))
                             throw expr_to_tactic_exception(e, "invalid 'rewrite' tactic, invalid argument");
                     }
                     return mk_rewrite_tactic(elab, args);
                 });
}

void finalize_rewrite_tactic() {
    delete g_rewriter_max_iterations;
    delete g_rewriter_syntactic;
    delete g_rewriter_trace;
    delete g_rewrite_tac;
    delete g_rewrite_reduce_name;
    delete g_rewrite_reduce_opcode;
    delete g_rewrite_unfold_name;
    delete g_rewrite_unfold_opcode;
    delete g_rewrite_fold_name;
    delete g_rewrite_fold_opcode;
    delete g_rewrite_elem_name;
    delete g_rewrite_elem_opcode;
}
}
