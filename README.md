# smartssd
Example for Samsung smart ssd

./compression-client --xclbin={Compiled XCLBIN}.xclbin  --compress={Compress or Decompress} --input={filename} --enable_p2p={Using P2P or not}


# how to build
Build step
```bash
1. `./cmake .`
2. `make all`
```
# Precondition
File system format, generate sample data
```bash
mkfs.ext4 /dev/nvme0n1
mount /dev/nvme0n1 /mnt/smartssd
cp test.txt /mnt/smartssd
```
