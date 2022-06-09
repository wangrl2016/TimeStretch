//
// Created by wang rl on 2022/6/8.
//

#ifdef NDEBUG /* N.B. assert used with active statements so enable always. */
#undef NDEBUG /* Must undef above assert.h or other that might include it. */
#endif

#include "sox.h"
// #include "util.h"
#include <stdio.h>
#include <assert.h>
#include <cstdlib>
#include <fstream>
#include <glog/logging.h>

/* Example of reading and writing audio files stored in memory buffers
 * rather than actual files.
 *
 * Usage: example5 input output
 */

/* Uncomment following line for fixed instead of malloc'd buffer: */
/*#define FIXED_BUFFER */

#if defined FIXED_BUFFER
#define buffer_size 123456
static char buffer[buffer_size];
#endif

int main(int argc, char* argv[]) {
    // Initialize Google’s logging library.
    google::InitGoogleLogging(argv[0]);
    fLI::FLAGS_stderrthreshold = google::INFO;

    sox_effects_chain_t * chain;
    sox_effect_t * e;
    char * args[10];
    sox_signalinfo_t interm_signal; /* @ intermediate points in the chain. */

    static sox_format_t* in, * out; /* input and output files */
#define MAX_SAMPLES (size_t)2048
    sox_sample_t samples[MAX_SAMPLES]; /* Temporary store whilst copying. */
#if !defined FIXED_BUFFER
    char* buffer;
    size_t buffer_size;
#endif
    size_t number_read;
//
//    assert(argc == 3);

    /* All libSoX applications must start by initialising the SoX library */
    assert(sox_init() == SOX_SUCCESS);

    /* Open the input file (with default parameters) */
    assert((in = sox_open_read(argv[1], NULL, NULL, NULL)));
#if defined FIXED_BUFFER
    assert((out = sox_open_mem_write(buffer, buffer_size, &in->signal, NULL, "sox", NULL)));
#else
    assert((out = sox_open_memstream_write(&buffer, &buffer_size, &in->signal, NULL, "wav", NULL)));
#endif

    chain = sox_create_effects_chain(&in->encoding, &out->encoding);

    interm_signal = in->signal; /* NB: deep copy */

    e = sox_create_effect(sox_find_effect("input"));
    args[0] = (char *)in, assert(sox_effect_options(e, 1, args) == SOX_SUCCESS);
    assert(sox_add_effect(chain, e, &interm_signal, &in->signal) == SOX_SUCCESS);
    free(e);

//    e = sox_create_effect(sox_find_effect("tempo"));
////    args[0] = (char*) "1.5";
////    sox_effect_options(e, 1, args) == SOX_SUCCESS;
//
//    assert(sox_add_effect(chain, e, &interm_signal, &in->signal) == SOX_SUCCESS);
//    free(e);
    e = sox_create_effect(sox_find_effect("tempo"));
    char* tempo[] = { "0.8" };
    assert(sox_effect_options(e, 1, tempo) == SOX_SUCCESS);
    assert(sox_add_effect(chain, e, &interm_signal, &in->signal) == SOX_SUCCESS);
    free(e);

    e = sox_create_effect(sox_find_effect("output"));
    args[0] = (char *)out, assert(sox_effect_options(e, 1, args) == SOX_SUCCESS);
    assert(sox_add_effect(chain, e, &interm_signal, &out->signal) == SOX_SUCCESS);
    free(e);

    sox_flow_effects(chain, NULL, NULL);

    sox_delete_effects_chain(chain);

//    int total_read = 0;
//    while ((number_read = sox_read(in, samples, MAX_SAMPLES))) {
//        total_read += int(number_read);
//        assert(sox_write(out, samples, number_read) == number_read);
//    }
    sox_close(out);
    sox_close(in);

    LOG(INFO) << "buffer_size " << buffer_size;

    std::ofstream of("mem_out.wav",
                     std::ios::out | std::ios::binary);
    of.write(buffer, buffer_size *sizeof(char));


//    assert((in = sox_open_mem_read(buffer, buffer_size, NULL, NULL, NULL)));
//    assert((out = sox_open_write(argv[2], &in->signal, NULL, NULL, NULL, NULL)));
//    while ((number_read = sox_read(in, samples, MAX_SAMPLES)))
//        assert(sox_write(out, samples, number_read) == number_read);
//    sox_close(out);
//    sox_close(in);
#if !defined FIXED_BUFFER
    free(buffer);
#endif

    sox_quit();
    return 0;
}