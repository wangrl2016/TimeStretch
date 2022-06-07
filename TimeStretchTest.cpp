//
// Created by wangrl on 2022/6/7.
//

#include <cassert>
#include <fstream>
#include <vector>
#include <tuple>
#include "glog/logging.h"
#include "sox.h"
#include "server_base/audio_utils.h"

using namespace WL::Service::Base;

int main(int argc, char* argv[]) {
    // Initialize Google’s logging library.
    google::InitGoogleLogging(argv[0]);
    fLI::FLAGS_stderrthreshold = google::INFO;

    assert(sox_init() == SOX_SUCCESS);

    std::ifstream in;
    in.open("/home/wangrl/github/TimeStretch/data", std::ios::binary);

    std::vector<char> dataVec;

    if (in.is_open()) {
        LOG(INFO) << "Read data success";
    }

    char c;
    while (in.read(&c, sizeof(c))) {
        dataVec.push_back(c);
    }

    LOG(INFO) << "Data size " << dataVec.size();

    // byte为单位
    assert(dataVec.size() == 104960);

    int dataSize = 104960;

    char* dataArr = (char*) calloc(104960, sizeof(char));

    for (int i = 0; i < dataVec.size(); i++) {
        dataArr[i] = dataVec[i];
    }

    // 对音频流进行处理
    std::vector<std::tuple<std::string, int, int>> soxList;
    std::tuple<std::string, int, int> temp = {"tempo=0.94", dataSize, 65};
    soxList.push_back(temp);

    snd_file sndFile = process_sox_chain_list(soxList,
                                              dataArr,
                                              dataSize,
                                              "mp3");

    LOG(INFO) << "sndFile offset " << sndFile.offset
            << ", size " << sndFile.size
            << ", timems " << sndFile.timems;


    std::ofstream outStream("dataVec.out",
                            std::ios::out | std::ios::binary);

    for (const char& c : dataVec) {
        outStream.write(&c, sizeof c);
    }



    sox_quit();

    return 0;
}
