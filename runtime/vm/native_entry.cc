// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/native_entry.h"

#include "include/dart_api.h"

#include "vm/bootstrap.h"
#include "vm/code_patcher.h"
#include "vm/dart_api_impl.h"
#include "vm/dart_api_state.h"
#include "vm/object_store.h"
#include "vm/reusable_handles.h"
#include "vm/stack_frame.h"
#include "vm/symbols.h"
#include "vm/tags.h"


namespace dart {

DEFINE_FLAG(bool, trace_natives, false,
            "Trace invocation of natives (debug mode only)");


NativeFunction NativeEntry::ResolveNative(const Library& library,
                                          const String& function_name,
                                          int number_of_arguments,
                                          bool* auto_setup_scope) {
  // Now resolve the native function to the corresponding native entrypoint.
  if (library.native_entry_resolver() == 0) {
    // Native methods are not allowed in the library to which this
    // class belongs in.
    return NULL;
  }
  Dart_EnterScope();  // Enter a new Dart API scope as we invoke API entries.
  Dart_NativeEntryResolver resolver = library.native_entry_resolver();
  Dart_NativeFunction native_function =
      resolver(Api::NewHandle(Thread::Current(), function_name.raw()),
               number_of_arguments, auto_setup_scope);
  Dart_ExitScope();  // Exit the Dart API scope.
  return reinterpret_cast<NativeFunction>(native_function);
}


const uint8_t* NativeEntry::ResolveSymbolInLibrary(const Library& library,
                                                   uword pc) {
  Dart_NativeEntrySymbol symbol_resolver =
      library.native_entry_symbol_resolver();
  if (symbol_resolver == 0) {
    // Cannot reverse lookup native entries.
    return NULL;
  }
  return symbol_resolver(reinterpret_cast<Dart_NativeFunction>(pc));
}


const uint8_t* NativeEntry::ResolveSymbol(uword pc) {
  Thread* thread = Thread::Current();
  REUSABLE_GROWABLE_OBJECT_ARRAY_HANDLESCOPE(thread);
  GrowableObjectArray& libs = reused_growable_object_array_handle.Handle();
  libs ^= thread->isolate()->object_store()->libraries();
  ASSERT(!libs.IsNull());
  intptr_t num_libs = libs.Length();
  for (intptr_t i = 0; i < num_libs; i++) {
    REUSABLE_LIBRARY_HANDLESCOPE(thread);
    Library& lib = reused_library_handle.Handle();
    lib ^= libs.At(i);
    ASSERT(!lib.IsNull());
    const uint8_t* r = ResolveSymbolInLibrary(lib, pc);
    if (r != NULL) {
      return r;
    }
  }
  return NULL;
}


uword NativeEntry::NativeCallWrapperEntry() {
  uword entry = reinterpret_cast<uword>(NativeEntry::NativeCallWrapper);
#if defined(USING_SIMULATOR)
  entry = Simulator::RedirectExternalReference(
      entry, Simulator::kNativeCall, NativeEntry::kNumCallWrapperArguments);
#endif
  return entry;
}


void NativeEntry::NativeCallWrapper(Dart_NativeArguments args,
                                    Dart_NativeFunction func) {
  CHECK_STACK_ALIGNMENT;
  VERIFY_ON_TRANSITION;
  NativeArguments* arguments = reinterpret_cast<NativeArguments*>(args);
  /* Tell MemorySanitizer 'arguments' is initialized by generated code. */
  MSAN_UNPOISON(arguments, sizeof(*arguments));
  Thread* thread = arguments->thread();
  Isolate* isolate = thread->isolate();

  ApiState* state = isolate->api_state();
  ASSERT(state != NULL);
  ApiLocalScope* current_top_scope = thread->api_top_scope();
  ApiLocalScope* scope = thread->api_reusable_scope();
  TRACE_NATIVE_CALL("0x%" Px "", reinterpret_cast<uintptr_t>(func));
  if (scope == NULL) {
    scope = new ApiLocalScope(current_top_scope,
                              thread->top_exit_frame_info());
    ASSERT(scope != NULL);
  } else {
    scope->Reinit(thread,
                  current_top_scope,
                  thread->top_exit_frame_info());
    thread->set_api_reusable_scope(NULL);
  }
  thread->set_api_top_scope(scope);  // New scope is now the top scope.

  func(args);

  ASSERT(current_top_scope == scope->previous());
  thread->set_api_top_scope(current_top_scope);  // Reset top scope to previous.
  if (thread->api_reusable_scope() == NULL) {
    scope->Reset(thread);  // Reset the old scope which we just exited.
    thread->set_api_reusable_scope(scope);
  } else {
    ASSERT(thread->api_reusable_scope() != scope);
    delete scope;
  }
  DEOPTIMIZE_ALOT;
  VERIFY_ON_TRANSITION;
}


static NativeFunction ResolveNativeFunction(Zone* zone,
                                            const Function& func,
                                            bool* is_bootstrap_native) {
  const Class& cls = Class::Handle(zone, func.Owner());
  const Library& library = Library::Handle(zone, cls.library());

  *is_bootstrap_native =
      Bootstrap::IsBootstapResolver(library.native_entry_resolver());

  const String& native_name = String::Handle(zone, func.native_name());
  ASSERT(!native_name.IsNull());

  const int num_params = NativeArguments::ParameterCountForResolution(func);
  bool auto_setup_scope = true;
  return NativeEntry::ResolveNative(
      library, native_name, num_params, &auto_setup_scope);
}


uword NativeEntry::LinkNativeCallEntry() {
  uword entry = reinterpret_cast<uword>(NativeEntry::LinkNativeCall);
#if defined(USING_SIMULATOR)
  entry = Simulator::RedirectExternalReference(
      entry, Simulator::kBootstrapNativeCall, NativeEntry::kNumArguments);
#endif
  return entry;
}


void NativeEntry::LinkNativeCall(Dart_NativeArguments args) {
  CHECK_STACK_ALIGNMENT;
  VERIFY_ON_TRANSITION;
  NativeArguments* arguments = reinterpret_cast<NativeArguments*>(args);
  /* Tell MemorySanitizer 'arguments' is initialized by generated code. */
  MSAN_UNPOISON(arguments, sizeof(*arguments));
  TRACE_NATIVE_CALL("%s", "LinkNative");

  NativeFunction target_function = NULL;
  bool call_through_wrapper = false;
#ifdef USING_SIMULATOR
  bool is_native_auto_setup_scope = false;
#endif

  {
    StackZone zone(arguments->thread());

    DartFrameIterator iterator;
    StackFrame* caller_frame = iterator.NextFrame();

    const Code& code = Code::Handle(caller_frame->LookupDartCode());
    const Function& func = Function::Handle(code.function());
#ifdef USING_SIMULATOR
    is_native_auto_setup_scope = func.IsNativeAutoSetupScope();
#endif

    if (FLAG_trace_natives) {
      OS::Print("Resolving native target for %s\n", func.ToCString());
    }

    bool is_bootstrap_native = false;
    target_function = ResolveNativeFunction(
        arguments->thread()->zone(), func, &is_bootstrap_native);
    ASSERT(target_function != NULL);

#if defined(DEBUG)
    {
      NativeFunction current_function = NULL;
      const Code& current_trampoline = Code::Handle(
          CodePatcher::GetNativeCallAt(caller_frame->pc(),
                                       code,
                                       &current_function));
#if !defined(USING_SIMULATOR)
      ASSERT(current_function ==
             reinterpret_cast<NativeFunction>(LinkNativeCall));
#else
      ASSERT(current_function ==
             reinterpret_cast<NativeFunction>(
                 Simulator::RedirectExternalReference(
                     reinterpret_cast<uword>(LinkNativeCall),
                     Simulator::kBootstrapNativeCall,
                     NativeEntry::kNumArguments)));
#endif
      ASSERT(current_trampoline.raw() ==
             StubCode::CallBootstrapCFunction_entry()->code());
    }
#endif

    const intptr_t argc_tag = NativeArguments::ComputeArgcTag(func);
    const bool is_leaf_call =
        (argc_tag & NativeArguments::AutoSetupScopeMask()) == 0;

    call_through_wrapper = !is_bootstrap_native && !is_leaf_call;

    const Code& trampoline = Code::Handle(call_through_wrapper ?
        StubCode::CallNativeCFunction_entry()->code() :
        StubCode::CallBootstrapCFunction_entry()->code());

    NativeFunction patch_target_function = target_function;
#if defined(USING_SIMULATOR)
    if (!call_through_wrapper || !is_native_auto_setup_scope) {
      patch_target_function = reinterpret_cast<NativeFunction>(
          Simulator::RedirectExternalReference(
              reinterpret_cast<uword>(patch_target_function),
              Simulator::kBootstrapNativeCall, NativeEntry::kNumArguments));
    }
#endif

    CodePatcher::PatchNativeCallAt(
        caller_frame->pc(), code, patch_target_function, trampoline);

    if (FLAG_trace_natives) {
      OS::Print("    -> %p (%s, %s)\n",
                target_function,
                is_bootstrap_native ? "bootstrap" : "non-bootstrap",
                is_leaf_call ? "leaf" : "non-leaf");
    }
  }
  VERIFY_ON_TRANSITION;

  // Tail-call resolved target.
  if (call_through_wrapper) {
    NativeEntry::NativeCallWrapper(
        args, reinterpret_cast<Dart_NativeFunction>(target_function));
  } else {
    target_function(arguments);
  }
}


}  // namespace dart
