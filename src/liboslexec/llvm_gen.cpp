// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include <cmath>

#include <OpenImageIO/fmath.h>

#include "oslexec_pvt.h"
#include <OSL/genclosure.h>
#include "backendllvm.h"

using namespace OSL;
using namespace OSL::pvt;

OSL_NAMESPACE_BEGIN

namespace pvt {

static ustring op_and("and");
static ustring op_bitand("bitand");
static ustring op_bitor("bitor");
static ustring op_break("break");
static ustring op_ceil("ceil");
static ustring op_cellnoise("cellnoise");
static ustring op_color("color");
static ustring op_compl("compl");
static ustring op_continue("continue");
static ustring op_dowhile("dowhile");
static ustring op_eq("eq");
static ustring op_error("error");
static ustring op_fabs("fabs");
static ustring op_floor("floor");
static ustring op_for("for");
static ustring op_format("format");
static ustring op_fprintf("fprintf");
static ustring op_ge("ge");
static ustring op_gt("gt");
static ustring op_hashnoise("hashnoise");
static ustring op_if("if");
static ustring op_le("le");
static ustring op_logb("logb");
static ustring op_lt("lt");
static ustring op_min("min");
static ustring op_neq("neq");
static ustring op_normal("normal");
static ustring op_or("or");
static ustring op_point("point");
static ustring op_printf("printf");
static ustring op_round("round");
static ustring op_shl("shl");
static ustring op_shr("shr");
static ustring op_sign("sign");
static ustring op_step("step");
static ustring op_trunc("trunc");
static ustring op_vector("vector");
static ustring op_warning("warning");
static ustring op_xor("xor");

static ustring u_distance("distance");
static ustring u_index("index");



/// Macro that defines the arguments to LLVM IR generating routines
///
#define LLVMGEN_ARGS BackendLLVM &rop, int opnum

/// Macro that defines the full declaration of an LLVM generator.
///
#define LLVMGEN(name) bool name(LLVMGEN_ARGS)

// Forward decl
LLVMGEN(llvm_gen_generic);



void
BackendLLVM::llvm_gen_debug_printf(string_view message)
{
    // Bake everything into the format specifier string instead of
    // passing arguments.
    ustring s = ustring::fmtformat("({} {}) {}\n", inst()->shadername(),
                                   inst()->layername(), message);

    llvm::Value* valargs[] = { sg_void_ptr(),
                               llvm_const_hash(s),
                               ll.constant(0) /*arg_count*/,
                               ll.void_ptr_null() /*arg_types*/,
                               ll.constant(0) /*arg_values_size*/,
                               ll.void_ptr_null() /*arg_values*/ };

    ll.call_function("osl_gen_printfmt", valargs);
}



void
BackendLLVM::llvm_gen_warning(string_view message)
{
    // Bake everything into the format specifier string instead of
    // passing arguments.
    ustring s              = ustring::fmtformat("{}\n", message);
    llvm::Value* valargs[] = { sg_void_ptr(),
                               llvm_const_hash(s),
                               ll.constant(0) /*arg_count*/,
                               ll.void_ptr_null() /*arg_types*/,
                               ll.constant(0) /*arg_values_size*/,
                               ll.void_ptr_null() /*arg_values*/ };
    ll.call_function("osl_gen_warningfmt", valargs);
}



void
BackendLLVM::llvm_gen_error(string_view message)
{
    // Bake everything into the format specifier string instead of
    // passing arguments.
    ustring s              = ustring::fmtformat("{}\n", message);
    llvm::Value* valargs[] = { sg_void_ptr(),
                               llvm_const_hash(s),
                               ll.constant(0) /*arg_count*/,
                               ll.void_ptr_null() /*arg_types*/,
                               ll.constant(0) /*arg_values_size*/,
                               ll.void_ptr_null() /*arg_values*/ };
    ll.call_function("osl_gen_errorfmt", valargs);
}



void
BackendLLVM::llvm_call_layer(int layer, bool unconditional)
{
    // Make code that looks like:
    //     if (! groupdata->run[parentlayer])
    //         parent_layer (sg, groupdata, userdata_base_ptr,
    //                       output_base_ptr, shadeindex, interactive_params);
    // if it's a conditional call, or
    //     parent_layer (sg, groupdata, userdata_base_ptr,
    //                   output_base_ptr, shadeindex, interactive_params);
    // if it's run unconditionally.
    // The code in the parent layer itself will set its 'executed' flag.

    llvm::Value* args[]
        = { sg_ptr(),          groupdata_ptr(), userdata_base_ptr(),
            output_base_ptr(), shadeindex(),    m_llvm_interactive_params_ptr };

    ShaderInstance* parent       = group()[layer];
    llvm::Value* trueval         = ll.constant_bool(true);
    llvm::Value* layerfield      = layer_run_ref(layer_remap(layer));
    llvm::BasicBlock *then_block = NULL, *after_block = NULL;
    if (!unconditional) {
        llvm::Value* executed = ll.op_load(ll.type_bool(), layerfield);
        executed              = ll.op_ne(executed, trueval);
        then_block            = ll.new_basic_block("");
        after_block           = ll.new_basic_block("");
        ll.op_branch(executed, then_block, after_block);
        // insert point is now then_block
    }

    // Mark the call as a fast call
    llvm::Value* funccall
        = ll.call_function(layer_function_name(group(), *parent).c_str(), args);
    if (!parent->entry_layer())
        ll.mark_fast_func_call(funccall);

    if (!unconditional)
        ll.op_branch(after_block);  // also moves insert point

    shadingsys().m_stat_call_layers_inserted++;
}



void
BackendLLVM::llvm_run_connected_layers(Symbol& sym, int symindex, int opnum,
                                       std::set<int>* already_run)
{
    if (sym.valuesource() != Symbol::ConnectedVal)
        return;  // Nothing to do

    bool inmain = (opnum >= inst()->maincodebegin()
                   && opnum < inst()->maincodeend());

    for (int c = 0; c < inst()->nconnections(); ++c) {
        const Connection& con(inst()->connection(c));
        // If the connection gives a value to this param
        if (con.dst.param == symindex) {
            // Non-lazy layers are run upfront directly via llvm_call_layer.
            // Eliding them here doesn't change the semantics of execution,
            // but it will prevent optixTrace calls from being repeatedly
            // inlined when lazytrace=0.
            if (!group()[con.srclayer]->run_lazily())
                continue;

            // already_run is a set of layers run for this particular op.
            // Just so we don't stupidly do several consecutive checks on
            // whether we ran this same layer. It's JUST for this op.
            if (already_run) {
                if (already_run->count(con.srclayer))
                    continue;  // already ran that one on this op
                else
                    already_run->insert(con.srclayer);  // mark it
            }

            if (inmain) {
                // There is an instance-wide m_layers_already_run that tries
                // to remember which earlier layers have unconditionally
                // been run at any point in the execution of this layer. But
                // only honor (and modify) that when in the main code
                // section, not when in init ops, which are inherently
                // conditional.
                if (m_layers_already_run.count(con.srclayer)) {
                    continue;  // already unconditionally ran the layer
                }
                if (!m_in_conditional[opnum]) {
                    // Unconditionally running -- mark so we don't do it
                    // again. If we're inside a conditional, don't mark
                    // because it may not execute the conditional body.
                    m_layers_already_run.insert(con.srclayer);
                }
            }

            if (shadingsys().m_opt_useparam && inmain) {
                // m_call_layers_inserted tracks if we've already run this layer inside the current basic block
                const CallLayerKey key = { m_bblockids[opnum], con.srclayer };
                if (m_call_layers_inserted.count(key)) {
                    continue;
                }
                m_call_layers_inserted.insert(key);
            }

            // If the earlier layer it comes from has not yet been
            // executed, do so now.
            llvm_call_layer(con.srclayer);
        }
    }
}



OSL_PRAGMA_WARNING_PUSH
OSL_GCC_PRAGMA(GCC diagnostic ignored "-Wunused-parameter")

LLVMGEN(llvm_gen_nop)
{
    return true;
}

OSL_PRAGMA_WARNING_POP



LLVMGEN(llvm_gen_useparam)
{
    OSL_DASSERT(!rop.inst()->unused()
                && "oops, thought this layer was unused, why do we call it?");

    // If we have multiple params needed on this statement, don't waste
    // time checking the same upstream layer more than once.
    std::set<int> already_run;

    Opcode& op(rop.inst()->ops()[opnum]);
    for (int i = 0; i < op.nargs(); ++i) {
        Symbol& sym  = *rop.opargsym(op, i);
        int symindex = rop.inst()->arg(op.firstarg() + i);
        rop.llvm_run_connected_layers(sym, symindex, opnum, &already_run);
        // If it's an interpolated (userdata) parameter and we're
        // initializing them lazily, now we have to do it.
        if ((sym.symtype() == SymTypeParam
             || sym.symtype() == SymTypeOutputParam)
            && sym.interpolated() && !sym.typespec().is_closure()
            && !sym.connected() && !sym.connected_down()
            && rop.shadingsys().lazy_userdata()) {
            rop.llvm_assign_initial_value(sym);
        }
    }

    rop.increment_useparam_ops();

    return true;
}



// Used for printf, error, warning, format, fprintf
LLVMGEN(llvm_gen_printf_legacy)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    // Prepare the args for the call

    // Which argument is the format string?  Usually 0, but for op
    // format() and fprintf(), the formatting string is argument #1.
    int format_arg = (op.opname() == "format" || op.opname() == "fprintf") ? 1
                                                                           : 0;
    Symbol& format_sym = *rop.opargsym(op, format_arg);

    std::vector<llvm::Value*> call_args;
    if (!format_sym.is_constant()) {
        rop.shadingcontext()->warningfmt(
            "{} must currently have constant format\n", op.opname());
        return false;
    }

    // For some ops, we push the shader globals pointer
    if (op.opname() == op_printf || op.opname() == op_error
        || op.opname() == op_warning || op.opname() == op_fprintf)
        call_args.push_back(rop.sg_void_ptr());

    // fprintf also needs the filename
    if (op.opname() == op_fprintf) {
        Symbol& Filename = *rop.opargsym(op, 0);
        llvm::Value* fn  = rop.llvm_load_value(Filename);
        call_args.push_back(fn);
    }

    // We're going to need to adjust the format string as we go, but I'd
    // like to reserve a spot for the char*.
    size_t new_format_slot = call_args.size();
    call_args.push_back(NULL);

    ustring format_ustring = format_sym.get_string();
    const char* format     = format_ustring.c_str();
    std::string s;
    int arg           = format_arg + 1;
    size_t optix_size = 0;  // how much buffer size does optix need?
    while (*format != '\0') {
        if (*format == '%') {
            if (format[1] == '%') {
                // '%%' is a literal '%'
                s += "%%";
                format += 2;  // skip both percentages
                continue;
            }
            const char* oldfmt = format;  // mark beginning of format
            while (*format && *format != 'c' && *format != 'd' && *format != 'e'
                   && *format != 'f' && *format != 'g' && *format != 'i'
                   && *format != 'm' && *format != 'n' && *format != 'o'
                   && *format != 'p' && *format != 's' && *format != 'u'
                   && *format != 'v' && *format != 'x' && *format != 'X')
                ++format;
            char formatchar = *format++;  // Also eat the format char
            if (arg >= op.nargs()) {
                rop.shadingcontext()->errorfmt(
                    "Mismatch between format string and arguments ({}:{})",
                    op.sourcefile(), op.sourceline());
                return false;
            }

            std::string ourformat(oldfmt, format);  // straddle the format
            // Doctor it to fix mismatches between format and data
            Symbol& sym(*rop.opargsym(op, arg));
            OSL_ASSERT(!sym.typespec().is_structure_based());

            TypeDesc simpletype(sym.typespec().simpletype());
            int num_elements   = simpletype.numelements();
            int num_components = simpletype.aggregate;
            if ((sym.typespec().is_closure_based()
                 || simpletype.basetype == TypeDesc::STRING)
                && formatchar != 's') {
                ourformat[ourformat.length() - 1] = 's';
            }
            if (simpletype.basetype == TypeDesc::INT && formatchar != 'd'
                && formatchar != 'i' && formatchar != 'o' && formatchar != 'u'
                && formatchar != 'x' && formatchar != 'X') {
                ourformat[ourformat.length() - 1] = 'd';
            }
            if (simpletype.basetype == TypeDesc::FLOAT && formatchar != 'f'
                && formatchar != 'g' && formatchar != 'c' && formatchar != 'e'
                && formatchar != 'm' && formatchar != 'n' && formatchar != 'p'
                && formatchar != 'v') {
                ourformat[ourformat.length() - 1] = 'f';
            }
            // NOTE(boulos): Only for debug mode do the derivatives get printed...
            for (int a = 0; a < num_elements; ++a) {
                llvm::Value* arrind = simpletype.arraylen ? rop.ll.constant(a)
                                                          : NULL;
                if (sym.typespec().is_closure_based()) {
                    s += ourformat;
                    llvm::Value* v = rop.llvm_load_value(sym, 0, arrind, 0);
                    v = rop.ll.call_function("osl_closure_to_string",
                                             rop.sg_void_ptr(), v);
                    call_args.push_back(v);
                    continue;
                }

                for (int c = 0; c < num_components; c++) {
                    if (c != 0 || a != 0)
                        s += " ";
                    s += ourformat;

                    llvm::Value* loaded = rop.llvm_load_value(sym, 0, arrind,
                                                              c);
                    if (simpletype.basetype == TypeDesc::FLOAT) {
                        // C varargs convention upconverts float->double.
                        loaded = rop.ll.op_float_to_double(loaded);
                        // Ensure that 64-bit values are aligned to 8-byte boundaries
                        optix_size = (optix_size + sizeof(double) - 1)
                                     & ~(sizeof(double) - 1);
                        optix_size += sizeof(double);
                    } else if (simpletype.basetype == TypeDesc::INT)
                        optix_size += sizeof(int);
                    else if (simpletype.basetype == TypeDesc::STRING)
                        optix_size += sizeof(uint64_t);
                    call_args.push_back(loaded);
                }
            }
            ++arg;
        } else {
            // Everything else -- just copy the character and advance
            s += *format++;
        }
    }


#if OSL_USE_OPTIX
    // In OptiX, printf currently supports 0 or 1 arguments, and the signature
    // requires 1 argument, so push a null pointer onto the call args if there
    // is no argument.
    if (rop.use_optix() && arg == format_arg + 1) {
        call_args.push_back(rop.ll.void_ptr_null());
        // we push the size of the arguments on the stack
        optix_size += sizeof(uint64_t);
    }
#endif

    // TODO: optix cache should handle ustrings generated during llvm-gen
    if (!rop.use_optix_cache()) {
        // Some ops prepend things
        if (op.opname() == op_error || op.opname() == op_warning) {
            s = fmtformat("Shader {} [{}]: {}", op.opname(),
                          rop.inst()->shadername(), s);
        }
    }

    // Now go back and put the new format string in its place
#if OSL_USE_OPTIX
    if (rop.use_optix()) {
        // In OptiX7+ case, we do this:
        // void* args = { args_size, arg0, arg1, arg2 };
        // (where args_size is the size of arg0 + arg1 + arg2...)
        //
        // Make sure host has the format string so it can print it
        call_args[new_format_slot] = rop.llvm_const_hash(s);
        size_t nargs               = call_args.size() - (new_format_slot + 1);
        // Allocate space to store the arguments to osl_printf().
        //  Don't forget to pad a little extra to hold the size of the arguments itself.
        llvm::Value* voids = rop.ll.op_alloca(
            rop.ll.type_char(), optix_size + sizeof(uint64_t),
            fmtformat("printf_argbuf_L{}sz{}_", op.sourceline(), optix_size),
            8);

        // Size of the collection of arguments comes before all the arguments
        {
            llvm::Value* args_size = rop.ll.constant64(optix_size);
            llvm::Value* memptr    = rop.ll.offset_ptr(voids, 0);
            llvm::Value* iptr      = rop.ll.ptr_cast(memptr,
                                                     rop.ll.type_longlong_ptr(),
                                                     "printf_argbuf_as_llptr");
            rop.ll.op_store(args_size, iptr);
        }
        optix_size = sizeof(
            uint64_t);  // first 'args' element is the size of the argument list
        for (size_t i = 0; i < nargs; ++i) {
            llvm::Value* arg = call_args[new_format_slot + 1 + i];
            if (arg->getType()->isFloatingPointTy()
                || arg->getType()->isIntegerTy(64)) {
                // Ensure that 64-bit values are aligned to 8-byte boundaries
                optix_size = (optix_size + sizeof(double) - 1)
                             & ~(sizeof(double) - 1);
            }
            llvm::Value* memptr = rop.ll.offset_ptr(voids, optix_size);
            if (arg->getType()->isIntegerTy()) {
                llvm::Type* ptr_type = nullptr;
                if (arg->getType()->isIntegerTy(64)) {
                    optix_size += sizeof(uint64_t);
                    ptr_type = rop.ll.type_int64_ptr();
                } else {
                    optix_size += sizeof(int);
                    ptr_type = rop.ll.type_int_ptr();
                }
                llvm::Value* iptr = rop.ll.ptr_cast(memptr, ptr_type);
                rop.ll.op_store(arg, iptr);
            } else if (arg->getType()->isFloatingPointTy()) {
                llvm::Value* fptr = rop.ll.ptr_cast(memptr,
                                                    rop.ll.type_double_ptr());
                rop.ll.op_store(arg, fptr);
                optix_size += sizeof(double);
            } else {
                llvm::Value* vptr = rop.ll.ptr_to_cast(memptr,
                                                       rop.ll.type_void_ptr());
                rop.ll.op_store(arg, vptr);
                optix_size += sizeof(uint64_t);
            }
        }
        call_args.resize(new_format_slot + 2);
        call_args.back() = rop.ll.void_ptr(voids);
    } else
#endif
    {
        call_args[new_format_slot] = rop.llvm_const_hash(s);
    }

    // Construct the function name and call it.
    std::string opname = std::string("osl_") + op.opname().string();
    llvm::Value* ret   = rop.ll.call_function(opname.c_str(), call_args);

    // The format op returns a string value, put in in the right spot
    if (op.opname() == op_format)
        rop.llvm_store_value(ret, *rop.opargsym(op, 0));
    return true;
}

// Used for printf, error, warning, format, fprintf
LLVMGEN(llvm_gen_print_fmt)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    // Prepare the args for the call

    // Which argument is the format string?  Usually 0, but for op
    // format() and fprintf(), the formatting string is argument #1.
    int format_arg     = (op.opname() == op_format || op.opname() == op_fprintf)
                             ? 1
                             : 0;
    Symbol& format_sym = *rop.opargsym(op, format_arg);

    std::vector<llvm::Value*> call_args;
    if (!format_sym.is_constant()) {
        rop.shadingcontext()->warningfmt(
            "{} must currently have constant format\n", op.opname());
        return false;
    }

    call_args.push_back(rop.sg_void_ptr());

    // fprintf also needs the filename
    if (op.opname() == op_fprintf) {
        Symbol& Filename         = *rop.opargsym(op, 0);
        ustring filename_ustring = Filename.get_string();
        call_args.push_back(rop.llvm_const_hash(filename_ustring));  //filename
    }

    // We're going to need to adjust the format string as we go, but I'd
    // like to reserve a spot for the char*.
    // size_t new_format_slot = call_args.size();
    // call_args.push_back(NULL);

    ustring format_ustring = format_sym.get_string();
    const char* format
        = format_ustring.c_str();  //contains all the printf formating characters
    std::string s;
    int arg = format_arg + 1;
    // Example call: printf("The values at %s is %d, %f, %6.2f", "marble", 5, 6.4, 123.456);
    std::vector<EncodedType> encodedtypes;
    int arg_values_size = 0u;
    std::vector<llvm::Value*> loaded_arg_values;
    while (*format != '\0') {
        if (*format == '%') {
            if (format[1] == '%') {
                // '%%' is a literal '%'
                // The fmtlib expects just a single %
                s += "%";
                format += 2;  // skip both percentages
                continue;
            }
            const char* oldfmt = format;  // mark beginning of format
            while (*format && *format != 'c' && *format != 'd' && *format != 'e'
                   && *format != 'f' && *format != 'g' && *format != 'i'
                   && *format != 'm' && *format != 'n' && *format != 'o'
                   && *format != 'p' && *format != 's' && *format != 'u'
                   && *format != 'v' && *format != 'x' && *format != 'X') {
                ++format;
            }

            char formatchar = *format++;  // Also eat the format char
            if (arg >= op.nargs()) {
                rop.shadingcontext()->errorfmt(
                    "Mismatch between format string and arguments ({}:{})",
                    op.sourcefile(), op.sourceline());
                return false;
            }

            std::string ourformat(oldfmt, format);  // straddle the format

            // printf specifier uses - to indicate left justified alignment and ignores extra - chars present
            // libfmt specifier uses < to indicate left justified alignment and does not ignore extra chars
            // so change - to < and erase any extraneous -
            auto pos_of_minus = ourformat.find_first_of('-');
            if (pos_of_minus != std::string::npos) {
                ourformat.replace(pos_of_minus, 1 /*num chars to replace*/,
                                  1 /*num times to repeat char*/, '<');
                pos_of_minus = ourformat.find_first_of('-');
                while (pos_of_minus != std::string::npos) {
                    ourformat.erase(pos_of_minus, 1);
                    pos_of_minus = ourformat.find_first_of('-');
                }
            }

            // Doctor it to fix mismatches between format and data
            Symbol& sym(*rop.opargsym(op, arg));
            OSL_ASSERT(!sym.typespec().is_structure_based());
            TypeDesc simpletype(sym.typespec().simpletype());
            int num_elements   = simpletype.numelements();
            int num_components = simpletype.aggregate;
            if ((sym.typespec().is_closure_based()
                 || simpletype.basetype == TypeDesc::STRING)
                && formatchar != 's') {
                ourformat[ourformat.length() - 1] = 's';
            }

            //%i is not legal in fmtlib and it will be converted to a d
            if (simpletype.basetype == TypeDesc::INT
                && formatchar != 'd'
                /* && formatchar != 'i'*/
                && formatchar != 'o' && formatchar != 'u' && formatchar != 'x'
                && formatchar != 'X') {
                ourformat[ourformat.length() - 1] = 'd';
            }

            // //fmtlib does not use a fmt specifier for unsigned ints
            // if (simpletype.basetype == TypeDesc::INT && formatchar == 'u'{
            //     ourformat[ourformat.length() - 1] = '';
            // }


            //%m,%n,%v,%p, and %c are not legal in C-style printf and end up getting filtered by oslc
            if (simpletype.basetype == TypeDesc::FLOAT && formatchar != 'f'
                && formatchar != 'g' /*&& formatchar != 'c'*/ && formatchar != 'e'
                /*&& formatchar != 'm' && formatchar != 'n' && formatchar != 'p' */
                /*&& formatchar != 'v'*/) {
                ourformat[ourformat.length() - 1] = 'f';
            }

            EncodedType et = EncodedType::kUstringHash;
            if (simpletype.basetype == TypeDesc::INT) {
                //to mimic printf behavior when a hex specifier is used we are promoting the int to uint32_t
                if (formatchar == 'x' || formatchar == 'X') {
                    et = EncodedType::kUInt32;
                } else {
                    et = EncodedType::kInt32;
                }
            }
            if (simpletype.basetype == TypeDesc::FLOAT)
                et = EncodedType::kFloat;

            std::string myformat("{:");
            ourformat.replace(0, 1, "");
            myformat += ourformat;
            myformat += "}";

            TypeDesc symty    = sym.typespec().simpletype();
            TypeDesc basetype = TypeDesc::BASETYPE(symty.basetype);
            // NOTE(boulos): Only for debug mode do the derivatives get printed...
            for (int a = 0; a < num_elements; ++a) {
                llvm::Value* const_arrind = simpletype.arraylen
                                                ? rop.ll.constant(a)
                                                : NULL;
                if (sym.typespec().is_closure_based()) {
                    s += myformat;

                    llvm::Value* v = rop.llvm_load_value(sym, 0, const_arrind,
                                                         0);
                    v = rop.ll.call_function("osl_closure_to_ustringhash",
                                             rop.sg_void_ptr(), v);
                    encodedtypes.push_back(et);
                    arg_values_size += pvt::size_of_encoded_type(et);
                    loaded_arg_values.push_back(v);
                    continue;
                }

                for (int c = 0; c < num_components; c++) {
                    if (c != 0 || a != 0)
                        s += " ";
                    s += myformat;

                    // TODO: Add llvm_load_value that does this check
                    // internally to reduce bloat and chance of missing it
                    llvm::Value* loaded
                        = sym.is_constant()
                              ? rop.llvm_load_constant_value(sym, a, c,
                                                             basetype)
                              : rop.llvm_load_value(sym, 0, const_arrind, c,
                                                    basetype);

                    if (sym.typespec().is_string_based()
                        && (rop.ll.ustring_rep()
                            == LLVM_Util::UstringRep::charptr)) {
                        // Don't think this will need to be here soon
                        loaded = rop.ll.call_function("osl_gen_ustringhash_pod",
                                                      loaded);
                    }

                    encodedtypes.push_back(et);
                    arg_values_size += pvt::size_of_encoded_type(et);
                    loaded_arg_values.push_back(loaded);
                }
            }
            ++arg;

        } else {
            // Everything else -- just copy the character and advance
            char current_char = *format++;
            s += current_char;
            if (current_char == '{' || current_char == '}') {
                // fmtlib expects { to be {{
                //            and } to be }}
                // so just duplicate the character
                s += current_char;
            }
        }
    }
    if (!rop.use_optix_cache()) {
        // Some ops prepend things
        if (op.opname() == op_error || op.opname() == op_warning) {
            s = fmtformat("Shader {} [{}]: {}", op.opname(),
                          rop.inst()->shadername(), s);
        }
    }
    ustring s_ustring(s.c_str());
    call_args.push_back(rop.llvm_const_hash(s_ustring));

    OSL_ASSERT(encodedtypes.size() == loaded_arg_values.size());
    int arg_count = static_cast<int>(encodedtypes.size());
    call_args.push_back(rop.ll.constant(arg_count));

    llvm::Value* encodedtypes_on_stack
        = rop.ll.op_alloca(rop.ll.type_int8(), arg_count,
                           std::string("encodedtypes"));
    llvm::Value* loaded_arg_values_on_stack
        = rop.ll.op_alloca(rop.ll.type_int8(), arg_values_size,
                           std::string("argValues"));

    int bytesToArg = 0;
    for (int argindex = 0; argindex < arg_count; ++argindex) {
        EncodedType et = encodedtypes[argindex];
        rop.ll.op_store(rop.ll.constant8(static_cast<uint8_t>(et)),
                        rop.ll.GEP(rop.ll.type_int8(), encodedtypes_on_stack,
                                   argindex));

        llvm::Value* loadedArgValue = loaded_arg_values[argindex];

        llvm::Type* type_ptr = nullptr;
        switch (et) {
        case EncodedType::kUstringHash: {
            type_ptr = rop.ll.type_ptr(rop.ll.type_int64());

        } break;
        case EncodedType::kUInt32:
            // fallthrough
        case EncodedType::kInt32: {
            type_ptr = rop.ll.type_int_ptr();
        } break;

        case EncodedType::kFloat: {
            type_ptr = rop.ll.type_float_ptr();

        } break;
        default:
            // Although more encoded types exist, the 3 above are the only
            // ones we expect to be produced by OSL language itself.
            OSL_ASSERT(0 && "Unhandled EncodeType");
            break;
        }

        rop.ll.op_store(loadedArgValue,
                        rop.ll.ptr_cast(rop.ll.GEP(rop.ll.type_int8(),
                                                   loaded_arg_values_on_stack,
                                                   bytesToArg),
                                        type_ptr));
        bytesToArg += pvt::size_of_encoded_type(et);
    }

    call_args.push_back(rop.ll.void_ptr(encodedtypes_on_stack));
    call_args.push_back(rop.ll.constant(arg_values_size));
    call_args.push_back(rop.ll.void_ptr(loaded_arg_values_on_stack));

    // Construct the function name and call it.
    const char* rs_func_name = nullptr;

    if (op.opname() == op_printf)
        rs_func_name = "osl_gen_printfmt";
    if (op.opname() == op_error)
        rs_func_name = "osl_gen_errorfmt";
    if (op.opname() == op_warning)
        rs_func_name = "osl_gen_warningfmt";
    if (op.opname() == op_fprintf)
        rs_func_name = "osl_gen_filefmt";

    // NOTE: format creates a new ustring, so only works on host
    if (op.opname() == op_format)
        rs_func_name = "osl_formatfmt";

    llvm::Value* ret = rop.ll.call_function(rs_func_name, call_args);

    // The format op returns a string value, put in in the right spot
    if (op.opname() == op_format)
        rop.llvm_store_value(ret, *rop.opargsym(op, 0));

    return true;
}


LLVMGEN(llvm_gen_printf)
{
    if (rop.use_optix())
        return llvm_gen_printf_legacy(rop, opnum);
    else
        return llvm_gen_print_fmt(rop, opnum);
}


LLVMGEN(llvm_gen_add)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);

    OSL_DASSERT(!A.typespec().is_array() && !B.typespec().is_array());
    if (Result.typespec().is_closure()) {
        OSL_DASSERT(A.typespec().is_closure() && B.typespec().is_closure());
        llvm::Value* valargs[] = { rop.sg_void_ptr(), rop.llvm_load_value(A),
                                   rop.llvm_load_value(B) };
        llvm::Value* res       = rop.ll.call_function("osl_add_closure_closure",
                                                      valargs);
        rop.llvm_store_value(res, Result, 0, NULL, 0);
        return true;
    }

    TypeDesc type      = Result.typespec().simpletype();
    int num_components = type.aggregate;

    // The following should handle f+f, v+v, v+f, f+v, i+i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value* a = rop.loadLLVMValue(A, i, 0, type);
        llvm::Value* b = rop.loadLLVMValue(B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value* r = rop.ll.op_add(a, b);
        rop.storeLLVMValue(r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        if (A.has_derivs() || B.has_derivs()) {
            for (int d = 1; d <= 2; ++d) {  // dx, dy
                for (int i = 0; i < num_components; i++) {
                    llvm::Value* a = rop.loadLLVMValue(A, i, d, type);
                    llvm::Value* b = rop.loadLLVMValue(B, i, d, type);
                    llvm::Value* r = rop.ll.op_add(a, b);
                    rop.storeLLVMValue(r, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs(Result);
        }
    }
    return true;
}



LLVMGEN(llvm_gen_sub)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);

    TypeDesc type      = Result.typespec().simpletype();
    int num_components = type.aggregate;

    OSL_DASSERT(!Result.typespec().is_closure_based()
                && "subtraction of closures not supported");

    // The following should handle f-f, v-v, v-f, f-v, i-i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value* a = rop.loadLLVMValue(A, i, 0, type);
        llvm::Value* b = rop.loadLLVMValue(B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value* r = rop.ll.op_sub(a, b);
        rop.storeLLVMValue(r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        if (A.has_derivs() || B.has_derivs()) {
            for (int d = 1; d <= 2; ++d) {  // dx, dy
                for (int i = 0; i < num_components; i++) {
                    llvm::Value* a = rop.loadLLVMValue(A, i, d, type);
                    llvm::Value* b = rop.loadLLVMValue(B, i, d, type);
                    llvm::Value* r = rop.ll.op_sub(a, b);
                    rop.storeLLVMValue(r, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs(Result);
        }
    }
    return true;
}



LLVMGEN(llvm_gen_mul)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);

    TypeDesc type                  = Result.typespec().simpletype();
    OSL_MAYBE_UNUSED bool is_float = !Result.typespec().is_closure_based()
                                     && Result.typespec().is_float_based();
    int num_components = type.aggregate;

    // multiplication involving closures
    if (Result.typespec().is_closure()) {
        llvm::Value* valargs[3];
        valargs[0] = rop.sg_void_ptr();
        bool tfloat;
        if (A.typespec().is_closure()) {
            tfloat     = B.typespec().is_float();
            valargs[1] = rop.llvm_load_value(A);
            valargs[2] = tfloat ? rop.llvm_load_value(B) : rop.llvm_void_ptr(B);
        } else {
            tfloat     = A.typespec().is_float();
            valargs[1] = rop.llvm_load_value(B);
            valargs[2] = tfloat ? rop.llvm_load_value(A) : rop.llvm_void_ptr(A);
        }
        llvm::Value* res
            = tfloat ? rop.ll.call_function("osl_mul_closure_float", valargs)
                     : rop.ll.call_function("osl_mul_closure_color", valargs);
        rop.llvm_store_value(res, Result, 0, NULL, 0);
        return true;
    }

    // multiplication involving matrices
    if (Result.typespec().is_matrix()) {
        if (A.typespec().is_float()) {
            if (B.typespec().is_matrix())
                rop.llvm_call_function("osl_mul_mmf", Result, B, A);
            else
                OSL_ASSERT(0 && "frontend should not allow");
        } else if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function("osl_mul_mmf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function("osl_mul_mmm", Result, A, B);
            else
                OSL_ASSERT(0 && "frontend should not allow");
        } else
            OSL_ASSERT(0 && "frontend should not allow");
        if (Result.has_derivs())
            rop.llvm_zero_derivs(Result);
        return true;
    }

    // The following should handle f*f, v*v, v*f, f*v, i*i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value* a = rop.llvm_load_value(A, 0, i, type);
        llvm::Value* b = rop.llvm_load_value(B, 0, i, type);
        if (!a || !b)
            return false;
        llvm::Value* r = rop.ll.op_mul(a, b);
        rop.llvm_store_value(r, Result, 0, i);

        if (Result.has_derivs() && (A.has_derivs() || B.has_derivs())) {
            // Multiplication of duals: (a*b, a*b.dx + a.dx*b, a*b.dy + a.dy*b)
            OSL_DASSERT(is_float);
            llvm::Value* ax  = rop.llvm_load_value(A, 1, i, type);
            llvm::Value* bx  = rop.llvm_load_value(B, 1, i, type);
            llvm::Value* abx = rop.ll.op_mul(a, bx);
            llvm::Value* axb = rop.ll.op_mul(ax, b);
            llvm::Value* rx  = rop.ll.op_add(abx, axb);
            llvm::Value* ay  = rop.llvm_load_value(A, 2, i, type);
            llvm::Value* by  = rop.llvm_load_value(B, 2, i, type);
            llvm::Value* aby = rop.ll.op_mul(a, by);
            llvm::Value* ayb = rop.ll.op_mul(ay, b);
            llvm::Value* ry  = rop.ll.op_add(aby, ayb);
            rop.llvm_store_value(rx, Result, 1, i);
            rop.llvm_store_value(ry, Result, 2, i);
        }
    }

    if (Result.has_derivs() && !(A.has_derivs() || B.has_derivs())) {
        // Result has derivs, operands do not
        rop.llvm_zero_derivs(Result);
    }

    return true;
}



LLVMGEN(llvm_gen_div)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);

    TypeDesc type      = Result.typespec().simpletype();
    bool is_float      = Result.typespec().is_float_based();
    int num_components = type.aggregate;

    OSL_DASSERT(!Result.typespec().is_closure_based());

    // division involving matrices
    if (Result.typespec().is_matrix()) {
        if (A.typespec().is_float()) {
            OSL_ASSERT(!B.typespec().is_float() && "frontend should not allow");
            if (B.typespec().is_matrix())
                rop.llvm_call_function("osl_div_mfm", Result, A, B);
            else
                OSL_ASSERT(0);
        } else if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function("osl_div_mmf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function("osl_div_mmm", Result, A, B);
            else
                OSL_ASSERT(0);
        } else
            OSL_ASSERT(0);
        if (Result.has_derivs())
            rop.llvm_zero_derivs(Result);
        return true;
    }

    // The following should handle f/f, v/v, v/f, f/v, i/i
    // That's all that should be allowed by oslc.
    const char* safe_div = is_float ? "osl_safe_div_fff" : "osl_safe_div_iii";
    bool deriv = (Result.has_derivs() && (A.has_derivs() || B.has_derivs()));
    for (int i = 0; i < num_components; i++) {
        llvm::Value* a = rop.llvm_load_value(A, 0, i, type);
        llvm::Value* b = rop.llvm_load_value(B, 0, i, type);
        if (!a || !b)
            return false;
        llvm::Value* a_div_b;
        if (B.is_constant() && !rop.is_zero(B))
            a_div_b = rop.ll.op_div(a, b);
        else
            a_div_b = rop.ll.call_function(safe_div, a, b);
        llvm::Value *rx = NULL, *ry = NULL;

        if (deriv) {
            // Division of duals: (a/b, 1/b*(ax-a/b*bx), 1/b*(ay-a/b*by))
            OSL_DASSERT(is_float);
            llvm::Value* binv;
            if (B.is_constant() && !rop.is_zero(B))
                binv = rop.ll.op_div(rop.ll.constant(1.0f), b);
            else
                binv = rop.ll.call_function(safe_div, rop.ll.constant(1.0f), b);
            llvm::Value* ax             = rop.llvm_load_value(A, 1, i, type);
            llvm::Value* bx             = rop.llvm_load_value(B, 1, i, type);
            llvm::Value* a_div_b_mul_bx = rop.ll.op_mul(a_div_b, bx);
            llvm::Value* ax_minus_a_div_b_mul_bx
                = rop.ll.op_sub(ax, a_div_b_mul_bx);
            rx              = rop.ll.op_mul(binv, ax_minus_a_div_b_mul_bx);
            llvm::Value* ay = rop.llvm_load_value(A, 2, i, type);
            llvm::Value* by = rop.llvm_load_value(B, 2, i, type);
            llvm::Value* a_div_b_mul_by = rop.ll.op_mul(a_div_b, by);
            llvm::Value* ay_minus_a_div_b_mul_by
                = rop.ll.op_sub(ay, a_div_b_mul_by);
            ry = rop.ll.op_mul(binv, ay_minus_a_div_b_mul_by);
        }

        rop.llvm_store_value(a_div_b, Result, 0, i);
        if (deriv) {
            rop.llvm_store_value(rx, Result, 1, i);
            rop.llvm_store_value(ry, Result, 2, i);
        }
    }

    if (Result.has_derivs() && !(A.has_derivs() || B.has_derivs())) {
        // Result has derivs, operands do not
        rop.llvm_zero_derivs(Result);
    }

    return true;
}



LLVMGEN(llvm_gen_modulus)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);

    TypeDesc type      = Result.typespec().simpletype();
    bool is_float      = Result.typespec().is_float_based();
    int num_components = type.aggregate;

#ifdef OSL_LLVM_NO_BITCODE
    // On Windows 32 bit this calls an unknown instruction, probably need to
    // link with LLVM compiler-rt to fix, for now just fall back to op
    if (is_float)
        return llvm_gen_generic(rop, opnum);
#endif

    // The following should handle f%f, v%v, v%f, i%i
    // That's all that should be allowed by oslc.
    const char* safe_mod = is_float ? "osl_fmod_fff" : "osl_safe_mod_iii";
    for (int i = 0; i < num_components; i++) {
        llvm::Value* a = rop.loadLLVMValue(A, i, 0, type);
        llvm::Value* b = rop.loadLLVMValue(B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value* r;
        if (!rop.use_optix() && B.is_constant() && !rop.is_zero(B))
            r = rop.ll.op_mod(a, b);
        else
            r = rop.ll.call_function(safe_mod, a, b);
        rop.storeLLVMValue(r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        OSL_DASSERT(is_float);
        if (A.has_derivs()) {
            // Modulus of duals: (a mod b, ax, ay)
            for (int d = 1; d <= 2; ++d) {
                for (int i = 0; i < num_components; i++) {
                    llvm::Value* deriv = rop.loadLLVMValue(A, i, d, type);
                    rop.storeLLVMValue(deriv, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs(Result);
        }
    }
    return true;
}



LLVMGEN(llvm_gen_neg)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);

    TypeDesc type      = Result.typespec().simpletype();
    int num_components = type.aggregate;
    for (int d = 0; d < 3; ++d) {  // dx, dy
        for (int i = 0; i < num_components; i++) {
            llvm::Value* a = rop.llvm_load_value(A, d, i, type);
            llvm::Value* r = rop.ll.op_neg(a);
            rop.llvm_store_value(r, Result, d, i);
        }
        if (!Result.has_derivs())
            break;
    }
    return true;
}



// Implementation for clamp
LLVMGEN(llvm_gen_clamp)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& X      = *rop.opargsym(op, 1);
    Symbol& Min    = *rop.opargsym(op, 2);
    Symbol& Max    = *rop.opargsym(op, 3);

    TypeDesc type      = Result.typespec().simpletype();
    int num_components = type.aggregate;
    for (int i = 0; i < num_components; i++) {
        // First do the lower bound
        llvm::Value* val   = rop.llvm_load_value(X, 0, i, type);
        llvm::Value* min   = rop.llvm_load_value(Min, 0, i, type);
        llvm::Value* cond  = rop.ll.op_lt(val, min);
        val                = rop.ll.op_select(cond, min, val);
        llvm::Value *valdx = NULL, *valdy = NULL;
        if (Result.has_derivs()) {
            valdx              = rop.llvm_load_value(X, 1, i, type);
            valdy              = rop.llvm_load_value(X, 2, i, type);
            llvm::Value* mindx = rop.llvm_load_value(Min, 1, i, type);
            llvm::Value* mindy = rop.llvm_load_value(Min, 2, i, type);
            valdx              = rop.ll.op_select(cond, mindx, valdx);
            valdy              = rop.ll.op_select(cond, mindy, valdy);
        }
        // Now do the upper bound
        llvm::Value* max = rop.llvm_load_value(Max, 0, i, type);
        cond             = rop.ll.op_gt(val, max);
        val              = rop.ll.op_select(cond, max, val);
        if (Result.has_derivs()) {
            llvm::Value* maxdx = rop.llvm_load_value(Max, 1, i, type);
            llvm::Value* maxdy = rop.llvm_load_value(Max, 2, i, type);
            valdx              = rop.ll.op_select(cond, maxdx, valdx);
            valdy              = rop.ll.op_select(cond, maxdy, valdy);
        }
        rop.llvm_store_value(val, Result, 0, i);
        rop.llvm_store_value(valdx, Result, 1, i);
        rop.llvm_store_value(valdy, Result, 2, i);
    }
    return true;
}



LLVMGEN(llvm_gen_mix)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);
    Symbol& X      = *rop.opargsym(op, 3);
    TypeDesc type  = Result.typespec().simpletype();
    OSL_DASSERT(!Result.typespec().is_closure_based()
                && Result.typespec().is_float_based());
    int num_components = type.aggregate;
    int x_components   = X.typespec().aggregate();
    bool derivs        = (Result.has_derivs()
                   && (A.has_derivs() || B.has_derivs() || X.has_derivs()));

    llvm::Value* one         = rop.ll.constant(1.0f);
    llvm::Value* x           = rop.llvm_load_value(X, 0, 0, type);
    llvm::Value* one_minus_x = rop.ll.op_sub(one, x);
    llvm::Value* xx = derivs ? rop.llvm_load_value(X, 1, 0, type) : NULL;
    llvm::Value* xy = derivs ? rop.llvm_load_value(X, 2, 0, type) : NULL;
    for (int i = 0; i < num_components; i++) {
        llvm::Value* a = rop.llvm_load_value(A, 0, i, type);
        llvm::Value* b = rop.llvm_load_value(B, 0, i, type);
        if (!a || !b)
            return false;
        if (i > 0 && x_components > 1) {
            // Only need to recompute x and 1-x if they change
            x           = rop.llvm_load_value(X, 0, i, type);
            one_minus_x = rop.ll.op_sub(one, x);
        }
        // r = a*one_minus_x + b*x
        llvm::Value* r1 = rop.ll.op_mul(a, one_minus_x);
        llvm::Value* r2 = rop.ll.op_mul(b, x);
        llvm::Value* r  = rop.ll.op_add(r1, r2);
        rop.llvm_store_value(r, Result, 0, i);

        if (derivs) {
            // mix of duals:
            //   (a*one_minus_x + b*x,
            //    a*one_minus_x.dx + a.dx*one_minus_x + b*x.dx + b.dx*x,
            //    a*one_minus_x.dy + a.dy*one_minus_x + b*x.dy + b.dy*x)
            // and since one_minus_x.dx = -x.dx, one_minus_x.dy = -x.dy,
            //   (a*one_minus_x + b*x,
            //    -a*x.dx + a.dx*one_minus_x + b*x.dx + b.dx*x,
            //    -a*x.dy + a.dy*one_minus_x + b*x.dy + b.dy*x)
            llvm::Value* ax = rop.llvm_load_value(A, 1, i, type);
            llvm::Value* bx = rop.llvm_load_value(B, 1, i, type);
            if (i > 0 && x_components > 1)
                xx = rop.llvm_load_value(X, 1, i, type);
            llvm::Value* rx1 = rop.ll.op_mul(a, xx);
            llvm::Value* rx2 = rop.ll.op_mul(ax, one_minus_x);
            llvm::Value* rx  = rop.ll.op_sub(rx2, rx1);
            llvm::Value* rx3 = rop.ll.op_mul(b, xx);
            rx               = rop.ll.op_add(rx, rx3);
            llvm::Value* rx4 = rop.ll.op_mul(bx, x);
            rx               = rop.ll.op_add(rx, rx4);

            llvm::Value* ay = rop.llvm_load_value(A, 2, i, type);
            llvm::Value* by = rop.llvm_load_value(B, 2, i, type);
            if (i > 0 && x_components > 1)
                xy = rop.llvm_load_value(X, 2, i, type);
            llvm::Value* ry1 = rop.ll.op_mul(a, xy);
            llvm::Value* ry2 = rop.ll.op_mul(ay, one_minus_x);
            llvm::Value* ry  = rop.ll.op_sub(ry2, ry1);
            llvm::Value* ry3 = rop.ll.op_mul(b, xy);
            ry               = rop.ll.op_add(ry, ry3);
            llvm::Value* ry4 = rop.ll.op_mul(by, x);
            ry               = rop.ll.op_add(ry, ry4);

            rop.llvm_store_value(rx, Result, 1, i);
            rop.llvm_store_value(ry, Result, 2, i);
        }
    }

    if (Result.has_derivs() && !derivs) {
        // Result has derivs, operands do not
        rop.llvm_zero_derivs(Result);
    }

    return true;
}



LLVMGEN(llvm_gen_select)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);
    Symbol& X      = *rop.opargsym(op, 3);
    TypeDesc type  = Result.typespec().simpletype();
    OSL_DASSERT(!Result.typespec().is_closure_based()
                && Result.typespec().is_float_based());
    int num_components = type.aggregate;
    int x_components   = X.typespec().aggregate();
    OSL_DASSERT(x_components <= 3);
    bool derivs = (Result.has_derivs() && (A.has_derivs() || B.has_derivs()));

    llvm::Value* zero = X.typespec().is_int() ? rop.ll.constant(0)
                                              : rop.ll.constant(0.0f);
    llvm::Value* cond[3];
    for (int i = 0; i < x_components; ++i)
        cond[i] = rop.ll.op_ne(rop.llvm_load_value(X, 0, i), zero);

    for (int i = 0; i < num_components; i++) {
        llvm::Value* a = rop.llvm_load_value(A, 0, i, type);
        llvm::Value* b = rop.llvm_load_value(B, 0, i, type);
        llvm::Value* c = (i >= x_components) ? cond[0] : cond[i];
        llvm::Value* r = rop.ll.op_select(c, b, a);
        rop.llvm_store_value(r, Result, 0, i);
        if (derivs) {
            for (int d = 1; d < 3; ++d) {
                a = rop.llvm_load_value(A, d, i, type);
                b = rop.llvm_load_value(B, d, i, type);
                r = rop.ll.op_select(c, b, a);
                rop.llvm_store_value(r, Result, d, i);
            }
        }
    }

    if (Result.has_derivs() && !derivs) {
        // Result has derivs, operands do not
        rop.llvm_zero_derivs(Result);
    }
    return true;
}



// Implementation for min/max
LLVMGEN(llvm_gen_minmax)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& x      = *rop.opargsym(op, 1);
    Symbol& y      = *rop.opargsym(op, 2);

    TypeDesc type      = Result.typespec().simpletype();
    int num_components = type.aggregate;
    for (int i = 0; i < num_components; i++) {
        // First do the lower bound
        llvm::Value* x_val = rop.llvm_load_value(x, 0, i, type);
        llvm::Value* y_val = rop.llvm_load_value(y, 0, i, type);

        llvm::Value* cond = NULL;
        // NOTE(boulos): Using <= instead of < to match old behavior
        // (only matters for derivs)
        if (op.opname() == op_min) {
            cond = rop.ll.op_le(x_val, y_val);
        } else {
            cond = rop.ll.op_gt(x_val, y_val);
        }

        llvm::Value* res_val = rop.ll.op_select(cond, x_val, y_val);
        rop.llvm_store_value(res_val, Result, 0, i);
        if (Result.has_derivs()) {
            llvm::Value* x_dx = rop.llvm_load_value(x, 1, i, type);
            llvm::Value* x_dy = rop.llvm_load_value(x, 2, i, type);
            llvm::Value* y_dx = rop.llvm_load_value(y, 1, i, type);
            llvm::Value* y_dy = rop.llvm_load_value(y, 2, i, type);
            rop.llvm_store_value(rop.ll.op_select(cond, x_dx, y_dx), Result, 1,
                                 i);
            rop.llvm_store_value(rop.ll.op_select(cond, x_dy, y_dy), Result, 2,
                                 i);
        }
    }
    return true;
}



LLVMGEN(llvm_gen_bitwise_binary_op)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    Symbol& B      = *rop.opargsym(op, 2);
    OSL_DASSERT(Result.typespec().is_int() && A.typespec().is_int()
                && B.typespec().is_int());

    llvm::Value* a = rop.loadLLVMValue(A);
    llvm::Value* b = rop.loadLLVMValue(B);
    if (!a || !b)
        return false;
    llvm::Value* r = NULL;
    if (op.opname() == op_bitand)
        r = rop.ll.op_and(a, b);
    else if (op.opname() == op_bitor)
        r = rop.ll.op_or(a, b);
    else if (op.opname() == op_xor)
        r = rop.ll.op_xor(a, b);
    else if (op.opname() == op_shl)
        r = rop.ll.op_shl(a, b);
    else if (op.opname() == op_shr)
        r = rop.ll.op_shr(a, b);
    else
        return false;
    rop.storeLLVMValue(r, Result);
    return true;
}



// Simple (pointwise) unary ops (Abs, ...,
LLVMGEN(llvm_gen_unary_op)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& dst        = *rop.opargsym(op, 0);
    Symbol& src        = *rop.opargsym(op, 1);
    bool dst_derivs    = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_float_based();
    bool src_float = src.typespec().is_float_based();

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        llvm::Value* src_load = rop.loadLLVMValue(src, i, 0);
        if (!src_load)
            return false;

        llvm::Value* src_val = src_load;

        // Perform the op
        llvm::Value* result = 0;
        ustring opname      = op.opname();

        if (opname == op_compl) {
            OSL_DASSERT(dst.typespec().is_int());
            result = rop.ll.op_not(src_val);
        } else {
            // Don't know how to handle this.
            rop.shadingcontext()->errorfmt(
                "Don't know how to handle op '{}', eliding the store\n",
                opname);
        }

        // Store the result
        if (result) {
            // if our op type doesn't match result, convert
            if (dst_float && !src_float) {
                // Op was int, but we need to store float
                result = rop.ll.op_int_to_float(result);
            } else if (!dst_float && src_float) {
                // Op was float, but we need to store int
                result = rop.ll.op_float_to_int(result);
            }  // otherwise just fine
            rop.storeLLVMValue(result, dst, i, 0);
        }

        if (dst_derivs) {
            // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
            rop.shadingcontext()->infofmt("punting on derivatives for now\n");
            // FIXME!!
        }
    }
    return true;
}



// Simple assignment
LLVMGEN(llvm_gen_assign)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& Src(*rop.opargsym(op, 1));

    return rop.llvm_assign_impl(Result, Src);
}



// Entire array copying
LLVMGEN(llvm_gen_arraycopy)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& Src(*rop.opargsym(op, 1));

    return rop.llvm_assign_impl(Result, Src);
}



// Vector component reference
LLVMGEN(llvm_gen_compref)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& Val    = *rop.opargsym(op, 1);
    Symbol& Index  = *rop.opargsym(op, 2);

    llvm::Value* c = rop.llvm_load_value(Index);
    if (rop.inst()->master()->range_checking()) {
        if (!(Index.is_constant() && Index.get_int() >= 0
              && Index.get_int() < 3)) {
            llvm::Value* args[]
                = { c,
                    rop.ll.constant(3),
                    rop.llvm_const_hash(Val.unmangled()),
                    rop.sg_void_ptr(),
                    rop.llvm_const_hash(op.sourcefile()),
                    rop.ll.constant(op.sourceline()),
                    rop.llvm_const_hash(rop.group().name()),
                    rop.ll.constant(rop.layer()),
                    rop.llvm_const_hash(rop.inst()->layername()),
                    rop.llvm_const_hash(rop.inst()->shadername()) };
            c = rop.ll.call_function("osl_range_check", args);
        }
    }

    for (int d = 0; d < 3; ++d) {  // deriv
        llvm::Value* val = NULL;
        if (Index.is_constant()) {
            int i = Index.get_int();
            i     = Imath::clamp(i, 0, 2);
            val   = rop.llvm_load_value(Val, d, i);
        } else {
            val = rop.llvm_load_component_value(Val, d, c);
        }
        rop.llvm_store_value(val, Result, d);
        if (!Result.has_derivs())  // skip the derivs if we don't need them
            break;
    }
    return true;
}



// Vector component assignment
LLVMGEN(llvm_gen_compassign)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& Index  = *rop.opargsym(op, 1);
    Symbol& Val    = *rop.opargsym(op, 2);

    llvm::Value* c = rop.llvm_load_value(Index);
    if (rop.inst()->master()->range_checking()) {
        if (!(Index.is_constant() && Index.get_int() >= 0
              && Index.get_int() < 3)) {
            llvm::Value* args[]
                = { c,
                    rop.ll.constant(3),
                    rop.llvm_const_hash(Result.unmangled()),
                    rop.sg_void_ptr(),
                    rop.llvm_const_hash(op.sourcefile()),
                    rop.ll.constant(op.sourceline()),
                    rop.llvm_const_hash(rop.group().name()),
                    rop.ll.constant(rop.layer()),
                    rop.llvm_const_hash(rop.inst()->layername()),
                    rop.llvm_const_hash(rop.inst()->shadername()) };
            c = rop.ll.call_function("osl_range_check", args);
        }
    }

    for (int d = 0; d < 3; ++d) {  // deriv
        llvm::Value* val = rop.llvm_load_value(Val, d, 0, TypeFloat);
        if (Index.is_constant()) {
            int i = Index.get_int();
            i     = Imath::clamp(i, 0, 2);
            rop.llvm_store_value(val, Result, d, i);
        } else {
            rop.llvm_store_component_value(val, Result, d, c);
        }
        if (!Result.has_derivs())  // skip the derivs if we don't need them
            break;
    }
    return true;
}



// Matrix component reference
LLVMGEN(llvm_gen_mxcompref)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& M      = *rop.opargsym(op, 1);
    Symbol& Row    = *rop.opargsym(op, 2);
    Symbol& Col    = *rop.opargsym(op, 3);

    llvm::Value* row = rop.llvm_load_value(Row);
    llvm::Value* col = rop.llvm_load_value(Col);
    if (rop.inst()->master()->range_checking()) {
        if (!(Row.is_constant() && Col.is_constant() && Row.get_int() >= 0
              && Row.get_int() < 4 && Col.get_int() >= 0
              && Col.get_int() < 4)) {
            llvm::Value* args[]
                = { row,
                    rop.ll.constant(4),
                    rop.llvm_const_hash(M.name()),
                    rop.sg_void_ptr(),
                    rop.llvm_const_hash(op.sourcefile()),
                    rop.ll.constant(op.sourceline()),
                    rop.llvm_const_hash(rop.group().name()),
                    rop.ll.constant(rop.layer()),
                    rop.llvm_const_hash(rop.inst()->layername()),
                    rop.llvm_const_hash(rop.inst()->shadername()) };
            if (!(Row.is_constant() && Row.get_int() >= 0
                  && Row.get_int() < 4)) {
                row = rop.ll.call_function("osl_range_check", args);
            }
            if (!(Col.is_constant() && Col.get_int() >= 0
                  && Col.get_int() < 4)) {
                args[0] = col;
                col     = rop.ll.call_function("osl_range_check", args);
            }
        }
    }

    llvm::Value* val = NULL;
    if (Row.is_constant() && Col.is_constant()) {
        int r    = Imath::clamp(Row.get_int(), 0, 3);
        int c    = Imath::clamp(Col.get_int(), 0, 3);
        int comp = 4 * r + c;
        val      = rop.llvm_load_value(M, 0, comp);
    } else {
        llvm::Value* comp = rop.ll.op_mul(row, rop.ll.constant(4));
        comp              = rop.ll.op_add(comp, col);
        val               = rop.llvm_load_component_value(M, 0, comp);
    }
    rop.llvm_store_value(val, Result);
    rop.llvm_zero_derivs(Result);

    return true;
}



// Matrix component assignment
LLVMGEN(llvm_gen_mxcompassign)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& Row    = *rop.opargsym(op, 1);
    Symbol& Col    = *rop.opargsym(op, 2);
    Symbol& Val    = *rop.opargsym(op, 3);

    llvm::Value* row = rop.llvm_load_value(Row);
    llvm::Value* col = rop.llvm_load_value(Col);
    if (rop.inst()->master()->range_checking()) {
        if (!(Row.is_constant() && Col.is_constant() && Row.get_int() >= 0
              && Row.get_int() < 4 && Col.get_int() >= 0
              && Col.get_int() < 4)) {
            llvm::Value* args[]
                = { row,
                    rop.ll.constant(4),
                    rop.llvm_const_hash(Result.name()),
                    rop.sg_void_ptr(),
                    rop.llvm_const_hash(op.sourcefile()),
                    rop.ll.constant(op.sourceline()),
                    rop.llvm_const_hash(rop.group().name()),
                    rop.ll.constant(rop.layer()),
                    rop.llvm_const_hash(rop.inst()->layername()),
                    rop.llvm_const_hash(rop.inst()->shadername()) };
            if (!(Row.is_constant() && Row.get_int() >= 0
                  && Row.get_int() < 4)) {
                row = rop.ll.call_function("osl_range_check", args);
            }
            if (!(Col.is_constant() && Col.get_int() >= 0
                  && Col.get_int() < 4)) {
                args[0] = col;
                col     = rop.ll.call_function("osl_range_check", args);
            }
        }
    }

    llvm::Value* val = rop.llvm_load_value(Val, 0, 0, TypeFloat);

    if (Row.is_constant() && Col.is_constant()) {
        int r    = Imath::clamp(Row.get_int(), 0, 3);
        int c    = Imath::clamp(Col.get_int(), 0, 3);
        int comp = 4 * r + c;
        rop.llvm_store_value(val, Result, 0, comp);
    } else {
        llvm::Value* comp = rop.ll.op_mul(row, rop.ll.constant(4));
        comp              = rop.ll.op_add(comp, col);
        rop.llvm_store_component_value(val, Result, 0, comp);
    }
    return true;
}



// Array length
LLVMGEN(llvm_gen_arraylength)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& A      = *rop.opargsym(op, 1);
    OSL_DASSERT(Result.typespec().is_int() && A.typespec().is_array());

    int len = A.typespec().is_unsized_array() ? A.initializers()
                                              : A.typespec().arraylength();
    rop.llvm_store_value(rop.ll.constant(len), Result);
    return true;
}



// Array reference
LLVMGEN(llvm_gen_aref)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& Src    = *rop.opargsym(op, 1);
    Symbol& Index  = *rop.opargsym(op, 2);

    // Get array index we're interested in
    llvm::Value* index = rop.loadLLVMValue(Index);
    if (!index)
        return false;
    if (rop.inst()->master()->range_checking()) {
        if (!(Index.is_constant() && Index.get_int() >= 0
              && Index.get_int() < Src.typespec().arraylength())) {
            llvm::Value* args[]
                = { index,
                    rop.ll.constant(Src.typespec().arraylength()),
                    rop.llvm_const_hash(Src.unmangled()),
                    rop.sg_void_ptr(),
                    rop.llvm_const_hash(op.sourcefile()),
                    rop.ll.constant(op.sourceline()),
                    rop.llvm_const_hash(rop.group().name()),
                    rop.ll.constant(rop.layer()),
                    rop.llvm_const_hash(rop.inst()->layername()),
                    rop.llvm_const_hash(rop.inst()->shadername()) };
            index = rop.ll.call_function("osl_range_check", args);
        }
    }

    int num_components = Src.typespec().simpletype().aggregate;
    for (int d = 0; d <= 2; ++d) {
        for (int c = 0; c < num_components; ++c) {
            llvm::Value* val = rop.llvm_load_value(Src, d, index, c);
            rop.storeLLVMValue(val, Result, c, d);
        }
        if (!Result.has_derivs())
            break;
    }

    return true;
}



// Array assignment
LLVMGEN(llvm_gen_aassign)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& Index  = *rop.opargsym(op, 1);
    Symbol& Src    = *rop.opargsym(op, 2);

    // Get array index we're interested in
    llvm::Value* index = rop.loadLLVMValue(Index);
    if (!index)
        return false;
    if (rop.inst()->master()->range_checking()) {
        if (!(Index.is_constant() && Index.get_int() >= 0
              && Index.get_int() < Result.typespec().arraylength())) {
            llvm::Value* args[]
                = { index,
                    rop.ll.constant(Result.typespec().arraylength()),
                    rop.llvm_const_hash(Result.unmangled()),
                    rop.sg_void_ptr(),
                    rop.llvm_const_hash(op.sourcefile()),
                    rop.ll.constant(op.sourceline()),
                    rop.llvm_const_hash(rop.group().name()),
                    rop.ll.constant(rop.layer()),
                    rop.llvm_const_hash(rop.inst()->layername()),
                    rop.llvm_const_hash(rop.inst()->shadername()) };
            index = rop.ll.call_function("osl_range_check", args);
        }
    }

    int num_components = Result.typespec().simpletype().aggregate;

    // Allow float <=> int casting
    TypeDesc cast;
    if (num_components == 1 && !Result.typespec().is_closure()
        && !Src.typespec().is_closure()
        && (Result.typespec().is_int_based()
            || Result.typespec().is_float_based())
        && (Src.typespec().is_int_based() || Src.typespec().is_float_based())) {
        cast          = Result.typespec().simpletype();
        cast.arraylen = 0;
    } else {
        // Try to warn before llvm_fatal_error is called which provides little
        // context as to what went wrong.
        OSL_ASSERT(Result.typespec().simpletype().basetype
                   == Src.typespec().simpletype().basetype);
    }

    for (int d = 0; d <= 2; ++d) {
        for (int c = 0; c < num_components; ++c) {
            llvm::Value* val = rop.loadLLVMValue(Src, c, d, cast);
            rop.llvm_store_value(val, Result, d, index, c);
        }
        if (!Result.has_derivs())
            break;
    }

    return true;
}



// Construct color, optionally with a color transformation from a named
// color space.
LLVMGEN(llvm_gen_construct_color)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result             = *rop.opargsym(op, 0);
    bool using_space           = (op.nargs() == 5);
    Symbol& Space              = *rop.opargsym(op, 1);
    OSL_MAYBE_UNUSED Symbol& X = *rop.opargsym(op, 1 + using_space);
    OSL_MAYBE_UNUSED Symbol& Y = *rop.opargsym(op, 2 + using_space);
    OSL_MAYBE_UNUSED Symbol& Z = *rop.opargsym(op, 3 + using_space);
    OSL_DASSERT(Result.typespec().is_triple() && X.typespec().is_float()
                && Y.typespec().is_float() && Z.typespec().is_float()
                && (using_space == false || Space.typespec().is_string()));

    // First, copy the floats into the vector
    int dmax = Result.has_derivs() ? 3 : 1;
    for (int d = 0; d < dmax; ++d) {   // loop over derivs
        for (int c = 0; c < 3; ++c) {  // loop over components
            const Symbol& comp = *rop.opargsym(op, c + 1 + using_space);
            llvm::Value* val = rop.llvm_load_value(comp, d, NULL, 0, TypeFloat);
            rop.llvm_store_value(val, Result, d, NULL, c);
        }
    }

    // Do the color space conversion in-place, if called for
    if (using_space) {
        llvm::Value* args[] = {
            rop.sg_void_ptr(),             // shader globals
            rop.llvm_void_ptr(Result, 0),  // color
            rop.llvm_load_value(Space),    // from
        };
        rop.ll.call_function("osl_prepend_color_from", args);
        // FIXME(deriv): Punt on derivs for color ctrs with space names.
        // We should try to do this right, but we never had it right for
        // the interpreter, to it's probably not an emergency.
        if (Result.has_derivs())
            rop.llvm_zero_derivs(Result);
    }

    return true;
}



// Construct spatial triple (point, vector, normal), optionally with a
// transformation from a named coordinate system.
LLVMGEN(llvm_gen_construct_triple)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result             = *rop.opargsym(op, 0);
    bool using_space           = (op.nargs() == 5);
    Symbol& Space              = *rop.opargsym(op, 1);
    OSL_MAYBE_UNUSED Symbol& X = *rop.opargsym(op, 1 + using_space);
    OSL_MAYBE_UNUSED Symbol& Y = *rop.opargsym(op, 2 + using_space);
    OSL_MAYBE_UNUSED Symbol& Z = *rop.opargsym(op, 3 + using_space);
    OSL_DASSERT(Result.typespec().is_triple() && X.typespec().is_float()
                && Y.typespec().is_float() && Z.typespec().is_float()
                && (using_space == false || Space.typespec().is_string()));

    // First, copy the floats into the vector
    int dmax = Result.has_derivs() ? 3 : 1;
    for (int d = 0; d < dmax; ++d) {   // loop over derivs
        for (int c = 0; c < 3; ++c) {  // loop over components
            const Symbol& comp = *rop.opargsym(op, c + 1 + using_space);
            llvm::Value* val = rop.llvm_load_value(comp, d, NULL, 0, TypeFloat);
            rop.llvm_store_value(val, Result, d, NULL, c);
        }
    }

    // Do the transformation in-place, if called for
    if (using_space) {
        ustring from, to;  // N.B. initialize to empty strings
        if (Space.is_constant()) {
            from = Space.get_string();
            if (from == Strings::common
                || from == rop.shadingsys().commonspace_synonym())
                return true;  // no transformation necessary
        }
        TypeDesc::VECSEMANTICS vectype = TypeDesc::POINT;
        if (op.opname() == "vector")
            vectype = TypeDesc::VECTOR;
        else if (op.opname() == "normal")
            vectype = TypeDesc::NORMAL;

        llvm::Value* from_arg = rop.llvm_load_value(Space);
        llvm::Value* to_arg   = rop.llvm_const_hash(Strings::common);

        llvm::Value* args[] = { rop.sg_void_ptr(),
                                rop.llvm_void_ptr(Result),
                                rop.ll.constant(Result.has_derivs()),
                                rop.llvm_void_ptr(Result),
                                rop.ll.constant(Result.has_derivs()),
                                from_arg,
                                to_arg,
                                rop.ll.constant((int)vectype) };
        RendererServices* rend(rop.shadingsys().renderer());
        if (rend->transform_points(NULL, from, to, 0.0f, NULL, NULL, 0,
                                   vectype)) {
            // renderer potentially knows about a nonlinear transformation.
            // Note that for the case of non-constant strings, passing empty
            // from & to will make transform_points just tell us if ANY
            // nonlinear transformations potentially are supported.
            rop.ll.call_function("osl_transform_triple_nonlinear", args);
        } else {
            // definitely not a nonlinear transformation
            rop.ll.call_function("osl_transform_triple", args);
        }
    }

    return true;
}



/// matrix constructor.  Comes in several varieties:
///    matrix (float)
///    matrix (space, float)
///    matrix (...16 floats...)
///    matrix (space, ...16 floats...)
///    matrix (fromspace, tospace)
LLVMGEN(llvm_gen_matrix)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result        = *rop.opargsym(op, 0);
    int nargs             = op.nargs();
    bool using_space      = (nargs == 3 || nargs == 18);
    bool using_two_spaces = (nargs == 3
                             && rop.opargsym(op, 2)->typespec().is_string());
    int nfloats           = nargs - 1 - (int)using_space;
    OSL_DASSERT(nargs == 2 || nargs == 3 || nargs == 17 || nargs == 18);

    if (using_two_spaces) {
        llvm::Value* args[] = {
            rop.sg_void_ptr(),                          // shader globals
            rop.llvm_void_ptr(Result),                  // result
            rop.llvm_load_value(*rop.opargsym(op, 1)),  // from
            rop.llvm_load_value(*rop.opargsym(op, 2)),  // to
        };
        rop.ll.call_function("osl_get_from_to_matrix", args);
    } else {
        if (nfloats == 1) {
            for (int i = 0; i < 16; i++) {
                llvm::Value* src_val
                    = ((i % 4) == (i / 4)) ? rop.llvm_load_value(
                          *rop.opargsym(op, 1 + using_space))
                                           : rop.ll.constant(0.0f);
                rop.llvm_store_value(src_val, Result, 0, i);
            }
        } else if (nfloats == 16) {
            for (int i = 0; i < 16; i++) {
                llvm::Value* src_val = rop.llvm_load_value(
                    *rop.opargsym(op, i + 1 + using_space));
                rop.llvm_store_value(src_val, Result, 0, i);
            }
        } else {
            OSL_ASSERT(0);
        }
        if (using_space) {
            llvm::Value* args[] = {
                rop.sg_void_ptr(),                          // shader globals
                rop.llvm_void_ptr(Result),                  // result
                rop.llvm_load_value(*rop.opargsym(op, 1)),  // from
            };
            rop.ll.call_function("osl_prepend_matrix_from", args);
        }
    }
    if (Result.has_derivs())
        rop.llvm_zero_derivs(Result);
    return true;
}



/// int getmatrix (fromspace, tospace, M)
LLVMGEN(llvm_gen_getmatrix)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 4);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& From   = *rop.opargsym(op, 1);
    Symbol& To     = *rop.opargsym(op, 2);
    Symbol& M      = *rop.opargsym(op, 3);

    llvm::Value* args[] = {
        rop.sg_void_ptr(),     // shader globals
        rop.llvm_void_ptr(M),  // matrix result
        rop.llvm_load_value(From),
        rop.llvm_load_value(To),
    };
    llvm::Value* result = rop.ll.call_function("osl_get_from_to_matrix", args);
    rop.llvm_store_value(result, Result);
    rop.llvm_zero_derivs(M);
    return true;
}



// transform{,v,n} (string tospace, triple p)
// transform{,v,n} (string fromspace, string tospace, triple p)
// transform{,v,n} (matrix, triple p)
LLVMGEN(llvm_gen_transform)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    int nargs      = op.nargs();
    Symbol* Result = rop.opargsym(op, 0);
    Symbol* From   = (nargs == 3) ? NULL : rop.opargsym(op, 1);
    Symbol* To     = rop.opargsym(op, (nargs == 3) ? 1 : 2);
    Symbol* P      = rop.opargsym(op, (nargs == 3) ? 2 : 3);

    if (To->typespec().is_matrix()) {
        // llvm_ops has the matrix version already implemented
        llvm_gen_generic(rop, opnum);
        return true;
    }

    // Named space versions from here on out.
    ustring from, to;  // N.B.: initialize to empty strings
    if ((From == NULL || From->is_constant()) && To->is_constant()) {
        // We can know all the space names at this time
        from        = From ? From->get_string() : Strings::common;
        to          = To->get_string();
        ustring syn = rop.shadingsys().commonspace_synonym();
        if (from == syn)
            from = Strings::common;
        if (to == syn)
            to = Strings::common;
        if (from == to) {
            // An identity transformation, just copy
            if (Result != P)  // don't bother in-place copy
                rop.llvm_assign_impl(*Result, *P);
            return true;
        }
    }
    TypeDesc::VECSEMANTICS vectype = TypeDesc::POINT;
    if (op.opname() == "transformv")
        vectype = TypeDesc::VECTOR;
    else if (op.opname() == "transformn")
        vectype = TypeDesc::NORMAL;
    llvm::Value* args[] = { rop.sg_void_ptr(),
                            rop.llvm_void_ptr(*P),
                            rop.ll.constant(P->has_derivs()),
                            rop.llvm_void_ptr(*Result),
                            rop.ll.constant(Result->has_derivs()),
                            rop.llvm_load_value(*From),
                            rop.llvm_load_value(*To),
                            rop.ll.constant((int)vectype) };
    RendererServices* rend(rop.shadingsys().renderer());
    if (rend->transform_points(NULL, from, to, 0.0f, NULL, NULL, 0, vectype)) {
        // renderer potentially knows about a nonlinear transformation.
        // Note that for the case of non-constant strings, passing empty
        // from & to will make transform_points just tell us if ANY
        // nonlinear transformations potentially are supported.
        rop.ll.call_function("osl_transform_triple_nonlinear", args);
    } else {
        // definitely not a nonlinear transformation
        rop.ll.call_function("osl_transform_triple", args);
    }
    return true;
}



// transformc (string fromspace, string tospace, color p)
LLVMGEN(llvm_gen_transformc)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 4);
    Symbol* Result = rop.opargsym(op, 0);
    Symbol& From   = *rop.opargsym(op, 1);
    Symbol& To     = *rop.opargsym(op, 2);
    Symbol* C      = rop.opargsym(op, 3);

    llvm::Value* args[] = { rop.sg_void_ptr(),
                            rop.llvm_void_ptr(*C),
                            rop.ll.constant(C->has_derivs()),
                            rop.llvm_void_ptr(*Result),
                            rop.ll.constant(Result->has_derivs()),
                            rop.llvm_load_value(From),
                            rop.llvm_load_value(To) };

    rop.ll.call_function("osl_transformc", args);
    return true;
}



// Derivs
LLVMGEN(llvm_gen_DxDy)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& Src(*rop.opargsym(op, 1));
    int deriv = (op.opname() == "Dx") ? 1 : 2;

    for (int i = 0; i < Result.typespec().aggregate(); ++i) {
        llvm::Value* src_val = rop.llvm_load_value(Src, deriv, i);
        rop.storeLLVMValue(src_val, Result, i, 0);
    }

    // Don't have 2nd order derivs
    rop.llvm_zero_derivs(Result);
    return true;
}



// Dz
LLVMGEN(llvm_gen_Dz)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& Src(*rop.opargsym(op, 1));

    if (&Src == rop.inst()->symbol(rop.inst()->Psym())) {
        // dPdz -- the only Dz we know how to take
        int deriv = 3;
        for (int i = 0; i < Result.typespec().aggregate(); ++i) {
            llvm::Value* src_val = rop.llvm_load_value(Src, deriv, i);
            rop.storeLLVMValue(src_val, Result, i, 0);
        }
        // Don't have 2nd order derivs
        rop.llvm_zero_derivs(Result);
    } else {
        // Punt, everything else for now returns 0 for Dz
        // FIXME?
        rop.llvm_assign_zero(Result);
    }
    return true;
}



LLVMGEN(llvm_gen_filterwidth)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& Src(*rop.opargsym(op, 1));

    OSL_DASSERT(Src.typespec().is_float() || Src.typespec().is_triple());
    if (Src.has_derivs()) {
        if (Src.typespec().is_float()) {
            llvm::Value* r = rop.ll.call_function("osl_filterwidth_fdf",
                                                  rop.llvm_void_ptr(Src));
            rop.llvm_store_value(r, Result);
        } else {
            rop.ll.call_function("osl_filterwidth_vdv",
                                 rop.llvm_void_ptr(Result),
                                 rop.llvm_void_ptr(Src));
        }
        // Don't have 2nd order derivs
        rop.llvm_zero_derivs(Result);
    } else {
        // No derivs to be had
        rop.llvm_assign_zero(Result);
    }

    return true;
}



// Comparison ops
LLVMGEN(llvm_gen_compare_op)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& A(*rop.opargsym(op, 1));
    Symbol& B(*rop.opargsym(op, 2));
    OSL_DASSERT(Result.typespec().is_int() && !Result.has_derivs());

    if (A.typespec().is_closure()) {
        OSL_ASSERT(B.typespec().is_int()
                   && "Only closure==0 and closure!=0 allowed");
        llvm::Value* a = rop.llvm_load_value(A);
        llvm::Value* b = rop.ll.void_ptr_null();
        llvm::Value* r = (op.opname() == op_eq) ? rop.ll.op_eq(a, b)
                                                : rop.ll.op_ne(a, b);
        // Convert the single bit bool into an int
        r = rop.ll.op_bool_to_int(r);
        rop.llvm_store_value(r, Result);
        return true;
    }

    int num_components = std::max(A.typespec().aggregate(),
                                  B.typespec().aggregate());
    bool float_based   = A.typespec().is_float_based()
                       || B.typespec().is_float_based();
    TypeDesc cast(float_based ? TypeDesc::FLOAT : TypeDesc::UNKNOWN);

    llvm::Value* final_result = 0;
    ustring opname            = op.opname();

    for (int i = 0; i < num_components; i++) {
        // Get A&B component i -- note that these correctly handle mixed
        // scalar/triple comparisons as well as int->float casts as needed.
        llvm::Value* a = rop.loadLLVMValue(A, i, 0, cast);
        llvm::Value* b = rop.loadLLVMValue(B, i, 0, cast);

        // Trickery for mixed matrix/scalar comparisons -- compare
        // on-diagonal to the scalar, off-diagonal to zero
        if (A.typespec().is_matrix() && !B.typespec().is_matrix()) {
            if ((i / 4) != (i % 4))
                b = rop.ll.constant(0.0f);
        }
        if (!A.typespec().is_matrix() && B.typespec().is_matrix()) {
            if ((i / 4) != (i % 4))
                a = rop.ll.constant(0.0f);
        }

        // Perform the op
        llvm::Value* result = 0;
        if (opname == op_lt) {
            result = rop.ll.op_lt(a, b);
        } else if (opname == op_le) {
            result = rop.ll.op_le(a, b);
        } else if (opname == op_eq) {
            result = rop.ll.op_eq(a, b);
        } else if (opname == op_ge) {
            result = rop.ll.op_ge(a, b);
        } else if (opname == op_gt) {
            result = rop.ll.op_gt(a, b);
        } else if (opname == op_neq) {
            result = rop.ll.op_ne(a, b);
        } else {
            // Don't know how to handle this.
            OSL_ASSERT(0 && "Comparison error");
        }
        OSL_DASSERT(result);

        if (final_result) {
            // Combine the component bool based on the op
            if (opname != op_neq)  // final_result &= result
                final_result = rop.ll.op_and(final_result, result);
            else  // final_result |= result
                final_result = rop.ll.op_or(final_result, result);
        } else {
            final_result = result;
        }
    }
    OSL_ASSERT(final_result);

    // Convert the single bit bool into an int for now.
    final_result = rop.ll.op_bool_to_int(final_result);
    rop.storeLLVMValue(final_result, Result, 0, 0);
    return true;
}



// int regex_search (string subject, string pattern)
// int regex_search (string subject, int results[], string pattern)
// int regex_match (string subject, string pattern)
// int regex_match (string subject, int results[], string pattern)
LLVMGEN(llvm_gen_regex)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    int nargs = op.nargs();
    OSL_DASSERT(nargs == 3 || nargs == 4);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& Subject(*rop.opargsym(op, 1));
    bool do_match_results = (nargs == 4);
    bool fullmatch        = (op.opname() == "regex_match");
    Symbol& Match(*rop.opargsym(op, 2));
    Symbol& Pattern(*rop.opargsym(op, 2 + do_match_results));
    OSL_DASSERT(Result.typespec().is_int() && Subject.typespec().is_string()
                && Pattern.typespec().is_string());
    OSL_DASSERT(!do_match_results
                || (Match.typespec().is_array()
                    && Match.typespec().elementtype().is_int()));

    llvm::Value* call_args[] = {
        rop.sg_void_ptr(),             // First arg is ShaderGlobals ptr
        rop.llvm_load_value(Subject),  // Next arg is subject string
        rop.llvm_void_ptr(
            Match),  // Pass the results array and length (just pass 0 if no results wanted).
        do_match_results ? rop.ll.constant(Match.typespec().arraylength())
                         : rop.ll.constant(0),
        rop.llvm_load_value(Pattern),  // Pass the regex match pattern
        rop.ll.constant(fullmatch),  // Pass whether or not to do the full match
    };
    llvm::Value* ret = rop.ll.call_function("osl_regex_impl", call_args);
    rop.llvm_store_value(ret, Result);
    return true;
}



// Generic llvm code generation.  See the comments in llvm_ops.cpp for
// the full list of assumptions and conventions.  But in short:
//   1. All polymorphic and derivative cases implemented as functions in
//      llvm_ops.cpp -- no custom IR is needed.
//   2. Naming conention is: osl_NAME_{args}, where args is the
//      concatenation of type codes for all args including return value --
//      f/i/v/m/s for float/int/triple/matrix/string, and df/dv/dm for
//      duals.
//   3. The function returns scalars as an actual return value (that
//      must be stored), but "returns" aggregates or duals in the first
//      argument.
//   4. Duals and aggregates are passed as void*'s, float/int/string
//      passed by value.
//   5. Note that this only works if triples are all treated identically,
//      this routine can't be used if it must be polymorphic based on
//      color, point, vector, normal differences.
//
LLVMGEN(llvm_gen_generic)
{
    // most invocations of this function will only need a handful of args
    // so avoid dynamic allocation where possible
    constexpr int SHORT_NUM_ARGS = 16;
    const Symbol* short_args[SHORT_NUM_ARGS];
    std::vector<const Symbol*> long_args;
    Opcode& op(rop.inst()->ops()[opnum]);
    const Symbol** args = short_args;
    if (op.nargs() > SHORT_NUM_ARGS) {
        long_args.resize(op.nargs());
        args = long_args.data();
    }
    Symbol& Result      = *rop.opargsym(op, 0);
    bool any_deriv_args = false;
    for (int i = 0; i < op.nargs(); ++i) {
        Symbol* s(rop.opargsym(op, i));
        args[i] = s;
        any_deriv_args |= (i > 0 && s->has_derivs()
                           && !s->typespec().is_matrix());
    }

    // Special cases: functions that have no derivs -- suppress them
    if (any_deriv_args)
        if (op.opname() == op_logb || op.opname() == op_floor
            || op.opname() == op_ceil || op.opname() == op_round
            || op.opname() == op_step || op.opname() == op_trunc
            || op.opname() == op_sign)
            any_deriv_args = false;

    std::string name = std::string("osl_") + op.opname().string() + "_";
    for (int i = 0; i < op.nargs(); ++i) {
        Symbol* s(rop.opargsym(op, i));
        if (any_deriv_args && Result.has_derivs() && s->has_derivs()
            && !s->typespec().is_matrix())
            name += "d";
        if (s->typespec().is_float())
            name += "f";
        else if (s->typespec().is_triple())
            name += "v";
        else if (s->typespec().is_matrix())
            name += "m";
        else if (s->typespec().is_string())
            name += "s";
        else if (s->typespec().is_int())
            name += "i";
        else
            OSL_ASSERT(0);
    }

    if (!Result.has_derivs() || !any_deriv_args) {
        // Don't compute derivs -- either not needed or not provided in args
        if (Result.typespec().aggregate() == TypeDesc::SCALAR) {
            llvm::Value* r = rop.llvm_call_function(
                name.c_str(), cspan<const Symbol*>(args + 1, op.nargs() - 1));
            rop.llvm_store_value(r, Result);
        } else {
            rop.llvm_call_function(name.c_str(),
                                   cspan<const Symbol*>(args, op.nargs()));
        }
        rop.llvm_zero_derivs(Result);
    } else {
        // Cases with derivs
        OSL_ASSERT(Result.has_derivs() && any_deriv_args);
        rop.llvm_call_function(name.c_str(),
                               cspan<const Symbol*>(args, op.nargs()), true);
    }
    return true;
}



LLVMGEN(llvm_gen_sincos)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Theta      = *rop.opargsym(op, 0);
    Symbol& Sin_out    = *rop.opargsym(op, 1);
    Symbol& Cos_out    = *rop.opargsym(op, 2);
    bool theta_deriv   = Theta.has_derivs();
    bool result_derivs = (Sin_out.has_derivs() || Cos_out.has_derivs());

    std::string name = std::string("osl_sincos_");
    for (int i = 0; i < op.nargs(); ++i) {
        Symbol* s(rop.opargsym(op, i));
        if (s->has_derivs() && result_derivs && theta_deriv)
            name += "d";
        if (s->typespec().is_float())
            name += "f";
        else if (s->typespec().is_triple())
            name += "v";
        else
            OSL_ASSERT(0);
    }
    // push back llvm arguments
    llvm::Value* valargs[]
        = { (theta_deriv && result_derivs) || Theta.typespec().is_triple()
                ? rop.llvm_void_ptr(Theta)
                : rop.llvm_load_value(Theta),
            rop.llvm_void_ptr(Sin_out), rop.llvm_void_ptr(Cos_out) };
    rop.ll.call_function(name.c_str(), valargs);

    // If the input angle didn't have derivatives, we would not have
    // called the version of sincos with derivs; however in that case we
    // need to clear the derivs of either of the outputs that has them.
    if (Sin_out.has_derivs() && !theta_deriv)
        rop.llvm_zero_derivs(Sin_out);
    if (Cos_out.has_derivs() && !theta_deriv)
        rop.llvm_zero_derivs(Cos_out);

    return true;
}



LLVMGEN(llvm_gen_andor)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& result = *rop.opargsym(op, 0);
    Symbol& a      = *rop.opargsym(op, 1);
    Symbol& b      = *rop.opargsym(op, 2);

    llvm::Value* i1_res = NULL;
    llvm::Value* a_val  = rop.llvm_load_value(a, 0, 0, TypeInt);
    llvm::Value* b_val  = rop.llvm_load_value(b, 0, 0, TypeInt);
    if (op.opname() == op_and) {
        // From the old bitcode generated
        // define i32 @osl_and_iii(i32 %a, i32 %b) nounwind readnone ssp {
        //     %1 = icmp ne i32 %b, 0
        //  %not. = icmp ne i32 %a, 0
        //     %2 = and i1 %1, %not.
        //     %3 = zext i1 %2 to i32
        //   ret i32 %3
        llvm::Value* b_ne_0    = rop.ll.op_ne(b_val, rop.ll.constant(0));
        llvm::Value* a_ne_0    = rop.ll.op_ne(a_val, rop.ll.constant(0));
        llvm::Value* both_ne_0 = rop.ll.op_and(b_ne_0, a_ne_0);
        i1_res                 = both_ne_0;
    } else {
        // Also from the bitcode
        // %1 = or i32 %b, %a
        // %2 = icmp ne i32 %1, 0
        // %3 = zext i1 %2 to i32
        llvm::Value* or_ab      = rop.ll.op_or(a_val, b_val);
        llvm::Value* or_ab_ne_0 = rop.ll.op_ne(or_ab, rop.ll.constant(0));
        i1_res                  = or_ab_ne_0;
    }
    llvm::Value* i32_res = rop.ll.op_bool_to_int(i1_res);
    rop.llvm_store_value(i32_res, result, 0, 0);
    return true;
}


LLVMGEN(llvm_gen_if)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& cond = *rop.opargsym(op, 0);

    // Load the condition variable and figure out if it's nonzero
    llvm::Value* cond_val = rop.llvm_test_nonzero(cond);

    // Branch on the condition, to our blocks
    llvm::BasicBlock* then_block  = rop.ll.new_basic_block("then");
    llvm::BasicBlock* else_block  = rop.ll.new_basic_block("else");
    llvm::BasicBlock* after_block = rop.ll.new_basic_block("");
    rop.ll.op_branch(cond_val, then_block, else_block);

    // Then block
    rop.build_llvm_code(opnum + 1, op.jump(0), then_block);
    rop.ll.op_branch(after_block);

    // Else block
    rop.build_llvm_code(op.jump(0), op.jump(1), else_block);
    rop.ll.op_branch(after_block);  // insert point is now after_block

    // Continue on with the previous flow
    return true;
}



LLVMGEN(llvm_gen_loop_op)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& cond = *rop.opargsym(op, 0);

    // Branch on the condition, to our blocks
    llvm::BasicBlock* cond_block  = rop.ll.new_basic_block("cond");
    llvm::BasicBlock* body_block  = rop.ll.new_basic_block("body");
    llvm::BasicBlock* step_block  = rop.ll.new_basic_block("step");
    llvm::BasicBlock* after_block = rop.ll.new_basic_block("");
    // Save the step and after block pointers for possible break/continue
    rop.ll.push_loop(step_block, after_block);

    // Initialization (will be empty except for "for" loops)
    rop.build_llvm_code(opnum + 1, op.jump(0));

    // For "do-while", we go straight to the body of the loop, but for
    // "for" or "while", we test the condition next.
    rop.ll.op_branch(op.opname() == op_dowhile ? body_block : cond_block);

    // Load the condition variable and figure out if it's nonzero
    rop.build_llvm_code(op.jump(0), op.jump(1), cond_block);
    llvm::Value* cond_val = rop.llvm_test_nonzero(cond);

    // Jump to either LoopBody or AfterLoop
    rop.ll.op_branch(cond_val, body_block, after_block);

    // Body of loop
    rop.build_llvm_code(op.jump(1), op.jump(2), body_block);
    rop.ll.op_branch(step_block);

    // Step
    rop.build_llvm_code(op.jump(2), op.jump(3), step_block);
    rop.ll.op_branch(cond_block);

    // Continue on with the previous flow
    rop.ll.set_insert_point(after_block);
    rop.ll.pop_loop();

    return true;
}



LLVMGEN(llvm_gen_loopmod_op)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 0);
    if (op.opname() == op_break) {
        rop.ll.op_branch(rop.ll.loop_after_block());
    } else {  // continue
        rop.ll.op_branch(rop.ll.loop_step_block());
    }
    llvm::BasicBlock* next_block = rop.ll.new_basic_block("");
    rop.ll.set_insert_point(next_block);
    return true;
}



static llvm::Value*
llvm_gen_texture_options(BackendLLVM& rop, int opnum, int first_optional_arg,
                         bool tex3d, int nchans, llvm::Value*& alpha,
                         llvm::Value*& dalphadx, llvm::Value*& dalphady,
                         llvm::Value*& errormessage)
{
    llvm::Value* opt = rop.temp_texture_options_void_ptr();
    rop.ll.call_function("osl_init_texture_options", rop.sg_void_ptr(), opt);
    llvm::Value* missingcolor = NULL;
    TextureOpt optdefaults;  // So we can check the defaults
    bool swidth_set = false, twidth_set = false, rwidth_set = false;
    bool sblur_set = false, tblur_set = false, rblur_set = false;
    bool swrap_set = false, twrap_set = false, rwrap_set = false;
    bool firstchannel_set = false, fill_set = false, interp_set = false;
    // bool time_set = false;
    bool subimage_set = false;

    Opcode& op(rop.inst()->ops()[opnum]);
    for (int a = first_optional_arg; a < op.nargs(); ++a) {
        Symbol& Name(*rop.opargsym(op, a));
        OSL_DASSERT(Name.typespec().is_string()
                    && "optional texture token must be a string");
        OSL_DASSERT(a + 1 < op.nargs()
                    && "malformed argument list for texture");
        ustring name = Name.get_string();
        ++a;  // advance to next argument

        if (name.empty())  // skip empty string param name
            continue;

        Symbol& Val(*rop.opargsym(op, a));
        TypeDesc valtype  = Val.typespec().simpletype();
        const int* ival   = Val.typespec().is_int() && Val.is_constant()
                                ? (const int*)Val.data()
                                : NULL;
        const float* fval = Val.typespec().is_float() && Val.is_constant()
                                ? (const float*)Val.data()
                                : NULL;

#define PARAM_INT(paramname)                                            \
    if (name == Strings::paramname && valtype == TypeDesc::INT) {       \
        if (!paramname##_set && ival && *ival == optdefaults.paramname) \
            continue; /* default constant */                            \
        llvm::Value* val = rop.llvm_load_value(Val);                    \
        rop.ll.call_function("osl_texture_set_" #paramname, opt, val);  \
        paramname##_set = true;                                         \
        continue;                                                       \
    }

#define PARAM_FLOAT(paramname)                                         \
    if (name == Strings::paramname                                     \
        && (valtype == TypeDesc::FLOAT || valtype == TypeDesc::INT)) { \
        if (!paramname##_set                                           \
            && ((ival && *ival == optdefaults.paramname)               \
                || (fval && *fval == optdefaults.paramname)))          \
            continue; /* default constant */                           \
        llvm::Value* val = rop.llvm_load_value(Val);                   \
        if (valtype == TypeDesc::INT)                                  \
            val = rop.ll.op_int_to_float(val);                         \
        rop.ll.call_function("osl_texture_set_" #paramname, opt, val); \
        paramname##_set = true;                                        \
        continue;                                                      \
    }

#define PARAM_FLOAT_STR(paramname)                                            \
    if (name == Strings::paramname                                            \
        && (valtype == TypeDesc::FLOAT || valtype == TypeDesc::INT)) {        \
        if (!s##paramname##_set && !t##paramname##_set && !r##paramname##_set \
            && ((ival && *ival == optdefaults.s##paramname)                   \
                || (fval && *fval == optdefaults.s##paramname)))              \
            continue; /* default constant */                                  \
        llvm::Value* val = rop.llvm_load_value(Val);                          \
        if (valtype == TypeDesc::INT)                                         \
            val = rop.ll.op_int_to_float(val);                                \
        rop.ll.call_function("osl_texture_set_st" #paramname, opt, val);      \
        if (tex3d)                                                            \
            rop.ll.call_function("osl_texture_set_r" #paramname, opt, val);   \
        s##paramname##_set = true;                                            \
        t##paramname##_set = true;                                            \
        r##paramname##_set = true;                                            \
        continue;                                                             \
    }

#define PARAM_STRING_CODE(paramname, decoder, fieldname)                    \
    if (name == Strings::paramname && valtype == TypeDesc::STRING) {        \
        if (Val.is_constant()) {                                            \
            int code = (int)decoder(Val.get_string());                      \
            if (!paramname##_set && code == (int)optdefaults.fieldname)     \
                continue;                                                   \
            if (code >= 0) {                                                \
                llvm::Value* val = rop.ll.constant(code);                   \
                rop.ll.call_function("osl_texture_set_" #paramname "_code", \
                                     opt, val);                             \
            }                                                               \
        } else {                                                            \
            llvm::Value* val = rop.llvm_load_value(Val);                    \
            rop.ll.call_function("osl_texture_set_" #paramname, opt, val);  \
        }                                                                   \
        paramname##_set = true;                                             \
        continue;                                                           \
    }

        PARAM_FLOAT_STR(width)
        PARAM_FLOAT(swidth)
        PARAM_FLOAT(twidth)
        PARAM_FLOAT(rwidth)
        PARAM_FLOAT_STR(blur)
        PARAM_FLOAT(sblur)
        PARAM_FLOAT(tblur)
        PARAM_FLOAT(rblur)

        if (name == Strings::wrap && valtype == TypeDesc::STRING) {
            if (Val.is_constant()) {
                int mode = (int)TextureOpt::decode_wrapmode(Val.get_string());
                llvm::Value* val = rop.ll.constant(mode);
                rop.ll.call_function("osl_texture_set_stwrap_code", opt, val);
                if (tex3d)
                    rop.ll.call_function("osl_texture_set_rwrap_code", opt,
                                         val);
            } else {
                llvm::Value* val = rop.llvm_load_value(Val);
                rop.ll.call_function("osl_texture_set_stwrap", opt, val);
                if (tex3d)
                    rop.ll.call_function("osl_texture_set_rwrap", opt, val);
            }
            swrap_set = twrap_set = rwrap_set = true;
            continue;
        }
        PARAM_STRING_CODE(swrap, TextureOpt::decode_wrapmode, swrap)
        PARAM_STRING_CODE(twrap, TextureOpt::decode_wrapmode, twrap)
        PARAM_STRING_CODE(rwrap, TextureOpt::decode_wrapmode, rwrap)

        PARAM_FLOAT(fill)
        PARAM_INT(firstchannel)
        PARAM_INT(subimage)

        if (name == Strings::subimage && valtype == TypeDesc::STRING) {
            if (Val.is_constant()) {
                ustring v = Val.get_string();
                if (v.empty() && !subimage_set) {
                    continue;  // Ignore nulls unless they are overrides
                }
            }
            llvm::Value* val = rop.llvm_load_value(Val);
            rop.ll.call_function("osl_texture_set_subimagename", opt, val);
            subimage_set = true;
            continue;
        }

        PARAM_STRING_CODE(interp, tex_interp_to_code, interpmode)

        if (name == Strings::alpha && valtype == TypeDesc::FLOAT) {
            alpha = rop.llvm_get_pointer(Val);
            if (Val.has_derivs()) {
                dalphadx = rop.llvm_get_pointer(Val, 1);
                dalphady = rop.llvm_get_pointer(Val, 2);
                // NO z derivs!  dalphadz = rop.llvm_get_pointer (Val, 3);
            }
            continue;
        }
        if (name == Strings::errormessage && valtype == TypeDesc::STRING) {
            errormessage = rop.llvm_get_pointer(Val);
            continue;
        }
        if (name == Strings::missingcolor && equivalent(valtype, TypeColor)) {
            if (!missingcolor) {
                // If not already done, allocate enough storage for the
                // missingcolor value (4 floats), and call the special
                // function that points the TextureOpt.missingcolor to it.
                missingcolor = rop.ll.op_alloca(rop.ll.type_float(), 4);
                rop.ll.call_function("osl_texture_set_missingcolor_arena", opt,
                                     rop.ll.void_ptr(missingcolor));
            }
            rop.ll.op_memcpy(rop.ll.void_ptr(missingcolor),
                             rop.llvm_void_ptr(Val), (int)sizeof(Color3));
            continue;
        }
        if (name == Strings::missingalpha && valtype == TypeDesc::FLOAT) {
            if (!missingcolor) {
                // If not already done, allocate enough storage for the
                // missingcolor value (4 floats), and call the special
                // function that points the TextureOpt.missingcolor to it.
                missingcolor = rop.ll.op_alloca(rop.ll.type_float(), 4);
                rop.ll.call_function("osl_texture_set_missingcolor_arena", opt,
                                     rop.ll.void_ptr(missingcolor));
            }
            llvm::Value* val = rop.llvm_load_value(Val);
            rop.ll.call_function("osl_texture_set_missingcolor_alpha", opt,
                                 rop.ll.constant(nchans), val);
            continue;
        }
        if (name == Strings::colorspace && valtype == TypeDesc::STRING) {
            if (Val.is_constant()) {
                // Just ignore this option for now.
                // FIXME: need full implementation
                continue;
            } else {
                rop.shadingcontext()->errorfmt(
                    "texture{} optional argument \"{}\" must be constant after optimization ({}:{})",
                    tex3d ? "3d" : "", name, op.sourcefile(), op.sourceline());
                continue;
            }
        }


        // PARAM_FLOAT(time)
        if (name == Strings::time
            && (valtype == TypeDesc::FLOAT || valtype == TypeDesc::INT)) {
            // NOTE: currently no supported 3d texture format makes use of
            // time. So there is no time in the TextureOpt struct, but will
            // silently accept and ignore the time option.
            continue;
        }

        rop.shadingcontext()->errorfmt(
            "Unknown texture{} optional argument: \"{}\", <{}> ({}:{})",
            tex3d ? "3d" : "", name, valtype, op.sourcefile(), op.sourceline());
#undef PARAM_INT
#undef PARAM_FLOAT
#undef PARAM_FLOAT_STR
#undef PARAM_STRING_CODE

#if 0
        // Helps me find any constant optional params that aren't elided
        if (Name.is_constant() && Val.is_constant()) {
            std::cout << "! texture constant optional arg '" << name << "'\n";
            if (Val.typespec().is_float()) std::cout << "\tf " << Val.get_float() << "\n";
            if (Val.typespec().is_int()) std::cout << "\ti " << Val.get_int() << "\n";
            if (Val.typespec().is_string()) std::cout << "\t" << Val.get_string() << "\n";
        }
#endif
    }

    return opt;
}



LLVMGEN(llvm_gen_texture)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result   = *rop.opargsym(op, 0);
    Symbol& Filename = *rop.opargsym(op, 1);
    Symbol& S        = *rop.opargsym(op, 2);
    Symbol& T        = *rop.opargsym(op, 3);
    int nchans       = Result.typespec().aggregate();

    bool user_derivs       = false;
    int first_optional_arg = 4;
    if (op.nargs() > 4 && rop.opargsym(op, 4)->typespec().is_float()) {
        user_derivs        = true;
        first_optional_arg = 8;
        OSL_DASSERT(rop.opargsym(op, 5)->typespec().is_float());
        OSL_DASSERT(rop.opargsym(op, 6)->typespec().is_float());
        OSL_DASSERT(rop.opargsym(op, 7)->typespec().is_float());
    }

    llvm::Value* opt;  // TextureOpt
    llvm::Value *alpha = NULL, *dalphadx = NULL, *dalphady = NULL;
    llvm::Value* errormessage = NULL;
    opt = llvm_gen_texture_options(rop, opnum, first_optional_arg, false /*3d*/,
                                   nchans, alpha, dalphadx, dalphady,
                                   errormessage);

    RendererServices::TextureHandle* texture_handle = NULL;
    if (Filename.is_constant() && rop.shadingsys().opt_texture_handle()) {
        texture_handle
            = rop.renderer()->get_texture_handle(Filename.get_string(),
                                                 rop.shadingcontext(), nullptr);
        // FIXME(colorspace): that nullptr should be replaced by a TextureOpt*
        // that has the colorspace set.
    }

    // Now call the osl_texture function, passing the options and all the
    // explicit args like texture coordinates.
    llvm::Value* args[] = {
        rop.sg_void_ptr(),
        rop.llvm_load_value(Filename),
        rop.ll.constant_ptr(texture_handle),
        opt,
        rop.llvm_load_value(S),
        rop.llvm_load_value(T),
        user_derivs ? rop.llvm_load_value(*rop.opargsym(op, 4))
                    : rop.llvm_load_value(S, 1),
        user_derivs ? rop.llvm_load_value(*rop.opargsym(op, 5))
                    : rop.llvm_load_value(T, 1),
        user_derivs ? rop.llvm_load_value(*rop.opargsym(op, 6))
                    : rop.llvm_load_value(S, 2),
        user_derivs ? rop.llvm_load_value(*rop.opargsym(op, 7))
                    : rop.llvm_load_value(T, 2),
        rop.ll.constant(nchans),
        rop.ll.void_ptr(rop.llvm_get_pointer(Result, 0)),
        rop.ll.void_ptr(rop.llvm_get_pointer(Result, 1)),
        rop.ll.void_ptr(rop.llvm_get_pointer(Result, 2)),
        rop.ll.void_ptr(alpha ? alpha : rop.ll.void_ptr_null()),
        rop.ll.void_ptr(dalphadx ? dalphadx : rop.ll.void_ptr_null()),
        rop.ll.void_ptr(dalphady ? dalphady : rop.ll.void_ptr_null()),
        rop.ll.void_ptr(errormessage ? errormessage : rop.ll.void_ptr_null()),
    };
    rop.ll.call_function("osl_texture", args);
    rop.generated_texture_call(texture_handle != NULL);
    return true;
}



LLVMGEN(llvm_gen_texture3d)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result   = *rop.opargsym(op, 0);
    Symbol& Filename = *rop.opargsym(op, 1);
    Symbol& P        = *rop.opargsym(op, 2);
    int nchans       = Result.typespec().aggregate();

    bool user_derivs       = false;
    int first_optional_arg = 3;
    if (op.nargs() > 3 && rop.opargsym(op, 3)->typespec().is_triple()) {
        user_derivs        = true;
        first_optional_arg = 6;
        OSL_DASSERT(rop.opargsym(op, 3)->typespec().is_triple());
        OSL_DASSERT(rop.opargsym(op, 4)->typespec().is_triple());
        OSL_DASSERT(rop.opargsym(op, 5)->typespec().is_triple());
    }

    llvm::Value* opt;  // TextureOpt
    llvm::Value *alpha = NULL, *dalphadx = NULL, *dalphady = NULL;
    llvm::Value* errormessage = NULL;
    opt = llvm_gen_texture_options(rop, opnum, first_optional_arg, true /*3d*/,
                                   nchans, alpha, dalphadx, dalphady,
                                   errormessage);

    RendererServices::TextureHandle* texture_handle = NULL;
    if (Filename.is_constant() && rop.shadingsys().opt_texture_handle()) {
        texture_handle
            = rop.renderer()->get_texture_handle(Filename.get_string(),
                                                 rop.shadingcontext(), nullptr);
        // FIXME(colorspace): that nullptr should be replaced by a TextureOpt*
        // that has the colorspace set.
    }

    // Now call the osl_texture3d function, passing the options and all the
    // explicit args like texture coordinates.
    llvm::Value* args[] = {
        rop.sg_void_ptr(),
        rop.llvm_load_value(Filename),
        rop.ll.constant_ptr(texture_handle),
        opt,
        rop.llvm_void_ptr(P),
        // Auto derivs of P if !user_derivs
        user_derivs ? rop.llvm_void_ptr(*rop.opargsym(op, 3))
                    : rop.llvm_void_ptr(P, 1),
        user_derivs ? rop.llvm_void_ptr(*rop.opargsym(op, 4))
                    : rop.llvm_void_ptr(P, 2),
        // NOTE:  osl_texture3d will need to handle *dPdz possibly being null
        user_derivs ? rop.llvm_void_ptr(*rop.opargsym(op, 5))
                    : rop.ll.void_ptr_null(),
        rop.ll.constant(nchans),
        rop.ll.void_ptr(rop.llvm_void_ptr(Result, 0)),
        rop.ll.void_ptr(rop.llvm_void_ptr(Result, 1)),
        rop.ll.void_ptr(rop.llvm_void_ptr(Result, 2)),
        rop.ll.void_ptr(alpha ? alpha : rop.ll.void_ptr_null()),
        rop.ll.void_ptr(dalphadx ? dalphadx : rop.ll.void_ptr_null()),
        rop.ll.void_ptr(dalphady ? dalphady : rop.ll.void_ptr_null()),
        rop.ll.void_ptr(errormessage ? errormessage : rop.ll.void_ptr_null()),
    };

    rop.ll.call_function("osl_texture3d", args);
    rop.generated_texture_call(texture_handle != NULL);
    return true;
}



LLVMGEN(llvm_gen_environment)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result   = *rop.opargsym(op, 0);
    Symbol& Filename = *rop.opargsym(op, 1);
    Symbol& R        = *rop.opargsym(op, 2);
    int nchans       = Result.typespec().aggregate();

    bool user_derivs       = false;
    int first_optional_arg = 3;
    if (op.nargs() > 3 && rop.opargsym(op, 3)->typespec().is_triple()) {
        user_derivs        = true;
        first_optional_arg = 5;
        OSL_DASSERT(rop.opargsym(op, 4)->typespec().is_triple());
    }

    llvm::Value* opt;  // TextureOpt
    llvm::Value *alpha = NULL, *dalphadx = NULL, *dalphady = NULL;
    llvm::Value* errormessage = NULL;
    opt = llvm_gen_texture_options(rop, opnum, first_optional_arg, false /*3d*/,
                                   nchans, alpha, dalphadx, dalphady,
                                   errormessage);

    RendererServices::TextureHandle* texture_handle = NULL;
    if (Filename.is_constant() && rop.shadingsys().opt_texture_handle()) {
        texture_handle
            = rop.renderer()->get_texture_handle(Filename.get_string(),
                                                 rop.shadingcontext(), nullptr);
        // FIXME(colorspace): that nullptr should be replaced by a TextureOpt*
        // that has the colorspace set.
    }

    // Now call the osl_environment function, passing the options and all the
    // explicit args like texture coordinates.
    llvm::Value* args[] = {
        rop.sg_void_ptr(),
        rop.llvm_load_value(Filename),
        rop.ll.constant_ptr(texture_handle),
        opt,
        rop.llvm_void_ptr(R),
        user_derivs ? rop.llvm_void_ptr(*rop.opargsym(op, 3))
                    : rop.llvm_void_ptr(R, 1),
        user_derivs ? rop.llvm_void_ptr(*rop.opargsym(op, 4))
                    : rop.llvm_void_ptr(R, 2),
        rop.ll.constant(nchans),
        rop.llvm_void_ptr(Result, 0),
        rop.llvm_void_ptr(Result, 1),
        rop.llvm_void_ptr(Result, 2),
        alpha ? rop.ll.void_ptr(alpha) : rop.ll.void_ptr_null(),
        dalphadx ? rop.ll.void_ptr(dalphadx) : rop.ll.void_ptr_null(),
        dalphady ? rop.ll.void_ptr(dalphady) : rop.ll.void_ptr_null(),
        rop.ll.void_ptr(errormessage ? errormessage : rop.ll.void_ptr_null()),
    };
    rop.ll.call_function("osl_environment", args);
    rop.generated_texture_call(texture_handle != NULL);
    return true;
}



static llvm::Value*
llvm_gen_trace_options(BackendLLVM& rop, int opnum, int first_optional_arg)
{
    llvm::Value* opt = rop.temp_trace_options_void_ptr();
    rop.ll.call_function("osl_init_trace_options", rop.sg_void_ptr(), opt);
    Opcode& op(rop.inst()->ops()[opnum]);
    for (int a = first_optional_arg; a < op.nargs(); ++a) {
        Symbol& Name(*rop.opargsym(op, a));
        OSL_DASSERT(Name.typespec().is_string()
                    && "optional trace token must be a string");
        OSL_DASSERT(a + 1 < op.nargs() && "malformed argument list for trace");
        ustring name = Name.get_string();

        ++a;  // advance to next argument
        Symbol& Val(*rop.opargsym(op, a));
        TypeDesc valtype = Val.typespec().simpletype();

        llvm::Value* val = rop.llvm_load_value(Val);
        if (name == Strings::mindist && valtype == TypeDesc::FLOAT) {
            rop.ll.call_function("osl_trace_set_mindist", opt, val);
        } else if (name == Strings::maxdist && valtype == TypeDesc::FLOAT) {
            rop.ll.call_function("osl_trace_set_maxdist", opt, val);
        } else if (name == Strings::shade && valtype == TypeDesc::INT) {
            rop.ll.call_function("osl_trace_set_shade", opt, val);
        } else if (name == Strings::traceset && valtype == TypeDesc::STRING) {
            rop.ll.call_function("osl_trace_set_traceset", opt, val);
        } else {
            rop.shadingcontext()->errorfmt(
                "Unknown trace() optional argument: \"{}\", <{}> ({}:{})", name,
                valtype, op.sourcefile(), op.sourceline());
        }
    }

    return opt;
}



LLVMGEN(llvm_gen_trace)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    Symbol& Result         = *rop.opargsym(op, 0);
    Symbol& Pos            = *rop.opargsym(op, 1);
    Symbol& Dir            = *rop.opargsym(op, 2);
    int first_optional_arg = 3;

    llvm::Value* opt;  // TraceOpt
    opt = llvm_gen_trace_options(rop, opnum, first_optional_arg);

    // Now call the osl_trace function, passing the options and all the
    // explicit args like trace coordinates.
    llvm::Value* args[] = {
        rop.sg_void_ptr(),         opt,
        rop.llvm_void_ptr(Pos, 0), rop.llvm_void_ptr(Pos, 1),
        rop.llvm_void_ptr(Pos, 2), rop.llvm_void_ptr(Dir, 0),
        rop.llvm_void_ptr(Dir, 1), rop.llvm_void_ptr(Dir, 2),
    };
    llvm::Value* r = rop.ll.call_function("osl_trace", args);
    rop.llvm_store_value(r, Result);

    // Mark the instance as containing a trace call.
    // With lazytrace=0, we will want to flag the instance
    // for eager execution.
    rop.inst()->has_trace_op(true);

    return true;
}



static std::string
arg_typecode(Symbol* sym, bool derivs)
{
    const TypeSpec& t(sym->typespec());
    if (t.is_int())
        return "i";
    else if (t.is_matrix())
        return "m";
    else if (t.is_string())
        return "s";

    std::string name;
    if (derivs)
        name = "d";
    if (t.is_float())
        name += "f";
    else if (t.is_triple())
        name += "v";
    else
        OSL_ASSERT(0);
    return name;
}



static llvm::Value*
llvm_gen_noise_options(BackendLLVM& rop, int opnum, int first_optional_arg)
{
    llvm::Value* opt = rop.temp_noise_options_void_ptr();
    rop.ll.call_function("osl_init_noise_options", rop.sg_void_ptr(), opt);

    Opcode& op(rop.inst()->ops()[opnum]);
    for (int a = first_optional_arg; a < op.nargs(); ++a) {
        Symbol& Name(*rop.opargsym(op, a));
        OSL_DASSERT(Name.typespec().is_string()
                    && "optional noise token must be a string");
        OSL_DASSERT(a + 1 < op.nargs() && "malformed argument list for noise");
        ustring name = Name.get_string();

        ++a;  // advance to next argument
        Symbol& Val(*rop.opargsym(op, a));
        TypeDesc valtype = Val.typespec().simpletype();

        if (name.empty())  // skip empty string param name
            continue;

        if (name == Strings::anisotropic && Val.typespec().is_int()) {
            rop.ll.call_function("osl_noiseparams_set_anisotropic", opt,
                                 rop.llvm_load_value(Val));
        } else if (name == Strings::do_filter && Val.typespec().is_int()) {
            rop.ll.call_function("osl_noiseparams_set_do_filter", opt,
                                 rop.llvm_load_value(Val));
        } else if (name == Strings::direction && Val.typespec().is_triple()) {
            rop.ll.call_function("osl_noiseparams_set_direction", opt,
                                 rop.llvm_void_ptr(Val));
        } else if (name == Strings::bandwidth
                   && (Val.typespec().is_float() || Val.typespec().is_int())) {
            rop.ll.call_function("osl_noiseparams_set_bandwidth", opt,
                                 rop.llvm_load_value(Val, 0, NULL, 0,
                                                     TypeFloat));
        } else if (name == Strings::impulses
                   && (Val.typespec().is_float() || Val.typespec().is_int())) {
            rop.ll.call_function("osl_noiseparams_set_impulses", opt,
                                 rop.llvm_load_value(Val, 0, NULL, 0,
                                                     TypeFloat));
        } else {
            rop.shadingcontext()->errorfmt(
                "Unknown {} optional argument: \"{}\", <{}> ({}:{})",
                op.opname(), name, valtype, op.sourcefile(), op.sourceline());
        }
    }
    return opt;
}



// T noise ([string name,] float s, ...);
// T noise ([string name,] float s, float t, ...);
// T noise ([string name,] point P, ...);
// T noise ([string name,] point P, float t, ...);
// T pnoise ([string name,] float s, float sper, ...);
// T pnoise ([string name,] float s, float t, float sper, float tper, ...);
// T pnoise ([string name,] point P, point Pper, ...);
// T pnoise ([string name,] point P, float t, point Pper, float tper, ...);
LLVMGEN(llvm_gen_noise)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    bool periodic = (op.opname() == Strings::pnoise
                     || op.opname() == Strings::psnoise);

    int arg        = 0;  // Next arg to read
    Symbol& Result = *rop.opargsym(op, arg++);
    int outdim     = Result.typespec().is_triple() ? 3 : 1;
    Symbol* Name   = rop.opargsym(op, arg++);
    ustring name;
    if (Name->typespec().is_string()) {
        name = Name->is_constant() ? Name->get_string() : ustring();
    } else {
        // Not a string, must be the old-style noise/pnoise
        --arg;  // forget that arg
        Name = NULL;
        name = op.opname();
    }

    Symbol *S = rop.opargsym(op, arg++), *T = NULL;
    Symbol *Sper = NULL, *Tper = NULL;
    int indim   = S->typespec().is_triple() ? 3 : 1;
    bool derivs = S->has_derivs();

    if (periodic) {
        if (op.nargs() > (arg + 1)
            && (rop.opargsym(op, arg + 1)->typespec().is_float()
                || rop.opargsym(op, arg + 1)->typespec().is_triple())) {
            // 2D or 4D
            ++indim;
            T = rop.opargsym(op, arg++);
            derivs |= T->has_derivs();
        }
        Sper = rop.opargsym(op, arg++);
        if (indim == 2 || indim == 4)
            Tper = rop.opargsym(op, arg++);
    } else {
        // non-periodic case
        if (op.nargs() > arg && rop.opargsym(op, arg)->typespec().is_float()) {
            // either 2D or 4D, so needs a second index
            ++indim;
            T = rop.opargsym(op, arg++);
            derivs |= T->has_derivs();
        }
    }
    derivs &= Result.has_derivs();  // ignore derivs if result doesn't need

    bool pass_name = false, pass_sg = false, pass_options = false;
    if (name.empty()) {
        // name is not a constant
        name      = periodic ? Strings::genericpnoise : Strings::genericnoise;
        pass_name = true;
        pass_sg   = true;
        pass_options = true;
        derivs       = true;  // always take derivs if we don't know noise type
    } else if (name == Strings::perlin || name == Strings::snoise
               || name == Strings::psnoise) {
        name = periodic ? Strings::psnoise : Strings::snoise;
        // derivs = false;
    } else if (name == Strings::uperlin || name == Strings::noise
               || name == Strings::pnoise) {
        name = periodic ? Strings::pnoise : Strings::noise;
        // derivs = false;
    } else if (name == Strings::cell || name == Strings::cellnoise) {
        name   = periodic ? Strings::pcellnoise : Strings::cellnoise;
        derivs = false;  // cell noise derivs are always zero
    } else if (name == Strings::hash || name == Strings::hashnoise) {
        name   = periodic ? Strings::phashnoise : Strings::hashnoise;
        derivs = false;  // hash noise derivs are always zero
    } else if (name == Strings::simplex && !periodic) {
        name = Strings::simplexnoise;
    } else if (name == Strings::usimplex && !periodic) {
        name = Strings::usimplexnoise;
    } else if (name == Strings::gabor) {
        // already named
        pass_name    = true;
        pass_sg      = true;
        pass_options = true;
        derivs       = true;
        name         = periodic ? Strings::gaborpnoise : Strings::gabornoise;
    } else {
        rop.shadingcontext()->errorfmt(
            "{}noise type \"{}\" is unknown, called from ({}:{})",
            (periodic ? "periodic " : ""), name, op.sourcefile(),
            op.sourceline());
        return false;
    }

    if (rop.shadingsys().no_noise()) {
        // renderer option to replace noise with constant value. This can be
        // useful as a profiling aid, to see how much it speeds up to have
        // trivial expense for noise calls.
        if (name == Strings::uperlin || name == Strings::noise
            || name == Strings::usimplexnoise || name == Strings::usimplex
            || name == Strings::cell || name == Strings::cellnoise
            || name == Strings::hash || name == Strings::hashnoise
            || name == Strings::pcellnoise || name == Strings::pnoise)
            name = ustring("unullnoise");
        else
            name = ustring("nullnoise");
        pass_name    = false;
        periodic     = false;
        pass_sg      = false;
        pass_options = false;
    }

    llvm::Value* opt = NULL;
    if (pass_options) {
        opt = llvm_gen_noise_options(rop, opnum, arg);
    }

    std::string funcname = "osl_" + name.string() + "_"
                           + arg_typecode(&Result, derivs);
    llvm::Value* args[10];
    int nargs = 0;
    if (pass_name) {
        args[nargs++] = rop.llvm_load_value(*Name);
    }
    llvm::Value* tmpresult = NULL;
    // triple return, or float return with derivs, passes result pointer
    if (outdim == 3 || derivs) {
        if (derivs && !Result.has_derivs()) {
            tmpresult     = rop.llvm_load_arg(Result, true);
            args[nargs++] = tmpresult;
        } else
            args[nargs++] = rop.llvm_void_ptr(Result);
    }
    funcname += arg_typecode(S, derivs);
    args[nargs++] = rop.llvm_load_arg(*S, derivs);
    if (T) {
        funcname += arg_typecode(T, derivs);
        args[nargs++] = rop.llvm_load_arg(*T, derivs);
    }

    if (periodic) {
        funcname += arg_typecode(Sper, false /* no derivs */);
        args[nargs++] = rop.llvm_load_arg(*Sper, false);
        if (Tper) {
            funcname += arg_typecode(Tper, false /* no derivs */);
            args[nargs++] = rop.llvm_load_arg(*Tper, false);
        }
    }

    if (pass_sg)
        args[nargs++] = rop.sg_void_ptr();
    if (pass_options)
        args[nargs++] = opt;

    OSL_DASSERT(nargs < int(sizeof(args) / sizeof(args[0])));

#if 0
    llvm::outs() << "About to push " << funcname << "\n";
    for (int i = 0;  i < nargs;  ++i)
        llvm::outs() << "    " << *args[i] << "\n";
#endif

    llvm::Value* r
        = rop.ll.call_function(funcname.c_str(),
                               cspan<llvm::Value*>(args, args + nargs));
    if (outdim == 1 && !derivs) {
        // Just plain float (no derivs) returns its value
        rop.llvm_store_value(r, Result);
    } else if (derivs && !Result.has_derivs()) {
        // Function needed to take derivs, but our result doesn't have them.
        // We created a temp, now we need to copy to the real result.
        tmpresult = rop.llvm_ptr_cast(tmpresult, Result.typespec());
        for (int c = 0; c < Result.typespec().aggregate(); ++c) {
            llvm::Value* v = rop.llvm_load_value(tmpresult, Result.typespec(),
                                                 0, NULL, c);
            rop.llvm_store_value(v, Result, 0, c);
        }
    }  // N.B. other cases already stored their result in the right place

    // Clear derivs if result has them but we couldn't compute them
    if (Result.has_derivs() && !derivs)
        rop.llvm_zero_derivs(Result);

    if (rop.shadingsys().profile() >= 1)
        rop.ll.call_function("osl_count_noise", rop.sg_void_ptr());

    return true;
}



LLVMGEN(llvm_gen_getattribute)
{
    // getattribute() has eight "flavors":
    //   * getattribute (attribute_name, value)
    //   * getattribute (attribute_name, value[])
    //   * getattribute (attribute_name, index, value)
    //   * getattribute (attribute_name, index, value[])
    //   * getattribute (object, attribute_name, value)
    //   * getattribute (object, attribute_name, value[])
    //   * getattribute (object, attribute_name, index, value)
    //   * getattribute (object, attribute_name, index, value[])
    Opcode& op(rop.inst()->ops()[opnum]);
    int nargs = op.nargs();
    OSL_DASSERT(nargs >= 3 && nargs <= 5);

    bool array_lookup  = rop.opargsym(op, nargs - 2)->typespec().is_int();
    bool object_lookup = rop.opargsym(op, 2)->typespec().is_string()
                         && nargs >= 4;
    int object_slot = (int)object_lookup;
    int attrib_slot = object_slot + 1;
    int index_slot  = array_lookup ? nargs - 2 : 0;

    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& ObjectName
        = *rop.opargsym(op, object_slot);  // only valid if object_slot is true
    Symbol& Attribute = *rop.opargsym(op, attrib_slot);
    Symbol& Index
        = *rop.opargsym(op, index_slot);  // only valid if array_lookup is true
    Symbol& Destination = *rop.opargsym(op, nargs - 1);
    OSL_DASSERT(!Result.typespec().is_closure_based()
                && !ObjectName.typespec().is_closure_based()
                && !Attribute.typespec().is_closure_based()
                && !Index.typespec().is_closure_based()
                && !Destination.typespec().is_closure_based());

    // We'll pass the destination's attribute type directly to the
    // RenderServices callback so that the renderer can perform any
    // necessary conversions from its internal format to OSL's.
    TypeDesc dest_type = Destination.typespec().simpletype();

    llvm::Value* obj_name_arg  = object_lookup ? rop.llvm_load_value(ObjectName)
                                               : rop.llvm_const_hash(ustring());
    llvm::Value* attr_name_arg = rop.llvm_load_value(Attribute);

    ustring object_name      = (object_lookup && ObjectName.is_constant())
                                   ? ObjectName.get_string()
                                   : ustring();
    ustring* object_name_ptr = (object_lookup && ObjectName.is_constant())
                                   ? &object_name
                                   : nullptr;

    ustring attribute_name = Attribute.is_constant() ? Attribute.get_string()
                                                     : ustring();
    ustring* attribute_name_ptr = Attribute.is_constant() ? &attribute_name
                                                          : nullptr;

    int array_index = (array_lookup && Index.is_constant()) ? Index.get_int()
                                                            : 0;
    int* array_index_ptr = (array_lookup && Index.is_constant()) ? &array_index
                                                                 : nullptr;

    if (rop.renderer()->supports("build_attribute_getter")) {
        AttributeGetterSpec spec;
        rop.renderer()->build_attribute_getter(rop.group(), object_lookup,
                                               object_name_ptr,
                                               attribute_name_ptr, array_lookup,
                                               array_index_ptr, dest_type,
                                               Destination.has_derivs(), spec);
        if (!spec.function_name().empty()) {
            std::vector<llvm::Value*> args;
            args.reserve(spec.arg_count() + 1);
            for (size_t index = 0; index < spec.arg_count(); ++index) {
                const auto& arg = spec.arg(index);
                if (arg.is_holding<AttributeSpecBuiltinArg>()) {
                    switch (arg.get_builtin()) {
                    default: OSL_DASSERT(false); break;
                    case AttributeSpecBuiltinArg::OpaqueExecutionContext:
                        args.push_back(rop.sg_void_ptr());
                        break;
                    case AttributeSpecBuiltinArg::ShadeIndex:
                        args.push_back(rop.shadeindex());
                        break;
                    case AttributeSpecBuiltinArg::Derivatives:
                        args.push_back(
                            rop.ll.constant_bool(Destination.has_derivs()));
                        break;
                    case AttributeSpecBuiltinArg::Type:
                        args.push_back(rop.ll.constant(dest_type));
                        break;
                    case AttributeSpecBuiltinArg::ArrayIndex:
                        if (array_lookup)
                            args.push_back(rop.llvm_load_value(Index));
                        else
                            args.push_back(rop.ll.constant((int)0));
                        break;
                    case AttributeSpecBuiltinArg::IsArrayLookup:
                        args.push_back(rop.ll.constant_bool(array_lookup));
                        break;
                    case AttributeSpecBuiltinArg::ObjectName:
                        args.push_back(obj_name_arg);
                        break;
                    case AttributeSpecBuiltinArg::AttributeName:
                        args.push_back(attr_name_arg);
                        break;
                    }
                } else {
                    append_constant_arg(rop, arg, args);
                }
            }
            args.push_back(rop.llvm_void_ptr(Destination));
            llvm::Value* r = rop.ll.call_function(spec.function_name().c_str(),
                                                  args);
            rop.llvm_store_value(rop.ll.op_bool_to_int(r), Result);
        } else {
            rop.llvm_store_value(rop.ll.constant(0), Result);
        }
    } else {
        llvm::Value* args[] = {
            rop.sg_void_ptr(),
            rop.ll.constant((int)Destination.has_derivs()),
            obj_name_arg,
            attr_name_arg,
            rop.ll.constant((int)array_lookup),
            rop.llvm_load_value(Index),
            rop.ll.constant(dest_type),
            rop.llvm_void_ptr(Destination),
        };
        llvm::Value* r = rop.ll.call_function("osl_get_attribute", args);
        rop.llvm_store_value(r, Result);
    }

    return true;
}



LLVMGEN(llvm_gen_gettextureinfo)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() == 4 || op.nargs() == 6);
    bool use_coords  = (op.nargs() == 6);
    Symbol& Result   = *rop.opargsym(op, 0);
    Symbol& Filename = *rop.opargsym(op, 1);
    Symbol& Dataname = *rop.opargsym(op, use_coords ? 4 : 2);
    Symbol& Data     = *rop.opargsym(op, use_coords ? 5 : 3);
    Symbol* S        = use_coords ? rop.opargsym(op, 2) : nullptr;
    Symbol* T        = use_coords ? rop.opargsym(op, 3) : nullptr;

    OSL_DASSERT(
        !Result.typespec().is_closure_based() && Filename.typespec().is_string()
        && (S == nullptr || S->typespec().is_float())
        && (T == nullptr || T->typespec().is_float())
        && Dataname.typespec().is_string()
        && !Data.typespec().is_closure_based() && Result.typespec().is_int());

    RendererServices::TextureHandle* texture_handle = NULL;
    if (Filename.is_constant() && rop.shadingsys().opt_texture_handle()) {
        texture_handle
            = rop.renderer()->get_texture_handle(Filename.get_string(),
                                                 rop.shadingcontext(), nullptr);
    }

    std::vector<llvm::Value*> args;
    args.push_back(rop.sg_void_ptr());
    args.push_back(rop.llvm_load_value(Filename));
    args.push_back(rop.ll.constant_ptr(texture_handle));
    if (use_coords) {
        args.push_back(rop.llvm_load_value(*S));
        args.push_back(rop.llvm_load_value(*T));
    }
    args.push_back(rop.llvm_load_value(Dataname));
    // this passes a TypeDesc to an LLVM op-code
    args.push_back(rop.ll.constant((int)Data.typespec().simpletype().basetype));
    args.push_back(rop.ll.constant((int)Data.typespec().simpletype().arraylen));
    args.push_back(
        rop.ll.constant((int)Data.typespec().simpletype().aggregate));
    // destination
    args.push_back(rop.llvm_void_ptr(Data));
    // errormessage
    args.push_back(rop.ll.void_ptr_null());
    llvm::Value* r = rop.ll.call_function(use_coords ? "osl_get_textureinfo_st"
                                                     : "osl_get_textureinfo",
                                          args);
    rop.llvm_store_value(r, Result);
    /* Do not leave derivs uninitialized */
    if (Data.has_derivs())
        rop.llvm_zero_derivs(Data);
    rop.generated_texture_call(texture_handle != NULL);

    return true;
}



LLVMGEN(llvm_gen_getmessage)
{
    // getmessage() has four "flavors":
    //   * getmessage (attribute_name, value)
    //   * getmessage (attribute_name, value[])
    //   * getmessage (source, attribute_name, value)
    //   * getmessage (source, attribute_name, value[])
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() == 3 || op.nargs() == 4);
    int has_source = (op.nargs() == 4);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& Source = *rop.opargsym(op, 1);
    Symbol& Name   = *rop.opargsym(op, 1 + has_source);
    Symbol& Data   = *rop.opargsym(op, 2 + has_source);
    OSL_DASSERT(Result.typespec().is_int() && Name.typespec().is_string());
    OSL_DASSERT(has_source == 0 || Source.typespec().is_string());

    if (has_source && Source.is_constant() && Source.get_string() == "trace") {
        llvm::Value* args[5];
        args[0] = rop.sg_void_ptr();
        args[1] = rop.llvm_load_value(Name);

        if (Data.typespec().is_closure_based()) {
            // FIXME: secret handshake for closures ...
            args[2] = rop.ll.constant(
                TypeDesc(TypeDesc::UNKNOWN, Data.typespec().arraylength()));
            // We need a void ** here so the function can modify the closure
            args[3] = rop.llvm_void_ptr(Data);
        } else {
            args[2] = rop.ll.constant(Data.typespec().simpletype());
            args[3] = rop.llvm_void_ptr(Data);
        }
        args[4] = rop.ll.constant((int)Data.has_derivs());

        llvm::Value* r = rop.ll.call_function("osl_trace_get", args);
        rop.llvm_store_value(r, Result);
        return true;
    }

    llvm::Value* args[9];
    args[0] = rop.sg_void_ptr();
    args[1] = has_source ? rop.llvm_load_value(Source)
                         : rop.ll.constant64(uint64_t(ustring().hash()));
    args[2] = rop.llvm_load_value(Name);

    if (Data.typespec().is_closure_based()) {
        // FIXME: secret handshake for closures ...
        args[3] = rop.ll.constant(
            TypeDesc(TypeDesc::UNKNOWN, Data.typespec().arraylength()));
        // We need a void ** here so the function can modify the closure
        args[4] = rop.llvm_void_ptr(Data);
    } else {
        args[3] = rop.ll.constant(Data.typespec().simpletype());
        args[4] = rop.llvm_void_ptr(Data);
    }
    args[5] = rop.ll.constant((int)Data.has_derivs());

    args[6] = rop.ll.constant(rop.inst()->id());
    args[7] = rop.llvm_const_hash(op.sourcefile());
    args[8] = rop.ll.constant(op.sourceline());

    llvm::Value* r = rop.ll.call_function("osl_getmessage", args);
    rop.llvm_store_value(r, Result);
    return true;
}



LLVMGEN(llvm_gen_setmessage)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() == 2);
    Symbol& Name = *rop.opargsym(op, 0);
    Symbol& Data = *rop.opargsym(op, 1);
    OSL_DASSERT(Name.typespec().is_string());

    llvm::Value* args[7];
    args[0] = rop.sg_void_ptr();
    args[1] = rop.llvm_load_value(Name);
    if (Data.typespec().is_closure_based()) {
        // FIXME: secret handshake for closures ...
        args[2] = rop.ll.constant(
            TypeDesc(TypeDesc::UNKNOWN, Data.typespec().arraylength()));
        // We need a void ** here so the function can modify the closure
        args[3] = rop.llvm_void_ptr(Data);
    } else {
        args[2] = rop.ll.constant(Data.typespec().simpletype());
        args[3] = rop.llvm_void_ptr(Data);
    }

    args[4] = rop.ll.constant(rop.inst()->id());
    args[5] = rop.llvm_const_hash(op.sourcefile());
    args[6] = rop.ll.constant(op.sourceline());

    rop.ll.call_function("osl_setmessage", args);
    return true;
}



LLVMGEN(llvm_gen_get_simple_SG_field)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() == 1);

    Symbol& Result = *rop.opargsym(op, 0);
    int sg_index   = rop.ShaderGlobalNameToIndex(op.opname());
    OSL_DASSERT(sg_index >= 0);
    llvm::Value* sg_field = rop.ll.GEP(rop.llvm_type_sg(), rop.sg_ptr(), 0,
                                       sg_index);
    llvm::Type* sg_field_type
        = rop.ll.type_struct_field_at_index(rop.llvm_type_sg(), sg_index);
    llvm::Value* r = rop.ll.op_load(sg_field_type, sg_field);
    rop.llvm_store_value(r, Result);

    return true;
}



LLVMGEN(llvm_gen_calculatenormal)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() == 2);

    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& P      = *rop.opargsym(op, 1);

    OSL_DASSERT(Result.typespec().is_triple() && P.typespec().is_triple());
    if (!P.has_derivs()) {
        rop.llvm_assign_zero(Result);
        return true;
    }

    llvm::Value* args[] = {
        rop.llvm_void_ptr(Result),
        rop.sg_void_ptr(),
        rop.llvm_void_ptr(P),
    };
    rop.ll.call_function("osl_calculatenormal", args);
    if (Result.has_derivs())
        rop.llvm_zero_derivs(Result);
    return true;
}



LLVMGEN(llvm_gen_area)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() == 2);

    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& P      = *rop.opargsym(op, 1);

    OSL_DASSERT(Result.typespec().is_float() && P.typespec().is_triple());
    if (!P.has_derivs()) {
        rop.llvm_assign_zero(Result);
        return true;
    }

    llvm::Value* r = rop.ll.call_function("osl_area", rop.llvm_void_ptr(P));
    rop.llvm_store_value(r, Result);
    if (Result.has_derivs())
        rop.llvm_zero_derivs(Result);
    return true;
}



LLVMGEN(llvm_gen_spline)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() >= 4 && op.nargs() <= 5);

    bool has_knot_count = (op.nargs() == 5);
    Symbol& Result      = *rop.opargsym(op, 0);
    Symbol& Spline      = *rop.opargsym(op, 1);
    Symbol& Value       = *rop.opargsym(op, 2);
    Symbol& Knot_count  = *rop.opargsym(op, 3);  // might alias Knots
    Symbol& Knots       = has_knot_count ? *rop.opargsym(op, 4)
                                         : *rop.opargsym(op, 3);

    OSL_DASSERT(!Result.typespec().is_closure_based()
                && Spline.typespec().is_string() && Value.typespec().is_float()
                && !Knots.typespec().is_closure_based()
                && Knots.typespec().is_array()
                && (!has_knot_count
                    || (has_knot_count && Knot_count.typespec().is_int())));

    std::string name = fmtformat("osl_{}_", op.opname());
    // only use derivatives for result if:
    //   result has derivs and (value || knots) have derivs
    bool result_derivs = Result.has_derivs()
                         && (Value.has_derivs() || Knots.has_derivs());

    if (result_derivs)
        name += "d";
    if (Result.typespec().is_float())
        name += "f";
    else if (Result.typespec().is_triple())
        name += "v";

    if (result_derivs && Value.has_derivs())
        name += "d";
    if (Value.typespec().is_float())
        name += "f";
    else if (Value.typespec().is_triple())
        name += "v";

    if (result_derivs && Knots.has_derivs())
        name += "d";
    if (Knots.typespec().simpletype().elementtype() == TypeDesc::FLOAT)
        name += "f";
    else if (Knots.typespec().simpletype().elementtype().aggregate
             == TypeDesc::VEC3)
        name += "v";

    llvm::Value* args[] = {
        rop.llvm_void_ptr(Result),
        rop.llvm_load_value(Spline),
        rop.llvm_void_ptr(Value),  // make things easy
        rop.llvm_void_ptr(Knots),
        has_knot_count ? rop.llvm_load_value(Knot_count)
                       : rop.ll.constant((int)Knots.typespec().arraylength()),
        rop.ll.constant((int)Knots.typespec().arraylength()),
    };
    rop.ll.call_function(name.c_str(), args);

    if (Result.has_derivs() && !result_derivs)
        rop.llvm_zero_derivs(Result);

    return true;
}



static void
llvm_gen_keyword_fill(BackendLLVM& rop, Opcode& op,
                      const ClosureRegistry::ClosureEntry* clentry,
                      ustring clname, llvm::Value* mem_void_ptr, int argsoffset)
{
    OSL_DASSERT(((op.nargs() - argsoffset) % 2) == 0);

    int Nattrs = (op.nargs() - argsoffset) / 2;

    for (int attr_i = 0; attr_i < Nattrs; ++attr_i) {
        int argno     = attr_i * 2 + argsoffset;
        Symbol& Key   = *rop.opargsym(op, argno);
        Symbol& Value = *rop.opargsym(op, argno + 1);
        OSL_DASSERT(Key.typespec().is_string());
        OSL_ASSERT(Key.is_constant());
        ustring key        = Key.get_string();
        TypeDesc ValueType = Value.typespec().simpletype();

        bool legal = false;
        // Make sure there is some keyword arg that has the name and the type
        for (int t = 0; t < clentry->nkeyword; ++t) {
            const ClosureParam& p = clentry->params[clentry->nformal + t];
            // strcmp might be too much, we could precompute the ustring for the param,
            // but in this part of the code is not a big deal
            if (equivalent(p.type, ValueType) && !strcmp(key.c_str(), p.key)) {
                // store data
                OSL_DASSERT(p.offset + p.field_size <= clentry->struct_size);
                llvm::Value* dst = rop.ll.offset_ptr(mem_void_ptr, p.offset);
                llvm::Value* src = rop.llvm_void_ptr(Value);
                rop.ll.op_memcpy(dst, src, (int)p.type.size(),
                                 4 /* use 4 byte alignment for now */);
                legal = true;
                break;
            }
        }
        if (!legal) {
            rop.shadingcontext()->warningfmt(
                "Unsupported closure keyword arg \"{}\" for {} ({}:{})", key,
                clname, op.sourcefile(), op.sourceline());
        }
    }
}



LLVMGEN(llvm_gen_closure)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() >= 2);  // at least the result and the ID

    Symbol& Result = *rop.opargsym(op, 0);
    int weighted   = rop.opargsym(op, 1)->typespec().is_string() ? 0 : 1;
    Symbol* weight = weighted ? rop.opargsym(op, 1) : NULL;
    Symbol& Id     = *rop.opargsym(op, 1 + weighted);
    OSL_DASSERT(Result.typespec().is_closure());
    OSL_DASSERT(Id.typespec().is_string());
    ustring closure_name = Id.get_string();

    const ClosureRegistry::ClosureEntry* clentry
        = rop.shadingsys().find_closure(closure_name);
    if (!clentry) {
        rop.shadingcontext()->errorfmt(
            "Closure '{}' is not supported by the current renderer, called from {}:{} in shader \"{}\", layer {} \"{}\", group \"{}\"",
            closure_name, op.sourcefile(), op.sourceline(),
            rop.inst()->shadername(), rop.layer(), rop.inst()->layername(),
            rop.group().name());
        return false;
    }

    OSL_DASSERT(op.nargs() >= (2 + weighted + clentry->nformal));

    // Call osl_allocate_closure_component(closure, id, size).  It returns
    // the memory for the closure parameter data.
    llvm::Value* render_ptr = rop.ll.constant_ptr(rop.shadingsys().renderer(),
                                                  rop.ll.type_void_ptr());
    llvm::Value* sg_ptr     = rop.sg_void_ptr();
    llvm::Value* id_int     = rop.ll.constant(clentry->id);
    llvm::Value* size_int   = rop.ll.constant(clentry->struct_size);
    llvm::Value* return_ptr
        = weighted
              ? rop.ll.call_function("osl_allocate_weighted_closure_component",
                                     sg_ptr, id_int, size_int,
                                     rop.llvm_void_ptr(*weight))
              : rop.ll.call_function("osl_allocate_closure_component", sg_ptr,
                                     id_int, size_int);
    llvm::Value* comp_void_ptr = return_ptr;

    // For the weighted closures, we need a surrounding "if" so that it's safe
    // for osl_allocate_weighted_closure_component to return NULL (unless we
    // know for sure that it's constant weighted and that the weight is
    // not zero).
    llvm::BasicBlock* next_block = NULL;
    if (weighted && !(weight->is_constant() && !rop.is_zero(*weight))) {
        llvm::BasicBlock* notnull_block = rop.ll.new_basic_block(
            "non_null_closure");
        next_block        = rop.ll.new_basic_block("");
        llvm::Value* cond = rop.ll.op_ne(return_ptr, rop.ll.void_ptr_null());
        rop.ll.op_branch(cond, notnull_block, next_block);
        // new insert point is nonnull_block
    }

    llvm::Value* comp_ptr
        = rop.ll.ptr_cast(comp_void_ptr, rop.llvm_type_closure_component_ptr());
    // Get the address of the primitive buffer, which is the 2nd field
    llvm::Value* mem_void_ptr = rop.ll.GEP(rop.llvm_type_closure_component(),
                                           comp_ptr, 0, 2);
    mem_void_ptr = rop.ll.ptr_cast(mem_void_ptr, rop.ll.type_void_ptr());

    // If the closure has a "prepare" method, call
    // prepare(renderer, id, memptr).  If there is no prepare method, just
    // zero out the closure parameter memory.
    if (clentry->prepare) {
        // Call clentry->prepare(renderservices *, int id, void *mem)
        llvm::Value* funct_ptr
            = rop.ll.constant_ptr((void*)clentry->prepare,
                                  rop.llvm_type_prepare_closure_func());
        llvm::Value* args[] = { render_ptr, id_int, mem_void_ptr };
        rop.ll.call_function(funct_ptr, args);
    } else {
        rop.ll.op_memset(mem_void_ptr, 0, clentry->struct_size, 4 /*align*/);
    }

    // Here is where we fill the struct using the params
    for (int carg = 0; carg < clentry->nformal; ++carg) {
        const ClosureParam& p = clentry->params[carg];
        if (p.key != NULL)
            break;
        OSL_DASSERT(p.offset + p.field_size <= clentry->struct_size);
        Symbol& sym = *rop.opargsym(op, carg + 2 + weighted);
        TypeDesc t  = sym.typespec().simpletype();

        if (!sym.typespec().is_closure_array() && !sym.typespec().is_structure()
            && equivalent(t, p.type)) {
            llvm::Value* dst = rop.ll.offset_ptr(mem_void_ptr, p.offset);
            llvm::Value* src = rop.llvm_void_ptr(sym);
            rop.ll.op_memcpy(dst, src, (int)p.type.size(),
                             4 /* use 4 byte alignment for now */);
        } else {
            rop.shadingcontext()->errorfmt(
                "Incompatible formal argument {} to '{}' closure ({} {}, expected {}). Prototypes don't match renderer registry ({}:{}).",
                carg + 1, closure_name, sym.typespec(), sym.unmangled(), p.type,
                op.sourcefile(), op.sourceline());
        }
    }

    // If the closure has a "setup" method, call
    // setup(render_services, id, mem_ptr).
    if (clentry->setup) {
        // Call clentry->setup(renderservices *, int id, void *mem)
        llvm::Value* funct_ptr
            = rop.ll.constant_ptr((void*)clentry->setup,
                                  rop.llvm_type_setup_closure_func());
        llvm::Value* args[] = { render_ptr, id_int, mem_void_ptr };
        rop.ll.call_function(funct_ptr, args);
    }

    llvm_gen_keyword_fill(rop, op, clentry, closure_name, mem_void_ptr,
                          2 + weighted + clentry->nformal);

    if (next_block)
        rop.ll.op_branch(next_block);

    // Store result at the end, otherwise Ci = modifier(Ci) won't work
    rop.llvm_store_value(return_ptr, Result, 0, NULL, 0);

    return true;
}



LLVMGEN(llvm_gen_pointcloud_search)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() >= 5);
    Symbol& Result     = *rop.opargsym(op, 0);
    Symbol& Filename   = *rop.opargsym(op, 1);
    Symbol& Center     = *rop.opargsym(op, 2);
    Symbol& Radius     = *rop.opargsym(op, 3);
    Symbol& Max_points = *rop.opargsym(op, 4);

    OSL_DASSERT(Result.typespec().is_int() && Filename.typespec().is_string()
                && Center.typespec().is_triple() && Radius.typespec().is_float()
                && Max_points.typespec().is_int());

    std::vector<Symbol*>
        clear_derivs_of;  // arguments whose derivs we need to zero at the end
    int attr_arg_offset = 5;  // where the opt attrs begin
    Symbol* Sort        = NULL;
    if (op.nargs() > 5 && rop.opargsym(op, 5)->typespec().is_int()) {
        Sort = rop.opargsym(op, 5);
        ++attr_arg_offset;
    }
    int nattrs = (op.nargs() - attr_arg_offset) / 2;

    // Generate local space for the names/types/values arrays
    llvm::Value* names  = rop.ll.op_alloca(rop.ll.type_int64(), nattrs);
    llvm::Value* types  = rop.ll.op_alloca(rop.ll.type_typedesc(), nattrs);
    llvm::Value* values = rop.ll.op_alloca(rop.ll.type_void_ptr(), nattrs);

    std::vector<llvm::Value*> args;
    args.push_back(rop.sg_void_ptr());              // 0 sg
    args.push_back(rop.llvm_load_value(Filename));  // 1 filename
    args.push_back(rop.llvm_void_ptr(Center));      // 2 center
    args.push_back(rop.llvm_load_value(Radius));    // 3 radius

    constexpr int maxPointsArgumentIndex = 4;  // 4 max_points
    OSL_ASSERT(args.size() == maxPointsArgumentIndex);
    llvm::Value* maxPointsVal = rop.llvm_load_value(Max_points);
    args.push_back(maxPointsVal);  // 4 max_points

    args.push_back(Sort ? rop.llvm_load_value(*Sort)  // 5 sort
                        : rop.ll.constant(0));
    constexpr int indicesArgumentIndex = 6;     // 6 indices
    args.push_back(NULL);                       // 6 indices
    args.push_back(rop.ll.constant_ptr(NULL));  // 7 distances
    args.push_back(rop.ll.constant(0));         // 8 derivs_offset
    args.push_back(NULL);                       // 9 nattrs
    int capacity    = 0x7FFFFFFF;               // Lets put a 32 bit limit
    int extra_attrs = 0;                        // Extra query attrs to search
    // This loop does three things. 1) Look for the special attributes
    // "distance", "index" and grab the pointer. 2) Compute the minimmum
    // size of the provided output arrays to check against max_points
    // 3) push optional args to the arg list
    for (int i = 0; i < nattrs; ++i) {
        Symbol& Name  = *rop.opargsym(op, attr_arg_offset + i * 2);
        Symbol& Value = *rop.opargsym(op, attr_arg_offset + i * 2 + 1);

        OSL_DASSERT(Name.typespec().is_string());
        TypeDesc simpletype = Value.typespec().simpletype();
        if (Name.is_constant() && Name.get_string() == u_index
            && simpletype.elementtype() == TypeDesc::INT) {
            args[indicesArgumentIndex] = rop.llvm_void_ptr(Value);
        } else if (Name.is_constant() && Name.get_string() == u_distance
                   && simpletype.elementtype() == TypeDesc::FLOAT) {
            args[7] = rop.llvm_void_ptr(Value);
            if (Value.has_derivs()) {
                if (Center.has_derivs())
                    // deriv offset is the size of the array
                    args[8] = rop.ll.constant((int)simpletype.numelements());
                else
                    clear_derivs_of.push_back(&Value);
            }
        } else if (!rop.use_optix()) {
            //TODO: Implement custom attribute arguments for OptiX

            // It is a regular attribute, push it to the arg list
            llvm::Value* write_args[]
                = { rop.ll.void_ptr(names),    rop.ll.void_ptr(types),
                    rop.ll.void_ptr(values),   rop.ll.constant(extra_attrs),
                    rop.llvm_load_value(Name), rop.ll.constant(simpletype),
                    rop.llvm_void_ptr(Value) };
            rop.ll.call_function("osl_pointcloud_write_helper", write_args);
            if (Value.has_derivs())
                clear_derivs_of.push_back(&Value);
            extra_attrs++;
        }
        // minimum capacity of the output arrays
        capacity = std::min(static_cast<int>(simpletype.numelements()),
                            capacity);
    }

    args[9] = rop.ll.constant(extra_attrs);
    args.push_back(rop.ll.void_ptr(names));   // attribute names array
    args.push_back(rop.ll.void_ptr(types));   // attribute types array
    args.push_back(rop.ll.void_ptr(values));  // attribute values array

    if (!args[indicesArgumentIndex]) {
        llvm::Value* indices = rop.ll.op_alloca(rop.ll.type_int(), capacity);
        args[indicesArgumentIndex] = rop.ll.void_ptr(indices);
    }

    if (Max_points.is_constant()) {
        // Compare capacity to the requested number of points.
        // Choose not to do a runtime check because generated code will still work,
        // arrays will only be filled up to the capacity,
        // per the OSL language specification
        const int const_max_points = Max_points.get_int();
        if (capacity < const_max_points) {
            rop.shadingcontext()->warningfmt(
                "Arrays too small for pointcloud lookup at ({}:{}) ({}:{})",
                op.sourcefile().c_str(), op.sourceline());
            args[maxPointsArgumentIndex] = rop.ll.constant(capacity);
        }
    } else {
        // Clamp max points to the capacity of the arrays
        llvm::Value* capacity_val = rop.ll.constant(capacity);
        llvm::Value* cond         = rop.ll.op_le(capacity_val, maxPointsVal);
        llvm::Value* clampedMaxPoints = rop.ll.op_select(cond, capacity_val,
                                                         maxPointsVal);
        args[maxPointsArgumentIndex]  = clampedMaxPoints;
    }

    llvm::Value* count = rop.ll.call_function("osl_pointcloud_search", args);
    // Clear derivs if necessary
    for (size_t i = 0; i < clear_derivs_of.size(); ++i)
        rop.llvm_zero_derivs(*clear_derivs_of[i], count);
    // Store result
    rop.llvm_store_value(count, Result);

    return true;
}



LLVMGEN(llvm_gen_pointcloud_get)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() >= 6);

    Symbol& Result    = *rop.opargsym(op, 0);
    Symbol& Filename  = *rop.opargsym(op, 1);
    Symbol& Indices   = *rop.opargsym(op, 2);
    Symbol& Count     = *rop.opargsym(op, 3);
    Symbol& Attr_name = *rop.opargsym(op, 4);
    Symbol& Data      = *rop.opargsym(op, 5);

    llvm::Value* count = rop.llvm_load_value(Count);

    // reduce the specified count to be below the arraylength of the indices and data
    int element_count           = std::min(Data.typespec().arraylength(),
                                           Indices.typespec().arraylength());
    llvm::Value* elem_count_val = rop.ll.constant(element_count);
    llvm::Value* cond           = rop.ll.op_le(elem_count_val, count);
    llvm::Value* clampedCount   = rop.ll.op_select(cond, elem_count_val, count);

    llvm::Value* args[] = {
        rop.sg_void_ptr(),
        rop.llvm_load_value(Filename),
        rop.llvm_void_ptr(Indices),
        clampedCount,
        rop.llvm_load_value(Attr_name),
        rop.ll.constant(Data.typespec().simpletype()),
        rop.llvm_void_ptr(Data),
    };
    llvm::Value* found = rop.ll.call_function("osl_pointcloud_get", args);
    rop.llvm_store_value(found, Result);
    if (Data.has_derivs()) {
        rop.llvm_zero_derivs(Data, clampedCount);
    }

    return true;
}



LLVMGEN(llvm_gen_pointcloud_write)
{
    Opcode& op(rop.inst()->ops()[opnum]);

    OSL_DASSERT(op.nargs() >= 3);
    Symbol& Result   = *rop.opargsym(op, 0);
    Symbol& Filename = *rop.opargsym(op, 1);
    Symbol& Pos      = *rop.opargsym(op, 2);
    OSL_DASSERT(Result.typespec().is_int() && Filename.typespec().is_string()
                && Pos.typespec().is_triple());
    OSL_DASSERT((op.nargs() & 1) && "must have an even number of attribs");

    int nattrs = (op.nargs() - 3) / 2;

    // Generate local space for the names/types/values arrays
    llvm::Value* names  = rop.ll.op_alloca(rop.ll.type_int64(), nattrs);
    llvm::Value* types  = rop.ll.op_alloca(rop.ll.type_typedesc(), nattrs);
    llvm::Value* values = rop.ll.op_alloca(rop.ll.type_void_ptr(), nattrs);

    // Fill in the arrays with the params, use helper function because
    // it's a pain to offset things into the array ourselves.
    for (int i = 0; i < nattrs; ++i) {
        Symbol* namesym     = rop.opargsym(op, 3 + 2 * i);
        Symbol* valsym      = rop.opargsym(op, 3 + 2 * i + 1);
        llvm::Value* args[] = {
            rop.ll.void_ptr(names),
            rop.ll.void_ptr(types),
            rop.ll.void_ptr(values),
            rop.ll.constant(i),
            rop.llvm_load_value(*namesym),                     // name[i]
            rop.ll.constant(valsym->typespec().simpletype()),  // type[i]
            rop.llvm_void_ptr(*valsym)                         // value[i]
        };
        rop.ll.call_function("osl_pointcloud_write_helper", args);
    }

    llvm::Value* args[] = {
        rop.sg_void_ptr(),              // shaderglobals pointer
        rop.llvm_load_value(Filename),  // name
        rop.llvm_void_ptr(Pos),         // position
        rop.ll.constant(nattrs),        // number of attributes
        rop.ll.void_ptr(names),         // attribute names array
        rop.ll.void_ptr(types),         // attribute types array
        rop.ll.void_ptr(values)         // attribute values array
    };
    llvm::Value* ret = rop.ll.call_function("osl_pointcloud_write", args);
    rop.llvm_store_value(ret, Result);

    return true;
}


LLVMGEN(llvm_gen_dict_find)
{
    // OSL has two variants of this function:
    //     dict_find (string dict, string query)
    //     dict_find (int nodeID, string query)
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 3);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& Source = *rop.opargsym(op, 1);
    Symbol& Query  = *rop.opargsym(op, 2);
    OSL_DASSERT(
        Result.typespec().is_int() && Query.typespec().is_string()
        && (Source.typespec().is_int() || Source.typespec().is_string()));
    bool sourceint      = Source.typespec().is_int();  // is it an int?
    llvm::Value* args[] = { rop.sg_void_ptr(), rop.llvm_load_value(Source),
                            rop.llvm_load_value(Query) };
    const char* func    = sourceint ? "osl_dict_find_iis" : "osl_dict_find_iss";
    llvm::Value* ret    = rop.ll.call_function(func, args);
    rop.llvm_store_value(ret, Result);
    return true;
}



LLVMGEN(llvm_gen_dict_next)
{
    // dict_net is very straightforward -- just insert sg ptr as first arg
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 2);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& NodeID = *rop.opargsym(op, 1);
    OSL_DASSERT(Result.typespec().is_int() && NodeID.typespec().is_int());
    llvm::Value* ret = rop.ll.call_function("osl_dict_next", rop.sg_void_ptr(),
                                            rop.llvm_load_value(NodeID));
    rop.llvm_store_value(ret, Result);
    return true;
}



LLVMGEN(llvm_gen_dict_value)
{
    // int dict_value (int nodeID, string attribname, output TYPE value)
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 4);
    Symbol& Result = *rop.opargsym(op, 0);
    Symbol& NodeID = *rop.opargsym(op, 1);
    Symbol& Name   = *rop.opargsym(op, 2);
    Symbol& Value  = *rop.opargsym(op, 3);
    OSL_DASSERT(Result.typespec().is_int() && NodeID.typespec().is_int()
                && Name.typespec().is_string());
    llvm::Value* args[] = {
        rop.sg_void_ptr(),            // arg 0: shaderglobals ptr
        rop.llvm_load_value(NodeID),  // arg 1: nodeID
        rop.llvm_load_value(Name),    // arg 2: attribute name
        rop.ll.constant(
            Value.typespec().simpletype()),  // arg 3: encoded type of Value
        rop.llvm_void_ptr(Value),            // arg 4: pointer to Value
    };
    llvm::Value* ret = rop.ll.call_function("osl_dict_value", args);
    rop.llvm_store_value(ret, Result);
    return true;
}



LLVMGEN(llvm_gen_split)
{
    // int split (string str, output string result[], string sep, int maxsplit)
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() >= 3 && op.nargs() <= 5);
    Symbol& R       = *rop.opargsym(op, 0);
    Symbol& Str     = *rop.opargsym(op, 1);
    Symbol& Results = *rop.opargsym(op, 2);
    OSL_DASSERT(R.typespec().is_int() && Str.typespec().is_string()
                && Results.typespec().is_array()
                && Results.typespec().is_string_based());

    llvm::Value* args[5];
    args[0] = rop.llvm_load_value(Str);
    args[1] = rop.llvm_void_ptr(Results);
    if (op.nargs() >= 4) {
        Symbol& Sep = *rop.opargsym(op, 3);
        OSL_DASSERT(Sep.typespec().is_string());
        args[2] = rop.llvm_load_value(Sep);
    } else {
        args[2] = rop.ll.constant(ustring("").c_str());
    }
    if (op.nargs() >= 5) {
        Symbol& Maxsplit = *rop.opargsym(op, 4);
        OSL_DASSERT(Maxsplit.typespec().is_int());
        args[3] = rop.llvm_load_value(Maxsplit);
    } else {
        args[3] = rop.ll.constant(Results.typespec().arraylength());
    }
    args[4]          = rop.ll.constant(Results.typespec().arraylength());
    llvm::Value* ret = rop.ll.call_function("osl_split", args);
    rop.llvm_store_value(ret, R);
    return true;
}



LLVMGEN(llvm_gen_raytype)
{
    // int raytype (string name)
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 2);
    Symbol& Result       = *rop.opargsym(op, 0);
    Symbol& Name         = *rop.opargsym(op, 1);
    llvm::Value* args[2] = { rop.sg_void_ptr(), NULL };
    const char* func     = NULL;
    if (Name.is_constant()) {
        // We can statically determine the bit pattern
        ustring name = Name.get_string();
        args[1]      = rop.ll.constant(rop.shadingsys().raytype_bit(name));
        func         = "osl_raytype_bit";
    } else {
        // No way to know which name is being asked for
        args[1] = rop.llvm_load_value(Name);
        func    = "osl_raytype_name";
    }
    llvm::Value* ret = rop.ll.call_function(func, args);
    rop.llvm_store_value(ret, Result);
    return true;
}



// color blackbody (float temperatureK)
// color wavelength_color (float wavelength_nm)  // same function signature
LLVMGEN(llvm_gen_blackbody)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 2);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& Temperature(*rop.opargsym(op, 1));
    OSL_DASSERT(Result.typespec().is_triple()
                && Temperature.typespec().is_float());

    llvm::Value* args[] = { rop.sg_void_ptr(), rop.llvm_void_ptr(Result),
                            rop.llvm_load_value(Temperature) };
    rop.ll.call_function(fmtformat("osl_{}_vf", op.opname()).c_str(), args);

    // Punt, zero out derivs.
    // FIXME -- only of some day, someone truly needs blackbody() to
    // correctly return derivs with spatially-varying temperature.
    if (Result.has_derivs())
        rop.llvm_zero_derivs(Result);

    return true;
}



// float luminance (color c)
LLVMGEN(llvm_gen_luminance)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 2);
    Symbol& Result(*rop.opargsym(op, 0));
    Symbol& C(*rop.opargsym(op, 1));
    OSL_DASSERT(Result.typespec().is_float() && C.typespec().is_triple());

    bool deriv          = C.has_derivs() && Result.has_derivs();
    llvm::Value* args[] = { rop.sg_void_ptr(), rop.llvm_void_ptr(Result),
                            rop.llvm_void_ptr(C) };
    rop.ll.call_function(deriv ? "osl_luminance_dfdv" : "osl_luminance_fv",
                         args);

    if (Result.has_derivs() && !C.has_derivs())
        rop.llvm_zero_derivs(Result);

    return true;
}



LLVMGEN(llvm_gen_isconstant)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 2);
    Symbol& Result(*rop.opargsym(op, 0));
    OSL_DASSERT(Result.typespec().is_int());
    Symbol& A(*rop.opargsym(op, 1));
    rop.llvm_store_value(rop.ll.constant(A.is_constant() ? 1 : 0), Result);
    return true;
}



LLVMGEN(llvm_gen_functioncall)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 1);

    llvm::BasicBlock* after_block = rop.ll.push_function();

    unsigned int op_num_function_starts_at = opnum + 1;
    unsigned int op_num_function_ends_at   = op.jump(0);
    if (rop.ll.debug_is_enabled()) {
        Symbol& functionNameSymbol(*rop.opargsym(op, 0));
        OSL_DASSERT(functionNameSymbol.is_constant());
        OSL_DASSERT(functionNameSymbol.typespec().is_string());
        ustring functionName = functionNameSymbol.get_string();
        ustring file_name
            = rop.inst()->op(op_num_function_starts_at).sourcefile();
        unsigned int method_line
            = rop.inst()->op(op_num_function_starts_at).sourceline();
        rop.ll.debug_push_inlined_function(functionName, file_name,
                                           method_line);
    }

    // Generate the code for the body of the function
    rop.build_llvm_code(op_num_function_starts_at, op_num_function_ends_at);
    rop.ll.op_branch(after_block);

    // Continue on with the previous flow
    if (rop.ll.debug_is_enabled()) {
        rop.ll.debug_pop_inlined_function();
    }
    rop.ll.pop_function();

    return true;
}



LLVMGEN(llvm_gen_functioncall_nr)
{
    OSL_ASSERT(rop.ll.debug_is_enabled()
               && "no return version should only exist when debug is enabled");
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_ASSERT(op.nargs() == 1);

    Symbol& functionNameSymbol(*rop.opargsym(op, 0));
    OSL_ASSERT(functionNameSymbol.is_constant());
    OSL_ASSERT(functionNameSymbol.typespec().is_string());
    ustring functionName = functionNameSymbol.get_string();

    int op_num_function_starts_at = opnum + 1;
    int op_num_function_ends_at   = op.jump(0);
    OSL_ASSERT(
        op.farthest_jump() == op_num_function_ends_at
        && "As we are not doing any branching, we should ensure that the inlined function truly ends at the farthest jump");
    const Opcode& startop(rop.inst()->op(op_num_function_starts_at));
    rop.ll.debug_push_inlined_function(functionName, startop.sourcefile(),
                                       startop.sourceline());

    // Generate the code for the body of the function
    rop.build_llvm_code(op_num_function_starts_at, op_num_function_ends_at);

    // Continue on with the previous flow
    rop.ll.debug_pop_inlined_function();

    return true;
}



LLVMGEN(llvm_gen_return)
{
    Opcode& op(rop.inst()->ops()[opnum]);
    OSL_DASSERT(op.nargs() == 0);
    if (op.opname() == Strings::op_exit) {
        // If it's a real "exit", totally jump out of the shader instance.
        // The exit instance block will be created if it doesn't yet exist.
        rop.ll.op_branch(rop.llvm_exit_instance_block());
    } else {
        // If it's a "return", jump to the exit point of the function.
        rop.ll.op_branch(rop.ll.return_block());
    }
    llvm::BasicBlock* next_block = rop.ll.new_basic_block("");
    rop.ll.set_insert_point(next_block);
    return true;
}



OSL_PRAGMA_WARNING_PUSH
OSL_GCC_PRAGMA(GCC diagnostic ignored "-Wunused-parameter")

LLVMGEN(llvm_gen_end)
{
    // Dummy routine needed only for the op_descriptor table
    return false;
}

OSL_PRAGMA_WARNING_POP


};  // namespace pvt
OSL_NAMESPACE_END
