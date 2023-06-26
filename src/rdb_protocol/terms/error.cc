// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/op.hpp"

namespace ql {

class error_term_t : public op_term_t {
public:
    error_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(0, 1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(eval_error *err_out, scope_env_t *env, args_t *args, eval_flags_t) const {
        if (args->num_args() == 0) {
            rfail(base_exc_t::EMPTY_USER, "Empty ERROR term outside a default block.");
        } else {
            auto v = args->arg(err_out, env, 0);
            if (err_out->has()) { return noval(); }
            rfail(base_exc_t::USER, "%s", v->as_str().to_std().c_str());
        }
    }
    virtual const char *name() const { return "error"; }
};

class default_term_t : public op_term_t {
public:
    default_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(2)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(eval_error *err_out, scope_env_t *env, args_t *args, eval_flags_t) const {
        datum_t func_arg;
        scoped_ptr_t<exc_t> err;
        scoped_ptr_t<val_t> v;
        try {
            eval_error eval_err;
            v = args->arg(&eval_err, env, 0);
            /* Duplicates code in the catch blocks. */
            if (eval_err.exc.has()) {
                if (eval_err.exc->get_type() == base_exc_t::NON_EXISTENCE) {
                    err.init(new exc_t(*eval_err.exc));  // TODO: Just std::move(eval_err.exc)?
                    func_arg = datum_t(eval_err.exc->what());  // TODO: <- take care here if we std::move.
                } else {
                    *err_out = std::move(eval_err);
                    return noval();
                }

            } else if (eval_err.datum_exc.has()) {
                if (eval_err.datum_exc->get_type() == base_exc_t::NON_EXISTENCE) {
                    const datum_exc_t& e = *eval_err.datum_exc;
                    err.init(new exc_t(e.get_type(), e.what(), backtrace()));
                    func_arg = datum_t(e.what());
                } else {
                    *err_out = std::move(eval_err);
                    return noval();
                }
            }

            if (v->get_type().is_convertible(val_t::type_t::DATUM)) {
                func_arg = v->as_datum();
                if (func_arg.get_type() != datum_t::R_NULL) {
                    return v;
                }
            } else {
                return v;
            }
        } catch (const exc_t &e) {
            /* Duplicated above. */
            if (e.get_type() == base_exc_t::NON_EXISTENCE) {
                err.init(new exc_t(e));
                func_arg = datum_t(e.what());
            } else {
                throw;
            }
        } catch (const datum_exc_t &e) {
            if (e.get_type() == base_exc_t::NON_EXISTENCE) {
                err.init(new exc_t(e.get_type(), e.what(), backtrace()));
                func_arg = datum_t(e.what());
            } else {
                throw;
            }
        }
        r_sanity_check(func_arg.has());
        r_sanity_check(func_arg.get_type() == datum_t::R_NULL
                       || func_arg.get_type() == datum_t::R_STR);
        try {
            eval_error eval_err;
            scoped_ptr_t<val_t> def = args->arg(&eval_err, env, 1);
            if (eval_err.has()) {
                eval_err.throw_exc();
            }
            if (def->get_type().is_convertible(val_t::type_t::FUNC)) {
                return def->as_func()->call(env->env, func_arg);
            } else {
                return def;
            }
        } catch (const base_exc_t &e) {
            if (e.get_type() == base_exc_t::EMPTY_USER) {
                if (err.has()) {
                    throw *err;
                } else {
                    r_sanity_check(func_arg.get_type() == datum_t::R_NULL);
                    return v;
                }
            } else {
                throw;
            }
        }
    }
    virtual const char *name() const { return "error"; }
    virtual bool can_be_grouped() const { return false; }
};

counted_t<term_t> make_error_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<error_term_t>(env, term);
}
counted_t<term_t> make_default_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<default_term_t>(env, term);
}


} // namespace ql
