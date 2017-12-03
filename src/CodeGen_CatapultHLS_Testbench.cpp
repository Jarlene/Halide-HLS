#include <iostream>
#include <limits>

#include "CodeGen_CatapultHLS_Testbench.h"
#include "CodeGen_Internal.h"
#include "Substitute.h"
#include "IROperator.h"
#include "Param.h"
#include "Var.h"
#include "Lerp.h"
#include "Simplify.h"

namespace Halide {
namespace Internal {

using std::ostream;
using std::endl;
using std::string;
using std::vector;
using std::pair;
using std::map;

class CatapultHLS_Closure : public Closure {
public:
    CatapultHLS_Closure(Stmt s)  {
        s.accept(this);
    }

    vector<CatapultHLS_Argument> arguments(const Scope<CodeGen_CatapultHLS_Base::Stencil_Type> &scope);

protected:
    using Closure::visit;

};


vector<CatapultHLS_Argument> CatapultHLS_Closure::arguments(const Scope<CodeGen_CatapultHLS_Base::Stencil_Type> &streams_scope) {
    vector<CatapultHLS_Argument> res;
    for (const pair<string, Buffer> &i : buffers) {
        debug(0) << "buffer: " << i.first << " " << i.second.size;
        if (i.second.read) debug(0) << " (read)";
        if (i.second.write) debug(0) << " (write)";
        debug(0) << "\n";
        //Array as passing arguments
        CodeGen_CatapultHLS_Base::Stencil_Type stype = {};
        stype.type = CodeGen_CatapultHLS_Base::Stencil_Type::StencilContainerType::Array;
        res.push_back({i.first, false, Expr(0), i.second.type, stype});
    }
    //internal_assert(buffers.empty()) << "we expect no references to buffers in a hw pipeline.\n";
    for (const pair<string, Type> &i : vars) {
        debug(3) << "var: " << i.first << "\n";
        if(ends_with(i.first, ".stream") ||
           ends_with(i.first, ".stencil") ) {
            CodeGen_CatapultHLS_Base::Stencil_Type stype = streams_scope.get(i.first);
            res.push_back({i.first, true, Expr(0), Type(), stype});
        } else if (ends_with(i.first, ".stencil_update")) {
            internal_error << "we don't expect to see a stencil_update type in CatapultHLS_Closure.\n";
        } else {
            // it is a scalar variable
            res.push_back({i.first, false, Expr(0), i.second, CodeGen_CatapultHLS_Base::Stencil_Type()});
        }
    }
    return res;
}

namespace {
const string hls_headers =
    "#include <hls_stream.h>\n"
    "#include \"Stencil.h\"\n"
    "#include \"hls_target.h\"\n";
}

CodeGen_CatapultHLS_Testbench::CodeGen_CatapultHLS_Testbench(ostream &tb_stream,
                                             Target target,
                                             OutputKind output_kind)
    : CodeGen_CatapultHLS_Base(tb_stream, target, output_kind, ""),
      cg_target("hls_target", target) {
    cg_target.init_module();

    stream << hls_headers;
}

CodeGen_CatapultHLS_Testbench::~CodeGen_CatapultHLS_Testbench() {
}

void CodeGen_CatapultHLS_Testbench::visit(const ProducerConsumer *op) {
    if (op->is_producer && starts_with(op->name, "_hls_target.")) {
        Stmt hw_body = op->body;

        debug(1) << "compute the closure for hardware pipeline "
                 << op->name << '\n';
        CatapultHLS_Closure c(hw_body);
        vector<CatapultHLS_Argument> args = c.arguments(stencils);

        for(size_t i = 0; i < args.size(); i++) {
            if(init_scope.contains(args[i].name)){
                args[i].size = init_scope.get(args[i].name);
            }
        }

        // generate CatapultHLS target code using the child code generator
        string ip_name = unique_name("hls_target");
        cg_target.add_kernel(hw_body, ip_name, args);

        // emits the target function call
        do_indent();
        stream << ip_name << "("; // avoid starting with '_'
        for(size_t i = 0; i < args.size(); i++) {
            stream << print_name(args[i].name);
            if(i != args.size() - 1)
                stream << ", ";
        }
        stream <<");\n";
    } else {
        CodeGen_CatapultHLS_Base::visit(op);
    }
}

void CodeGen_CatapultHLS_Testbench::visit(const Call *op) {
    if (op->name == "stream_subimage") {
        std::ostringstream rhs;
        // add intrinsic functions to convert memory buffers to streams
        // syntax:
        //   stream_subimage(direction, buffer_var, stream_var, address_of_subimage_origin,
        //                   dim_0_stride, dim_0_extent, ...)
        internal_assert(op->args.size() >= 6 && op->args.size() <= 12);
        const StringImm *direction = op->args[0].as<StringImm>();
        string a1 = print_expr(op->args[1]);
        string a2 = print_expr(op->args[2]);
        string a3 = print_expr(op->args[3]);
        if (direction->value == "buffer_to_stream") {
            rhs << "subimage_to_stream(";
        } else if (direction->value == "stream_to_buffer") {
            rhs << "stream_to_subimage(";
        } else {
            internal_error;
        }
        rhs << a1 << ", " << a2 << ", " << a3;
        for (size_t i = 4; i < op->args.size(); i++) {
            rhs << ", " << print_expr(op->args[i]);
        }
        rhs <<");\n";

        do_indent();
        stream << rhs.str();

        id = "0"; // skip evaluation
    } else if (op->name == "buffer_to_stencil") {
        internal_assert(op->args.size() == 2);
        // add a suffix to buffer var, in order to be compatible with CodeGen_C
        string a0 = print_expr(op->args[0]);
        string a1 = print_expr(op->args[1]);
        do_indent();
        stream << "buffer_to_stencil(" << a0 << ", " << a1 << ");\n";
        id = "0"; // skip evaluation
    } else if (op->name == "address_of") {
        std::ostringstream rhs;
        const Load *l = op->args[0].as<Load>();
        internal_assert(op->args.size() == 1 && l);
        rhs << "(("
            << print_type(l->type.element_of()) // index is in elements, not vectors.
            << " *)"
            << print_name(l->name)
            << " + "
            << print_expr(l->index)
            << ")";
        print_assignment(op->type, rhs.str());
    } else if (op->name == Call::buffer_init){
        const Variable* buffer_memory = op->args[0].as<Variable>();
        const Call* buffer_shape = op->args[8].as<Call>();
        if(buffer_memory && buffer_shape && buffer_shape->name==Call::make_struct){ 
            string buffer_name = buffer_memory->name;
            vector<Expr> shape = buffer_shape->args;
            Expr total_size = Expr(1);
            int dims = shape.size() / 4;
            for(int i=0; i<dims; i++){
                Expr dim_extent = shape[4*i+1];
                total_size = simplify(Mul::make(total_size, dim_extent));
            }
            init_scope.push(buffer_name.substr(0, buffer_name.find(".buffer")), total_size);
            debug(0) << "Buffer Init: " << buffer_name.substr(0, buffer_name.find(".buffer")) << " " << total_size << "\n";
        }else{
            CodeGen_CatapultHLS_Base::visit(op);
        }
    } else {
        CodeGen_CatapultHLS_Base::visit(op);
    }
}

void CodeGen_CatapultHLS_Testbench::visit(const Allocate *op) {
    
    Expr total_size = Expr(1);
    for (auto &dim_extent : op->extents) {
        total_size = simplify(Mul::make(total_size, dim_extent));
    }
    init_scope.push(op->name, total_size);
    CodeGen_CatapultHLS_Base::visit(op);
    
}

void CodeGen_CatapultHLS_Testbench::visit(const Realize *op) {
    if (ends_with(op->name, ".stream")) {
        // create a AXI stream type
        internal_assert(op->types.size() == 1);
        allocations.push(op->name, {op->types[0]});
        Stencil_Type stream_type({Stencil_Type::StencilContainerType::AxiStream,
                    op->types[0], op->bounds, 1});
        stencils.push(op->name, stream_type);

        // emits the declaration for the stream
        do_indent();
        stream << print_stencil_type(stream_type) << ' ' << print_name(op->name) << ";\n";
        stream << print_stencil_pragma(op->name);

        // traverse down
        op->body.accept(this);

        // We didn't generate free stmt inside for stream type
        allocations.pop(op->name);
        stencils.pop(op->name);
    } else {
        CodeGen_CatapultHLS_Base::visit(op);
    }
}

void CodeGen_CatapultHLS_Testbench::visit(const Block *op) {
    // emit stream_to_buffer call after the bulk of IR containing hardware pipeline
    // This is ugly right now, as the CatapultHLS simulation model and DMA programming model
    // are different on the order of pipeline IR and stream_to_buffer call..
    const Evaluate *eval = op->first.as<Evaluate>();
    if (!eval) {
        CodeGen_CatapultHLS_Base::visit(op);
        return;
    }
    const Call *call = eval->value.as<Call>();
    if (!call) {
        CodeGen_CatapultHLS_Base::visit(op);
        return;
    }
    if (call->name == "stream_subimage") {
        const StringImm *direction = call->args[0].as<StringImm>();
        if (direction->value == "stream_to_buffer") {
            internal_assert(op->rest.defined());
            op->rest.accept(this);
            op->first.accept(this);
            return;
        }
    }
    CodeGen_CatapultHLS_Base::visit(op);
    return;
}

}
}