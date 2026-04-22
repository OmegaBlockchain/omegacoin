// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/load.h>

#include <coinjoin/client.h>
#include <coinjoin/options.h>
#include <fs.h>
#include <net.h>
#include <interfaces/chain.h>
#include <scheduler.h>
#include <util/string.h>
#include <util/system.h>
#include <util/translation.h>
#include <wallet/salvage.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <univalue.h>

namespace {
bool IsRecoverableWalletLoadError(const bilingual_str& error)
{
    const std::string& message = error.original;
    return message.find("Wallet corrupted") != std::string::npos ||
           message.find("can't open database") != std::string::npos ||
           message.find("BerkeleyDatabase: Error") != std::string::npos ||
           message.find("Failed to open database") != std::string::npos;
}

bool AutoRecoverWallet(const fs::path& wallet_path, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    bilingual_str salvage_error;
    std::vector<bilingual_str> salvage_warnings;
    if (!RecoverDatabaseFile(wallet_path, salvage_error, salvage_warnings)) {
        if (!salvage_error.empty()) {
            error = salvage_error;
        }
        return false;
    }

    warnings.insert(warnings.end(), salvage_warnings.begin(), salvage_warnings.end());
    warnings.push_back(Untranslated(strprintf(
        "Automatically salvaged wallet '%s'. The original file was backed up and a full rescan has been scheduled.",
        wallet_path.string())));
    gArgs.ForceSetArg("-rescan", "2");
    return true;
}
} // namespace

bool VerifyWallets(interfaces::Chain& chain)
{
    if (gArgs.IsArgSet("-walletdir")) {
        fs::path wallet_dir = gArgs.GetArg("-walletdir", "");
        boost::system::error_code error;
        // The canonical path cleans the path, preventing >1 Berkeley environment instances for the same directory
        fs::path canonical_wallet_dir = fs::canonical(wallet_dir, error);
        if (error || !fs::exists(wallet_dir)) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" does not exist"), wallet_dir.string()));
            return false;
        } else if (!fs::is_directory(wallet_dir)) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" is not a directory"), wallet_dir.string()));
            return false;
        // The canonical path transforms relative paths into absolute ones, so we check the non-canonical version
        } else if (!wallet_dir.is_absolute()) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" is a relative path"), wallet_dir.string()));
            return false;
        }
        gArgs.ForceSetArg("-walletdir", canonical_wallet_dir.string());
    }

    LogPrintf("Using wallet directory %s\n", GetWalletDir().string());

    chain.initMessage(_("Verifying wallet(s)...").translated);

    // For backwards compatibility if an unnamed top level wallet exists in the
    // wallets directory, include it in the default list of wallets to load.
    if (!gArgs.IsArgSet("wallet")) {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error_string;
        options.require_existing = true;
        options.verify = false;
        if (MakeWalletDatabase("", options, status, error_string)) {
            gArgs.LockSettings([&](util::Settings& settings) {
                util::SettingsValue wallets(util::SettingsValue::VARR);
                wallets.push_back(""); // Default wallet name is ""
                settings.rw_settings["wallet"] = wallets;
            });
        }
    }

    // Keep track of each wallet absolute path to detect duplicates.
    std::set<fs::path> wallet_paths;

    for (const auto& wallet_file : gArgs.GetArgs("-wallet")) {
        const fs::path path = fs::absolute(wallet_file, GetWalletDir());

        if (!wallet_paths.insert(path).second) {
            chain.initWarning(strprintf(_("Ignoring duplicate -wallet %s."), wallet_file));
            continue;
        }

        DatabaseOptions options;
        DatabaseStatus status;
        options.require_existing = true;
        options.verify = true;
        bilingual_str error_string;
        if (!MakeWalletDatabase(wallet_file, options, status, error_string)) {
            if (status == DatabaseStatus::FAILED_NOT_FOUND) {
                chain.initWarning(Untranslated(strprintf("Skipping -wallet path that doesn't exist. %s\n", error_string.original)));
            } else if (status == DatabaseStatus::FAILED_VERIFY) {
                std::vector<bilingual_str> warnings;
                if (!AutoRecoverWallet(path, error_string, warnings)) {
                    chain.initError(error_string);
                    return false;
                }
                chain.initWarning(Join(warnings, Untranslated("\n")));

                DatabaseStatus retry_status;
                bilingual_str retry_error;
                if (!MakeWalletDatabase(wallet_file, options, retry_status, retry_error)) {
                    chain.initError(retry_error);
                    return false;
                }
            } else {
                chain.initError(error_string);
                return false;
            }
        }
    }

    return true;
}

bool LoadWallets(interfaces::Chain& chain)
{
    try {
        std::set<fs::path> wallet_paths;
        for (const std::string& name : gArgs.GetArgs("-wallet")) {
            if (!wallet_paths.insert(name).second) {
                continue;
            }
            DatabaseOptions options;
            DatabaseStatus status;
            options.require_existing = true;
            options.verify = false; // No need to verify, assuming verified earlier in VerifyWallets()
            bilingual_str error_string;
            std::vector<bilingual_str> warnings;
            bool recovered_wallet = false;
            std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error_string);
            const fs::path wallet_path = fs::absolute(name, GetWalletDir());
            if (!database && status == DatabaseStatus::FAILED_NOT_FOUND) {
                continue;
            }
            if (!database && status == DatabaseStatus::FAILED_VERIFY) {
                if (!AutoRecoverWallet(wallet_path, error_string, warnings)) {
                    chain.initError(error_string);
                    return false;
                }
                recovered_wallet = true;
                database = MakeWalletDatabase(name, options, status, error_string);
            }
            std::shared_ptr<CWallet> pwallet = database ? CWallet::Create(chain, name, std::move(database), options.create_flags, error_string, warnings) : nullptr;
            if (!pwallet && !warnings.empty()) {
                chain.initWarning(Join(warnings, Untranslated("\n")));
                warnings.clear();
            }
            if (!pwallet && !recovered_wallet && IsRecoverableWalletLoadError(error_string)) {
                if (!AutoRecoverWallet(wallet_path, error_string, warnings)) {
                    chain.initError(error_string);
                    return false;
                }
                recovered_wallet = true;
                database = MakeWalletDatabase(name, options, status, error_string);
                pwallet = database ? CWallet::Create(chain, name, std::move(database), options.create_flags, error_string, warnings) : nullptr;
            }
            if (!warnings.empty()) chain.initWarning(Join(warnings, Untranslated("\n")));
            if (!pwallet) {
                chain.initError(error_string);
                return false;
            }
            AddWallet(pwallet);
        }
        return true;
    } catch (const std::runtime_error& e) {
        chain.initError(Untranslated(e.what()));
        return false;
    }
}

void StartWallets(CScheduler& scheduler, const ArgsManager& args)
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets()) {
        pwallet->postInitProcess();
    }

    // Schedule periodic wallet flushes and tx rebroadcasts
    if (args.GetBoolArg("-flushwallet", DEFAULT_FLUSHWALLET)) {
        scheduler.scheduleEvery(MaybeCompactWalletDB, std::chrono::milliseconds{500});
    }
    scheduler.scheduleEvery(MaybeResendWalletTxs, std::chrono::milliseconds{1000});
}

void FlushWallets()
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets()) {
        if (CCoinJoinClientOptions::IsEnabled()) {
            // Stop CoinJoin, release keys
            auto it = coinJoinClientManagers.find(pwallet->GetName());
            it->second->ResetPool();
            it->second->StopMixing();
        }
        pwallet->Flush();
    }
}

void StopWallets()
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets()) {
        pwallet->Close();
    }
}

void UnloadWallets()
{
    auto wallets = GetWallets();
    while (!wallets.empty()) {
        auto wallet = wallets.back();
        wallets.pop_back();
        std::vector<bilingual_str> warnings;
        RemoveWallet(wallet, std::nullopt, warnings);
        UnloadWallet(std::move(wallet));
    }
}
