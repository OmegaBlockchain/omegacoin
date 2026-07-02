#!/usr/bin/env python3
# Copyright (c) 2026 The Omega Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test the phantom-masternode purge (PHANTOM_MN_REMEDIATION_PLAN.md):
#  - the one-time ban of excluded proTxHashes at -phantompurgeheight
#  - permanent exclusion: ProUpServTx for a listed proTxHash is rejected from the
#    purge height onward (bad-protx-phantom-banned), while it is still accepted
#    before the purge height (the rule is height-gated)
#  - non-listed masternodes are untouched and can still be revived via ProUpServTx
#  - banned masternodes drop out of payment selection
#  - the purge replays deterministically across invalidateblock/reconsiderblock
#    and across a full -reindex
#
# Note: the ProUpServTx rejection is exercised via the mempool here. Block-level
# enforcement runs through the identical CheckProUpServTx call (CheckSpecialTx is
# shared between mempool acceptance and ProcessSpecialTxsInBlock), so the mempool
# test plus the -reindex replay covers both paths.
#

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, wait_until


class Masternode(object):
    pass


class PhantomPurgeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.supports_cli = False
        self.extra_args = [["-budgetparams=10:10:10", "-dip3params=135:150"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("funding controller node")
        while node.getbalance() < 10 * 1000:
            node.generate(10)

        self.log.info("activating DIP3")
        node.generate(151 - node.getblockcount())
        assert node.getblockchaininfo()['blocks'] >= 150

        self.log.info("registering masternodes")
        mns = {}
        for i in range(1, 7):
            mns[i] = self.register_fund_mn(node, i, '127.0.0.1:%d' % (30000 + i))
        # mn7: registered with an empty service -> starts PoSe-banned, will be listed
        mns[7] = self.register_fund_mn(node, 7, '')
        node.generate(1)

        for i in range(1, 7):
            assert_equal(self.ban_height(node, mns[i]), -1)
        assert self.ban_height(node, mns[7]) != -1

        purge_height = node.getblockcount() + 20
        self.log.info("restarting with -phantompurgeheight=%d, excluding mn1, mn2, mn7" % purge_height)
        phantom_args = ["-phantompurgeheight=%d" % purge_height,
                        "-phantomexclude=%s" % mns[1].protx_hash,
                        "-phantomexclude=%s" % mns[2].protx_hash,
                        "-phantomexclude=%s" % mns[7].protx_hash]
        self.restart_node(0, extra_args=self.extra_args[0] + phantom_args)

        self.log.info("before the purge height a listed MN can still send ProUpServTx (height gate)")
        assert node.getblockcount() < purge_height
        self.update_service(node, mns[1], '127.0.0.1:%d' % (30000 + 1))
        node.generate(1)
        assert_equal(self.ban_height(node, mns[1]), -1)

        self.log.info("mining to the purge height")
        node.generate(purge_height - node.getblockcount())
        assert_equal(node.getblockcount(), purge_height)

        self.log.info("checking purge result")
        assert_equal(self.ban_height(node, mns[1]), purge_height)
        assert_equal(self.ban_height(node, mns[2]), purge_height)
        assert self.ban_height(node, mns[7]) != -1  # was already banned, stays banned
        for i in range(3, 7):
            assert_equal(self.ban_height(node, mns[i]), -1)

        self.log.info("ProUpServTx for a listed proTxHash is now rejected")
        for i in [1, 2, 7]:
            node.sendtoaddress(mns[i].fundsAddr, Decimal('0.001'))
            node.generate(1)
            assert_raises_rpc_error(-1, 'bad-protx-phantom-banned', node.protx,
                                    'update_service', mns[i].protx_hash,
                                    '127.0.0.1:%d' % (30100 + i), mns[i].blsMnkey, "", mns[i].fundsAddr)

        self.log.info("same-collateral ProRegTx replacement of a listed MN is rejected")
        # Without this rule a listed phantom could be re-registered under a fresh,
        # unlisted proTxHash by a ProRegTx referencing its existing collateral,
        # bypassing the permanent exclusion without ever spending the collateral.
        collateral = node.protx('info', mns[2].protx_hash)
        node.sendtoaddress(mns[2].fundsAddr, Decimal('0.001'))
        node.generate(1)
        new_bls = node.bls('generate')
        new_owner = node.getnewaddress()
        assert_raises_rpc_error(-1, 'bad-protx-phantom-collateral', node.protx,
                                'register', collateral['collateralHash'], collateral['collateralIndex'],
                                '127.0.0.1:30202', new_owner, new_bls['public'], new_owner, 0,
                                node.getnewaddress(), mns[2].fundsAddr)

        self.log.info("ProUpServTx for a non-listed valid MN still works")
        self.update_service(node, mns[3], '127.0.0.1:%d' % (30000 + 3))
        node.generate(1)
        assert_equal(self.ban_height(node, mns[3]), -1)

        self.log.info("a non-listed banned MN can still be revived after the purge")
        mns[8] = self.register_fund_mn(node, 8, '')
        node.generate(1)
        assert self.ban_height(node, mns[8]) != -1
        self.update_service(node, mns[8], '127.0.0.1:%d' % (30000 + 8))
        node.generate(1)
        assert_equal(self.ban_height(node, mns[8]), -1)

        self.log.info("banned MNs drop out of payment selection")
        # the payee of the first post-purge block was selected by the pre-purge list,
        # so skip two blocks before sampling
        node.generate(2)
        paid = set()
        scan_from = node.getblockcount() + 1
        node.generate(20)
        for h in range(scan_from, node.getblockcount() + 1):
            block = node.getblock(node.getblockhash(h), 2)
            for out in block['tx'][0]['vout']:
                for addr in out['scriptPubKey'].get('addresses', []):
                    paid.add(addr)
        for i in [1, 2, 7]:
            assert mns[i].rewards_address not in paid, "banned mn%d was paid" % i
        for i in [3, 4, 5, 6, 8]:
            assert mns[i].rewards_address in paid, "valid mn%d was never paid" % i

        self.log.info("purge replays deterministically across a reorg")
        purge_block_hash = node.getblockhash(purge_height)
        node.invalidateblock(purge_block_hash)
        assert_equal(self.ban_height(node, mns[1]), -1)
        node.reconsiderblock(purge_block_hash)
        assert_equal(self.ban_height(node, mns[1]), purge_height)

        self.log.info("purge replays deterministically across -reindex")
        tip = node.getbestblockhash()
        self.restart_node(0, extra_args=self.extra_args[0] + phantom_args + ["-reindex"])
        wait_until(lambda: node.getbestblockhash() == tip, timeout=120)
        assert_equal(self.ban_height(node, mns[1]), purge_height)
        assert_equal(self.ban_height(node, mns[2]), purge_height)
        for i in range(3, 7):
            assert_equal(self.ban_height(node, mns[i]), -1)
        assert_equal(self.ban_height(node, mns[8]), -1)

        self.log.info("same-collateral ProRegTx replacement of a non-listed MN still works")
        collateral = node.protx('info', mns[6].protx_hash)
        node.sendtoaddress(mns[6].fundsAddr, Decimal('0.001'))
        node.generate(1)
        new_bls = node.bls('generate')
        new_owner = node.getnewaddress()
        replacement_hash = node.protx('register', collateral['collateralHash'],
                                      collateral['collateralIndex'], '127.0.0.1:30206',
                                      new_owner, new_bls['public'], new_owner, 0,
                                      node.getnewaddress(), mns[6].fundsAddr)
        node.generate(1)
        assert_equal(node.protx('info', replacement_hash)['state']['PoSeBanHeight'], -1)
        # the replaced MN is gone from the deterministic list
        assert_raises_rpc_error(-8, None, node.protx, 'info', mns[6].protx_hash)

    def register_fund_mn(self, node, idx, service):
        mn = Masternode()
        mn.idx = idx
        blsKey = node.bls('generate')
        mn.fundsAddr = node.getnewaddress()
        mn.ownerAddr = node.getnewaddress()
        mn.operatorAddr = blsKey['public']
        mn.votingAddr = mn.ownerAddr
        mn.blsMnkey = blsKey['secret']
        mn.collateral_address = node.getnewaddress()
        mn.rewards_address = node.getnewaddress()

        node.sendtoaddress(mn.fundsAddr, Decimal('1000.001'))
        node.generate(1)
        mn.protx_hash = node.protx('register_fund', mn.collateral_address, service, mn.ownerAddr,
                                   mn.operatorAddr, mn.votingAddr, 0, mn.rewards_address, mn.fundsAddr)
        return mn

    def update_service(self, node, mn, service):
        node.sendtoaddress(mn.fundsAddr, Decimal('0.001'))
        node.generate(1)
        node.protx('update_service', mn.protx_hash, service, mn.blsMnkey, "", mn.fundsAddr)

    def ban_height(self, node, mn):
        return node.protx('info', mn.protx_hash)['state']['PoSeBanHeight']


if __name__ == '__main__':
    PhantomPurgeTest().main()
