
#include <defns.h>

class SmartSSD {
    public:
        SmartSSD(const std::string& binaryFile, uint8_t device_id, bool p2p_enable);
        ~SmartSSD();

        void SetInputFileList (const std::vector<std::string>& inputFile);
        void SetOutputFileList (const std::vector<std::string>& outputFile);
        void OpenInputFiles();
        void OpenOutputFiles();
        void CloseInputFiles();
        void CloseOutputFiles();

        void initBuffer();
        void readFile(size_t size = 0);
        void writeFile(size_t size = 0);
        
        virtual void preProcess();
        virtual void run();
        virtual void postProcess();
        static uint32_t get_file_size(std::ifstream& file) {
            file.seekg(0, file.end);
            uint32_t file_size = file.tellg();
            file.seekg(0, file.beg);
            return file_size;
        }
    protected:
        cl::Program* m_program;
        cl::Context* m_context;
        cl::CommandQueue* m_q;

        bool m_p2p_enable;
        std::string m_kernal_name[8];
        
        std::vector<std::string> inputFileVec;
        std::vector<std::string> outputFileVec;
        
        std::vector<int> inputFDVec;
        std::vector<uint32_t> inputFileSizeVec;
        
        std::vector<int> outputFDVec;
        std::vector<uint32_t> outputFileSizeVec;
        
        std::vector<cl::Buffer*> bufInputVec;
        std::vector<cl::Buffer*> bufOutputVec;
        
        std::vector<uint8_t*> bufp2pInVec;
        std::vector<uint8_t*> bufp2pOutVec;
        
        // only for Non-P2P
        std::vector<uint8_t*> originalDataInHostVec;
        std::vector<uint8_t*> resultDataInHostVec;
    private:
        std::chrono::duration<double, std::nano> m_input_file_open_time;
        std::chrono::duration<double, std::nano> m_output_file_open_time;
        std::chrono::duration<double, std::nano> m_ssd_read_time;
        std::chrono::duration<double, std::nano> m_ssd_write_time;
        
        uint32_t m_input_file_size;
        uint32_t m_output_file_size;
};