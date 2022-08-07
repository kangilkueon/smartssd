#include <defns.h>

#include <boost/program_options.hpp>
#include <lz4_p2p_comp.hpp>
#include <vector>

#define MEMORY_SIZE 2U << 31

using namespace std;

struct Options {
  string compress_xclbin;
  unsigned long block_size;
  unsigned num_memory;
  string inputname;
  string filename;
  uint32_t memory_size;
  bool enable_p2p;
} g_options{};


// The default value set as non-P2P, so that design can work for all platforms.
// For P2P enabled platform, user need to manually change this macro value to true.
#ifndef ENABLE_P2P
#define ENABLE_P2P false
#endif

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
    for (uint32_t fid = 0; fid < inFileVec.size(); fid++) {
        std::string inFile_name = inFileVec[fid];
        std::ifstream inFile(inFile_name.c_str(), std::ifstream::binary);
        if (!inFile) {
            std::cout << "Unable to open file";
            exit(1);
        }
        uint32_t input_size = xflz4::get_file_size(inFile);

        std::string outFile_name = outFileVec[fid];

        char* in = (char*)aligned_alloc(4096, input_size);
        inFile.read(in, input_size);
        inVec.push_back(in);
        inSizeVec.push_back(input_size);
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Done ..." << std::endl;
    std::cout << "\n\n";
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Started ..." << std::endl;
    xflz4 xlz(compress_bin, 0, block_size);
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Done ..." << std::endl;
    std::cout << "\n";
    std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P Compression Started ..." << std::endl;
    xlz.compress_in_line_multiple_files(inVec, outFileVec, inSizeVec, enable_p2p);
    std::cout << "\n\n";
    std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P Compression Done ..." << std::endl;
}

int validateFile(std::string& inFile_name, std::string& origFile_name) {
    std::string command = "cmp " + inFile_name + " " + origFile_name;
    int ret = system(command.c_str());
    return ret;
}

void xil_compress_file_list(std::string& file_list, uint32_t block_size, std::string& compress_bin, bool enable_p2p) {
    std::ifstream infilelist_comp(file_list.c_str());
    std::string line_comp;

    std::vector<std::string> inFileList;
    std::vector<std::string> outFileList;
    std::vector<std::string> origFileList;

    while (std::getline(infilelist_comp, line_comp)) {
        std::string orig_file = line_comp;
        std::string out_file = line_comp + ".lz4";
        inFileList.push_back(line_comp);
        origFileList.push_back(orig_file);
        outFileList.push_back(out_file);
    }
    compress_multiple_files(inFileList, outFileList, block_size, compress_bin, enable_p2p);
    std::cout << "\nCompression is successful. No errors found.\n";
    std::cout << std::endl;
}

void xil_compress_file(std::string& file, uint32_t block_size, std::string& compress_bin, bool enable_p2p) {
    std::string line_comp = file.c_str();

    std::vector<std::string> inFileList;
    std::vector<std::string> outFileList;
    std::vector<std::string> origFileList;

    std::string orig_file = line_comp;
    std::string out_file = line_comp + ".lz4";
    inFileList.push_back(line_comp);
    origFileList.push_back(orig_file);
    outFileList.push_back(out_file);
    compress_multiple_files(inFileList, outFileList, block_size, compress_bin, enable_p2p);
    std::cout << "\nCompression is successful. No errors found.\n";
    std::cout << std::endl;
}

size_t getFileSize(const std::string& fileName) {
    ifstream file(fileName, ios::binary | ios::ate);
    if (file) {
        file.seekg(0, ios::end);
        streamsize size = file.tellg();
        return size;
    } else {
        return 0;
    }
}
int main(int argc, char *argv[]) {
  namespace po = boost::program_options;

  po::options_description desc("Options");
  po::positional_options_description g_pos; /* no positional options */

  desc.add_options()("help,h", "Show help")
      ("compress_xclbin", po::value<std::string>()->required(), "Kernel compression bin xclbin file")
      ("num_memory", po::value<unsigned>()->default_value(1), "Number of memory to compress")
      ("inputname", po::value<string>()->required(), "Output file name in ssd")
      ("filename", po::value<string>()->required(), "Output file name in ssd")
      ("memory_size", po::value<uint32_t>()->required(), "Memory size to compress (MB)")
      ("block_size", po::value<unsigned long>()->default_value(BLOCK_SIZE_IN_KB), "Compress block size (KB)")
      ("enable_p2p", po::value<bool>()->default_value(false), "Compress block size (KB)");

  po::variables_map vm;
  po::store(
      po::command_line_parser(argc, argv).options(desc).positional(g_pos).run(),
      vm);

  if (vm.count("help") > 0) {
    std::cout << desc;
    return -1;
  }

  g_options.block_size = vm["block_size"].as<unsigned long>();
  g_options.compress_xclbin = vm["compress_xclbin"].as<string>();
  g_options.num_memory = vm["num_memory"].as<unsigned>();
  g_options.inputname = vm["inputname"].as<string>();
  g_options.filename = vm["filename"].as<string>();
  g_options.memory_size = vm["memory_size"].as<uint32_t>() * (1 << 20);

  vector<string> inVec;
  vector<string> outVec;
  vector<uint32_t> inSizeVec;

  for (unsigned i = 0; i < g_options.num_memory; i++) {
    outVec.push_back(g_options.filename + "." + to_string(i));
    inVec.push_back(g_options.inputname);

    size_t fileSize = getFileSize(g_options.inputname);
    std::cout << "[DEBUG] input file size : " << fileSize << std::endl;
    inSizeVec.push_back(fileSize);
  }

  xil_compress_file(g_options.inputname, g_options.block_size, g_options.compress_xclbin, g_options.enable_p2p);
#if 0
    // "-l" List of Files
    if (!filelist.empty()) {
        std::cout << "\n" << std::endl;
        xil_compress_file_list(filelist, g_options.block_size, g_options.compress_xclbin, enable_p2p);
    }
  std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup "
               "Started ..."
            << std::endl;
  xflz4 xlz(g_options.compress_xclbin, 0, g_options.block_size);
  std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup "
               "Done ..."
            << std::endl;
  std::cout << "\n";
  std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P Compression Started ..."
            << std::endl;
  xlz.compress_in_line_multiple_files(inVec, outVec, inSizeVec, true);
  std::cout << "\n\n";
  std::cout << "\x1B[36m[FPGA LZ4]\033[0m LZ4 P2P Compression Done ..."
            << std::endl;
#endif
}
