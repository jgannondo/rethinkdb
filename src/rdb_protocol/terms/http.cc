// Copyright 2010-2014 RethinkDB, all rights reserved.
#include <stdint.h>

#include <string>
#include "debug.hpp"

#include "math.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/terms/terms.hpp"
#include "extproc/http_runner.hpp"

namespace ql {

class http_term_t : public op_term_t {
public:
    http_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        op_term_t(env, term, argspec_t(1),
                  optargspec_t({"data",
                                "timeout",
                                "method",
                                "params",
                                "header",
                                "attempts",
                                "redirects",
                                "verify",
                                "page",
                                "page_limit",
                                "auth",
                                "result_format" }))
    { }
private:
    virtual const char *name() const { return "http"; }

    // No HTTP term is considered deterministic
    virtual bool is_deterministic() const {
        return false;
    }

    virtual counted_t<val_t> eval_impl(scope_env_t *env,
                                       UNUSED eval_flags_t flags);

    // Functions to get optargs into the http_opts_t
    void get_optargs(scope_env_t *env, http_opts_t *opts_out);

    void get_bool_optarg(const std::string &optarg_name,
                         scope_env_t *env,
                         bool *bool_out);

    void get_redirects(scope_env_t *env,
                       uint32_t *redirects_out);

    void get_attempts(scope_env_t *env,
                      uint64_t *attempts_out);

    void get_result_format(scope_env_t *env,
                           http_result_format_t *result_format_out);

    void get_params(scope_env_t *env,
                    counted_t<const datum_t> *params_out);

    void get_data(scope_env_t *env,
                  std::string *data_out,
                  std::map<std::string, std::string> *form_data_out,
                  std::vector<std::string> *header_out,
                  http_method_t method);

    void get_timeout_ms(scope_env_t *env,
                        uint64_t *timeout_ms_out);

    void get_header(scope_env_t *env,
                    std::vector<std::string> *header_out);

    void get_method(scope_env_t *env,
                    http_method_t *method_out);

    void get_auth(scope_env_t *env,
                  http_opts_t::http_auth_t *auth_out);

    void get_page_and_limit(scope_env_t *env,
                            counted_t<func_t> *depaginate_fn_out,
                            int64_t *depaginate_limit_out);

    // Helper functions, used in optarg parsing
    void verify_header_string(const std::string &str,
                              pb_rcheckable_t *header);

    std::string print_http_param(const counted_t<const datum_t> &datum,
                                 const char *val_name,
                                 const char *key_name,
                                 pb_rcheckable_t *val);

    std::string get_auth_item(const counted_t<const datum_t> &datum,
                              const std::string &name,
                              pb_rcheckable_t *auth);

    // Have a maximum timeout of 30 days
    static const uint64_t MAX_TIMEOUT_MS = 2592000000ull;
};

void check_url_params(const counted_t<const datum_t> &params,
                      pb_rcheckable_t *val) {
    if (params->get_type() == datum_t::R_OBJECT) {
        const std::map<std::string, counted_t<const datum_t> > &params_map =
            params->as_object();
        for (auto it = params_map.begin(); it != params_map.end(); ++it) {
            if (it->second->get_type() != datum_t::R_NUM &&
                it->second->get_type() != datum_t::R_STR &&
                it->second->get_type() != datum_t::R_NULL) {
                rfail_target(val, base_exc_t::GENERIC,
                             "Expected `params.%s` to be a NUMBER, STRING or NULL, "
                             "but found %s:\n%s",
                             it->first.c_str(), it->second->get_type_name().c_str(),
                             it->second->print().c_str());
            }
        }
    } else {
        rfail_target(val, base_exc_t::GENERIC,
                     "Expected `params` to be an OBJECT, but found %s:\n%s",
                     params->get_type_name().c_str(),
                     params->print().c_str());
    }
}

class http_datum_stream_t : public eager_datum_stream_t {
public:
    http_datum_stream_t(http_opts_t &&_opts,
                        counted_t<func_t> &&_depaginate_fn,
                        int64_t _depaginate_limit,
                        const protob_t<const Backtrace> &bt) :
        eager_datum_stream_t(bt),
        opts(std::move(_opts)),
        depaginate_fn(_depaginate_fn),
        depaginate_limit(_depaginate_limit),
        more(depaginate_limit != 0) { }

    bool is_array() { return false; }
    bool is_exhausted() const { return !more && batch_cache_exhausted(); }
    bool is_cfeed() const { return false; }

private:
    std::vector<counted_t<const datum_t> >
    next_raw_batch(env_t *env, UNUSED const batchspec_t &batchspec);

    // Helper functions used during `next_raw_batch`
    bool apply_depaginate(env_t *env, const http_result_t &res);

    bool handle_depage_result(counted_t<const datum_t> depage);
    bool apply_depage_url(counted_t<const datum_t> new_url);
    void apply_depage_params(counted_t<const datum_t> new_params);

    http_opts_t opts;
    object_buffer_t<http_runner_t> runner;
    counted_t<func_t> depaginate_fn;
    int64_t depaginate_limit;
    bool more;
};


void check_error_result(const http_result_t &res,
                        const http_opts_t &opts,
                        pb_rcheckable_t *parent) {
    if (!res.error.empty()) {
        std::string error_string = strprintf("Error in HTTP %s of `%s`: %s.",
                                             http_method_to_str(opts.method).c_str(),
                                             opts.url.c_str(),
                                             res.error.c_str());
        if (res.header.has()) {
            error_string.append("\nheader:\n" + res.header->print());
        }

        if (res.body.has()) {
            error_string.append("\nbody:\n" + res.body->print());
        }

        // Any error coming back from the extproc may be due to the fragility of
        // interfacing with external servers.  Provide a non-existence error so that
        // users may call `r.default` for more robustness.
        rfail_target(parent, base_exc_t::NON_EXISTENCE,
                     "%s", error_string.c_str());
    }
}

void dispatch_http(env_t *env,
                   const http_opts_t &opts,
                   http_runner_t *runner,
                   http_result_t *res_out,
                   pb_rcheckable_t* parent) {
    try {
        runner->http(opts, res_out, env->interruptor);
    } catch (const http_worker_exc_t &ex) {
        res_out->error.assign("crash in a worker process");
    } catch (const interrupted_exc_t &ex) {
        res_out->error.assign("interrupted");
    } catch (const std::exception &ex) {
        res_out->error = std::string("encounted an exception - ") + ex.what();
    } catch (...) {
        res_out->error.assign("encountered an unknown exception");
    }

    check_error_result(*res_out, opts, parent);
}

counted_t<val_t> http_term_t::eval_impl(scope_env_t *env,
                                        UNUSED eval_flags_t flags) {
    http_opts_t opts;
    opts.url.assign(arg(env, 0)->as_str().to_std());
    opts.proxy.assign(env->env->reql_http_proxy);
    get_optargs(env, &opts);

    counted_t<func_t> depaginate_fn;
    int64_t depaginate_limit(0);
    get_page_and_limit(env, &depaginate_fn, &depaginate_limit);

    // If we're depaginating, return a stream that will be evaluated automatically
    if (depaginate_fn.has()) {
        counted_t<datum_stream_t> http_stream = counted_t<datum_stream_t>(
            new http_datum_stream_t(std::move(opts),
                                    std::move(depaginate_fn),
                                    depaginate_limit,
                                    backtrace()));
        return new_val(env->env, http_stream);
    }

    // Otherwise, just run the http operation and return the datum
    http_result_t res;
    http_runner_t runner(env->env->extproc_pool);
    dispatch_http(env->env, opts, &runner, &res, this);

    return new_val(res.body);
}

std::vector<counted_t<const datum_t> >
http_datum_stream_t::next_raw_batch(env_t *env, UNUSED const batchspec_t &batchspec) {
    if (!more) {
        return std::vector<counted_t<const datum_t> >();
    }

    if (!runner.has()) {
        runner.create(env->extproc_pool);
    }

    profile::sampler_t sampler(strprintf("Performing HTTP %s of `%s`",
                                         http_method_to_str(opts.method).c_str(),
                                         opts.url.c_str()),
                               env->trace);

    http_result_t res;
    dispatch_http(env, opts, runner.get(), &res, this);
    r_sanity_check(res.body.has());

    // Set doneness so next batch we return an empty result to indicate
    // the end of the stream
    more = apply_depaginate(env, res);

    if (res.body->get_type() == datum_t::R_ARRAY) {
        return res.body->as_array();
    }

    return std::vector<counted_t<const datum_t> >({ res.body });
}

// Returns true if another request should be made, false otherwise
bool http_datum_stream_t::apply_depaginate(env_t *env, const http_result_t &res) {
    if (depaginate_limit > 0) {
        --depaginate_limit;
    }
    if (depaginate_limit == 0) {
        return false;
    }

    // Provide an empty OBJECT datum instead of any non-existent arguments
    counted_t<const datum_t> empty = make_counted<const datum_t>(datum_t::R_OBJECT);
    std::map<std::string, counted_t<const datum_t> > arg_obj
            { { "params", opts.url_params.has() ? opts.url_params : empty },
              { "header", res.header.has() ? res.header : empty },
              { "body",   res.body.has() ? res.body : empty } };
    std::vector<counted_t<const datum_t> > args
        { make_counted<const datum_t>(std::move(arg_obj)) };

    try {
        counted_t<const datum_t> depage = depaginate_fn->call(env, args)->as_datum();
        return handle_depage_result(depage);
    } catch (const ql::exc_t &ex) {
        // Tack on some debugging info, as this shit can be tough
        throw ql::exc_t(ex.get_type(),
                        strprintf("Error in HTTP %s of `%s`: %s.\n"
                                  "Error occurred during `page` called with:\n%s\n",
                                  http_method_to_str(opts.method).c_str(),
                                  opts.url.c_str(),
                                  ex.what(),
                                  args[0]->print().c_str()),
                        ex.backtrace());
    }
}

bool http_datum_stream_t::apply_depage_url(counted_t<const datum_t> new_url) {
    // NULL url indicates no further depagination
    if (new_url->get_type() == datum_t::R_NULL) {
        return false;
    } else if (new_url->get_type() != datum_t::R_STR) {
        rfail(base_exc_t::GENERIC,
              "Expected `url` in OBJECT returned by `page` to be a "
              "STRING or NULL, but found %s.",
              new_url->get_type_name().c_str());
    }
    opts.url.assign(new_url->as_str().to_std());
    return true;
}

void http_datum_stream_t::apply_depage_params(counted_t<const datum_t> new_params) {
    // Verify new params and merge with the old ones, new taking precedence
    check_url_params(new_params, this);
    opts.url_params->merge(new_params);
}

bool http_datum_stream_t::handle_depage_result(counted_t<const datum_t> depage) {
    if (depage->get_type() == datum_t::R_NULL ||
        depage->get_type() == datum_t::R_STR) {
        return apply_depage_url(depage);
    } else if (depage->get_type() == datum_t::R_OBJECT) {
        counted_t<const datum_t> new_url = depage->get("url", NOTHROW);
        counted_t<const datum_t> new_params = depage->get("params", NOTHROW);
        if (!new_url.has() && !new_params.has()) {
            rfail(base_exc_t::GENERIC,
                  "OBJECT returned by `page` must contain "
                  "`url` and/or `params` fields.");
        }

        if (new_params.has()) {
            apply_depage_params(new_params);
        }

        if (new_url.has()) {
            return apply_depage_url(new_url);
        }
    } else {
        rfail(base_exc_t::GENERIC,
              "Expected `page` to return an OBJECT, but found %s.",
              depage->get_type_name().c_str());
    }

    return true;
}

// Depagination functions must follow the given specification:
// OBJECT fn(OBJECT, OBJECT, DATUM)
// Return value: OBJECT containing the following fields
//   url - the new url to get
//   params - the new parameters to use (will be merged with old parameters)
// Parameter 2: last URL parameters used
// Parameter 3: last headers received
// Parameter 4: last data received
void http_term_t::get_page_and_limit(scope_env_t *env,
                                     counted_t<func_t> *depaginate_fn_out,
                                     int64_t *depaginate_limit_out) {
    counted_t<val_t> page = optarg(env, "page");
    counted_t<val_t> page_limit = optarg(env, "page_limit");

    if (!page.has()) {
        return;
    } else if (!page_limit.has()) {
        rfail_target(page, base_exc_t::GENERIC,
                     "Cannot use `page` without specifying `page_limit` "
                     "(a positive number performs that many requests, -1 is unlimited).");
    }

    *depaginate_fn_out = page->as_func(PAGE_SHORTCUT);
    *depaginate_limit_out = page_limit->as_int<int64_t>();

    if (*depaginate_limit_out < -1) {
        rfail_target(page_limit, base_exc_t::GENERIC,
                     "`page_limit` should be greater than or equal to -1.");
    }
}

void http_term_t::get_optargs(scope_env_t *env,
                              http_opts_t *opts_out) {
    get_auth(env, &opts_out->auth);
    get_method(env, &opts_out->method);

    // get_data must be called before get_header, as it may set the Content-Type header
    // in some cases, and the user should be able to override that behavior
    get_data(env, &opts_out->data, &opts_out->form_data,
             &opts_out->header, opts_out->method);

    get_result_format(env, &opts_out->result_format);
    get_params(env, &opts_out->url_params);
    get_header(env, &opts_out->header);
    get_timeout_ms(env, &opts_out->timeout_ms);
    get_attempts(env, &opts_out->attempts);
    get_redirects(env, &opts_out->max_redirects);
    get_bool_optarg("verify", env, &opts_out->verify);
}

// The `timeout` optarg specifies the number of seconds to wait before erroring
// out of the HTTP request.  This must be a NUMBER, but may be fractional.
void http_term_t::get_timeout_ms(scope_env_t *env,
                                 uint64_t *timeout_ms_out) {
    counted_t<val_t> timeout = optarg(env, "timeout");
    if (timeout.has()) {
        double tmp = timeout->as_num();
        tmp *= 1000;

        if (tmp < 0) {
            rfail_target(timeout.get(), base_exc_t::GENERIC,
                         "`timeout` may not be negative.");
        } else {
            *timeout_ms_out = clamp<double>(tmp, 0, MAX_TIMEOUT_MS);
        }
    }
}

// Don't allow header strings to include newlines
void http_term_t::verify_header_string(const std::string &str,
                                       pb_rcheckable_t *header) {
    if (str.find_first_of("\r\n") != std::string::npos) {
        rfail_target(header, base_exc_t::GENERIC,
                     "A `header` item contains newline characters.");
    }
}

// The `header` optarg allows the user to specify HTTP header values.
// The optarg must be an OBJECT or an ARRAY of STRINGs.
// As an OBJECT, each value must be a STRING or NULL, and each key/value pair
//  will be converted into a single header line with the format "key: value".
//  If the value is NULL, the header line will be "key:".
// As an ARRAY, each item must be a STRING, and each item will result
//  in exactly one header line, being copied directly over
// Header lines are not allowed to contain newlines.
void http_term_t::get_header(scope_env_t *env,
                             std::vector<std::string> *header_out) {
    counted_t<val_t> header = optarg(env, "header");
    if (header.has()) {
        counted_t<const datum_t> datum_header = header->as_datum();
        if (datum_header->get_type() == datum_t::R_OBJECT) {
            const std::map<std::string, counted_t<const datum_t> > &header_map =
                datum_header->as_object();
            for (auto it = header_map.begin(); it != header_map.end(); ++it) {
                std::string str;
                if (it->second->get_type() == datum_t::R_STR) {
                    str = strprintf("%s: %s", it->first.c_str(),
                                    it->second->as_str().c_str());
                } else if (it->second->get_type() != datum_t::R_NULL) {
                    rfail_target(header.get(), base_exc_t::GENERIC,
                        "Expected `header.%s` to be a STRING or NULL, but found %s.",
                        it->first.c_str(), it->second->get_type_name().c_str());
                }
                verify_header_string(str, header.get());
                header_out->push_back(str);
            }
        } else if (datum_header->get_type() == datum_t::R_ARRAY) {
            for (size_t i = 0; i < datum_header->size(); ++i) {
                counted_t<const datum_t> line = datum_header->get(i);
                if (line->get_type() != datum_t::R_STR) {
                    rfail_target(header.get(), base_exc_t::GENERIC,
                        "Expected `header[%zu]` to be a STRING, but found %s.",
                        i, line->get_type_name().c_str());
                }
                std::string str = line->as_str().to_std();
                verify_header_string(str, header.get());
                header_out->push_back(str);
            }
        } else {
            rfail_target(header.get(), base_exc_t::GENERIC,
                "Expected `header` to be an ARRAY or OBJECT, but found %s.",
                datum_header->get_type_name().c_str());
        }
    }
}

// The `method` optarg must be a STRING, and specify one of the following
// supported HTTP request methods: GET, HEAD, POST, PUT, PATCH, or DELETE.
void http_term_t::get_method(scope_env_t *env,
                             http_method_t *method_out) {
    counted_t<val_t> method = optarg(env, "method");
    if (method.has()) {
        std::string method_str = method->as_str().to_std();
        if (method_str == "GET") {
            *method_out = http_method_t::GET;
        } else if (method_str == "HEAD") {
            *method_out = http_method_t::HEAD;
        } else if (method_str == "POST") {
            *method_out = http_method_t::POST;
        } else if (method_str == "PUT") {
            *method_out = http_method_t::PUT;
        } else if (method_str == "PATCH") {
            *method_out = http_method_t::PATCH;
        } else if (method_str == "DELETE") {
            *method_out = http_method_t::DELETE;
        } else {
            rfail_target(method.get(), base_exc_t::GENERIC,
                         "`method` (%s) is not recognized (GET, HEAD, POST, PUT, "
                         "PATCH and DELETE are allowed).", method_str.c_str());
        }
    }
}

std::string http_term_t::get_auth_item(const counted_t<const datum_t> &datum,
                                       const std::string &name,
                                       pb_rcheckable_t *auth) {
    counted_t<const datum_t> item = datum->get(name, NOTHROW);
    if (!item.has()) {
        rfail_target(auth, base_exc_t::GENERIC,
                     "`auth.%s` not found in the auth object.", name.c_str());
    } else if (item->get_type() != datum_t::R_STR) {
        rfail_target(auth, base_exc_t::GENERIC,
                     "Expected `auth.%s` to be a STRING, but found %s.",
                     name.c_str(), item->get_type_name().c_str());
    }
    return item->as_str().to_std();
}

// The `auth` optarg takes an object consisting of the following fields:
//  type - STRING, the type of authentication to perform 'basic' or 'digest'
//      defaults to 'basic'
//  user - STRING, the username to use
//  pass - STRING, the password to use
void http_term_t::get_auth(scope_env_t *env,
                           http_opts_t::http_auth_t *auth_out) {
    counted_t<val_t> auth = optarg(env, "auth");
    if (auth.has()) {
        counted_t<const datum_t> datum_auth = auth->as_datum();
        if (datum_auth->get_type() != datum_t::R_OBJECT) {
            rfail_target(auth.get(), base_exc_t::GENERIC,
                         "Expected `auth` to be an OBJECT, but found %s.",
                         datum_auth->get_type_name().c_str());
        }

        // Default to 'basic' if no type is specified
        std::string type;
        {
            counted_t<const datum_t> type_datum = datum_auth->get("type", NOTHROW);

            if (type_datum.has()) {
                if (type_datum->get_type() != datum_t::R_STR) {
                    rfail_target(auth.get(), base_exc_t::GENERIC,
                                 "Expected `auth.type` to be a STRING, but found %s.",
                                 datum_auth->get_type_name().c_str());
                }
                type.assign(type_datum->as_str().to_std());
            } else {
                type.assign("basic");
            }
        }

        std::string user = get_auth_item(datum_auth, "user", auth.get());
        std::string pass = get_auth_item(datum_auth, "pass", auth.get());

        if (type == "basic") {
            auth_out->make_basic_auth(std::move(user), std::move(pass));
        } else if (type == "digest") {
            auth_out->make_digest_auth(std::move(user), std::move(pass));
        } else {
            rfail_target(auth.get(), base_exc_t::GENERIC, "`auth.type` is not "
                         "recognized ('basic', and 'digest' are allowed).");
        }
    }
}

std::string http_term_t::print_http_param(const counted_t<const datum_t> &datum,
                                          const char *val_name,
                                          const char *key_name,
                                          pb_rcheckable_t *val) {
    if (datum->get_type() == datum_t::R_NUM) {
        return strprintf("%" PR_RECONSTRUCTABLE_DOUBLE,
                         datum->as_num());
    } else if (datum->get_type() == datum_t::R_STR) {
        return datum->as_str().to_std();
    } else if (datum->get_type() == datum_t::R_NULL) {
        return std::string();
    }

    rfail_target(val, base_exc_t::GENERIC,
                 "Expected `%s.%s` to be a NUMBER, STRING or NULL, but found %s.",
                 val_name, key_name, datum->get_type_name().c_str());
}

// The `data` optarg is used to pass in the data to be passed in the body of the
// request.  The sematics for this depend on the type of request being performed.
// This optarg is only allowed for PUT, POST, or PATCH requests.
// PUT, PATCH, and DELETE requests take any value and print it to JSON to pass to
//  the remote server (and will set Content-Type to application/json), unless the
//  value is a STRING, in which case it will be written directly to the request body.
// POST requests take either a STRING or OBJECT
//  POST with a STRING will pass the literal string in the request body
//  POST with an OBJECT must have NUMBER, STRING, or NULL values.  These will be
//   converted into a string of form-encoded key-value pairs of the format
//   "key=val&key=val" in the request body.
void http_term_t::get_data(scope_env_t *env,
        std::string *data_out,
        std::map<std::string, std::string> *form_data_out,
        std::vector<std::string> *header_out,
        http_method_t method) {
    counted_t<val_t> data = optarg(env, "data");
    if (data.has()) {
        counted_t<const datum_t> datum_data = data->as_datum();
        if (method == http_method_t::PUT ||
            method == http_method_t::PATCH ||
            method == http_method_t::DELETE) {
            if (datum_data->get_type() == datum_t::R_STR) {
                data_out->assign(datum_data->as_str().to_std());
            } else {
                // Set the Content-Type to application/json - this may be overwritten
                // later by the 'header' optarg
                header_out->push_back("Content-Type: application/json");
                data_out->assign(datum_data->print());
            }
        } else if (method == http_method_t::POST) {
            if (datum_data->get_type() == datum_t::R_STR) {
                // Use the put data for this, as we assume the user does any
                // encoding they need when they pass a string
                data_out->assign(datum_data->as_str().to_std());
            } else if (datum_data->get_type() == datum_t::R_OBJECT) {
                const std::map<std::string, counted_t<const datum_t> > &form_map =
                    datum_data->as_object();
                for (auto it = form_map.begin(); it != form_map.end(); ++it) {
                    std::string val_str = print_http_param(it->second,
                                                           "data",
                                                           it->first.c_str(),
                                                           data.get());
                    (*form_data_out)[it->first] = val_str;
                }
            } else {
                rfail_target(data.get(), base_exc_t::GENERIC,
                    "Expected `data` to be a STRING or OBJECT, but found %s.",
                    datum_data->get_type_name().c_str());
            }
        } else {
            rfail_target(this, base_exc_t::GENERIC,
                         "`data` should only be specified on a PUT, POST, PATCH, "
                         "or DELETE request.");
        }
    }
}

// The `params` optarg specifies parameters to append to the requested URL, in the
// format "?key=val&key=val". The optarg must be an OBJECT with NUMBER, STRING, or
// NULL values. A NULL value will result in "key=" with no value.
// Values are sanitized here, but converted in the extproc.
void http_term_t::get_params(scope_env_t *env,
        counted_t<const datum_t> *params_out) {
    counted_t<val_t> params = optarg(env, "params");
    if (params.has()) {
        *params_out = params->as_datum();
        check_url_params(*params_out, params.get());
    }
}

// The `result_format` optarg specifies how to interpret the HTTP result body from
// the server. This option must be a STRING, and one of `auto`, `json`, `jsonp`, or
// `text`.
//  json - The result should be JSON and parsed into native datum_t objects
//  jsonp - The result should be padded-JSON according to the format proposed on
//    www.json-p.org
//  text - The result should be returned as a literal string
//  auto - The result will be parsed as JSON if the Content-Type is application/json,
//         or a string otherwise.
void http_term_t::get_result_format(scope_env_t *env,
                                    http_result_format_t *result_format_out) {
    counted_t<val_t> result_format = optarg(env, "result_format");
    if (result_format.has()) {
        std::string result_format_str = result_format->as_str().to_std();
        if (result_format_str == "auto") {
            *result_format_out = http_result_format_t::AUTO;
        } else if (result_format_str == "json") {
            *result_format_out = http_result_format_t::JSON;
        } else if (result_format_str == "jsonp") {
            *result_format_out = http_result_format_t::JSONP;
        } else if (result_format_str == "text") {
            *result_format_out = http_result_format_t::TEXT;
        } else {
            rfail_target(result_format.get(), base_exc_t::GENERIC,
                         "`result_format` (%s) is not recognized, ('auto', 'json', "
                         "'jsonp', and 'text' are allowed).", result_format_str.c_str());
        }
    }
}

// The `attempts` optarg specifies the maximum number of times to attempt the
// request.  Reattempts will only be made when an HTTP error is returned that
// could feasibly be temporary.  This must be specified as an INTEGER >= 0.
void http_term_t::get_attempts(scope_env_t *env,
                               uint64_t *attempts_out) {
    counted_t<val_t> attempts = optarg(env, "attempts");
    if (attempts.has()) {
        *attempts_out = attempts->as_int<uint64_t>();
    }
}

// The `redirects` optarg specifies the maximum number of redirects to follow before
// erroring the query.  This must be passed as an INTEGER between 0 and 2^32 - 1.
void http_term_t::get_redirects(scope_env_t *env,
                                uint32_t *redirects_out) {
    counted_t<val_t> redirects = optarg(env, "redirects");
    if (redirects.has()) {
        *redirects_out = redirects->as_int<uint32_t>();
    }
}

// This is a generic function for parsing out a boolean optarg yet still providing a
// helpful message.  At the moment, it is only used for `page` and `verify`.
void http_term_t::get_bool_optarg(const std::string &optarg_name,
                                  scope_env_t *env,
                                  bool *bool_out) {
    counted_t<val_t> option = optarg(env, optarg_name);
    if (option.has()) {
        *bool_out = option->as_bool();
    }
}

counted_t<term_t> make_http_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<http_term_t>(env, term);
}

}  // namespace ql