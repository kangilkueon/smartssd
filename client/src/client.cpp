#include <defns.h>

#include <boost/program_options.hpp>
#include <lz4_p2p_comp.hpp>
#include <lz4_p2p_dec.hpp>
#include <vector>

#define MEMORY_SIZE 2U << 31

using namespace std;

struct Options {
  string xclbin;
  std::vector<std::string> inputFileList;
  bool compress;
  bool enable_p2p;
  bool multiple;
} g_options{};

void compress_multiple_files(const std::vector<std::string>& inFileVec,
                             const std::vector<std::string>& outFileVec,
                             uint32_t block_size,
                             const std::string& compress_bin,
                             bool enable_p2p) {
    std::vector<char*> inVec;
    std::vector<int> fd_p2p_vec;
    std::vector<char*> outVec;
    std::vector<uint32_t> inSizeVec;

    std::cout << "\n\nNumFiles:" << inFileVec.size() << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Started ..." << std::endl;
    uint32_t total_file_size = 0;

    std::chrono::duration<double, std::nano> file_open_time_ns(0);
    for (uint32_t fid = 0; fid < inFileVec.size(); fid++) {
        std::string inFile_name = inFileVec[fid];
        std::ifstream inFile(inFile_name.c_str(), std::ifstream::binary);
        if (!inFile) {
            std::cout << "Unable to open file";
            exit(1);
        }
        uint32_t input_size = xflz4::get_file_size(inFile);
        inFile.close();

        uint64_t input_size_4k_multiple = ((input_size - 1) / (4096) + 1) * 4096;
        inSizeVec.push_back(input_size_4k_multiple);
        total_file_size += input_size;

        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        int fd_p2p_c_in = open(inFile_name.c_str(), O_RDONLY | O_DIRECT);
        if (fd_p2p_c_in <= 0) {
            std::cout << "P2P: Unable to open input file, fd: " << fd_p2p_c_in << std::endl;
            exit(1);
        }
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        file_open_time_ns = file_open_time_ns + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
        fd_p2p_vec.push_back(fd_p2p_c_in);
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Done ..." << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m File open Time : " << file_open_time_ns.count() << " ns" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Total compress size : " << total_file_size << " B" << std::endl;
    std::cout << "\n";
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Started ..." << std::endl;
    xflz4 xlz(compress_bin, 0, block_size);
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Done ..." << std::endl;
    std::cout << "\n";
    std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P Compression Started ..." << std::endl;
    xlz.compress_in_line_multiple_files(fd_p2p_vec, outFileVec, inSizeVec, enable_p2p);
    std::cout << "\n\n";
    std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P Compression Done ..." << std::endl;
}

int validateFile(std::string& inFile_name, std::string& origFile_name) {
    std::string command = "cmp " + inFile_name + " " + origFile_name;
    int ret = system(command.c_str());
    return ret;
}

void xil_compress_file(std::vector<std::string> inFileList, uint32_t block_size, std::string& compress_bin, bool enable_p2p) {
    std::vector<std::string> outFileList;

    for (std::string inFile : inFileList)
    {
        std::string out_file = inFile + ".lz4";
        outFileList.push_back(out_file);
    }

    compress_multiple_files(inFileList, outFileList, block_size, compress_bin, enable_p2p);
    std::cout << "\nCompression is successful. No errors found.\n";
    std::cout << std::endl;
}
void decompress_multiple_files(const std::vector<std::string>& inFileVec,
                               const std::vector<std::string>& outFileVec,
                               const std::string& decompress_bin,
                               bool enable_p2p,
                               uint8_t maxCR) {
    std::vector<char*> outVec;
    std::vector<uint64_t> orgSizeVec;
    std::vector<uint64_t> inSizeVec;
    std::vector<int> fd_p2p_vec;
    std::vector<cl_event> userEventVec;
    uint64_t total_in_size = 0;

    std::chrono::duration<double, std::nano> file_open_time_ns(0);
    std::cout << "\n";
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Started ..." << std::endl;
    for (uint32_t fid = 0; fid < inFileVec.size(); fid++) {
        std::string inFile_name = inFileVec[fid];
        std::ifstream inFile(inFile_name.c_str(), std::ifstream::binary);
        uint32_t input_size = xfLz4::get_file_size(inFile);
        inFile.close();

        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        int fd_p2p_c_in = open(inFile_name.c_str(), O_RDONLY | O_DIRECT);
        if (fd_p2p_c_in <= 0) {
            std::cout << "P2P: Unable to open input file, fd: " << fd_p2p_c_in << std::endl;
            exit(1);
        }
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        file_open_time_ns = file_open_time_ns + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
        std::vector<uint8_t, aligned_allocator<uint8_t> > in_4kbytes(4 * KB);
        read(fd_p2p_c_in, (char*)in_4kbytes.data(), 4 * KB);
        lseek(fd_p2p_c_in, 0, SEEK_SET);
        fd_p2p_vec.push_back(fd_p2p_c_in);
        total_in_size += input_size;
        char* out = (char*)aligned_alloc(4096, maxCR * input_size);
        uint64_t orgSize = maxCR * input_size;
        outVec.push_back(out);
        orgSizeVec.push_back(67108864);
        inSizeVec.push_back(input_size);
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Done ..." << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m File open Time : " << file_open_time_ns.count() << " ns" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Total compress size : " << total_in_size << " B" << std::endl;
    std::cout << "\n";
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Started ..." << std::endl;
    xfLz4 xlz(decompress_bin);
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Done ..." << std::endl;
    std::cout << "\n";
    std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P DeCompression Started ..." << std::endl;
    std::cout << "\n";
    xlz.decompress_in_line_multiple_files(inFileVec, fd_p2p_vec, outVec, orgSizeVec, inSizeVec, enable_p2p);
    std::cout << "\n";
    std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P DeCompression Done ..." << std::endl;
    std::cout << "\n";
    std::cout << "\x1B[31m[Disk Operation]\033[0m Writing Output Files Started ..." << std::endl;
    for (uint32_t fid = 0; fid < inFileVec.size(); fid++) {
        std::string outFile_name = outFileVec[fid];
        std::ofstream outFile(outFile_name.c_str(), std::ofstream::binary);
        outFile.write((char*)outVec[fid], orgSizeVec[fid]);
        close(fd_p2p_vec[fid]);
        outFile.close();
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Writing Output Files Done ..." << std::endl;
}

void xil_decompress_file(std::vector<std::string> inFileList, std::string& decompress_bin, bool enable_p2p, uint8_t maxCR) {
    std::vector<std::string> outFileList;
    std::vector<std::string> orgFileList;

    for (std::string inFile : inFileList)
    {
        std::string out_file = inFile + ".org";
        std::string delimiter = ".lz4";
        std::string token = inFile.substr(0, inFile.find(delimiter));
        orgFileList.push_back(token);
        outFileList.push_back(out_file);
    }
    decompress_multiple_files(inFileList, outFileList, decompress_bin, enable_p2p, maxCR);
    std::cout << std::endl;
    for (size_t i = 0; i < inFileList.size(); i++) {
        auto ret = validateFile(orgFileList[i], outFileList[i]) ? "FAILED: " : "PASSED: ";
        std::cout << ret << inFileList[i] << std::endl;
    }
}

int main(int argc, char *argv[]) {
    namespace po = boost::program_options;

    po::options_description desc("Options");
    po::positional_options_description g_pos; /* no positional options */

    desc.add_options()("help,h", "Show help")
        ("xclbin", po::value<std::string>()->required(), "Kernel compression bin xclbin file")
        ("inputFileList", po::value<vector<string>>()->multitoken(), "input")
        ("compress", po::value<bool>()->default_value(true), "Number of memory to compress")
        ("enable_p2p", po::value<bool>()->default_value(false), "Compress block size (KB)");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    //po::store(po::command_line_parser(argc, argv).options(desc).positional(g_pos).run(), vm);
    notify(vm);

    if (vm.size() == 0 || vm.count("help") > 0)
    {
        std::cout << desc;
        return -1;
    }

    if (vm.count("inputFileList")) {
        g_options.inputFileList = vm["inputFileList"].as<vector< string> >();
    } 
    else 
    {
        std::cout << desc;
        return -1;
    }

    g_options.xclbin = vm["xclbin"].as<string>();
    g_options.compress = vm["compress"].as<bool>();
    g_options.enable_p2p = vm["enable_p2p"].as<bool>();

    if (g_options.compress == true)
    {
        xil_compress_file(g_options.inputFileList, BLOCK_SIZE_IN_KB, g_options.xclbin, g_options.enable_p2p);
    }
    else
    {
        xil_decompress_file(g_options.inputFileList, g_options.xclbin, g_options.enable_p2p, 58);
    }
    return 0;
}
