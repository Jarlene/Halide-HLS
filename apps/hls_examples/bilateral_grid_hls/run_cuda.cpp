#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <math.h>

#include <fcntl.h>
#include <unistd.h>

#include "benchmark.h"
#include "halide_image.h"
#include "halide_image_io.h"

#include "pipeline_native.h"
#include "pipeline_cuda.h"

using namespace Halide::Tools;

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: ./run input.png output.png\n");
        return 0;
    }

    int iter = 5;
    Image<uint8_t> input = load_image(argv[1]);
    Image<uint8_t> out_native(1024, 1024, 1);
    Image<uint8_t> out_zynq(1024, 1024, 1);
    Image<uint8_t> out_cuda(1024, 1024, 1);

    printf("\nstart timing code...\n");

    // Timing code. Timing doesn't include copying the input data to
    // the gpu or copying the output back.
    double min_t = benchmark(iter, 10, [&]() {
        pipeline_native(input, out_native);
      });
    printf("CPU program runtime: %g\n", min_t * 1e3);
    save_image(out_native, argv[2]);

    // Timing code. Timing doesn't include copying the input data to
    // the gpu or copying the output back.
    double min_t3 = benchmark(iter, 10, [&]() {
        pipeline_cuda(input, out_cuda);
      });
    printf("CUDA program runtime: %g\n", min_t3 * 1e3);
    save_image(out_cuda, "out_cuda.png");

    return 0;
}