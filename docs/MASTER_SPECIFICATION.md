# OEEOO COIN (OEEOO) — MASTER BLOCKCHAIN DEVELOPMENT PROMPT

Complete Project Specification, Finalized Decisions, Consensus Rules, Architecture, Security Requirements and Development Instructions

> **Rename record (2026-11-05):** This document is the user's original master prompt, re-read in full from the chat and renamed only: EarthCoin → Oeeoo Coin, EARTH → OEEOO, `EarthCoin-blockchain-by-Ahmad` → `OeeooCoin-blockchain-by-Ahmad`, and the network-magic literal string likewise. **Nothing else changed** — every number, rule, schedule and requirement is exactly as originally finalized. The only calculated update is the network-magic candidate in §11.2 / Appendix A (recomputed from the new literal string). Implementation status is tracked in Appendix A and `docs/DECISION_LOG.md`.

Project Name: Oeeoo Coin
Ticker / Symbol: OEEOO
Project Type: Independent decentralized Proof-of-Work blockchain
Development Language: C++
Consensus Model: Bitcoin-style Proof of Work
Hashing Algorithm: SHA-256 / SHA-256d
License: MIT License
Source Code: Public and open-source GitHub repository
Planned Repository: "OeeooCoin-blockchain-by-Ahmad"
Maximum Supply: 21,000,000 OEEOO
Founder Allocation: 1,000,000 OEEOO
Mining Allocation: 20,000,000 OEEOO
Target Block Time: 10 minutes
Mainnet Strategy: Direct Mainnet launch, with no separate public testnet selected.

---

## 1. YOUR ROLE AND MISSION

Act as a senior blockchain protocol architect, C++ systems engineer, cryptography engineer, distributed-systems developer, wallet engineer, mining engineer, network-security specialist and software testing engineer.

Your mission is to help design and implement Oeeoo Coin as a complete, independent, Bitcoin-style blockchain.

Oeeoo Coin must not be an ERC-20 token, BEP-20 token, smart-contract token or cryptocurrency that depends on another blockchain.

Oeeoo Coin must have its own:

- Blockchain and block structure.
- Consensus and Proof-of-Work validation.
- Native OEEOO currency.
- Transaction system.
- UTXO database.
- Cryptographic hashing.
- Wallet and address system.
- Mining and mining-pool support.
- P2P network.
- Independent nodes.
- Peer discovery.
- Difficulty adjustment.
- Transaction-fee system.
- Founder allocation and vesting mechanism.
- Governance and protocol-upgrade mechanism.
- RPC interface.
- Database architecture.
- Release and security-verification process.

The final goal is a genuinely operational, decentralized cryptocurrency network whose independent nodes can validate the same chain without trusting a central server.

### Critical instructions

1. Preserve all finalized Oeeoo Coin decisions in this specification.
2. Never independently change the supply, monetary policy, consensus, block timing, founder allocation, wallet policy, network identity or other finalized protocol rules.
3. Do not replace C++ with Rust, Python or another primary implementation language.
4. Do not convert Oeeoo Coin into a token.
5. Do not introduce a central authority that can arbitrarily create coins, reverse transactions or modify blockchain history.
6. Do not invent missing protocol values and present them as approved decisions.
7. When an engineering detail is unresolved, identify it explicitly, recommend a technically sound implementation and explain its implications.
8. Maintain a permanent technical decision log.
9. Implement the project in modules, not as one enormous unreviewable code file.
10. Write actual compilable code, not merely pseudocode or architectural descriptions.
11. Test every module before integrating it with the next module.
12. Never claim that something has been implemented, tested, audited or completed unless it actually has.
13. Do not promise that software is bug-free or impossible to hack.
14. Do not launch production Mainnet without independent security review and extensive verification.

The user has already made the main policy decisions. Your job is to implement them accurately, not repeatedly ask the user to reconsider them.

---

## 2. PROJECT IDENTITY AND FUNDAMENTAL PRINCIPLES

Oeeoo Coin is an independent cryptocurrency project inspired by Bitcoin's decentralized architecture.

The fundamental principles are:

- Decentralization.
- Public verification.
- Open-source development.
- Proof-of-Work security.
- Fixed maximum supply.
- Transparent monetary policy.
- No arbitrary coin creation.
- No central control over balances.
- No central control over transaction validation.
- No single founder with permanent authority over consensus.
- Independent node operation.
- Public mining.
- Community-based protocol upgrades.

Oeeoo Coin must operate on its own blockchain.

Every full node must independently verify transactions, blocks, proof of work, monetary issuance and consensus rules.

No website, mobile application, wallet company, mining pool, founder or centralized API should be able to override the consensus rules.

The project must remain open source under the MIT License.

The planned GitHub repository name is:

"OeeooCoin-blockchain-by-Ahmad"

Verify repository-name availability when creating it. Do not claim that it has been created or registered before that actually happens.

---

## 3. MONETARY POLICY AND MAXIMUM SUPPLY

This section is a critical consensus specification.

### 3.1 Maximum supply

The maximum possible supply of Oeeoo Coin is:

21,000,000 OEEOO

The supply is divided into exactly two allocations:

| Allocation | Amount |
|---|---|
| Founder allocation | 1,000,000 OEEOO |
| Mining allocation | 20,000,000 OEEOO |
| Total maximum supply | 21,000,000 OEEOO |

The founder allocation is included within the 21 million maximum supply.

It must never be added on top of the mining allocation.

No additional coins may be created beyond these allocations.

### 3.2 Decimal precision

Oeeoo Coin supports eight decimal places.

1 OEEOO = 100,000,000 smallest units.

Define the consensus constant:

"COIN = 100000000"

All monetary calculations must use integer smallest units.

Never use floating-point arithmetic for:

- Block rewards.
- Transaction amounts.
- Supply calculations.
- Fees.
- Wallet balances.
- Founder allocation.
- Vesting.
- Consensus validation.

Use safe integer arithmetic with overflow protection.

### 3.3 Mining issuance

The total mining allocation must be exactly 20 million OEEOO.

The following reward schedule is finalized:

| Block range | Block reward |
|---|---|
| 1–210,000 | 50 OEEOO |
| 210,001–420,000 | 25 OEEOO |
| 420,001–630,000 | 12.5 OEEOO |
| 630,001–840,000 | 6.25 OEEOO |
| 840,001–940,000 | 3.125 OEEOO |
| 940,001 onward | 0 OEEOO |

The first mined block has height 1.

The Genesis block has height 0.

### 3.4 Exact reward arithmetic

Represent the rewards in smallest units:

| Reward | Smallest units |
|---|---|
| 50 OEEOO | 5,000,000,000 |
| 25 OEEOO | 2,500,000,000 |
| 12.5 OEEOO | 1,250,000,000 |
| 6.25 OEEOO | 625,000,000 |
| 3.125 OEEOO | 312,500,000 |

Calculate the total issuance:

- First era: 10,500,000 OEEOO.
- Second era: 5,250,000 OEEOO.
- Third era: 2,625,000 OEEOO.
- Fourth era: 1,312,500 OEEOO.
- Fifth era: 312,500 OEEOO.

Total:

20,000,000 OEEOO exactly.

Block 940,000 completes the mining allocation.

Block 940,001 and every subsequent block must have zero block subsidy.

After subsidy termination, miners can continue earning transaction fees.

Implement the reward schedule as a dedicated, deterministic consensus function.

Create exhaustive unit tests for every reward boundary:

- Height 0.
- Height 1.
- Height 210,000.
- Height 210,001.
- Height 420,000.
- Height 420,001.
- Height 630,000.
- Height 630,001.
- Height 840,000.
- Height 840,001.
- Height 940,000.
- Height 940,001.

Also test that the sum of all scheduled mining issuance is exactly 20 million OEEOO.

No off-by-one errors are acceptable.

### 3.5 Mining after block 940,000

After the final mining subsidy:

- Block subsidy remains zero.
- Transaction fees continue to be available to miners.
- No new monetary issuance is permitted.
- The network must continue validating blocks and transactions.

---

## 4. CONSENSUS AND PROOF OF WORK

Oeeoo Coin uses Proof of Work.

The selected hashing family is SHA-256.

Use Bitcoin-style double SHA-256 (SHA256d) for block identifiers, transaction identifiers and proof-of-work hashing where specified by the protocol.

Formally:

"SHA256d(data) = SHA256(SHA256(data))"

The precise serialization and hashing byte order must be specified and tested.

### 4.1 Consensus requirements

Every full node must independently verify:

- Block header structure.
- Block hash.
- Proof of Work.
- Target difficulty.
- Block height.
- Previous-block linkage.
- Merkle root.
- Transaction validity.
- Coinbase validity.
- Block reward.
- Transaction fees.
- UTXO availability.
- Script execution.
- Double-spend prevention.
- Timestamp rules.
- Difficulty adjustment.
- Maximum block size.
- Supply constraints.

Invalid blocks must be rejected by every correctly operating node.

### 4.2 Block time

Target block interval:

10 minutes.

### 4.3 Difficulty adjustment

Difficulty must adjust every:

2,016 blocks.

Use Bitcoin-style difficulty-retargeting principles.

The implementation must define:

- Target timespan.
- Actual timespan.
- Retarget calculation.
- Maximum adjustment bounds.
- Integer arithmetic.
- Compact target encoding and decoding.
- Invalid-target rejection.
- Overflow and underflow protection.

The exact implementation must be deterministic across all supported platforms.

### 4.4 Genesis difficulty

The initial Genesis difficulty is selected to match Bitcoin's 2009 Genesis difficulty.

"nBits = 0x1d00ffff" is a candidate value to verify in the actual implementation.

Do not blindly copy a Bitcoin Genesis header or hash.

Oeeoo Coin must have its own Genesis block, its own transaction data, its own Merkle root and its own resulting block hash.

---

## 5. BLOCKCHAIN STRUCTURE

Implement an independent blockchain.

The initial block structure should contain Bitcoin-style core fields:

- Version.
- Previous block hash.
- Merkle root.
- Timestamp.
- Compact difficulty target ("nBits").
- Nonce.

Define the block header serialization exactly.

All fields must have fixed, documented sizes and byte order.

Implement:

- Block header.
- Full block.
- Transaction serialization.
- Transaction deserialization.
- Merkle tree.
- Merkle root calculation.
- Block identifier.
- Block validation.
- Block indexing.
- Chain-tip tracking.
- Cumulative chain work.

### 5.1 Genesis block

Genesis block height:

"0"

First mined block height:

"1"

Genesis version:

"1"

Genesis message:

"THINK ABOUT GOD ? IS ONLY ONE ALLAH"

This exact message must appear only in the Genesis block.

Do not automatically repeat it in ordinary blocks, wallet notifications, transactions or other parts of the protocol.

The Genesis timestamp must be the actual UTC date and time chosen at final launch after development is complete.

The Genesis block must be custom to Oeeoo Coin.

Calculate and independently verify:

- Genesis timestamp.
- Version.
- Previous hash.
- Merkle root.
- Difficulty bits.
- Nonce.
- Block hash.

The final Genesis parameters must be recorded in the repository and tested by independent implementations.

Do not claim the Genesis block is finalized before its hash and all parameters have been verified.

---

## 6. FOUNDER ALLOCATION AND AUTOMATIC VESTING

Founder allocation:

1,000,000 OEEOO

This amount is part of the 21 million maximum supply.

It is not part of the 20 million mining reward schedule.

### 6.1 Vesting schedule

The founder allocation is divided into ten equal tranches.

Each tranche:

100,000 OEEOO.

The release schedule is:

| Release date | Amount unlocked | Cumulative unlocked |
|---|---|---|
| Launch | 100,000 | 100,000 |
| First anniversary | 100,000 | 200,000 |
| Second anniversary | 100,000 | 300,000 |
| Third anniversary | 100,000 | 400,000 |
| Fourth anniversary | 100,000 | 500,000 |
| Fifth anniversary | 100,000 | 600,000 |
| Sixth anniversary | 100,000 | 700,000 |
| Seventh anniversary | 100,000 | 800,000 |
| Eighth anniversary | 100,000 | 900,000 |
| Ninth anniversary | 100,000 | 1,000,000 |

All founder coins become unlocked by the ninth anniversary.

### 6.2 Automatic vesting

Vesting must be enforced by protocol-level consensus rules.

It must not depend on a founder manually approving each release.

Locked coins cannot be spent or transferred.

Unlocked coins become freely spendable and transferable.

Use:

- Consensus allocation record.
- Scheduled UTXO activation.
- UTC anniversary calculations.
- Timestamp and block-height conditions.

The selected vesting approach is timestamp plus height validation.

The first valid block satisfying the defined UTC anniversary condition activates the relevant tranche.

### 6.3 Leap-year handling

If an anniversary falls on February 29 in a non-leap year, use:

February 28.

### 6.4 Reorganization safety

The vesting implementation must be safe during chain reorganizations.

Specify exactly how activation state is recalculated when a block is disconnected and another chain becomes active.

Never allow a reorganization to create duplicate founder coins or release the same tranche twice.

Founder allocation must not be treated blindly as an ordinary spendable Genesis coinbase.

Design the consensus allocation mechanism and its UTXO activation carefully.

### 6.5 Founder wallet security

Founder wallet:

2-of-3 multisignature.

Maintain three geographically separate offline recovery backups.

The private keys must be generated securely.

Do not put founder private keys or recovery phrases into public source code, logs, screenshots, repositories or AI prompts.

---

## 7. WALLET AND ADDRESS SYSTEM

Oeeoo Coin requires its own native wallet.

The wallet must support:

- Address generation.
- Address validation.
- Balance tracking.
- Transaction creation.
- Transaction signing.
- Transaction broadcasting.
- Transaction history.
- Fee estimation.
- Backup.
- Recovery.
- Encryption.
- Multisignature.
- Timelocks.
- Multiple addresses.

### 7.1 Address format at launch

The finalized launch choice is:

Legacy P2PKH only.

Do not implement SegWit or Taproot as launch address formats.

They may be considered through a future community-approved protocol upgrade.

Use:

- Base58Check encoding.
- P2PKH address structure.
- secp256k1.
- ECDSA signatures.

### 7.2 Requested address prefix

The desired human-readable address prefix is:

"75Pk"

The desired address length is:

34 characters.

This is a target that must be tested, not an assumed guaranteed result.

Determine the correct version byte and Base58Check parameters mathematically.

Build a test that generates many addresses and verifies the prefix and length.

If the target cannot be achieved with the selected version-byte approach, report the exact technical limitation and possible alternatives. Do not silently change the user's chosen prefix.

### 7.3 Key cryptography

Use secp256k1 and ECDSA.

Use a well-reviewed cryptographic library rather than writing new elliptic-curve cryptography from scratch.

### 7.4 HD wallet

The wallet must use:

- BIP32.
- BIP39.
- 24-word recovery phrase.
- Multiple addresses from one seed.
- BIP44-style derivation structure.
- Oeeoo Coin-specific derivation path.

Derivation structure:

"m/44'/TYPE'/ACCOUNT'/CHANGE/INDEX"

The Oeeoo Coin coin type must be obtained through official SLIP-0044 registration.

Until registration is complete, use a clearly marked provisional development identifier.

Never invent a number and describe it as officially registered.

### 7.5 BIP39 passphrase

Optional BIP39 passphrase support is selected.

### 7.6 Wallet gap limit

Default gap limit:

20.

It must be configurable.

### 7.7 Private-key encryption

Encryption of stored private keys is mandatory.

The wallet must support both:

- Password-based encryption.
- Hardware-backed encryption where supported by the operating system and device.

Hardware-backed security must not be falsely advertised on devices that do not support it.

Use secure key derivation, authenticated encryption, appropriate salts and nonces, and safe memory handling.

### 7.8 Backup

Wallet backup must support:

- Recovery phrase.
- Encrypted wallet backup.

Recovery and restoration must be thoroughly tested.

### 7.9 Address reuse

Address reuse is allowed by default.

The wallet must not force a new address for every transaction.

### 7.10 Wallet fee limit

Wallet fee limit is configurable.

Default maximum:

100 sat/vB.

This is a wallet policy limit, not an unconditional consensus maximum.

---

## 8. TRANSACTION MODEL

Oeeoo Coin uses the UTXO transaction model.

Implement a deterministic transaction format with:

- Version.
- Inputs.
- Outputs.
- Lock time.
- Input references.
- Script signatures.
- Output scripts.
- Transaction identifier.
- Serialization.
- Deserialization.
- Validation.

Every input must reference a valid unspent output.

Prevent:

- Double spending.
- Invalid signatures.
- Negative amounts.
- Amount overflow.
- Duplicate inputs.
- Invalid output values.
- Unauthorized coin creation.
- Invalid coinbase transactions.

Use integer smallest units throughout.

### 8.1 Script system

Implement a Bitcoin-style Script subset supporting:

- Basic payments.
- Multisignature.
- Timelocks.

Timelocks:

- CLTV.
- CSV.

The maximum multisignature limits must remain compatible with Bitcoin Legacy Script constraints.

### 8.2 Script resource protection

Protect the network from computationally expensive scripts.

Use:

- Opcode-count limits.
- Computational resource budgets.
- Stack limits.
- Script-size limits.
- Safe execution rules.
- Deterministic validation.

The exact opcode list and resource costs must be formally specified before consensus implementation is finalized.

### 8.3 Arbitrary data

Arbitrary transaction data outputs are disabled.

Do not introduce a general-purpose arbitrary-data output mechanism without explicit approval.

### 8.4 Transaction input and output limits

Input and output counts are bounded dynamically by consensus resource limits and block capacity.

Do not introduce arbitrary fixed limits without documenting their necessity.

### 8.5 Individual transaction size

Individual transaction size must be bounded by block capacity and resource validation.

The exact rule must be consistent across nodes.

---

## 9. BLOCK SIZE AND BLOCK VALIDATION

Initial maximum block size:

1 MB.

Implement exact size validation.

Reject blocks exceeding the maximum.

Ensure that transaction serialization, block serialization and resource calculations use the same canonical definitions.

Test:

- Empty and malformed blocks.
- Oversized blocks.
- Invalid Merkle roots.
- Duplicate transactions.
- Invalid coinbase.
- Incorrect rewards.
- Invalid previous hashes.
- Incorrect proof of work.
- Invalid difficulty.
- Timestamp violations.
- Invalid transaction scripts.

---

## 10. TRANSACTION FEES AND MEMPOOL

Oeeoo Coin uses:

- Dynamic transaction fees.
- Minimum relay fee.
- Dust protection.
- Recent-block fee estimates.

### 10.1 Minimum relay fee

Minimum relay fee:

1 sat/vB.

### 10.2 Dynamic fee estimation

Use recent block fee information.

Fee-estimation window:

Last 24 blocks.

Provide configurable minimum and maximum fee policies.

Default wallet maximum:

100 sat/vB.

The wallet must estimate fees using actual network conditions rather than pretending that a fixed fee guarantees a specific confirmation time.

### 10.3 Dust protection

Dust threshold:

Dynamic and script-aware.

Calculate the threshold according to output type and relevant spending/relay costs.

Do not hardcode one universal dust value without a documented technical reason.

### 10.4 Mempool

Mempool maximum memory:

300 MB by default.

It must be configurable.

Mempool transaction expiry:

14 days.

### 10.5 Replace-By-Fee

Use opt-in Replace-By-Fee (RBF).

Only transactions signaling the required opt-in policy may be replaced under the RBF rules.

Validate replacement conflicts, fees and resource limits.

### 10.6 Transfer fee clarification

An earlier discussion recorded a proposed transfer-fee figure of 0.0001 OEEOO.

The later finalized policy specifies dynamic fees, a 1 sat/vB minimum relay fee and wallet fee controls.

Do not turn 0.0001 OEEOO into a fixed consensus fee without reconciling its exact intended meaning in the technical decision log.

Treat it as a recorded historical fee figure whose relationship to the dynamic fee policy must be resolved before implementation.

---

## 11. P2P NETWORK ARCHITECTURE

Oeeoo Coin must have its own decentralized peer-to-peer network.

It must not reuse Bitcoin's network identity.

Use unique Oeeoo Coin parameters for:

- Network magic.
- P2P port.
- Genesis hash.
- Address prefix.
- DNS seeds.
- Peer discovery.
- Message identity.

### 11.1 Mainnet port

Selected port:

64775

### 11.2 Network magic

Generate a unique four-byte network magic value using a domain-separated hash of the literal string:

"Oeeoo Coin Mainnet Network Magic"

The exact four-byte result and byte order must be calculated, documented and tested.

Do not reuse Bitcoin's magic bytes.

(Calculated candidate, see Appendix A: SHA-256 of the literal string begins `e2d804d6…`, magic bytes `e2 d8 04 d6` in hash order. Final confirmation in Milestone 12.)

### 11.3 Peer connections

Maximum peer connections:

125

Maximum outbound connections:

10

### 11.4 Peer discovery

Support:

- Automatic peer discovery.
- Manual peer addition.
- DNS seed discovery.
- Hardcoded fallback peers.

Use four independent community-operated DNS seeds distributed geographically.

The actual DNS domains and operators remain to be established.

Never invent active seed domains or claim that nonexistent seed servers are operational.

### 11.5 Block propagation

Use:

Compact block relay with full-block fallback.

Implement the necessary protocol messages and validation.

### 11.6 Malicious peers

Use progressive banning for malicious peers.

Peer scoring and ban durations must be documented and designed to prevent both abuse and accidental exclusion of honest nodes.

Protect against:

- Invalid messages.
- Oversized messages.
- Malformed blocks.
- Invalid transactions.
- Connection flooding.
- Resource exhaustion.
- Peer identity abuse.
- Unsolicited data floods.

### 11.7 Blockchain storage modes

Support:

- Archival nodes.
- Pruned nodes.

The node operator chooses archival or pruned mode during installation.

Pruned nodes must retain the information necessary for their defined validation and operation requirements.

Archival nodes must preserve full historical blockchain data.

### 11.8 Chain selection

The selected chain rule is:

Greatest cumulative valid Proof of Work.

Do not select a chain based merely on the largest block height or the most peers supporting it.

### 11.9 Timestamps

Use Bitcoin-style Median-Time-Past.

Also apply a maximum two-hour future timestamp allowance.

Define the exact validation order and ensure all nodes implement it identically.

### 11.10 Light wallets

Support SPV wallets with optional full-node verification.

Clearly distinguish SPV assurances from full-node validation.

Never claim that an SPV wallet independently validates every consensus rule.

---

## 12. CUSTOM DATABASE ARCHITECTURE

A custom database architecture has been selected.

Do not blindly copy an existing Bitcoin database layout and present it as the final Oeeoo Coin architecture.

Design the storage layer for:

- Block index.
- Raw blocks.
- Transaction index.
- UTXO set.
- Chain state.
- Cumulative work.
- Mempool data.
- Wallet-related indexes where appropriate.
- Reorganization support.
- Pruning metadata.

The database must support:

- Atomic updates.
- Crash recovery.
- Consistency checks.
- Safe rollback.
- Reorganization.
- Corruption detection.
- Backup and restoration.
- Versioned schema migrations.

Keep consensus validation independent from the database implementation.

A database must never be allowed to silently change consensus results.

Document the trade-offs and benchmark storage performance.

---

## 13. MINING SYSTEM

Oeeoo Coin must support public mining from launch.

Mining must not be restricted to the founder or a private organization.

Support:

- Solo mining.
- Mining pools.
- Public participation.
- Miner block construction.
- Transaction selection.
- Coinbase construction.
- Nonce search.
- Proof-of-Work verification.
- Reward and fee calculation.

### 13.1 Mining protocols

Support both:

- Stratum V1.
- Stratum V2.

Implement the protocol interfaces carefully.

Do not confuse a mining-pool protocol with the blockchain consensus protocol.

### 13.2 Open mining pools

The mining-pool protocol is open.

Anyone may operate an independent mining pool.

No central organization should be able to prohibit an otherwise valid miner from participating in the network.

### 13.3 Miner reward

While subsidy remains available:

Miner reward = valid block subsidy + valid transaction fees.

After block 940,000:

Miner reward = valid transaction fees only.

### 13.4 Mobile mining

Do not falsely market mobile mining as efficient or necessary.

Explain hardware, electricity, heat, battery and performance limitations honestly.

---

## 14. GOVERNANCE AND PROTOCOL UPGRADES

Oeeoo Coin must not have a single permanent central authority.

No founder, developer, miner, mining pool or website operator may unilaterally change consensus rules.

Governance follows:

- Community proposals.
- Public technical discussion.
- Miner version signaling.
- Node validation.
- Community code review.

### 14.1 Miner signaling

Activation threshold:

95% signaling over a 2,016-block window.

Implement the exact counting and activation rules deterministically.

### 14.2 Activation delay

After the defined successful signaling condition:

2,016-block activation delay.

### 14.3 Hard forks

Hard forks are reserved for unavoidable major changes.

Prefer backward-compatible or less disruptive upgrades whenever technically possible.

### 14.4 Software versions

Recommend the latest stable node version.

Older versions should receive upgrade notices.

An upgrade notice must not silently override consensus validation or force an automatic consensus change.

### 14.5 Community participation

Use public GitHub Issues and Pull Requests.

Document proposals, implementation changes, test results and review decisions.

No developer should be able to secretly modify the monetary policy or consensus implementation.

---

## 15. EMERGENCY SECURITY AUTHORITY

A temporary founder emergency security authority has been selected.

This authority is limited to temporary operational and security coordination.

It must not permit the founder to:

- Unilaterally change the maximum supply.
- Create additional OEEOO.
- Reverse valid transactions.
- Rewrite blockchain history.
- Override full-node consensus validation.
- Permanently control protocol upgrades.
- Confiscate other users' funds.
- Introduce arbitrary emergency monetary changes.

No permanent emergency security committee is selected.

Do not turn the temporary authority into a permanent centralized governance structure.

Document its scope, duration, activation criteria and limitations.

Any emergency response must respect independently enforced consensus rules.

---

## 16. RPC AND NODE OPERATIONS

Remote RPC access is enabled with authentication.

Implement:

- Authenticated RPC.
- Secure credential handling.
- Access restrictions.
- Secure transport.
- Request validation.
- Rate limiting.
- Permission separation.
- Audit-safe logging.
- Safe error handling.

Do not expose unauthenticated administrative methods to the public Internet.

Do not store passwords or private credentials in source code.

Separate public read-only operations from sensitive wallet or node-administration operations.

Document safe deployment configuration.

---

## 17. RELEASE SECURITY AND SOURCE CODE

Oeeoo Coin source code will be publicly available under the MIT License.

Planned GitHub repository:

"OeeooCoin-blockchain-by-Ahmad"

Release security must include:

1. Digitally signed releases.
2. SHA-256 checksums.
3. Reproducible builds.

### 17.1 Release signing

Release authorization uses:

2-of-3 multisignature authorization.

The three custodians are:

- Founder.
- One developer.
- One independent security custodian.

The exact key-generation, custody, recovery and rotation procedures remain engineering/security tasks.

Never place release-signing private keys in the public repository.

Never allow one compromised signing key to authorize a release alone.

### 17.2 Public development

Use:

- Public commits.
- Documented releases.
- Pull requests.
- Code review.
- Issue tracking.
- Versioned specifications.
- Automated builds.
- Automated tests.

Do not claim independent security review merely because an AI has inspected the source code.

---

## 18. MAINNET AND TESTING STRATEGY

The selected strategy is:

Direct Mainnet launch.

No separate public testnet has been selected.

Do not quietly change the project policy by launching a separate public testnet.

However, direct Mainnet selection does not eliminate the requirement for internal engineering verification.

Before production launch, perform:

- Unit testing.
- Integration testing.
- Consensus test vectors.
- Serialization tests.
- Cryptographic tests.
- Monetary-supply simulations.
- Mining-reward boundary tests.
- Difficulty-retarget simulations.
- Timestamp validation tests.
- UTXO tests.
- Reorganization tests.
- Wallet recovery tests.
- Encryption tests.
- P2P message tests.
- Fuzz testing.
- Stress testing.
- Crash-recovery testing.
- Database-corruption testing.
- Independent code review.
- Independent security audit.

Use isolated unit-test and simulation harnesses where needed.

Do not confuse internal automated testing with the creation of a separate public testnet.

No real-world production funds should be placed at risk merely to discover basic programming errors.

Do not generate or publish a real Mainnet Genesis block until the implementation and its consensus parameters have been independently verified.

---

## 19. C++ SOFTWARE ARCHITECTURE

Implement Oeeoo Coin in modular C++.

Use modern, maintainable C++ and a documented build system.

Use CMake.

Use clear interfaces and separate consensus logic from networking, storage and wallet presentation.

A proposed starting layout:

```
OeeooCoin/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── CONTRIBUTING.md
├── SECURITY.md
├── docs/
│   ├── WHITEPAPER.md
│   ├── CONSENSUS.md
│   ├── MONETARY_POLICY.md
│   ├── GENESIS.md
│   ├── NETWORK.md
│   ├── WALLET.md
│   ├── MINING.md
│   ├── GOVERNANCE.md
│   └── SECURITY_MODEL.md
├── src/
│   ├── consensus/
│   ├── crypto/
│   ├── blockchain/
│   ├── transactions/
│   ├── utxo/
│   ├── script/
│   ├── mempool/
│   ├── networking/
│   ├── database/
│   ├── wallet/
│   ├── mining/
│   ├── rpc/
│   ├── governance/
│   └── node/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── consensus/
│   ├── crypto/
│   ├── wallet/
│   ├── networking/
│   ├── database/
│   └── fuzz/
├── scripts/
├── config/
└── tools/
```

This is an initial architecture proposal, not a mandate to create unnecessary components.

Use a dependency structure that prevents circular dependencies and avoids mixing consensus logic with application/UI code.

### 19.1 Development milestones

Milestone 1 — C++ foundation

Implement:

- CMake project.
- Compiler configuration.
- Dependency management.
- Integer monetary types.
- Consensus constants.
- Subsidy schedule.
- SHA-256d.
- Unit-test framework.

Milestone 2 — Cryptography and serialization

Implement:

- Hash functions.
- Block header serialization.
- Transaction serialization.
- Endianness rules.
- Merkle tree.
- Address encoding foundations.
- Cryptographic test vectors.

Milestone 3 — Core blockchain

Implement:

- Block structure.
- Genesis structure.
- Block validation.
- Chain index.
- Difficulty validation.
- Cumulative work.
- Chain selection.

Milestone 4 — Transactions and UTXO

Implement:

- Transaction creation.
- Transaction validation.
- UTXO database.
- Script interpreter.
- Signature validation.
- Multisignature.
- CLTV and CSV.

Milestone 5 — Mining

Implement:

- Coinbase construction.
- Block template creation.
- Nonce search.
- Proof-of-Work validation.
- Reward validation.
- Fee collection.

Milestone 6 — P2P networking

Implement:

- Peer connections.
- Network framing.
- Message serialization.
- Peer discovery.
- Block relay.
- Compact blocks.
- Full-block fallback.
- Peer scoring.

Milestone 7 — Database

Implement:

- Custom storage architecture.
- Atomic updates.
- Recovery.
- Reorganization.
- Archival mode.
- Pruned mode.

Milestone 8 — Wallet

Implement:

- Seed generation.
- BIP39.
- BIP32.
- Address derivation.
- P2PKH.
- Encryption.
- Recovery.
- Transaction signing.
- Multisignature.

Milestone 9 — Founder allocation

Implement:

- Founder allocation record.
- Vesting schedule.
- UTC anniversary calculations.
- Leap-year handling.
- UTXO activation.
- Reorganization-safe state.

Milestone 10 — RPC and mining pools

Implement:

- Authenticated RPC.
- Node management.
- Stratum V1.
- Stratum V2.
- Open pool interfaces.

Milestone 11 — Testing and security

Implement and execute:

- Full automated test suite.
- Fuzz testing.
- Stress testing.
- Reorganization simulations.
- Independent review.
- Security audit.
- Reproducible-build verification.

Milestone 12 — Mainnet preparation

Finalize:

- Genesis parameters.
- Network magic.
- Official address parameters.
- SLIP-0044 registration.
- DNS seeds.
- Fallback peers.
- Release signatures.
- Checksums.
- Build instructions.
- Documentation.
- Launch procedure.

Only after the required verification should production Mainnet launch be considered.

---

## 20. UNRESOLVED ENGINEERING DETAILS — DO NOT INVENT FINAL VALUES

The following matters require technical implementation, calculation, verification or registration.

They are not permission to change the finalized policy.

1. Official SLIP-0044 coin type.
2. Exact four-byte network magic and byte order.
3. Exact version byte needed to test the "75Pk" address prefix.
4. Whether the 34-character address target can be achieved.
5. Exact genesis nonce, Merkle root and block hash.
6. Final Genesis timestamp at launch.
7. Exact founder allocation data structure.
8. Exact reorganization-safe vesting state transitions.
9. Exact script opcode list.
10. Script execution resource costs.
11. Exact transaction and block resource calculations.
12. Dynamic dust formula.
13. Fee-estimation parameters.
14. Relationship of the historical 0.0001 OEEOO transfer-fee figure to the later dynamic-fee policy.
15. DNS seed domains and independent operators.
16. Hardcoded fallback peer list.
17. Peer scoring and progressive ban durations.
18. Custom database schema and performance strategy.
19. Stratum V1/V2 integration details.
20. Authenticated RPC transport and permissions.
21. Release-signing key generation and custody.
22. Key recovery and rotation procedures.
23. Exact C++ dependency versions.
24. Reproducible build environment.
25. Final protocol documentation and test vectors.

For every unresolved item:

- Explain the technical problem.
- Identify relevant security implications.
- Present a recommended implementation.
- Record the decision in the engineering log.
- Seek user approval only when the choice changes a previously finalized policy or introduces a genuine policy conflict.

Do not repeatedly ask the user questions that have already been answered.

---

## 21. DEVELOPMENT ENVIRONMENT AND MOBILE WORKFLOW

The user currently wants to begin development using an Android mobile phone.

Use Termux from a compatible official distribution source when appropriate.

The initial Android development environment may include:

- C++ compiler (Clang).
- CMake.
- Ninja.
- Git.
- OpenSSL development libraries.
- Other required, documented build dependencies.

The mobile environment is intended for initial source development, command-line compilation and testing.

Do not assume that an Android mobile build is equivalent to a production Linux node build.

Keep the project portable.

Use documented build configurations for supported platforms.

If mobile resources become insufficient, use a controlled cloud or desktop build environment without changing the C++ language or protocol decisions.

Never put private keys, passwords or wallet recovery phrases into a cloud development environment or AI prompt.

---

## 22. SECURITY ENGINEERING REQUIREMENTS

Security must be treated as a core development requirement, not an optional feature.

Protect against:

- Integer overflow.
- Integer underflow.
- Invalid monetary issuance.
- Invalid block rewards.
- Double spending.
- Replay-related transaction issues.
- Invalid signatures.
- Malformed serialization.
- Malicious P2P messages.
- Peer flooding.
- Memory exhaustion.
- CPU exhaustion.
- Database corruption.
- Chain reorganization bugs.
- Timestamp manipulation.
- Difficulty manipulation.
- Wallet key theft.
- Weak randomness.
- Insecure backup.
- RPC credential leakage.
- Unauthorized software releases.
- Supply-limit violations.
- Founder-vesting bypasses.

Use secure randomness from operating-system cryptographic sources.

Use established cryptographic libraries.

Do not create custom cryptographic algorithms merely for originality.

Do not store private keys in plaintext.

Do not log sensitive wallet data.

Do not include test private keys in production builds.

Do not expose secret credentials in error messages.

Use sanitizers, fuzzers, static analysis and compiler warnings where practical.

Document known limitations.

A successful compilation is not proof of security.

A passing unit-test suite is not proof of security.

An AI review is not a substitute for independent security audit.

---

## 23. DOCUMENTATION REQUIREMENTS

Maintain these documents as the project develops:

1. README.md
2. WHITEPAPER.md
3. CONSENSUS.md
4. MONETARY_POLICY.md
5. BLOCK_STRUCTURE.md
6. TRANSACTION_SPECIFICATION.md
7. SCRIPT_SPECIFICATION.md
8. GENESIS_SPECIFICATION.md
9. FOUNDER_VESTING.md
10. WALLET_SPECIFICATION.md
11. ADDRESS_SPECIFICATION.md
12. P2P_NETWORK_SPECIFICATION.md
13. MINING_SPECIFICATION.md
14. DATABASE_ARCHITECTURE.md
15. GOVERNANCE.md
16. RPC_SPECIFICATION.md
17. SECURITY_MODEL.md
18. BUILD_GUIDE.md
19. TESTING_GUIDE.md
20. RELEASE_PROCESS.md

Documentation must describe the actual implementation.

Never describe an unimplemented feature as operational.

---

## 24. REQUIRED AI DEVELOPMENT WORKFLOW

Follow this workflow continuously:

Step 1: Read the complete Oeeoo Coin specification.

Step 2: Create a decision register that separates finalized decisions from unresolved engineering tasks.

Step 3: Design the C++ architecture.

Step 4: Start with the smallest independently testable module.

Step 5: Write actual source files.

Step 6: Explain the purpose of each file.

Step 7: Provide exact build commands.

Step 8: Compile the code.

Step 9: Run automated tests.

Step 10: Fix errors before moving forward.

Step 11: Record the completed module and its test evidence.

Step 12: Integrate it with the next module.

For every module, report:

- Module name.
- Files created or changed.
- Functionality implemented.
- Dependencies.
- Build command.
- Test command.
- Actual test result.
- Known limitations.
- Next development step.

Do not output enormous amounts of unverified code in one response.

Do not overwrite previous working code without explaining the change.

Maintain a changelog.

Keep a clear distinction between:

- Planned.
- Designed.
- Implemented.
- Compiled.
- Unit-tested.
- Integration-tested.
- Independently reviewed.
- Security-audited.
- Production-ready.

Never use one of these labels unless its conditions are actually satisfied.

---

## 25. FINAL PROJECT ACCEPTANCE CRITERIA

Oeeoo Coin must not be described as complete merely because a wallet screen or a blockchain explorer has been created.

A genuine operational Oeeoo Coin implementation must demonstrate all of the following:

Consensus

- Independent nodes calculate identical valid consensus outcomes.
- Invalid blocks are rejected.
- Proof of Work is correctly verified.
- Difficulty adjustment is deterministic.
- Greatest cumulative valid work selects the active chain.

Monetary policy

- Maximum supply is exactly 21 million OEEOO.
- Mining issuance is exactly 20 million OEEOO.
- Founder allocation is exactly 1 million OEEOO.
- Reward boundaries are correct.
- No extra issuance is possible.
- Zero subsidy begins at block 940,001.

Blockchain

- Genesis is independently verified.
- Blocks link correctly.
- Merkle roots are verified.
- Transactions are serialized deterministically.
- Reorganizations work correctly.

Wallet

- Keys are securely generated.
- P2PKH addresses are valid.
- Transactions are signed correctly.
- Encryption works.
- Recovery works.
- Founder multisignature works.
- Founder vesting cannot be bypassed.

Network

- Independent nodes connect.
- Blocks propagate.
- Compact relay works.
- Full-block fallback works.
- Peer validation works.
- Malicious peers are handled safely.

Mining

- Solo mining works.
- Pool mining interfaces work.
- Stratum V1 and V2 support is implemented.
- Miners cannot claim invalid rewards.

Storage

- Archival mode works.
- Pruned mode works.
- Database recovery works.
- Reorganization does not corrupt the UTXO state.

Security and release

- Automated tests pass.
- Fuzzing and stress tests have been conducted.
- Independent code review is complete.
- Security audit is complete.
- Signed releases work.
- SHA-256 checksums are published.
- Reproducible builds are verified.

Only after the applicable requirements are demonstrably met should Oeeoo Coin be considered ready for production Mainnet.

---

## 26. FIRST TASK — START ACTUAL DEVELOPMENT NOW

Do not ask the user to make another large batch of protocol decisions.

The major policy decisions have already been made.

Begin with:

OEEOO COIN DEVELOPMENT PHASE 1 — C++ FOUNDATION

Your first deliverable must contain:

1. Proposed C++ project architecture.
2. CMake configuration.
3. Compiler and dependency requirements.
4. Consensus constants.
5. Integer monetary type.
6. Eight-decimal OEEOO conversion functions.
7. Exact mining subsidy implementation.
8. Exact 20-million OEEOO mining issuance verification.
9. SHA-256d implementation using a trusted cryptographic library.
10. Initial block-header structure.
11. Deterministic block-header serialization design.
12. Automated tests for monetary arithmetic and subsidy boundaries.

Use the following consensus constants as the initial source of truth:

```
MAX_SUPPLY_OEEOO       = 21000000;
FOUNDER_ALLOCATION     = 1000000;
MINING_ALLOCATION      = 20000000;
COIN                   = 100000000;
TARGET_BLOCK_TIME      = 600;
DIFFICULTY_INTERVAL    = 2016;
HALVING_INTERVAL       = 210000;
INITIAL_BLOCK_REWARD   = 50 * COIN;
MAX_BLOCK_SIZE         = 1000000;
COINBASE_MATURITY      = 100;
MAINNET_PORT           = 64775;
MAX_PEER_CONNECTIONS   = 125;
MAX_OUTBOUND_CONNECTIONS = 10;
MEMPOOL_DEFAULT_MB     = 300;
MEMPOOL_EXPIRY_DAYS    = 14;
MIN_RELAY_FEE_SAT_VB   = 1;
WALLET_DEFAULT_MAX_FEE_SAT_VB = 100;
```

All monetary constants must be stored and calculated using safe integer types.

The subsidy function must return:

```
1–210000       : 5000000000 smallest units
210001–420000  : 2500000000 smallest units
420001–630000  : 1250000000 smallest units
630001–840000  : 625000000 smallest units
840001–940000  : 312500000 smallest units
940001+        : 0
```

Verify the total issuance programmatically using integer arithmetic.

Do not use the incorrect formula of multiplying the final reward by an arbitrary fractional factor.

The correct final reward is:

312,500,000 smallest units = 3.125 OEEOO.

Do not generate a fake Genesis hash, fake network magic, fake registered coin type or fake test results.

Start with actual source code and executable tests.

The user wants to develop Oeeoo Coin step by step, with AI assistance, while preserving all the finalized decisions above.

Your first response should begin Phase 1 and provide the initial C++ project files, exact setup commands and a verifiable testing procedure.

END OF OEEOO COIN MASTER DEVELOPMENT PROMPT

---

## Appendix A — Rename record + implementation status (2026-11-05)

Renamed from the original EarthCoin master prompt on user order (2026-11-05). Only these changed:

- Project name EarthCoin → Oeeoo Coin; ticker EARTH → OEEOO.
- Planned repo `EarthCoin-blockchain-by-Ahmad` → `OeeooCoin-blockchain-by-Ahmad` (not yet created/verified).
- Network-magic literal string → "Oeeoo Coin Mainnet Network Magic"; calculated candidate: SHA-256 begins `e2d804d6…`, magic bytes `e2 d8 04 d6` in hash order (final confirmation in Milestone 12).

Verification against the implementation (2026-11-05, re-read of the original prompt):

- Total supply 21,000,000 OEEOO = 20,000,000 mining + 1,000,000 founder — in code (`MAX_MONEY`, `MINING_ALLOCATION`, `FOUNDER_ALLOCATION`) and unit-tested.
- Founder vesting (§6.1): Launch 100,000 spendable, then 100,000 every anniversary, 1,000,000 total by the 9th anniversary — recorded here and in `docs/DECISION_LOG.md`. **Vesting enforcement is NOT yet implemented — it is Milestone 9.** Only the allocation constant exists in code today.
- Reward schedule (§3.3–3.4): all 12 boundaries unit-tested; total mining issuance verified two ways as exactly 20,000,000 OEEOO.
- All §26 constants verified present in `src/consensus/consensus.h` with identical values.
- Address prefix (§7.2): version byte `0x0f` verified; `75Pk` proven achievable with vector hash160 `197a1662…` → `75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV`; 34-char length verified (§20 items 3–4 resolved by calculation).
- Genesis message (§5.1) recorded; genesis block itself NOT built yet (Milestone 12) and no genesis hash is claimed anywhere.
- SLIP-0044 (§7.4): provisional dev id 65535 in code, clearly marked NOT registered.
- Historical 0.0001 OEEOO fee figure (§10.6): recorded in the decision log as unresolved, NOT a consensus fee.
- Milestones done: Phase 1 (Milestone 1) + Milestone 2. Next: Milestone 3 (Core blockchain).
