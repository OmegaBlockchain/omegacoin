// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <evo/phantom_exclusions.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <set>

BOOST_AUTO_TEST_SUITE(evo_phantom_exclusions_tests)

BOOST_FIXTURE_TEST_CASE(exclusion_set_integrity, BasicTestingSetup)
{
    const auto& exclusions = GetPhantomExclusions();

    // The frozen mainnet list holds exactly the 285 phantoms from
    // phantom_probe_evidence.tsv. std::set deduplicates silently, so a wrong
    // size here would mean a duplicated (i.e. missing) entry in the source.
    BOOST_CHECK_EQUAL(exclusions.size(), 285U);

    // No null entry (uint256S of a malformed literal yields zero).
    BOOST_CHECK(exclusions.count(uint256()) == 0);

    // The 5 masternodes confirmed reachable in the Step 0 probe must never be
    // listed (see PHANTOM_MN_REMEDIATION_PLAN.md).
    const std::set<uint256> reachable{
        uint256S("67e38c78477a6488adfe0f04be0b1501a06660a61b270d0d76dfc103236d5372"), // 149.102.158.28
        uint256S("bcc76445f94b3dd9f3ec0bdbccbdc3f62b5efa3334e1d5a583d75ae8b3f9d8f0"), // 167.86.91.191
        uint256S("24897ef4cb71442c8e01e520ce10560f8178db833cc338cd8f62ce4e972429a4"), // 173.212.254.163
        uint256S("ee02bf91b6a9887fad8216a56a32ea72614baab43e4a6c8427300818a8dfb7b1"), // 185.62.81.131
        uint256S("52de5fb0f79c0316883faac0aac3e2d82530745b580be6902957fe173cb37fff"), // 207.180.202.182
    };
    for (const auto& hash : reachable) {
        BOOST_CHECK(!IsPhantomExcluded(hash));
    }

    // Spot-check membership through the lookup helper.
    BOOST_CHECK(IsPhantomExcluded(uint256S("cdb7ab0886214437daff63484fff52ffcb05d60d347c29d050144290ffa53a38"))); // 152.67.67.100
    BOOST_CHECK(IsPhantomExcluded(uint256S("ccc48a0acf903eb7ea4d283355e6eff69a33850f9dedad64e31526f875c394ef"))); // 93.226.251.3
}

BOOST_AUTO_TEST_SUITE_END()
