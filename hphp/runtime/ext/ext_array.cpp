/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   | Copyright (c) 1997-2010 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <runtime/ext/ext_array.h>
#include <runtime/ext/ext_iterator.h>
#include <runtime/ext/ext_function.h>
#include <runtime/ext/ext_continuation.h>
#include <runtime/ext/ext_collection.h>
#include <runtime/base/util/request_local.h>
#include <runtime/base/zend/zend_collator.h>
#include <runtime/base/builtin_functions.h>
#include <runtime/base/sort_flags.h>
#include <runtime/vm/translator/translator.h>
#include <runtime/vm/translator/translator-inline.h>
#include <runtime/eval/eval.h>
#include <runtime/base/array/hphp_array.h>
#include <runtime/base/array/zend_array.h>
#include <util/logger.h>

#define SORT_DESC               3
#define SORT_ASC                4

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

const int64 k_UCOL_DEFAULT = UCOL_DEFAULT;

const int64 k_UCOL_PRIMARY = UCOL_PRIMARY;
const int64 k_UCOL_SECONDARY = UCOL_SECONDARY;
const int64 k_UCOL_TERTIARY = UCOL_TERTIARY;
const int64 k_UCOL_DEFAULT_STRENGTH = UCOL_DEFAULT_STRENGTH;
const int64 k_UCOL_QUATERNARY = UCOL_QUATERNARY;
const int64 k_UCOL_IDENTICAL = UCOL_IDENTICAL;

const int64 k_UCOL_OFF = UCOL_OFF;
const int64 k_UCOL_ON = UCOL_ON;

const int64 k_UCOL_SHIFTED = UCOL_SHIFTED;
const int64 k_UCOL_NON_IGNORABLE = UCOL_NON_IGNORABLE;

const int64 k_UCOL_LOWER_FIRST = UCOL_LOWER_FIRST;
const int64 k_UCOL_UPPER_FIRST = UCOL_UPPER_FIRST;

const int64 k_UCOL_FRENCH_COLLATION = UCOL_FRENCH_COLLATION;
const int64 k_UCOL_ALTERNATE_HANDLING = UCOL_ALTERNATE_HANDLING;
const int64 k_UCOL_CASE_FIRST = UCOL_CASE_FIRST;
const int64 k_UCOL_CASE_LEVEL = UCOL_CASE_LEVEL;
const int64 k_UCOL_NORMALIZATION_MODE = UCOL_NORMALIZATION_MODE;
const int64 k_UCOL_STRENGTH = UCOL_STRENGTH;
const int64 k_UCOL_HIRAGANA_QUATERNARY_MODE = UCOL_HIRAGANA_QUATERNARY_MODE;
const int64 k_UCOL_NUMERIC_COLLATION = UCOL_NUMERIC_COLLATION;

using HPHP::VM::Transl::CallerFrame;

#define getCheckedArrayRetType(input, fail, type)                       \
  Variant::TypedValueAccessor tva_##input = input.getTypedAccessor();   \
  if (UNLIKELY(Variant::GetAccessorType(tva_##input) != KindOfArray)) { \
    throw_bad_array_exception();                                        \
    return fail;                                                        \
  }                                                                     \
  type arr_##input = Variant::GetAsArray(tva_##input);

#define getCheckedArrayRet(input, fail) \
  getCheckedArrayRetType(input, fail, CArrRef)
#define getCheckedArray(input) getCheckedArrayRet(input, null)

Variant f_array_change_key_case(CVarRef input, bool upper /* = false */) {
  getCheckedArrayRet(input, false);
  return ArrayUtil::ChangeKeyCase(arr_input, !upper);
}
Variant f_array_chunk(CVarRef input, int size,
                      bool preserve_keys /* = false */) {
  getCheckedArray(input);
  return ArrayUtil::Chunk(arr_input, size, preserve_keys);
}
Variant f_array_combine(CVarRef keys, CVarRef values) {
  getCheckedArray(keys);
  getCheckedArray(values);
  return ArrayUtil::Combine(arr_keys, arr_values);
}
Variant f_array_count_values(CVarRef input) {
  getCheckedArray(input);
  return ArrayUtil::CountValues(arr_input);
}
Variant f_array_fill_keys(CVarRef keys, CVarRef value) {
  getCheckedArray(keys);
  return ArrayUtil::CreateArray(arr_keys, value);
}

static bool filter_func(CVarRef value, const void *data) {
  if (hhvm) {
    HPHP::VM::CallCtx* ctx = (HPHP::VM::CallCtx*)data;
    Variant ret;
    g_vmContext->invokeFunc((TypedValue*)&ret, ctx->func, CREATE_VECTOR1(value),
                            ctx->this_, ctx->cls,
                            NULL, ctx->invName);
    return ret.toBoolean();
  } else {
    MethodCallPackage *mcp = (MethodCallPackage *)data;
    if (mcp->m_isFunc) {
      if (CallInfo::FuncInvoker1Args invoker = mcp->ci->getFunc1Args()) {
        return invoker(mcp->extra, 1, value);
      }
      return (mcp->ci->getFunc())(mcp->extra, CREATE_VECTOR1(value));
    } else {
      if (CallInfo::MethInvoker1Args invoker = mcp->ci->getMeth1Args()) {
        return invoker(*mcp, 1, value);
      }
      return (mcp->ci->getMeth())(*mcp, CREATE_VECTOR1(value));
    }
  }
}
Variant f_array_filter(CVarRef input, CVarRef callback /* = null_variant */) {
  getCheckedArray(input);

  if (callback.isNull()) {
    return ArrayUtil::Filter(arr_input);
  }
  if (hhvm) {
    HPHP::VM::CallCtx ctx;
    CallerFrame cf;
    ctx.func = vm_decode_function(callback, cf(), false, ctx.this_, ctx.cls,
                                  ctx.invName);
    if (ctx.func == NULL) {
      return null;
    }
    return ArrayUtil::Filter(arr_input, filter_func, &ctx);
  } else {
    MethodCallPackage mcp;
    String classname, methodname;
    bool doBind;
    if (!get_user_func_handler(callback, true,
                               mcp, classname, methodname, doBind)) {
      return null;
    }

    if (doBind) {
      // If 'doBind' is true, we need to set the late bound class before
      // calling the user callback
      FrameInjection::StaticClassNameHelper scn(
        ThreadInfo::s_threadInfo.getNoCheck(), classname);
      return ArrayUtil::Filter(arr_input, filter_func, &mcp);
    } else {
      return ArrayUtil::Filter(arr_input, filter_func, &mcp);
    }
  }
}

Variant f_array_flip(CVarRef trans) {
  getCheckedArrayRet(trans, false);
  return ArrayUtil::Flip(arr_trans);
}

HOT_FUNC
bool f_array_key_exists(CVarRef key, CVarRef search) {
  const ArrayData *ad;
  Variant::TypedValueAccessor sacc = search.getTypedAccessor();
  DataType saccType = Variant::GetAccessorType(sacc);
  if (LIKELY(saccType == KindOfArray)) {
    ad = Variant::GetArrayData(sacc);
  } else if (saccType == KindOfObject) {
    return f_array_key_exists(key, toArray(search));
  } else {
    throw_bad_type_exception("array_key_exists expects an array or an object; "
                           "false returned.");
    return false;
  }
  Variant::TypedValueAccessor kacc = key.getTypedAccessor();
  switch (Variant::GetAccessorType(kacc)) {
  case KindOfString:
  case KindOfStaticString: {
    int64 n = 0;
    StringData *sd = Variant::GetStringData(kacc);
    if (sd->isStrictlyInteger(n)) {
      return ad->exists(n);
    }
    return ad->exists(StrNR(sd));
  }
  case KindOfInt64:
    return ad->exists(Variant::GetInt64(kacc));
  case KindOfUninit:
  case KindOfNull:
    return ad->exists(empty_string);
  default:
    break;
  }
  raise_warning("Array key should be either a string or an integer");
  return false;
}

Variant f_array_keys(CVarRef input, CVarRef search_value /* = null_variant */,
                     bool strict /* = false */) {
  getCheckedArray(input);
  return arr_input.keys(search_value, strict);
}

static Variant map_func(CArrRef params, const void *data) {
  if (hhvm) {
    HPHP::VM::CallCtx* ctx = (HPHP::VM::CallCtx*)data;
    if (ctx == NULL) {
      if (params.size() == 1) {
        return params[0];
      }
      return params;
    }
    Variant ret;
    g_vmContext->invokeFunc((TypedValue*)&ret, ctx->func, params,
                            ctx->this_, ctx->cls,
                            NULL, ctx->invName);
    return ret;
  } else {
    if (!data) {
      if (params.size() == 1) {
        return params[0];
      }
      return params;
    }
    MethodCallPackage *mcp = (MethodCallPackage *)data;
    if (mcp->m_isFunc) {
      return (mcp->ci->getFunc())(mcp->extra, params);
    } else {
      return (mcp->ci->getMeth())(*mcp, params);
    }
  }
}
Variant f_array_map(int _argc, CVarRef callback, CVarRef arr1, CArrRef _argv /* = null_array */) {
  Array inputs;
  if (!arr1.isArray()) {
    throw_bad_array_exception();
    return null;
  }
  inputs.append(arr1);
  if (!_argv.empty()) {
    inputs = inputs.merge(_argv);
  }
  if (hhvm) {
    HPHP::VM::CallCtx ctx;
    ctx.func = NULL;
    if (!callback.isNull()) {
      CallerFrame cf;
      ctx.func = vm_decode_function(callback, cf(), false, ctx.this_, ctx.cls,
                                    ctx.invName);
    }
    if (ctx.func == NULL) {
      return ArrayUtil::Map(inputs, map_func, NULL);
    }
    return ArrayUtil::Map(inputs, map_func, &ctx);
  } else {
    MethodCallPackage mcp;
    String classname, methodname;
    bool doBind;
    if (callback.isNull() ||
        !get_user_func_handler(callback, true,
                               mcp, classname, methodname, doBind)) {
      return ArrayUtil::Map(inputs, map_func, NULL);
    }

    if (doBind) {
      // If 'doBind' is true, we need to set the late bound class before
      // calling the user callback
      FrameInjection::StaticClassNameHelper scn(
        ThreadInfo::s_threadInfo.getNoCheck(), classname);
      return ArrayUtil::Map(inputs, map_func, &mcp);
    } else {
      return ArrayUtil::Map(inputs, map_func, &mcp);
    }
  }
}

static void php_array_merge(Array &arr1, CArrRef arr2) {
  arr1.merge(arr2);
}

static void php_array_merge_recursive(PointerSet &seen, bool check,
                                      Array &arr1, CArrRef arr2) {
  if (check) {
    if (seen.find((void*)arr1.get()) != seen.end()) {
      raise_warning("array_merge_recursive(): recursion detected");
      return;
    }
    seen.insert((void*)arr1.get());
  }

  for (ArrayIter iter(arr2); iter; ++iter) {
    Variant key(iter.first());
    CVarRef value(iter.secondRef());
    if (key.isNumeric()) {
      arr1.appendWithRef(value);
    } else if (arr1.exists(key, true)) {
      // There is no need to do toKey() conversion, for a key that is already
      // in the array.
      Variant &v = arr1.lvalAt(key, AccessFlags::Key);
      Array subarr1(v.toArray()->copy());
      php_array_merge_recursive(seen, v.isReferenced(), subarr1,
                                value.toArray());
      v.unset(); // avoid contamination of the value that was strongly bound
      v = subarr1;
    } else {
      arr1.addLval(key, true).setWithRef(value);
    }
  }

  if (check) {
    seen.erase((void*)arr1.get());
  }
}

Variant f_array_merge(int _argc, CVarRef array1,
                      CArrRef _argv /* = null_array */) {
  getCheckedArray(array1);
  Array ret = Array::Create();
  php_array_merge(ret, arr_array1);
  for (ArrayIter iter(_argv); iter; ++iter) {
    Variant v = iter.second();
    if (!v.isArray()) {
      throw_bad_array_exception();
      return null;
    }
    CArrRef arr_v = v.asCArrRef();
    php_array_merge(ret, arr_v);
  }
  return ret;
}

Variant f_array_merge_recursive(int _argc, CVarRef array1,
                                CArrRef _argv /* = null_array */) {
  getCheckedArray(array1);
  Array ret = Array::Create();
  PointerSet seen;
  php_array_merge_recursive(seen, false, ret, arr_array1);
  assert(seen.empty());
  for (ArrayIter iter(_argv); iter; ++iter) {
    Variant v = iter.second();
    if (!v.isArray()) {
      throw_bad_array_exception();
      return null;
    }
    CArrRef arr_v = v.asCArrRef();
    php_array_merge_recursive(seen, false, ret, arr_v);
    assert(seen.empty());
  }
  return ret;
}

static void php_array_replace(Array &arr1, CArrRef arr2) {
  for (ArrayIter iter(arr2); iter; ++iter) {
    Variant key = iter.first();
    CVarRef value = iter.secondRef();
    arr1.lvalAt(key, AccessFlags::Key).setWithRef(value);
  }
}

static void php_array_replace_recursive(PointerSet &seen, bool check,
                                        Array &arr1, CArrRef arr2) {
  if (check) {
    if (seen.find((void*)arr1.get()) != seen.end()) {
      raise_warning("array_replace_recursive(): recursion detected");
      return;
    }
    seen.insert((void*)arr1.get());
  }

  for (ArrayIter iter(arr2); iter; ++iter) {
    Variant key = iter.first();
    CVarRef value = iter.secondRef();
    if (arr1.exists(key, true) && value.isArray()) {
      Variant &v = arr1.lvalAt(key, AccessFlags::Key);
      if (v.isArray()) {
        Array subarr1 = v.toArray();
        const ArrNR& arr_value = value.toArrNR();
        php_array_replace_recursive(seen, v.isReferenced(), subarr1,
                                    arr_value);
        v = subarr1;
      } else {
        arr1.set(key, value, true);
      }
    } else {
      arr1.lvalAt(key, AccessFlags::Key).setWithRef(value);
    }
  }

  if (check) {
    seen.erase((void*)arr1.get());
  }
}

Variant f_array_replace(int _argc, CVarRef array1,
                        CArrRef _argv /* = null_array */) {
  getCheckedArray(array1);
  Array ret = Array::Create();
  php_array_replace(ret, arr_array1);
  for (ArrayIter iter(_argv); iter; ++iter) {
    CVarRef v = iter.secondRef();
    getCheckedArray(v);
    php_array_replace(ret, arr_v);
  }
  return ret;
}

Variant f_array_replace_recursive(int _argc, CVarRef array1,
                                  CArrRef _argv /* = null_array */) {
  getCheckedArray(array1);
  Array ret = Array::Create();
  PointerSet seen;
  php_array_replace_recursive(seen, false, ret, arr_array1);
  assert(seen.empty());
  for (ArrayIter iter(_argv); iter; ++iter) {
    CVarRef v = iter.secondRef();
    getCheckedArray(v);
    php_array_replace_recursive(seen, false, ret, arr_v);
    assert(seen.empty());
  }
  return ret;
}

Variant f_array_pad(CVarRef input, int pad_size, CVarRef pad_value) {
  getCheckedArray(input);
  if (pad_size > 0) {
    return ArrayUtil::Pad(arr_input, pad_value, pad_size, true);
  }
  return ArrayUtil::Pad(arr_input, pad_value, -pad_size, false);
}

Variant f_array_product(CVarRef array) {
  getCheckedArray(array);
  if (arr_array.empty()) {
    return 0; // to be consistent with PHP
  }
  int64 i;
  double d;
  if (ArrayUtil::Product(arr_array, &i, &d) == KindOfInt64) {
    return i;
  } else {
    return d;
  }
}

Variant f_array_push(int _argc, VRefParam array, CVarRef var, CArrRef _argv /* = null_array */) {
  getCheckedArrayRetType(array, null, Array &);
  arr_array.append(var);
  for (ArrayIter iter(_argv); iter; ++iter) {
    arr_array.append(iter.second());
  }
  return arr_array.size();
}

Variant f_array_rand(CVarRef input, int num_req /* = 1 */) {
  getCheckedArray(input);
  return ArrayUtil::RandomKeys(arr_input, num_req);
}

static Variant reduce_func(CVarRef result, CVarRef operand, const void *data) {
  if (hhvm) {
    HPHP::VM::CallCtx* ctx = (HPHP::VM::CallCtx*)data;
    Variant ret;
    g_vmContext->invokeFunc((TypedValue*)&ret, ctx->func,
                            CREATE_VECTOR2(result, operand), ctx->this_,
                            ctx->cls, NULL, ctx->invName);
    return ret;
  } else {
    MethodCallPackage *mcp = (MethodCallPackage *)data;
    if (mcp->m_isFunc) {
      if (CallInfo::FuncInvoker2Args invoker = mcp->ci->getFunc2Args()) {
        return invoker(mcp->extra, 2, result, operand);
      }
      return (mcp->ci->getFunc())(mcp->extra, CREATE_VECTOR2(result, operand));
    } else {
      if (CallInfo::MethInvoker2Args invoker = mcp->ci->getMeth2Args()) {
        return invoker(*mcp, 2, result, operand);
      }
      return (mcp->ci->getMeth())(*mcp, CREATE_VECTOR2(result, operand));
    }
  }
}
Variant f_array_reduce(CVarRef input, CVarRef callback,
                       CVarRef initial /* = null_variant */) {
  getCheckedArray(input);
  if (hhvm) {
    HPHP::VM::CallCtx ctx;
    CallerFrame cf;
    ctx.func = vm_decode_function(callback, cf(), false, ctx.this_, ctx.cls,
                                  ctx.invName);
    if (ctx.func == NULL) {
      return null;
    }
    return ArrayUtil::Reduce(arr_input, reduce_func, &ctx, initial);
  } else {
    MethodCallPackage mcp;
    String classname, methodname;
    bool doBind;
    if (!get_user_func_handler(callback, true,
                               mcp, classname, methodname, doBind)) {
      return null;
    }

    if (doBind) {
      // If 'doBind' is true, we need to set the late bound class before
      // calling the user callback
      FrameInjection::StaticClassNameHelper scn(
        ThreadInfo::s_threadInfo.getNoCheck(), classname);
      return ArrayUtil::Reduce(arr_input, reduce_func, &mcp, initial);
    } else {
      return ArrayUtil::Reduce(arr_input, reduce_func, &mcp, initial);
    }
  }
}

Variant f_array_reverse(CVarRef array, bool preserve_keys /* = false */) {
  getCheckedArray(array);
  return ArrayUtil::Reverse(arr_array, preserve_keys);
}

Variant f_array_search(CVarRef needle, CVarRef haystack,
                       bool strict /* = false */) {
  getCheckedArrayRet(haystack, false);
  return arr_haystack.key(needle, strict);
}

Variant f_array_slice(CVarRef array, int offset,
                      CVarRef length /* = null_variant */,
                      bool preserve_keys /* = false */) {
  getCheckedArray(array);
  int64 len = length.isNull() ? 0x7FFFFFFF : length.toInt64();
  return ArrayUtil::Slice(arr_array, offset, len, preserve_keys);
}
Variant f_array_splice(VRefParam input, int offset,
                       CVarRef length /* = null_variant */,
                       CVarRef replacement /* = null_variant */) {
  getCheckedArray(input);
  Array ret(Array::Create());
  int64 len = length.isNull() ? 0x7FFFFFFF : length.toInt64();
  input = ArrayUtil::Splice(arr_input, offset, len, replacement, &ret);
  return ret;
}
Variant f_array_sum(CVarRef array) {
  getCheckedArray(array);
  int64 i;
  double d;
  if (ArrayUtil::Sum(arr_array, &i, &d) == KindOfInt64) {
    return i;
  } else {
    return d;
  }
}

int64 f_array_unshift(int _argc, VRefParam array, CVarRef var, CArrRef _argv /* = null_array */) {
  if (array.toArray()->isVectorData()) {
    if (!_argv.empty()) {
      for (ssize_t pos = _argv->iter_end(); pos != ArrayData::invalid_index;
        pos = _argv->iter_rewind(pos)) {
        array.prepend(_argv->getValueRef(pos));
      }
    }
    array.prepend(var);
  } else {
    {
      Array newArray;
      newArray.append(var);
      if (!_argv.empty()) {
        for (ssize_t pos = _argv->iter_begin();
             pos != ArrayData::invalid_index;
             pos = _argv->iter_advance(pos)) {
          newArray.append(_argv->getValueRef(pos));
        }
      }
      for (ArrayIter iter(array); iter; ++iter) {
        Variant key(iter.first());
        CVarRef value(iter.secondRef());
        if (key.isInteger()) {
          newArray.appendWithRef(value);
        } else {
          newArray.lvalAt(key, AccessFlags::Key).setWithRef(value);
        }
      }
      array = newArray;
    }
    // Reset the array's internal pointer
    if (array.is(KindOfArray)) {
      array.array_iter_reset();
    }
  }
  return array.toArray().size();
}

Variant f_array_values(CVarRef input) {
  getCheckedArray(input);
  return arr_input.values();
}

static void walk_func(VRefParam value, CVarRef key, CVarRef userdata,
                      const void *data) {
  if (hhvm) {
    HPHP::VM::CallCtx* ctx = (HPHP::VM::CallCtx*)data;
    Variant sink;
    g_vmContext->invokeFunc((TypedValue*)&sink, ctx->func,
                            CREATE_VECTOR3(ref(value), key, userdata),
                            ctx->this_, ctx->cls,
                            NULL, ctx->invName);
  } else {
    MethodCallPackage *mcp = (MethodCallPackage *)data;
    // Here to avoid crash in interpreter, we need to use different variation
    // in 'FewArgs' cases
    if (mcp->m_isFunc) {
      if (mcp->ci->getFuncFewArgs()) { // To test whether we have FewArgs
        if (userdata.isNull()) {
          (mcp->ci->getFunc2Args())(mcp->extra, 2, value, key);
        } else{
          (mcp->ci->getFunc3Args())(mcp->extra, 3, value, key, userdata);
        }
        return;
      }
      (mcp->ci->getFunc())(mcp->extra, CREATE_VECTOR3(ref(value), key,
                           userdata));
    } else {
      if (mcp->ci->getMethFewArgs()) { // To test whether we have FewArgs
        if (userdata.isNull()) {
          (mcp->ci->getMeth2Args())(*mcp, 2, value, key);
        } else {
          (mcp->ci->getMeth3Args())(*mcp, 3, value, key, userdata);
        }
        return;
      }
      (mcp->ci->getMeth())(*mcp, CREATE_VECTOR3(ref(value), key,
                           userdata));
    }
  }
}
bool f_array_walk_recursive(VRefParam input, CVarRef funcname,
                            CVarRef userdata /* = null_variant */) {
  if (!input.isArray()) {
    throw_bad_array_exception();
    return false;
  }
  if (hhvm) {
    HPHP::VM::CallCtx ctx;
    CallerFrame cf;
    ctx.func = vm_decode_function(funcname, cf(), false, ctx.this_, ctx.cls,
                                  ctx.invName);
    if (ctx.func == NULL) {
      return null;
    }
    PointerSet seen;
    ArrayUtil::Walk(input, walk_func, &ctx, true, &seen, userdata);
  } else {
    MethodCallPackage mcp;
    String classname, methodname;
    bool doBind;
    if (!get_user_func_handler(funcname, true,
                               mcp, classname, methodname, doBind)) {
      return null;
    }

    PointerSet seen;
    if (doBind) {
      // If 'doBind' is true, we need to set the late bound class before
      // calling the user callback
      FrameInjection::StaticClassNameHelper scn(
        ThreadInfo::s_threadInfo.getNoCheck(), classname);
      ArrayUtil::Walk(input, walk_func, &mcp, true, &seen, userdata);
    } else {
      ArrayUtil::Walk(input, walk_func, &mcp, true, &seen, userdata);
    }
  }
  return true;
}
bool f_array_walk(VRefParam input, CVarRef funcname,
                  CVarRef userdata /* = null_variant */) {
  if (!input.isArray()) {
    throw_bad_array_exception();
    return false;
  }
  if (hhvm) {
    HPHP::VM::CallCtx ctx;
    CallerFrame cf;
    ctx.func = vm_decode_function(funcname, cf(), false, ctx.this_, ctx.cls,
                                  ctx.invName);
    if (ctx.func == NULL) {
      return null;
    }
    ArrayUtil::Walk(input, walk_func, &ctx, false, NULL, userdata);
  } else {
    MethodCallPackage mcp;
    String classname, methodname;
    bool doBind;
    if (!get_user_func_handler(funcname, true,
                               mcp, classname, methodname, doBind)) {
      return null;
    }

    if (doBind) {
      // If 'doBind' is true, we need to set the late bound class before
      // calling the user callback
      FrameInjection::StaticClassNameHelper scn(
        ThreadInfo::s_threadInfo.getNoCheck(), classname);
      ArrayUtil::Walk(input, walk_func, &mcp, false, NULL, userdata);
    } else {
      ArrayUtil::Walk(input, walk_func, &mcp, false, NULL, userdata);
    }
  }
  return true;
}

static void compact(HPHP::VM::VarEnv* v, Array &ret, CVarRef var) {
  if (var.isArray()) {
    for (ArrayIter iter(var.getArrayData()); iter; ++iter) {
      compact(v, ret, iter.second());
    }
  } else {
    String varname = var.toString();
    if (!varname.empty() && v->lookup(varname.get()) != NULL) {
      ret.set(varname, *reinterpret_cast<Variant*>(v->lookup(varname.get())));
    }
  }
}

Array f_compact(int _argc, CVarRef varname, CArrRef _argv /* = null_array */) {
  if (hhvm) {
    Array ret = Array::Create();
    HPHP::VM::VarEnv* v = g_vmContext->getVarEnv();
    if (v) {
      compact(v, ret, varname);
      compact(v, ret, _argv);
    }
    return ret;
  } else {
    raise_error("Invalid call to f_compact");
    return Array::Create();
  }
}

template<typename T>
static void compact(T *variables, Array &ret, CVarRef var) {
  if (var.isArray()) {
    CArrRef vars = var.toCArrRef();
    for (ArrayIter iter(vars); iter; ++iter) {
      compact(variables, ret, iter.second());
    }
  } else {
    String varname = var.toString();
    if (!varname.empty() && variables->exists(varname)) {
      ret.set(varname, variables->get(varname));
    }
  }
}

Array compact(RVariableTable *variables, int _argc, CVarRef varname,
              CArrRef _argv /* = null_array */) {
  FUNCTION_INJECTION_BUILTIN(compact);
  Array ret = Array::Create();
  compact(variables, ret, varname);
  compact(variables, ret, _argv);
  return ret;
}

Array compact(LVariableTable *variables, int _argc, CVarRef varname,
              CArrRef _argv /* = null_array */) {
  FUNCTION_INJECTION_BUILTIN(compact);
  Array ret = Array::Create();
  compact(variables, ret, varname);
  compact(variables, ret, _argv);
  return ret;
}

static int php_count_recursive(CArrRef array) {
  long cnt = array.size();
  for (ArrayIter iter(array); iter; ++iter) {
    Variant value = iter.second();
    if (value.isArray()) {
      CArrRef arr_value = value.asCArrRef();
      cnt += php_count_recursive(arr_value);
    }
  }
  return cnt;
}

bool f_shuffle(VRefParam array) {
  if (!array.isArray()) {
    throw_bad_array_exception();
    return false;
  }
  array = ArrayUtil::Shuffle(array);
  return true;
}

int64 f_count(CVarRef var, bool recursive /* = false */) {
  switch (var.getType()) {
  case KindOfUninit:
  case KindOfNull:
    return 0;
  case KindOfObject:
    {
      Object obj = var.toObject();
      if (obj.instanceof("Countable")) {
        return obj->o_invoke("count", null_array, -1);
      }
    }
    break;
  case KindOfArray:
    if (recursive) {
      CArrRef arr_var = var.toCArrRef();
      return php_count_recursive(arr_var);
    }
    return var.getArrayData()->size();
  default:
    break;
  }
  return 1;
}

static StaticString s_Iterator("Iterator");
static StaticString s_IteratorAggregate("IteratorAggregate");
static StaticString s_getIterator("getIterator");

static Variant f_hphp_get_iterator(VRefParam iterable, bool isMutable) {
  if (iterable.isArray()) {
    if (isMutable) {
      return create_object(c_MutableArrayIterator::s_class_name,
                           CREATE_VECTOR1(ref(iterable)));
    }
    return create_object(c_ArrayIterator::s_class_name,
                         CREATE_VECTOR1(iterable));
  }
  if (iterable.isObject()) {
    CStrRef context = hhvm
                      ? g_vmContext->getContextClassName()
                      : FrameInjection::GetClassName(true);

    ObjectData *obj = iterable.getObjectData();
    Variant iterator;
    while (obj->o_instanceof(s_IteratorAggregate)) {
      iterator = obj->o_invoke(s_getIterator, Array());
      if (!iterator.isObject()) break;
      obj = iterator.getObjectData();
    }
    if (isMutable) {
      if (obj->o_instanceof(s_Iterator)) {
        throw FatalErrorException("An iterator cannot be used for "
                                  "iteration by reference");
      }
      Array properties = obj->o_toIterArray(context, true);
      return create_object(c_MutableArrayIterator::s_class_name,
                           CREATE_VECTOR1(ref(properties)));
    } else {
      if (obj->o_instanceof(s_Iterator)) {
        return obj;
      }
      return create_object(c_ArrayIterator::s_class_name,
                           CREATE_VECTOR1(obj->o_toIterArray(context)));
    }
  }
  raise_warning("Invalid argument supplied for iteration");
  if (isMutable) {
    return create_object(c_MutableArrayIterator::s_class_name,
                         CREATE_VECTOR1(Array::Create()));
  }
  return create_object(c_ArrayIterator::s_class_name,
                       CREATE_VECTOR1(Array::Create()));
}

Variant f_hphp_get_iterator(CVarRef iterable) {
  return f_hphp_get_iterator(directRef(iterable), false);
}

Variant f_hphp_get_mutable_iterator(VRefParam iterable) {
  return f_hphp_get_iterator(iterable, true);
}

bool f_in_array(CVarRef needle, CVarRef haystack, bool strict /* = false */) {
  getCheckedArrayRet(haystack, false);
  return arr_haystack.valueExists(needle, strict);
}

Variant f_range(CVarRef low, CVarRef high, CVarRef step /* = 1 */) {
  bool is_step_double = false;
  double dstep = 1.0;
  if (step.isDouble()) {
    dstep = step.toDouble();
    is_step_double = true;
  } else if (step.isString()) {
    int64 sn;
    double sd;
    DataType stype = step.toString()->isNumericWithVal(sn, sd, 0);
    if (stype == KindOfDouble) {
      is_step_double = true;
      dstep = sd;
    } else if (stype == KindOfInt64) {
      dstep = (double)sn;
    } else {
      dstep = step.toDouble();
    }
  } else {
    dstep = step.toDouble();
  }
  /* We only want positive step values. */
  if (dstep < 0.0) dstep *= -1;
  if (low.isString() && high.isString()) {
    String slow = low.toString();
    String shigh = high.toString();
    if (slow.size() >= 1 && shigh.size() >=1) {
      int64 n1, n2;
      double d1, d2;
      DataType type1 = slow->isNumericWithVal(n1, d1, 0);
      DataType type2 = shigh->isNumericWithVal(n2, d2, 0);
      if (type1 == KindOfDouble || type2 == KindOfDouble || is_step_double) {
        if (type1 != KindOfDouble) d1 = slow.toDouble();
        if (type2 != KindOfDouble) d2 = shigh.toDouble();
        return ArrayUtil::Range(d1, d2, dstep);
      }

      int64 lstep = (int64) dstep;
      if (type1 == KindOfInt64 || type2 == KindOfInt64) {
        if (type1 != KindOfInt64) n1 = slow.toInt64();
        if (type2 != KindOfInt64) n2 = shigh.toInt64();
        return ArrayUtil::Range((double)n1, (double)n2, lstep);
      }

      return ArrayUtil::Range((unsigned char)slow.charAt(0),
                              (unsigned char)shigh.charAt(0), lstep);
    }
  }

  if (low.is(KindOfDouble) || high.is(KindOfDouble) || is_step_double) {
    return ArrayUtil::Range(low.toDouble(), high.toDouble(), dstep);
  }

  int64 lstep = (int64) dstep;
  return ArrayUtil::Range(low.toDouble(), high.toDouble(), lstep);
}
///////////////////////////////////////////////////////////////////////////////
// diff/intersect helpers

static int cmp_func(CVarRef v1, CVarRef v2, const void *data) {
  Variant *callback = (Variant *)data;
  return f_call_user_func_array(*callback, CREATE_VECTOR2(v1, v2));
}

#define COMMA ,
#define diff_intersect_body(type,intersect_params,user_setup)   \
  getCheckedArray(array1);                                      \
  if (!arr_array1.size()) return arr_array1;                    \
  user_setup                                                    \
  Array ret = arr_array1.type(array2, intersect_params);        \
  if (ret.size()) {                                             \
    for (ArrayIter iter(_argv); iter; ++iter) {                 \
      ret = ret.type(iter.second(), intersect_params);          \
      if (!ret.size()) break;                                   \
    }                                                           \
  }                                                             \
  return ret;

///////////////////////////////////////////////////////////////////////////////
// diff functions

Variant f_array_diff(int _argc, CVarRef array1, CVarRef array2,
                   CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, false COMMA true,);
}

Variant f_array_udiff(int _argc, CVarRef array1, CVarRef array2,
                      CVarRef data_compare_func,
                      CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, false COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func,
                      Variant func = data_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

Variant f_array_diff_assoc(int _argc, CVarRef array1, CVarRef array2,
                           CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, true COMMA true,);
}

Variant f_array_diff_uassoc(int _argc, CVarRef array1, CVarRef array2,
                            CVarRef key_compare_func,
                            CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, true COMMA true COMMA cmp_func COMMA &func,
                      Variant func = key_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

Variant f_array_udiff_assoc(int _argc, CVarRef array1, CVarRef array2,
                            CVarRef data_compare_func,
                            CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, true COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func,
                      Variant func = data_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

Variant f_array_udiff_uassoc(int _argc, CVarRef array1, CVarRef array2,
                             CVarRef data_compare_func,
                             CVarRef key_compare_func,
                             CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, true COMMA true COMMA cmp_func COMMA &key_func
                      COMMA cmp_func COMMA &data_func,
                      Variant data_func = data_compare_func;
                      Variant key_func = key_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(key_func);
                        extra.prepend(data_func);
                        key_func = extra.pop();
                        data_func = extra.pop();
                      });
}

Variant f_array_diff_key(int _argc, CVarRef array1, CVarRef array2,
                         CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, true COMMA false,);
}

Variant f_array_diff_ukey(int _argc, CVarRef array1, CVarRef array2,
                          CVarRef key_compare_func,
                          CArrRef _argv /* = null_array */) {
  diff_intersect_body(diff, true COMMA false COMMA cmp_func COMMA &func,
                      Variant func = key_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

///////////////////////////////////////////////////////////////////////////////
// intersect functions

Variant f_array_intersect(int _argc, CVarRef array1, CVarRef array2,
                          CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, false COMMA true,);
}

Variant f_array_uintersect(int _argc, CVarRef array1, CVarRef array2,
                           CVarRef data_compare_func,
                           CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, false COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func,
                      Variant func = data_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

Variant f_array_intersect_assoc(int _argc, CVarRef array1, CVarRef array2,
                                CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, true COMMA true,);
}

Variant f_array_intersect_uassoc(int _argc, CVarRef array1, CVarRef array2,
                                 CVarRef key_compare_func,
                                 CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, true COMMA true COMMA cmp_func COMMA &func,
                      Variant func = key_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

Variant f_array_uintersect_assoc(int _argc, CVarRef array1, CVarRef array2,
                                 CVarRef data_compare_func,
                                 CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, true COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func,
                      Variant func = data_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

Variant f_array_uintersect_uassoc(int _argc, CVarRef array1, CVarRef array2,
                                  CVarRef data_compare_func,
                                  CVarRef key_compare_func,
                                  CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, true COMMA true COMMA cmp_func COMMA &key_func
                      COMMA cmp_func COMMA &data_func,
                      Variant data_func = data_compare_func;
                      Variant key_func = key_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(key_func);
                        extra.prepend(data_func);
                        key_func = extra.pop();
                        data_func = extra.pop();
                      });
}

Variant f_array_intersect_key(int _argc, CVarRef array1, CVarRef array2, CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, true COMMA false,);
}

Variant f_array_intersect_ukey(int _argc, CVarRef array1, CVarRef array2,
                             CVarRef key_compare_func, CArrRef _argv /* = null_array */) {
  diff_intersect_body(intersect, true COMMA false COMMA cmp_func COMMA &func,
                      Variant func = key_compare_func;
                      Array extra = _argv;
                      if (!extra.empty()) {
                        extra.prepend(func);
                        func = extra.pop();
                      });
}

///////////////////////////////////////////////////////////////////////////////
// sorting functions

class Collator : public RequestEventHandler {
public:
  String getLocale() {
    return m_locale;
  }
  intl_error &getErrorCodeRef() {
    return m_errcode;
  }
  bool setLocale(CStrRef locale) {
    if (m_locale.same(locale)) {
      return true;
    }
    if (m_ucoll) {
      ucol_close(m_ucoll);
      m_ucoll = NULL;
    }
    m_errcode.clear();
    m_ucoll = ucol_open(locale.data(), &(m_errcode.code));
    if (m_ucoll == NULL) {
      raise_warning("failed to load %s locale from icu data", locale.data());
      return false;
    }
    if (U_FAILURE(m_errcode.code)) {
      ucol_close(m_ucoll);
      m_ucoll = NULL;
      return false;
    }
    m_locale = locale;
    return true;
  }

  UCollator *getCollator() {
    return m_ucoll;
  }

  bool setAttribute(int64 attr, int64 val) {
    if (!m_ucoll) {
      Logger::Verbose("m_ucoll is NULL");
      return false;
    }
    m_errcode.clear();
    ucol_setAttribute(m_ucoll, (UColAttribute)attr,
                      (UColAttributeValue)val, &(m_errcode.code));
    if (U_FAILURE(m_errcode.code)) {
      Logger::Verbose("Error setting attribute value");
      return false;
    }
    return true;
  }

  bool setStrength(int64 strength) {
    if (!m_ucoll) {
      Logger::Verbose("m_ucoll is NULL");
      return false;
    }
    ucol_setStrength(m_ucoll, (UCollationStrength)strength);
    return true;
  }

  Variant getErrorCode() {
    if (!m_ucoll) {
      Logger::Verbose("m_ucoll is NULL");
      return false;
    }
    return m_errcode.code;
  }

  virtual void requestInit() {
    m_locale = String(uloc_getDefault(), CopyString);
    m_errcode.clear();
    m_ucoll = ucol_open(m_locale.data(), &(m_errcode.code));
    assert(m_ucoll);
  }
  virtual void requestShutdown() {
    m_locale.reset();
    m_errcode.clear();
    if (m_ucoll) {
      ucol_close(m_ucoll);
      m_ucoll = NULL;
    }
  }

private:
  String     m_locale;
  UCollator *m_ucoll;
  intl_error m_errcode;
};
IMPLEMENT_STATIC_REQUEST_LOCAL(Collator, s_collator);

static Array::PFUNC_CMP get_cmp_func(int sort_flags, bool ascending) {
  switch (sort_flags) {
  case SORT_NUMERIC:
    return ascending ?
      Array::SortNumericAscending : Array::SortNumericDescending;
  case SORT_STRING:
    return ascending ?
      Array::SortStringAscending : Array::SortStringDescending;
  case SORT_LOCALE_STRING:
    return ascending ?
      Array::SortLocaleStringAscending : Array::SortLocaleStringDescending;
  case SORT_REGULAR:
  default:
    return ascending ?
      Array::SortRegularAscending : Array::SortRegularDescending;
  }
}

class ArraySortTmp {
 public:
  ArraySortTmp(Array& arr) : m_arr(arr) {
    m_ad = arr.get()->escalateForSort();
    m_ad->incRefCount();
  }
  ~ArraySortTmp() {
    if (m_ad != m_arr.get()) {
      m_arr = m_ad;
      m_ad->decRefCount();
    }
  }
  ArrayData* operator->() { return m_ad; }
 private:
  Array& m_arr;
  ArrayData* m_ad;
};

static bool
php_sort(VRefParam array, int sort_flags, bool ascending, bool use_collator) {
  if (array.isArray()) {
    Array& arr_array = Variant::GetAsArray(array.getTypedAccessor());
    if (use_collator && sort_flags != SORT_LOCALE_STRING) {
      UCollator *coll = s_collator->getCollator();
      if (coll) {
        intl_error &errcode = s_collator->getErrorCodeRef();
        return collator_sort(array, sort_flags, ascending,
                             coll, &errcode);
      }
    }
    ArraySortTmp ast(arr_array);
    ast->sort(sort_flags, ascending);
    return true;
  }
  if (array.isObject()) {
    ObjectData* obj = array.getObjectData();
    if (obj->getCollectionType() == Collection::VectorType) {
      c_Vector* vec = static_cast<c_Vector*>(obj);
      vec->sort(sort_flags, ascending);
      return true;
    }
  }
  throw_bad_array_exception();
  return false;
}

static bool
php_asort(VRefParam array, int sort_flags, bool ascending, bool use_collator) {
  if (array.isArray()) {
    Array& arr_array = Variant::GetAsArray(array.getTypedAccessor());
    if (use_collator && sort_flags != SORT_LOCALE_STRING) {
      UCollator *coll = s_collator->getCollator();
      if (coll) {
        intl_error &errcode = s_collator->getErrorCodeRef();
        return collator_asort(array, sort_flags, ascending,
                              coll, &errcode);
      }
    }
    ArraySortTmp ast(arr_array);
    ast->asort(sort_flags, ascending);
    return true;
  }
  if (array.isObject()) {
    ObjectData* obj = array.getObjectData();
    if (obj->getCollectionType() == Collection::StableMapType) {
      c_StableMap* smp = static_cast<c_StableMap*>(obj);
      smp->asort(sort_flags, ascending);
      return true;
    }
  }
  throw_bad_array_exception();
  return false;
}

static bool
php_ksort(VRefParam array, int sort_flags, bool ascending) {
  if (array.isArray()) {
    Array& arr_array = Variant::GetAsArray(array.getTypedAccessor());
    ArraySortTmp ast(arr_array);
    ast->ksort(sort_flags, ascending);
    return true;
  }
  if (array.isObject()) {
    ObjectData* obj = array.getObjectData();
    if (obj->getCollectionType() == Collection::StableMapType) {
      c_StableMap* smp = static_cast<c_StableMap*>(obj);
      smp->ksort(sort_flags, ascending);
      return true;
    }
  }
  throw_bad_array_exception();
  return false;
}

bool f_sort(VRefParam array, int sort_flags /* = 0 */,
            bool use_collator /* = false */) {
  return php_sort(array, sort_flags, true, use_collator);
}

bool f_rsort(VRefParam array, int sort_flags /* = 0 */,
             bool use_collator /* = false */) {
  return php_sort(array, sort_flags, false, use_collator);
}

bool f_asort(VRefParam array, int sort_flags /* = 0 */,
             bool use_collator /* = false */) {
  return php_asort(array, sort_flags, true, use_collator);
}

bool f_arsort(VRefParam array, int sort_flags /* = 0 */,
              bool use_collator /* = false */) {
  return php_asort(array, sort_flags, false, use_collator);
}

bool f_ksort(VRefParam array, int sort_flags /* = 0 */) {
  return php_ksort(array, sort_flags, true);
}

bool f_krsort(VRefParam array, int sort_flags /* = 0 */) {
  return php_ksort(array, sort_flags, false);
}

// NOTE: PHP's implementation of natsort and natcasesort accepts ArrayAccess
// objects as well, which does not make much sense, and which is not supported
// here.

Variant f_natsort(VRefParam array) {
  return php_asort(array, SORT_NATURAL, true, false);
}

Variant f_natcasesort(VRefParam array) {
  return php_asort(array, SORT_NATURAL_CASE, true, false);
}

bool f_usort(VRefParam array, CVarRef cmp_function) {
  if (array.isArray()) {
    Array& arr_array = Variant::GetAsArray(array.getTypedAccessor());
    ArraySortTmp ast(arr_array);
    ast->usort(cmp_function);
    return true;
  }
  if (array.isObject()) {
    ObjectData* obj = array.getObjectData();
    if (obj->getCollectionType() == Collection::VectorType) {
      c_Vector* vec = static_cast<c_Vector*>(obj);
      vec->usort(cmp_function);
      return true;
    }
  }
  throw_bad_array_exception();
  return false;
}

bool f_uasort(VRefParam array, CVarRef cmp_function) {
  if (array.isArray()) {
    Array& arr_array = Variant::GetAsArray(array.getTypedAccessor());
    ArraySortTmp ast(arr_array);
    ast->uasort(cmp_function);
    return true;
  }
  if (array.isObject()) {
    ObjectData* obj = array.getObjectData();
    if (obj->getCollectionType() == Collection::StableMapType) {
      c_StableMap* smp = static_cast<c_StableMap*>(obj);
      smp->uasort(cmp_function);
      return true;
    }
  }
  throw_bad_array_exception();
  return false;
}

bool f_uksort(VRefParam array, CVarRef cmp_function) {
  if (array.isArray()) {
    Array& arr_array = Variant::GetAsArray(array.getTypedAccessor());
    ArraySortTmp ast(arr_array);
    ast->uksort(cmp_function);
    return true;
  }
  if (array.isObject()) {
    ObjectData* obj = array.getObjectData();
    if (obj->getCollectionType() == Collection::StableMapType) {
      c_StableMap* smp = static_cast<c_StableMap*>(obj);
      smp->uksort(cmp_function);
      return true;
    }
  }
  throw_bad_array_exception();
  return false;
}

bool f_array_multisort(int _argc, VRefParam ar1,
                       CArrRef _argv /* = null_array */) {
  getCheckedArrayRet(ar1, false);
  std::vector<Array::SortData> data;
  std::vector<Array> arrays;
  arrays.reserve(1 + _argv.size()); // so no resize would happen

  Array::SortData sd;
  sd.original = &ar1;
  arrays.push_back(arr_ar1);
  sd.array = &arrays.back();
  sd.by_key = false;

  int sort_flags = SORT_REGULAR;
  bool ascending = true;
  for (int i = 0; i < _argv.size(); i++) {
    Variant *v = &((Array&)_argv).lvalAt(i);
    Variant::TypedValueAccessor tva = v->getTypedAccessor();
    if (Variant::GetAccessorType(tva) == KindOfArray) {
      sd.cmp_func = get_cmp_func(sort_flags, ascending);
      data.push_back(sd);

      sort_flags = SORT_REGULAR;
      ascending = true;

      sd.original = v;
      arrays.push_back(Variant::GetAsArray(tva));
      sd.array = &arrays.back();
    } else {
      int n = v->toInt32();
      if (n == SORT_ASC) {
        ascending = true;
      } else if (n == SORT_DESC) {
        ascending = false;
      } else {
        sort_flags = n;
      }
    }
  }

  sd.cmp_func = get_cmp_func(sort_flags, ascending);
  data.push_back(sd);

  return Array::MultiSort(data, true);
}

Variant f_array_unique(CVarRef array, int sort_flags /* = 2 */) {
  // NOTE, PHP array_unique accepts ArrayAccess objects as well,
  // which is not supported here.
  getCheckedArray(array);
  switch (sort_flags) {
  case SORT_STRING:
  case SORT_LOCALE_STRING:
    return ArrayUtil::StringUnique(arr_array);
  case SORT_NUMERIC:
    return ArrayUtil::NumericUnique(arr_array);
  case SORT_REGULAR:
  default:
    return ArrayUtil::RegularSortUnique(arr_array);
  }
}

String f_i18n_loc_get_default() {
  return s_collator->getLocale();
}

bool f_i18n_loc_set_default(CStrRef locale) {
  return s_collator->setLocale(locale);
}

bool f_i18n_loc_set_attribute(int64 attr, int64 val) {
  return s_collator->setAttribute(attr, val);
}

bool f_i18n_loc_set_strength(int64 strength) {
  return s_collator->setStrength(strength);
}

Variant f_i18n_loc_get_error_code() {
  return s_collator->getErrorCode();
}

///////////////////////////////////////////////////////////////////////////////
}
