// SPDX-License-Identifier: GPL-3.0-only
// NextOS original lesson: lossless SMOL-V roundtrip and a narrow ESSL100 route.
#include "smolv.h"
#include "spirv_glsl.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <cstring>
int main(int argc,char **argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: translate INPUT.spv OUTPUT.glsl");
        std::ifstream input(argv[1],std::ios::binary);
        if(!input) throw std::runtime_error("input missing");
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});
        if(bytes.size()<20 || bytes.size()>4*1024*1024 || bytes.size()%4)
            throw std::runtime_error("invalid or excessive SPIR-V input");
        smolv::ByteArray compressed;
        if(!smolv::Encode(bytes.data(),bytes.size(),compressed,smolv::kEncodeFlagNone))
            throw std::runtime_error("SMOL-V encode failed");
        std::vector<uint32_t> decoded(bytes.size()/4);
        if(smolv::GetDecodedBufferSize(compressed.data(),compressed.size())!=bytes.size() ||
           !smolv::Decode(compressed.data(),compressed.size(),decoded.data(),bytes.size()) ||
           std::memcmp(bytes.data(),decoded.data(),bytes.size()))
            throw std::runtime_error("SMOL-V lossless roundtrip failed");
        spirv_cross::CompilerGLSL compiler(std::move(decoded));
        auto model=compiler.get_execution_model();
        if(model!=spv::ExecutionModelVertex && model!=spv::ExecutionModelFragment)
            throw std::runtime_error("unsupported execution model for this GLES2 lesson");
        auto options=compiler.get_common_options();options.version=100;options.es=true;
        compiler.set_common_options(options);
        std::string result=compiler.compile();
        if(result.find("#version 100")==std::string::npos)
            throw std::runtime_error("expected ESSL100 output");
        std::ofstream output(argv[2]);output<<result;
        if(!output) throw std::runtime_error("output failure");
        std::cout<<"PASS: lossless SMOL-V roundtrip; ESSL100 emitted; physical validation not performed\n";
        return 0;
    } catch(const std::exception &error) {
        std::cerr<<"TRANSLATION REJECTED: "<<error.what()<<"\n";return 2;
    }
}
