#ifndef SERVICE_BASE_AUDIO_UTILS_H_
#define SERVICE_BASE_AUDIO_UTILS_H_

#include<fstream>
#include<string.h>
#include<sstream>
#include<typeinfo>
#include <tuple>
#include "sox.h"

namespace WL::Service::Base {

void writeWAVHeader(
    char* buffer,
    size_t buffersize,
    int sampleRate,
    short channels);

template <typename T>
void write(std::ofstream& stream, const T& t) {
  stream.write((const char*)&t, sizeof(T));
}

template <typename T>
void writeFormat(std::ofstream& stream) {
  write<short>(stream, 1);
}

template <>
void writeFormat<float>(std::ofstream& stream);

template <typename SampleType>
void writeWAVData(
  char const* outFile,
  SampleType* buf,
  size_t bufSize,
  int sampleRate,
  short channels)
{
    std::ofstream stream(outFile, std::ios::binary);
    stream.write("RIFF", 4);
    write<int>(stream, 36 + bufSize);
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);
    write<int>(stream, 16);
    writeFormat<SampleType>(stream);                                // Format
    write<short>(stream, channels);                                 // Channels
    write<int>(stream, sampleRate);                                 // Sample Rate
    write<int>(stream, sampleRate * channels * sizeof(SampleType)); // Byterate
    write<short>(stream, channels * sizeof(SampleType));            // Frame size
    write<short>(stream, 8 * sizeof(SampleType));                   // Bits per sample
    stream.write("data", 4);
    stream.write((const char*)&bufSize, 4);
    stream.write((const char*)buf, bufSize);
}
// insert 44 bytes WAV header to the front
void* newBufferWithWAVHeader(const void *data, size_t size);
// used as callback function to write audio result
typedef size_t (*writeaudio_t)(const void *data, size_t size, void *context, std::string speaker, std::string phones, std::string text, std::string filetype, std::vector<std::tuple<std::string, int, int>> sox, std::string lipsync, bool islast, void *utt);

typedef struct snd_part {
  size_t offset;
  size_t length;
  size_t startms;
  size_t timems;
  int padms;
  int breakms;
  int phonecount;
  std::vector<std::string> soxlist;

  snd_part(size_t off, size_t len, size_t start, size_t time, int phcnt, std::vector<std::string> &sox)
  {
    offset = off;
    length = len;
    startms = start;
    timems = time;
    breakms = 0;
    phonecount = phcnt;
    soxlist = sox;
  }
} snd_part;

typedef struct snd_file {
	void *buffer;
  size_t offset;
	size_t size;
  size_t timems;
  std::vector<snd_part> parts;
} snd_file;

void* process_sox_decode_wav(const void *data, size_t size, const char* sourcefiletype, size_t *outsize);
snd_file process_sox_chain_list_type(std::vector<std::tuple<std::string, int, int>> &soxlist, const void *data, size_t size, const char* filetype, const char* sourcefiletype);
snd_file process_sox_chain_list(std::vector<std::tuple<std::string, int, int>> &soxlist, const void *data, size_t size, const char* filetype);
//snd_file process_sox_chain(std::string sox, const void *data, size_t size, const char* filetype);
}
#endif