#include <stdbool.h>
#include <string.h>
#include "language_interpreter.h"

#ifdef _MSC_VER
# define XP_WIN
#else
# define XP_UNIX
#endif
#include "jsapi.h"

typedef struct _js_internal {
    language_interpreter_t*li;
    JSRuntime *rt;
    JSContext *cx;
    JSObject *global;
    char*buffer;
} js_internal_t;

static JSClass global_class = {
    "global",
    JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS };

static void error_callback(JSContext *cx, const char *message, JSErrorReport *report) {

    js_internal_t*js = JS_GetContextPrivate(cx);
    if(js->li->verbosity > 0) {
        printf("%s:%u:%s\n",
            report->filename ? report->filename : "<no filename>",
            (unsigned int) report->lineno, message);
    }
}

JSBool myjs_trace(JSContext *cx, uintN argc, jsval *vp)
{
    JSString* str;
    jsval*value = JS_ARGV(cx, vp);
    if (!JS_ConvertArguments(cx, argc, value, "S", &str)) {
        return JS_FALSE;
    }
    char*cstr = JS_EncodeString(cx, str);
    printf("%s\n", cstr);
    JS_free(cx, cstr);

    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

JSBool myjs_sqr(JSContext *cx, uintN argc, jsval *vp)
{
    jsdouble x;
    if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "d", &x))
        return JS_FALSE;
    jsdouble result = x*x;
    JS_SET_RVAL(cx, vp, DOUBLE_TO_JSVAL(result));
    return JS_TRUE;
}

static JSFunctionSpec myjs_global_functions[] = {
    JS_FS("trace", myjs_trace, 1, 0),
    JS_FS("sqr", myjs_sqr, 1, 0),
    JS_FS_END
};

bool init_js(js_internal_t*js)
{
    js->rt = JS_NewRuntime(8L * 1024L * 1024L);
    if (js->rt == NULL)
        return false;
    js->cx = JS_NewContext(js->rt, 8192);
    if (js->cx == NULL)
        return false;

    JS_SetContextPrivate(js->cx, js);

    JS_SetOptions(js->cx, JSOPTION_VAROBJFIX | JSOPTION_JIT);
    JS_SetVersion(js->cx, JSVERSION_LATEST);
    JS_SetErrorReporter(js->cx, error_callback);

    //js->global = JS_NewObject(js->cx, &global_class, NULL, NULL);
    js->global = JS_NewCompartmentAndGlobalObject(js->cx, &global_class, NULL);
    if (js->global == NULL)
        return false;

    /* Populate the global object with the standard globals, like Object and Array. */
    if (!JS_InitStandardClasses(js->cx, js->global))
        return false;
    if (!JS_DefineFunctions(js->cx, js->global, myjs_global_functions))
        return false;

    js->buffer = malloc(65536);
    return true;
}

static bool define_function_js(language_interpreter_t*li, const char*script)
{
    js_internal_t*js = (js_internal_t*)li->internal;
    jsval rval;
    JSBool ok;
    ok = JS_EvaluateScript(js->cx, js->global, script, strlen(script), "__main__", 1, &rval);
    return ok;
}

static int call_function_js(language_interpreter_t*li, row_t*row)
{
    js_internal_t*js = (js_internal_t*)li->internal;
    jsval rval;
    JSBool ok;

    char*script = row_to_function_call(row, js->buffer, false);
    ok = JS_EvaluateScript(js->cx, js->global, script, strlen(script), "__main__", 1, &rval);
    int i = -1;
    if (ok) {
        int32 d;
        ok = JS_ValueToInt32(js->cx, rval, &d);
        if(ok)
            i = d;
    }
    return i;
}

void destroy_js(language_interpreter_t* li)
{
    js_internal_t*js = (js_internal_t*)li->internal;
    JS_DestroyContext(js->cx);
    JS_DestroyRuntime(js->rt);
    JS_ShutDown();
    free(js->buffer);
    free(js);
    free(li);
}

language_interpreter_t* javascript_interpreter_new()
{
    language_interpreter_t * li = calloc(1, sizeof(language_interpreter_t));
    li->name = "js";
    li->define_function = define_function_js;
    li->call_function = call_function_js;
    li->destroy = destroy_js;
    li->internal = calloc(1, sizeof(js_internal_t));
    js_internal_t*js = (js_internal_t*)li->internal;
    js->li = li;
    init_js(js);
    return li;
}

