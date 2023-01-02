#include <defns.h>

#include <boost/program_options.hpp>
#include <SmartSSD.hpp>
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
        Compress compressModule(g_options.xclbin, 0, g_options.enable_p2p, BLOCK_SIZE_IN_KB);
        compressModule.SetInputFileList(g_options.inputFileList);
        compressModule.MakeOutputFileList(g_options.inputFileList);
        compressModule.OpenInputFiles();
        compressModule.OpenOutputFiles();
        compressModule.SetOutputFileSize();

        compressModule.initBuffer();
        compressModule.readFile();
        compressModule.preProcess();
        compressModule.run();
        compressModule.postProcess();
        compressModule.writeFile();
        compressModule.CloseInputFiles();
        compressModule.CloseOutputFiles();
    }
    else
    {
        Decompress decompressModule(g_options.xclbin, 0, g_options.enable_p2p);
        decompressModule.SetInputFileList(g_options.inputFileList);
        decompressModule.MakeOutputFileList(g_options.inputFileList);
        decompressModule.OpenInputFiles();
        decompressModule.OpenOutputFiles();

        decompressModule.initBuffer();
        decompressModule.readFile();
        decompressModule.preProcess();
        decompressModule.run();
        decompressModule.postProcess();
        decompressModule.writeFile();
        decompressModule.CloseInputFiles();
        decompressModule.CloseOutputFiles();
        //xil_decompress_file(g_options.inputFileList, g_options.xclbin, g_options.enable_p2p, 58);
    }
    return 0;
}
