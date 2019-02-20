#!/usr/bin/env python3
# Copyright (c) 2018-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Backwards compatibility functional test

Test various backwards compatibility scenarios. Download the previous node binaries:

mkdir build/releases
contrib/devtools/previous_release.sh -f -b v0.18.0

Due to RPC changes introduced in various versions the below tests
won't work for older versions without some patches or workarounds.

Use only the latest patch version of each release, unless a test specifically
needs an older patch version.
"""

import os
import shutil

from test_framework.test_framework import BitcoinTestFramework, SkipTest

from test_framework.util import (
    assert_equal,
    sync_blocks
)

class BackwardsCompatibilityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        # Add new version after each release:
        self.extra_args = [
            ["-nowallet"], # Pre-release: use to mine blocks
            ["-nowallet"], # Pre-release: use to receive coins, swap wallets, etc
            ["-nowallet"], # v0.18.0
            ["-nowallet"] # v0.17.1
        ]

    def setup_nodes(self):
        if not os.path.isdir("build/releases"):
            raise SkipTest("This test requires binaries for previous releases")

        self.add_nodes(self.num_nodes, extra_args=self.extra_args, versions=[
            None,
            None,
            180000,
            170100
        ], binary=[
            self.options.bitcoind,
            self.options.bitcoind,
            self.config["environment"]["BUILDDIR"] + "/build/releases/v0.18.0/bin/bitcoind",
            self.config["environment"]["BUILDDIR"] + "/build/releases/v0.17.1/bin/bitcoind"
        ], binarycli=[
            self.options.bitcoincli,
            self.options.bitcoincli,
            self.config["environment"]["BUILDDIR"] + "/build/releases/v0.18.0/bin/bitcoin-cli",
            self.config["environment"]["BUILDDIR"] + "/build/releases/v0.17.1/bin/bitcoin-cli"
        ])

        self.start_nodes()

    def run_test(self):
        self.nodes[0].generate(101)

        sync_blocks(self.nodes)

        # Sanity check the test framework:
        res = self.nodes[self.num_nodes - 1].getblockchaininfo()
        assert_equal(res['blocks'], 101)

        node_master = self.nodes[self.num_nodes - 3]
        node_v18 = self.nodes[self.num_nodes - 2]
        node_v17 = self.nodes[self.num_nodes - 1]

        self.log.info("Test wallet backwards compatibility...")
        # Create a number of wallets and open them in older versions:

        # w1: regular wallet, created on master: update this test when default
        #     wallets can no longer be opened by older versions.
        node_master.createwallet(wallet_name="w1")
        wallet = node_master.get_wallet_rpc("w1")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] > 0)

        # w1_v18: regular wallet, created with v0.18
        node_v18.createwallet(wallet_name="w1_v18")
        wallet = node_v18.get_wallet_rpc("w1_v18")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] > 0)

        # w2: wallet with private keys disabled, created on master: update this
        #     test when default wallets private keys disabled can no longer be
        #     opened by older versions.
        node_master.createwallet(wallet_name="w2", disable_private_keys=True)
        wallet = node_master.get_wallet_rpc("w2")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'] == False)
        assert(info['keypoolsize'] == 0)

        # w2_v18: wallet with private keys disabled, created with v0.18
        node_v18.createwallet(wallet_name="w2_v18", disable_private_keys=True)
        wallet = node_v18.get_wallet_rpc("w2_v18")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'] == False)
        assert(info['keypoolsize'] == 0)

        # w3: blank wallet, created on master: update this
        #     test when default blank wallets can no longer be opened by older versions.
        node_master.createwallet(wallet_name="w3", blank=True)
        wallet = node_master.get_wallet_rpc("w3")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] == 0)

        # w3_v18: blank wallet, created with v0.18
        node_v18.createwallet(wallet_name="w3_v18", blank=True)
        wallet = node_v18.get_wallet_rpc("w3_v18")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] == 0)

        # Copy the wallets to older nodes:
        node_master_wallets_dir = os.path.join(node_master.datadir, "regtest/wallets")
        node_v18_wallets_dir = os.path.join(node_v18.datadir, "regtest/wallets")
        node_v17_wallets_dir = os.path.join(node_v17.datadir, "regtest/wallets")
        node_master.unloadwallet("w1")
        node_master.unloadwallet("w2")
        node_v18.unloadwallet("w1_v18")
        node_v18.unloadwallet("w2_v18")

        # Copy wallets to v0.17
        for wallet in os.listdir(node_master_wallets_dir):
            shutil.copytree(
                os.path.join(node_master_wallets_dir, wallet),
                os.path.join(node_v17_wallets_dir, wallet)
            )
        for wallet in os.listdir(node_v18_wallets_dir):
            shutil.copytree(
                os.path.join(node_v18_wallets_dir, wallet),
                os.path.join(node_v17_wallets_dir, wallet)
            )

        # Copy wallets to v0.18
        for wallet in os.listdir(node_master_wallets_dir):
            shutil.copytree(
                os.path.join(node_master_wallets_dir, wallet),
                os.path.join(node_v18_wallets_dir, wallet)
            )

        # Open the wallets in v0.18
        node_v18.loadwallet("w1")
        wallet = node_v18.get_wallet_rpc("w1")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] > 0)

        node_v18.loadwallet("w2")
        wallet = node_v18.get_wallet_rpc("w2")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'] == False)
        assert(info['keypoolsize'] == 0)

        node_v18.loadwallet("w3")
        wallet = node_v18.get_wallet_rpc("w3")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] == 0)

        # Open the wallets in v0.17
        node_v17.loadwallet("w1_v18")
        wallet = node_v17.get_wallet_rpc("w1_v18")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] > 0)

        node_v17.loadwallet("w1")
        wallet = node_v17.get_wallet_rpc("w1")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'])
        assert(info['keypoolsize'] > 0)

        node_v17.loadwallet("w2_v18")
        wallet = node_v17.get_wallet_rpc("w2_v18")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'] == False)
        assert(info['keypoolsize'] == 0)

        node_v17.loadwallet("w2")
        wallet = node_v17.get_wallet_rpc("w2")
        info = wallet.getwalletinfo()
        assert(info['private_keys_enabled'] == False)
        assert(info['keypoolsize'] == 0)

        # RPC loadwallet failure causes bitcoind to exit, in addition to the RPC
        # call failure, so the following test won't work:
        # assert_raises_rpc_error(-4, "Wallet loading failed.", node_v17.loadwallet, 'w3_v18')

        # Instead, we stop node and try to launch it with the wallet:
        self.stop_node(self.num_nodes - 1)
        node_v17.assert_start_raises_init_error(["-wallet=w3_v18"], "Error: Error loading w3_v18: Wallet requires newer version of Bitcoin Core")
        node_v17.assert_start_raises_init_error(["-wallet=w3"], "Error: Error loading w3: Wallet requires newer version of Bitcoin Core")

if __name__ == '__main__':
    BackwardsCompatibilityTest().main()
