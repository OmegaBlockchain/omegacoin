Omega coin is a powerful mode-based PoW + PoS-based cryptocurrency.

Block Spacing: 80 Seconds Stake Minimum Age: 8 Hours

Port: 7777 RPC Port: 7778

BUILD LINUX (see the Wiki for dependencies)

git clone https://github.com/omegacoinnetwork/omegacoin

cd omegacoin

./autogen.sh

./configure

make

make install

strip omegacoind

sudo cp omegacoind /usr/local/bin
