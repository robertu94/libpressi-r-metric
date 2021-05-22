# LibPressio R Metrics

This is a LibPressio Metrics module that uses RInside to compute metrics using the R programming language

## Using LibPressio R Metrics

Here is a minimal example that uses LibPressio R Metrics to compute the histogram bins
for the error distribution:

```c
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <libpressio.h>

// provides input function, found in ./test
#include "make_input_data.h"


int
main(int argc, char* argv[])
{
  // get a handle to a compressor
  struct pressio* library = pressio_instance();
  struct pressio_compressor* compressor = pressio_get_compressor(library, "sz");

  // configure metrics
  const char* metrics[] = { "rcpp" };
  struct pressio_metrics* metrics_plugin =
    pressio_new_metrics(library, metrics, 1);
  pressio_compressor_set_metrics(compressor, metrics_plugin);

  // configure the compressor
  struct pressio_options* sz_options =
    pressio_compressor_get_options(compressor);

  pressio_options_set_string(sz_options, "sz:error_bound_mode_str", "abs");
  pressio_options_set_double(sz_options, "sz:abs_err_bound", 1e-4);
  pressio_options_set_string(sz_options, "rcpp:script",
    "hist_data <- hist(abs(in0 - out0), plot=FALSE)\n"
    "counts <- hist_data$counts\n"
    "breaks <- hist_data$breaks\n"
  );
  const char* outs[] = {"counts", "breaks"};
  pressio_options_set_strings(sz_options, "rcpp:outputs", 2, outs);
  if (pressio_compressor_check_options(compressor, sz_options)) {
    printf("%s\n", pressio_compressor_error_msg(compressor));
    exit(pressio_compressor_error_code(compressor));
  }
  if (pressio_compressor_set_options(compressor, sz_options)) {
    printf("%s\n", pressio_compressor_error_msg(compressor));
    exit(pressio_compressor_error_code(compressor));
  }

  // load a 300x300x300 dataset into data created with malloc
  double* rawinput_data = make_input_data();
  size_t dims[] = { 300, 300, 300 };
  struct pressio_data* input_data =
    pressio_data_new_move(pressio_double_dtype, rawinput_data, 3, dims,
                          pressio_data_libc_free_fn, NULL);

  // creates an output dataset pointer
  struct pressio_data* compressed_data =
    pressio_data_new_empty(pressio_byte_dtype, 0, NULL);

  // configure the decompressed output area
  struct pressio_data* decompressed_data =
    pressio_data_new_empty(pressio_double_dtype, 3, dims);

  // compress the data
  if (pressio_compressor_compress(compressor, input_data, compressed_data)) {
    printf("%s\n", pressio_compressor_error_msg(compressor));
    exit(pressio_compressor_error_code(compressor));
  }

  // decompress the data
  if (pressio_compressor_decompress(compressor, compressed_data,
                                    decompressed_data)) {
    printf("%s\n", pressio_compressor_error_msg(compressor));
    exit(pressio_compressor_error_code(compressor));
  }

  // get the compression ratio
  struct pressio_options* metric_results =
    pressio_compressor_get_metrics_results(compressor);

  const char* msg;
  if(pressio_options_get_string(metric_results, "rcpp:error", &msg)) {
    printf("failed to get error\n");
    exit(1);
  }
  if(strlen(msg) > 0) {
    printf("error: %s\n", msg);
  }
  free((char*)msg);

  pressio_data *counts = pressio_data_new_empty(pressio_byte_dtype, 0, nullptr), *breaks = pressio_data_new_empty(pressio_byte_dtype, 0, nullptr);
  if (pressio_options_get_data(metric_results, "rcpp:results:counts",
                                 &counts)) {
    printf("failed to get counts\n");
    exit(1);
  }
  if (pressio_options_get_data(metric_results, "rcpp:results:breaks",
                                 &breaks)) {
    printf("failed to get breaks\n");
    exit(1);
  }
  
  size_t count_dims = pressio_data_get_dimension(counts, 0);
  size_t break_dims = pressio_data_get_dimension(breaks, 0);
  int* counts_ptr = (int*)(pressio_data_ptr(counts, nullptr));
  double* breaks_ptr = (double*)(pressio_data_ptr(breaks, nullptr));
  assert(count_dims + 1 == break_dims);

  for (size_t i = 0; i < count_dims; ++i) {
    printf("%0.6lg - %0.6lg : %d\n", breaks_ptr[i], breaks_ptr[i+1], counts_ptr[i]);
  }

  // free the input, decompressed, and compressed data
  pressio_data_free(decompressed_data);
  pressio_data_free(compressed_data);
  pressio_data_free(input_data);
  pressio_data_free(breaks);
  pressio_data_free(counts);

  // free options and the library
  pressio_options_free(sz_options);
  pressio_options_free(metric_results);
  pressio_metrics_free(metrics_plugin);
  pressio_compressor_release(compressor);
  pressio_release(library);
  return 0;
}

```

You must have libpressio, Rcpp, and RInside installed.

Simply compile and link against `libpressio` and `libpressio_r_metric`.  This can be done using pkg-config or CMake

```Makefile
CFLAGS=$(shell pkg-config --cflags libpressio)\
       $(shell pkg-config --cflags libpressio_r_metric)\
LDFLAGS=$(shell pkg-config --libs libpressio)\
       $(shell pkg-config --libs libpressio_r_metric)

myexample:
```
