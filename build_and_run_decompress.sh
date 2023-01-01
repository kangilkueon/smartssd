#!/bin/bash

source /opt/xilinx/xrt/setup.sh
source /tools/Xilinx/Vitis/2021.2/settings64.sh 
pushd ./client
make
echo "=== P2P Enable ==="
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=true --input=/mnt/smartssd/test.txt.lz4
popd

