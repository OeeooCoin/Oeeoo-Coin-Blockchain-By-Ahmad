// SPDX-License-Identifier: MIT
// Oeeoo Coin — BIP39 mnemonics (Milestone 8).
// 24-word mnemonics by default (256-bit entropy). The wordlist is the
// official BIP39 English list (2048 words, embedded in
// bip39_words.h; source text kept at src/wallet/bip39_english.txt).
// Seed derivation is PBKDF2-HMAC-SHA512, 2048 iterations, salt
// "mnemonic" + passphrase — exactly the BIP39 rule, so an Oeeoo
// mnemonic restores in any BIP39 tool (and vice versa).
#pragma once

#include <string>
#include <vector>

namespace oeeoo {

// Generate a fresh mnemonic from OS randomness. `entropyBits` must be
// 128, 160, 192, 224 or 256 (12/15/18/21/24 words). Default 256.
std::string GenerateMnemonic(int entropyBits = 256);
// Mnemonic for given entropy bytes (16/20/24/28/32 bytes). Empty on
// bad length.
std::string MnemonicFromEntropy(const std::vector<unsigned char>& entropy);
// Recover + validate entropy from a mnemonic. False on unknown word,
// wrong word count, or bad checksum.
bool EntropyFromMnemonic(const std::string& mnemonic,
                         std::vector<unsigned char>& entropy);
// 64-byte seed for a mnemonic + optional passphrase.
std::vector<unsigned char> MnemonicToSeed(
    const std::string& mnemonic, const std::string& passphrase = "");
// True when the mnemonic's words and checksum are valid.
bool ValidateMnemonic(const std::string& mnemonic);

}  // namespace oeeoo
