//
// Created by wangrl on 2022/6/7.
//

#include <cassert>
#include <fstream>
#include <vector>
#include "glog/logging.h"
#include "sox.h"

int main(int argc, char* argv[]) {
    // Initialize Google’s logging library.
    google::InitGoogleLogging(argv[0]);
    fLI::FLAGS_stderrthreshold = google::INFO;

    assert(sox_init() == SOX_SUCCESS);

    std::ifstream in;
    in.open("/home/wangrl/github/TimeStretch/data", std::ios::binary);

    std::vector<char> data;

    if (in.is_open()) {
        LOG(INFO) << "Read data success";
    }

    char c;
    while (in.read(&c, sizeof(c))) {
        data.push_back(c);
    }

    LOG(INFO) << "Data size " << data.size();

    // byte为单位
    assert(data.size() == 104960);

    int dataSize = 104960;

    std::ofstream outStream("data.out",
                            std::ios::out | std::ios::binary);

    for (const char& c : data) {
        outStream.write(&c, sizeof c);
    }



    sox_quit();

    return 0;
}
