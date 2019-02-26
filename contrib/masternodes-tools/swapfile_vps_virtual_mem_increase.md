# SwapFiles (Increasing VPS virtual memory)

## Creating 1GB Swapfile
```
cd /
sudo dd if=/dev/zero of=swapfile bs=1024 count=1048576
sudo mkswap swapfile
sudo swapon swapfile
sudo nano etc/fstab
/swapfile none swap sw 0 0
```
bs=1024k(x1Mo) count=1024 // Swapfile of 1Mo <BR />
bs=1024k(x2Mo) count=2048 // Swapfile of 2Mo <BR />
bs=1024k(x512Mo) count=524288 // Swapfile of 512Mo <BR />
bs=1024k(x1024Mo) count=1048576 // Swapfile of 1Go <BR />
bs=1024k(x2048Mo) count=2097152 // Swapfile of 2Go <BR />
bs=1024k(x4096Mo) count=4194304 // Swapfile of 4Go
