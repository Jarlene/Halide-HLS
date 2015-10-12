#include "Halide.h"
#include <stdio.h>

using namespace Halide;

Var x("x"), y("y"), z("z"), c("c");
Var x_grid("x_grid"), y_grid("y_grid"), xo("xo"), yo("yo"), x_in("x_in"), y_in("y_in");
uint8_t r_sigma = 32;
int s_sigma = 8;

class MyPipeline {
public:
    ImageParam input;
    RDom r;
    Func clamped, input2;
    Func histogram, downsampled;
    Func blurx, blury, blurz;
    Func output, hw_output;
    std::vector<Argument> args;

    MyPipeline()
        : input(UInt(8), 2), r(0, s_sigma, 0, s_sigma),
          input2("input2"), histogram("histogram"), downsampled("downsampled"),
          blurx("blurx"), blury("blury"), blurz("blurz"),
          output("output"), hw_output("hw_output")
    {
        // Add a boundary condition
        clamped = BoundaryConditions::repeat_edge(input);

        // Construct the bilateral grid
        Expr val = clamped(x * s_sigma + r.x - s_sigma/2, y * s_sigma + r.y - s_sigma/2);
        val = clamp(val, 0, 255);
        Expr zi = cast<int>((val + r_sigma/2) / r_sigma);
        histogram(x, y, z, c) = cast<uint16_t>(0);
        histogram(x, y, zi, c) += select(c == 0, val/16, 64/16);

        // Blur the grid using a five-tap filter
        blurz(x, y, z, c) = cast<uint16_t>(histogram(x, y, z-2, c) +
                             histogram(x, y, z-1, c)*4 +
                             histogram(x, y, z  , c)*6 +
                             histogram(x, y, z+1, c)*4 +
                             histogram(x, y, z+2, c)) / 16;
        blurx(x, y, z, c) = cast<uint16_t>(blurz(x-2, y, z, c) +
                             blurz(x-1, y, z, c)*4 +
                             blurz(x  , y, z, c)*6 +
                             blurz(x+1, y, z, c)*4 +
                             blurz(x+2, y, z, c)) / 16;
        blury(x, y, z, c) = cast<uint16_t>(blurx(x, y-2, z, c) +
                             blurx(x, y-1, z, c)*4 +
                             blurx(x, y  , z, c)*6 +
                             blurx(x, y+1, z, c)*4 +
                             blurx(x, y+2, z, c)) / 16;


        // Take trilinear samples to compute the output
        input2(x, y) = cast<uint16_t>(clamp(input(x, y), 0, 255));
        zi = cast<int>(input2(x, y) / r_sigma);
        Expr zf = cast<uint16_t>((input2(x, y) % r_sigma)  * (65536 / r_sigma));
        Expr xf = cast<uint16_t>((x % s_sigma) * (65536 / s_sigma));
        Expr yf = cast<uint16_t>((y % s_sigma) * (65536 / s_sigma));
        Expr xi = x/s_sigma;
        Expr yi = y/s_sigma;
        Func interpolated("interpolated");
        interpolated(x, y, c) =
            lerp(lerp(lerp(blury(xi, yi, zi, c), blury(xi+1, yi, zi, c), xf),
                      lerp(blury(xi, yi+1, zi, c), blury(xi+1, yi+1, zi, c), xf), yf),
                 lerp(lerp(blury(xi, yi, zi+1, c), blury(xi+1, yi, zi+1, c), xf),
                      lerp(blury(xi, yi+1, zi+1, c), blury(xi+1, yi+1, zi+1, c), xf), yf), zf);

        // Normalize
        val = interpolated(x, y, 0);
        Expr weight = interpolated(x, y, 1);
        Expr norm_val = clamp(val * 64 / weight, 0, 255);
        hw_output(x, y) = cast<uint8_t>(select(weight == 0, 0, norm_val));
        output(x, y) = hw_output(x, y);

        // The comment constraints and schedules.
        output.bound(x, 0, 1024);
        output.bound(y, 0, 1024);
        output.tile(x, y, xo, yo, x_in, y_in, 256, 256);
        output.tile(x_in, y_in, x_grid, y_grid, x_in, y_in, 8, 8);

        blury.store_at(output, xo).compute_at(output, x_grid).reorder(x, y, z, c);
        blurx.store_at(output, xo).compute_at(output, x_grid).reorder(x, y, z, c);
        blurz.store_at(output, xo).compute_at(output, x_grid).reorder(z, x, y, c);

        histogram.store_at(output, xo).compute_at(output, x_grid).reorder(c, z, x, y);
        histogram.update().reorder(c, r.x, r.y, x, y);

        clamped.store_at(output, xo).compute_at(output, x_grid);
        input2.store_at(output, xo).compute_at(output, x_grid);

        // Arguments
        args = {input};
    }

    void compile_cpu() {
        std::cout << "\ncompiling cpu code..." << std::endl;
        //output.print_loop_nest();

        //output.compile_to_c("pipeline_c.c", args, "pipeline_c");
        //output.compile_to_header("pipeline_c.h", args, "pipeline_c");
        //output.compile_to_lowered_stmt("pipeline_native.ir", args);
        //output.compile_to_lowered_stmt("pipeline_native.ir.html", args, HTML);
        output.compile_to_header("pipeline_native.h", args, "pipeline_native");
        output.compile_to_object("pipeline_native.o", args, "pipeline_native");
    }


    void compile_hls() {
        std::cout << "\ncompiling HLS code..." << std::endl;

        hw_output.store_at(output, xo).compute_at(output, x_grid);
        hw_output.accelerate_at(output, xo, {clamped, input2});

        // mark func as stream. TODO remove this in user app
        clamped.stream();
        histogram.stream();
        blurz.stream();
        blurx.stream();
        blury.stream();
        input2.stream();
        hw_output.stream();

        //output.print_loop_nest();

        //output.compile_to_lowered_stmt("pipeline_hls.ir", args);
        output.compile_to_lowered_stmt("pipeline_hls.ir.html", args, HTML);
        output.compile_to_hls("pipeline_hls.cpp", args, "pipeline_hls");
        output.compile_to_header("pipeline_hls.h", args, "pipeline_hls");
    }
};

int main(int argc, char **argv) {
    MyPipeline p1;
    p1.compile_cpu();

    MyPipeline p2;
    p2.compile_hls();

    return 0;
}