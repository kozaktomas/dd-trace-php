#include <php.h>
#include <ext/spl/spl_exceptions.h>

#include "ddtrace.h"
#include "dispatch.h"

#include <Zend/zend.h>
#include "compat_zend_string.h"
#include "dispatch_compat.h"

#include <Zend/zend_closures.h>
#include <Zend/zend_exceptions.h>
#include "debug.h"

#define BUSY_FLAG 1

#if PHP_VERSION_ID >= 70100
#define RETURN_VALUE_USED(opline) ((opline)->result_type != IS_UNUSED)
#else
#define RETURN_VALUE_USED(opline) (!((opline)->result_type & EXT_TYPE_UNUSED))
#endif

#if PHP_VERSION_ID < 70000
#undef EX
#define EX(x) ((execute_data)->x)
#endif

ZEND_EXTERN_MODULE_GLOBALS(ddtrace);

extern user_opcode_handler_t ddtrace_old_fcall_handler;
extern user_opcode_handler_t ddtrace_old_icall_handler;
extern user_opcode_handler_t ddtrace_old_fcall_by_name_handler;

static ddtrace_dispatch_t *lookup_dispatch(const HashTable *lookup, const char *function_name,
                                           uint32_t function_name_length) {
    if (function_name_length == 0) {
        function_name_length = strlen(function_name);
    }

    char *key = zend_str_tolower_dup(function_name, function_name_length);
    ddtrace_dispatch_t *dispatch = NULL;
    dispatch = zend_hash_str_find_ptr(lookup, key, function_name_length);

    efree(key);
    return dispatch;
}

static ddtrace_dispatch_t *find_dispatch(const char *scope_name, uint32_t scope_name_length, const char *function_name,
                                         uint32_t function_name_length TSRMLS_DC) {
    if (!function_name) {
        return NULL;
    }
    HashTable *class_lookup = NULL;
    class_lookup = zend_hash_str_find_ptr(&DDTRACE_G(class_lookup), scope_name, scope_name_length);

    if (!class_lookup) {
        DD_PRINTF("Dispatch Lookup for class: %s", scope_name);
        return NULL;
    }

    return lookup_dispatch(class_lookup, function_name, function_name_length);
}

static void execute_fcall(ddtrace_dispatch_t *dispatch, zend_execute_data *execute_data,
                          zval **return_value_ptr TSRMLS_DC) {
    zend_fcall_info fci = { 0 };
    zend_fcall_info_cache fcc = { 0 };
    char *error = NULL;
    zval closure;
    zend_op *opline = EX(opline);
    INIT_ZVAL(closure);

    if (return_value_ptr){
        if (*return_value_ptr == NULL){
            zval *tmp_ptr = NULL;
            // ALLOC_INIT_ZVAL(tmp_ptr);
            // *return_value_ptr = tmp_ptr;
        }
    }

    zval *this = NULL;

    zend_function *func;
#if PHP_VERSION_ID < 70000
    func = datadog_current_function(execute_data);

    if (dispatch->clazz) {
        this = datadog_this(func, execute_data);
    }

    zend_function *callable = (zend_function *)zend_get_closure_method_def(&dispatch->callable TSRMLS_CC);

    // convert passed callable to not be static as we're going to bind it to *this
    if (this) {
        callable->common.fn_flags &= ~ZEND_ACC_STATIC;
    }

    zend_create_closure(&closure, callable, dispatch->clazz, this TSRMLS_CC);
#else
    func = EX(func);
    this = Z_OBJ(EX(This)) ? &EX(This) : NULL;
    zend_create_closure(&closure, (zend_function *)zend_get_closure_method_def(&dispatch->callable), dispatch->clazz, dispatch->clazz, this TSRMLS_CC);
#endif

    if (zend_fcall_info_init(&closure, 0, &fci, &fcc, NULL, &error TSRMLS_CC) != SUCCESS) {
        if (func->common.scope) {
            zend_throw_exception_ex(
                spl_ce_InvalidArgumentException, 0 TSRMLS_CC, "cannot use return value set for %s::%s as function: %s",
                STRING_VAL(func->common.scope->name), STRING_VAL(func->common.function_name), error);
        } else {
            zend_throw_exception_ex(spl_ce_InvalidArgumentException, 0 TSRMLS_CC,
                                    "cannot use return value set for %s as function: %s",
                                    STRING_VAL(func->common.function_name), error);
        }
        if (error) {
            efree(error);
        }
        goto _exit_cleanup;
    }
       if (return_value_ptr && *return_value_ptr) {
            Z_DELREF_P(*return_value_ptr);
        }

    ddtrace_setup_fcall(execute_data, &fci, return_value_ptr TSRMLS_CC);

    if (zend_call_function(&fci, &fcc TSRMLS_CC) == SUCCESS) {
    }
    if (EG(return_value_ptr_ptr) && *EG(return_value_ptr_ptr)) {
        zval_ptr_dtor(EG(return_value_ptr_ptr));
        EG(return_value_ptr_ptr) = NULL;
    }


#if PHP_VERSION_ID < 70000
    if (fci.params) {
        efree(fci.params);
    }
#endif

_exit_cleanup:
#if PHP_VERSION_ID < 70000
    if (this) {
        Z_DELREF_P(this);
    }

#endif
    zval_dtor(&closure);
}

static int is_anonymous_closure(zend_function *fbc, const char *function_name, uint32_t *function_name_length_p) {
    if (!(fbc->common.fn_flags & ZEND_ACC_CLOSURE) || !function_name_length_p) {
        return 0;
    }

    if (*function_name_length_p == 0) {
        *function_name_length_p = strlen(function_name);
    }

    if ((*function_name_length_p == (sizeof("{closure}") - 1)) && strcmp(function_name, "{closure}") == 0) {
        return 1;
    } else {
        return 0;
    }
}

static zend_always_inline zend_bool executing_method(zend_execute_data *execute_data, zval *object) {
#if PHP_VERSION_ID < 70000
    return EX(opline)->opcode != ZEND_DO_FCALL && object;
#else
    return execute_data && object;
#endif
}


void zend_throw_exception_internal(zval *exception TSRMLS_DC) /* {{{ */
{
    DD_PRINTF("Hmm");
	if (exception != NULL) {
		zval *previous = EG(exception);
		zend_exception_set_previous(exception, EG(exception) TSRMLS_CC);
		EG(exception) = exception;
		if (previous) {
			return;
		}
	}
	if (!EG(current_execute_data)) {
		if(EG(exception)) {
			zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
		}
		zend_error(E_ERROR, "Exception thrown without a stack frame");
	}

	if (zend_throw_exception_hook) {
		zend_throw_exception_hook(exception TSRMLS_CC);
	}

	if (EG(current_execute_data)->opline == NULL ||
	    (EG(current_execute_data)->opline+1)->opcode == ZEND_HANDLE_EXCEPTION) {
		/* no need to rethrow the exception */
		return;
	}

	EG(opline_before_exception) = EG(current_execute_data)->opline;
	EG(current_execute_data)->opline = EG(exception_op);
}



#define FREE_OP(should_free) \
	if (should_free.var) { \
		if ((zend_uintptr_t)should_free.var & 1L) { \
			zval_dtor((zval*)((zend_uintptr_t)should_free.var & ~1L)); \
		} else { \
			zval_ptr_dtor(&should_free.var); \
		} \
	}

#include <assert.h>
static zend_always_inline zend_bool wrap_and_run(zend_execute_data *execute_data, zend_function *fbc,
                                                 const char *function_name, uint32_t function_name_length TSRMLS_DC) {
    zval *object = NULL, *original_object = EX(object);
    const char *common_scope = NULL;
    uint32_t common_scope_length = 0;
    // DD_PRINTF("ETF %0lx", EX(object));
    // if (EX(object) == 0x200){
    //     assert(0);
    // }

    if (fbc->common.scope) {
#if PHP_VERSION_ID < 70000

        object = EG(This);
        // if (!object && EX(object)) {
        //     object = EX(call)->object;
        // }
        if (!object && OBJECT()) {
            object = OBJECT();
        }

        common_scope = fbc->common.scope->name;
        common_scope_length = fbc->common.scope->name_length;

#else
        object = &EX(This);
        common_scope = ZSTR_VAL(fbc->common.scope->name);
        common_scope_length = ZSTR_LEN(fbc->common.scope->name);
#endif
    }
    DD_PRINTF("ETF %0lx", EX(object));

    ddtrace_dispatch_t *dispatch;

    if (executing_method(execute_data, object)) {
        DD_PRINTF("Looking for handler for %s#%s", common_scope, function_name);
        dispatch = find_dispatch(common_scope, common_scope_length, function_name, function_name_length TSRMLS_CC);
    } else {
        dispatch = lookup_dispatch(&DDTRACE_G(function_lookup), function_name, function_name_length);
    }
    DD_PRINTF("ETF %0lx", EX(object));




    if (!dispatch) {
        DD_PRINTF("Handler for %s not found", function_name);
    } else if (dispatch->flags & BUSY_FLAG) {
        DD_PRINTF("Handler for %s is BUSY", function_name);
    }

    // if (original_object != object && object) {
    //         zend_ptr_stack_3_push(&EG(arg_types_stack), fbc, object, EX(called_scope));

    // }
    // if (EX(opline)->opcode != ZEND_DO_FCALL_BY_NAME) {
    //     if (original_object) {
    //         zend_ptr_stack_3_push(&EG(arg_types_stack), fbc, original_object, EX(called_scope));
    //     } else if (object) {
    //         zend_ptr_stack_3_push(&EG(arg_types_stack), fbc, object, EX(called_scope));
    //     }
    // } else {
    //     DD_PRINTF("Should push ");
    // }

    if (original_object){
        // Z_ADDREF_P(original_object);
    }


    if (dispatch && (dispatch->flags ^ BUSY_FLAG)) {
        if (EX(opline)->opcode == ZEND_DO_FCALL)
            {
        zend_op *opline = EX(opline);
        zval *fname = opline->op1.zv;

        zend_free_op free_op1;

        zend_ptr_stack_3_push(&EG(arg_types_stack), EX(fbc), EX(object), EX(called_scope));

        if (CACHED_PTR(opline->op1.literal->cache_slot)) {
            EX(function_state).function = CACHED_PTR(opline->op1.literal->cache_slot);
        } else if (UNEXPECTED(zend_hash_quick_find(EG(function_table), Z_STRVAL_P(fname), Z_STRLEN_P(fname)+1, Z_HASH_P(fname), (void **) &EX(function_state).function)==FAILURE)) {
            SAVE_OPLINE();
            zend_error_noreturn(E_ERROR, "Call to undefined function %s()", fname->value.str.val);
        } else {
            CACHE_PTR(opline->op1.literal->cache_slot, EX(function_state).function);
        }
        EX(object) = NULL;

        // FREE_OP(free_op1);
    }

        if (fbc->common.scope && object) {
            EX(object) = original_object;
        }

        DD_PRINTF("ETF %0lx", EX(object));

        const zend_op *opline = EX(opline);
        zval rv;
        INIT_ZVAL(rv);

        dispatch->flags ^= BUSY_FLAG;  // guard against recursion, catching only topmost execution

#define EX_T(offset) (*(temp_variable *)((char *) EX(Ts) + offset))

#if PHP_VERSION_ID < 70000
        EX_T(opline->result.var).var.ptr = NULL;
        zval **return_value = &EX_T(opline->result.var).var.ptr;

        DD_PRINTF("ETF %0lx", EX(object));
        DD_PRINTF("Starting handler for %s#%s", common_scope, function_name);

        execute_fcall(dispatch, execute_data, return_value TSRMLS_CC);

        DD_PRINTF("ETF %0lx", EX(object));

        if (return_value != NULL) {
            // EX_TMP_VAR(execute_data, opline->result.var)->var.ptr = return_value;
            // EX_T(opline->result.var).var.ptr_ptr = return_value;
            DD_PRINTF("OHSHITTT");

        }
        if (!RETURN_VALUE_USED(opline)) {
                // zval_ptr_dtor(&EX_T(opline->result.var).var.ptr);
            } else {
                // Z_UNSET_ISREF_P(EX_T(opline->result.var).var.ptr);
                // Z_SET_REFCOUNT_P(EX_T(opline->result.var).var.ptr, 1);
                EX_T(opline->result.var).var.fcall_returned_reference = 0;
                EX_T(opline->result.var).var.ptr_ptr = &EX_T(opline->result.var).var.ptr;
            }

		// EG(scope) = EX(current_scope);
		// EG(called_scope) = EX(current_called_scope);

		// EX(current_this) = EG(This);
		// EX(current_scope) = EG(scope);
		// EX(current_called_scope) = EG(called_scope);
		// EG(This) = EX(object);
        // DD_PRINTF("ETF %0lx", EX(object));
		// EG(scope) = (fbc->type == ZEND_USER_FUNCTION || !EX(object)) ? fbc->common.scope : NULL;
		// EG(called_scope) = EX(called_scope);

	    //

        EG(return_value_ptr_ptr) = return_value;

#else
        zval *return_value = (RETURN_VALUE_USED(opline) ? EX_VAR(EX(opline)->result.var) : &rv);
        execute_fcall(dispatch, EX(call), &return_value TSRMLS_CC);
#endif

        dispatch->flags ^= BUSY_FLAG;

        if (!RETURN_VALUE_USED(opline)) {
            // zval_dtor(return_value);
            zval_ptr_dtor(EG(return_value_ptr_ptr));

            EG(return_value_ptr_ptr) = NULL;
        }
	    if (UNEXPECTED(EG(exception) != NULL)) {
            // if (original_object) {
            //     Z_DELREF_P(original_object);
            // }
            Z_DELREF_P(EG(exception));
            if (*return_value){
                // Z_DELREF_PP(return_value);
            }
            //
		    // zend_throw_exception_internal(NULL TSRMLS_CC);
        }

        DD_PRINTF("Handler for %s#%s exiting", common_scope, function_name);

        return 1;
    } else {
        return 0;
    }
}


zend_function *fcall_fbc(zend_execute_data *execute_data){
    zend_op *opline = EX(opline);
    zend_function *fbc = NULL;
    zval *fname = opline->op1.zv;

    if (CACHED_PTR(opline->op1.literal->cache_slot)) {
		return CACHED_PTR(opline->op1.literal->cache_slot);
	} else if (EXPECTED(zend_hash_quick_find(EG(function_table), Z_STRVAL_P(fname), Z_STRLEN_P(fname)+1, Z_HASH_P(fname), (void **) &fbc)==SUCCESS)) {
	   return fbc;
	} else {
		return NULL;
	}
}


static zend_always_inline zend_bool get_wrappable_function(zend_execute_data *execute_data, zend_function **fbc_p,
                                                           char const **function_name_p,
                                                           uint32_t *function_name_length_p) {
    zend_function *fbc = NULL;
    const char *function_name = NULL;
    uint32_t function_name_length = 0;

#if PHP_VERSION_ID < 70000
    if (EX(opline)->opcode == ZEND_DO_FCALL_BY_NAME) {
        fbc = FBC();
        function_name_length = 0;
        if (fbc) {
                function_name = fbc->common.function_name;
        }
    } else {
        zend_op *opline = EX(opline);
        zval *fname = opline->op1.zv;

        fbc = fcall_fbc(execute_data);
        function_name = Z_STRVAL_P(fname);
        function_name_length = Z_STRLEN_P(fname);
    }
#else
    fbc = FBC();
    // fbc = EX(call)->func;
    if (fbc->common.function_name) {
        function_name = ZSTR_VAL(fbc->common.function_name);
        function_name_length = ZSTR_LEN(fbc->common.function_name);
    }
#endif

    if (!function_name) {
        DD_PRINTF("No function name, skipping lookup");
        return 0;
    }

    if (!fbc) {
        DD_PRINTF("No function obj found, skipping lookup");
        return 0;
    }

    if (is_anonymous_closure(fbc, function_name, &function_name_length)) {
        DD_PRINTF("Anonymous closure, skipping lookup");
        return 0;
    }

    *fbc_p = fbc;
    *function_name_p = function_name;
    *function_name_length_p = function_name_length;
    return 1;
}

#define CTOR_CALL_BIT    0x1
#define CTOR_USED_BIT    0x2
#define DECODE_CTOR(ce) \
	((zend_class_entry*)(((zend_uintptr_t)(ce)) & ~(CTOR_CALL_BIT|CTOR_USED_BIT)))

static int update_opcode_leave(zend_execute_data *execute_data TSRMLS_DC) {
#if PHP_VERSION_ID < 70000
	EX(function_state).function = (zend_function *) EX(op_array);
    EX(function_state).arguments = NULL;
    EG(opline_ptr) = &EX(opline);
	EG(active_op_array) = EX(op_array);

    if (EG(return_value_ptr_ptr) && *EG(return_value_ptr_ptr)) {
        zval_ptr_dtor(EG(return_value_ptr_ptr));
    }
	// EG(return_value_ptr_ptr) = &EX(original_return_value);
    EX(original_return_value) = NULL;
    EG(return_value_ptr_ptr) = NULL;

    EG(active_symbol_table) = EX(symbol_table);

    // EG(This) = EX(current_this);
	EG(scope) = EX(current_scope);
	EG(called_scope) = EX(current_called_scope);
	EX(object) = EX(current_object);

    EX(called_scope) = DECODE_CTOR(EX(called_scope));

    zend_vm_stack_clear_multiple(TSRMLS_CC);
    // EX(call)--;
#else
    EX(call) = EX(call)->prev_execute_data;
#endif
    EX(opline) = EX(opline) + 1;
    // EG(opline_ptr) = & EX(opline);

    return ZEND_USER_OPCODE_LEAVE;
}

int default_dispatch(zend_execute_data *execute_data TSRMLS_DC) {
    if (EX(opline)->opcode == ZEND_DO_FCALL_BY_NAME) {
        if (ddtrace_old_fcall_by_name_handler) {
            return ddtrace_old_fcall_by_name_handler(execute_data TSRMLS_CC);
        }
    } else {
        if (ddtrace_old_fcall_handler) {
            return ddtrace_old_fcall_handler(execute_data TSRMLS_CC);
        }
    }

    return ZEND_USER_OPCODE_DISPATCH;
}

int ddtrace_wrap_fcall(zend_execute_data *execute_data TSRMLS_DC) {
    const char *function_name = NULL;
    uint32_t function_name_length = 0;
    zend_function *fbc = NULL;

    DD_PRINTF("OPCODE: %s", zend_get_opcode_name(EX(opline)->opcode));

    if (!get_wrappable_function(execute_data, &fbc, &function_name, &function_name_length)) {
        return default_dispatch(execute_data TSRMLS_CC);
    }

    if (wrap_and_run(execute_data, fbc, function_name, function_name_length TSRMLS_CC)) {
        return update_opcode_leave(execute_data TSRMLS_CC);
    }

    return default_dispatch(execute_data TSRMLS_CC);
}
